template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::deinitInternal(bool waitForever) {
	{
		LinkLock lock(_mutex);
		if (!lock) {
			return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
		}
		if (_state == LinkState::Uninitialized) {
			return LinkResult::ok();
		}
	}

	markStopping();
	wakeWorkers();

	LinkResult waitResult = waitForWorkers(waitForever);
	if (!waitResult) {
		return waitResult;
	}

	return freeRuntimeStorage();
}

template <size_t CallbackStorageSize> bool LinkClient<CallbackStorageSize>::isInitialized() const {
	LinkLock lock(const_cast<LinkMutex &>(_mutex));
	return lock && _state == LinkState::Running;
}

template <size_t CallbackStorageSize> LinkState LinkClient<CallbackStorageSize>::state() const {
	LinkLock lock(const_cast<LinkMutex &>(_mutex));
	if (!lock) {
		return LinkState::Uninitialized;
	}
	return _state;
}

template <size_t CallbackStorageSize>
LinkDiagnostics LinkClient<CallbackStorageSize>::diagnostics() const {
	LinkLock lock(const_cast<LinkMutex &>(_mutex));
	return lock ? _diagnostics : LinkDiagnostics{};
}

template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::fetch(const Request &request) {
	QueuedRequest queued;
	LinkConfig configSnapshot;
	uint32_t requestId = 0;
	{
		LinkLock lock(_mutex);
		if (!lock) {
			return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
		}
		if (_state == LinkState::Stopping) {
			return LinkResult::error(LinkErrorCode::Stopping, "link is stopping");
		}
		if (_state != LinkState::Running) {
			return LinkResult::error(LinkErrorCode::NotInitialized, "link is not initialized");
		}
		configSnapshot = _config;
		requestId = _nextRequestId++;
	}

	LinkResult copyResult = queued.copyFrom(request, configSnapshot, requestId);
	if (!copyResult) {
		return copyResult;
	}

	{
		LinkLock lock(_mutex);
		if (!lock) {
			return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
		}
		if (_state == LinkState::Stopping) {
			return LinkResult::error(LinkErrorCode::Stopping, "link is stopping");
		}
		if (_state != LinkState::Running) {
			return LinkResult::error(LinkErrorCode::NotInitialized, "link is not initialized");
		}
		if (_queueCount >= _config.queueSize) {
			return LinkResult::error(LinkErrorCode::QueueFull, "link queue is full");
		}

		size_t slotIndex = _config.queueSize;
		for (size_t i = 0; i < _config.queueSize; ++i) {
			if (!_slotUsed[i]) {
				slotIndex = i;
				break;
			}
		}
		if (slotIndex == _config.queueSize) {
			return LinkResult::error(LinkErrorCode::QueueFull, "link queue is full");
		}

		_slots[slotIndex] = std::move(queued);
		_slotUsed[slotIndex] = true;
		_queue[_queueTail] = slotIndex;
		_queueTail = (_queueTail + 1) % _config.queueSize;
		_queueCount++;
		_diagnostics.requestsSubmitted++;
	}

#if defined(ESP32)
	if (_items != nullptr) {
		xSemaphoreGive(_items);
	}
#endif
	return LinkResult::ok();
}

template <size_t CallbackStorageSize>
bool LinkClient<CallbackStorageSize>::popRequest(size_t &slotIndex) {
	LinkLock lock(_mutex);
	if (!lock || _queueCount == 0 || _queue == nullptr) {
		return false;
	}
	slotIndex = _queue[_queueHead];
	_queueHead = (_queueHead + 1) % _config.queueSize;
	_queueCount--;
	return true;
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::releaseSlot(size_t slotIndex) {
	LinkLock lock(_mutex);
	if (!lock || _slots == nullptr || _slotUsed == nullptr || slotIndex >= _config.queueSize) {
		return;
	}
	_slots[slotIndex].reset();
	_slotUsed[slotIndex] = false;
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::invokeCancelled(QueuedRequest &request) {
	if (request.responseMode == LinkResponseMode::Stream) {
		if (request.onStreamEnd) {
			LinkStreamResult result;
			result.error = {LinkErrorCode::Cancelled, "request cancelled"};
			result.httpStatus = 0;
			result.totalReceived = 0;
			request.onStreamEnd(result);
		}
		return;
	}
	if (request.parseJsonResponse) {
		if (request.onJsonResponse) {
			LinkJsonResponse response;
			response.error = {LinkErrorCode::Cancelled, "request cancelled"};
			request.onJsonResponse(response);
		}
		return;
	}
	if (request.onResponse) {
		LinkResponse response;
		response.error = {LinkErrorCode::Cancelled, "request cancelled"};
		request.onResponse(response);
	}
}

template <size_t CallbackStorageSize> void LinkClient<CallbackStorageSize>::taskEntry(void *arg) {
	WorkerRecord *worker = static_cast<WorkerRecord *>(arg);
	if (worker != nullptr && worker->owner != nullptr) {
		worker->owner->workerLoop(worker);
	}
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::workerLoop(WorkerRecord *worker) {
#if defined(ESP32)
	while (true) {
		{
			LinkLock lock(_mutex);
			if (lock && _state == LinkState::Stopping && _queueCount == 0) {
				break;
			}
		}
		if (_items != nullptr) {
			xSemaphoreTake(_items, portMAX_DELAY);
		}
		{
			LinkLock lock(_mutex);
			if (lock && _state == LinkState::Stopping && _queueCount == 0) {
				break;
			}
		}
		size_t slotIndex = 0;
		if (!popRequest(slotIndex)) {
			continue;
		}
		processRequest(*worker, _slots[slotIndex]);
		releaseSlot(slotIndex);
	}
	if (worker != nullptr) {
		cleanupPersistentHttpClient(*worker, HttpSessionCleanupReason::Shutdown);
		const bool createdWithCaps = worker->createdWithCaps;
		{
			LinkLock lock(_mutex);
			if (lock) {
				worker->active = false;
				worker->handle = nullptr;
			}
		}
		link_task_support::deleteCurrentTask(createdWithCaps);
	}
#else
	(void)worker;
#endif
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::recordRequestCompleted() {
	LinkLock lock(_mutex);
	if (lock) {
		_diagnostics.requestsCompleted++;
	}
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::processRequest(
    WorkerRecord &worker, QueuedRequest &request
) {
	LinkState currentState = state();
	if (currentState == LinkState::Stopping) {
		invokeCancelled(request);
		recordRequestCompleted();
		return;
	}
	performHttpRequest(worker, request);
	recordRequestCompleted();
}

template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::addJsonAccept(LinkHeaders &headers) const {
	if (!headers.has("Accept")) {
		return headers.set("Accept", "application/json");
	}
	return LinkResult::ok();
}

