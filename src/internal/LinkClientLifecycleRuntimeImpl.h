template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::deinitInternal(bool waitForever) {
	LinkLock lifecycleLock(_lifecycleMutex);
	if (!lifecycleLock) {
		return LinkResult::error(LinkErrorCode::InternalError, "lifecycle mutex lock failed");
	}
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
	if (_slots == nullptr || _slotUsed == nullptr) {
		return LinkResult::error(LinkErrorCode::InternalError, "link runtime storage is missing");
	}

	const uint32_t requestId = _nextRequestId++;
	LinkResult copyResult = queued.copyFrom(request, _config, requestId);
	if (!copyResult) {
		return copyResult;
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

#if defined(ESP32)
	const WorkerSignal signal{slotIndex, false};
	if (!_dispatchQueue || !_dispatchQueue.send(signal, 0)) {
		_slots[slotIndex].reset();
		_slotUsed[slotIndex] = false;
		return LinkResult::error(LinkErrorCode::InternalError, "worker signal failed");
	}
#endif

	_diagnostics.requestsSubmitted++;
	return LinkResult::ok();
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
#if defined(ESP32)
			LinkJsonResponse response(&*_jsonAllocator);
			response.headers.configurePlacement(_config.memory.allocation);
#else
			LinkJsonResponse response;
#endif
			response.error = {LinkErrorCode::Cancelled, "request cancelled"};
			request.onJsonResponse(response);
		}
		return;
	}
	if (request.onResponse) {
		LinkResponse response;
		response.headers.configurePlacement(_config.memory.allocation);
		response.body.setPlacement(_config.memory.allocation);
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
		WorkerSignal signal;
		if (!_dispatchQueue.receive(signal, portMAX_DELAY)) {
			continue;
		}
		if (signal.stop) {
			break;
		}
		if (signal.slotIndex >= _config.queueSize || _slots == nullptr || _slotUsed == nullptr) {
			continue;
		}
		processRequest(*worker, _slots[signal.slotIndex]);
		releaseSlot(signal.slotIndex);
	}
	if (worker != nullptr) {
		cleanupPersistentHttpClient(*worker, HttpSessionCleanupReason::Shutdown);
		{
			LinkLock lock(_mutex);
			if (lock) {
				worker->active = false;
				worker->readyForDelete = true;
			}
		}
		link_task_support::suspendCurrentTask();
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
void LinkClient<CallbackStorageSize>::processRequest(WorkerRecord &worker, QueuedRequest &request) {
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
