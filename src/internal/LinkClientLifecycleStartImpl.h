template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::validateConfig(const LinkConfig &config) const {
	if (config.queueSize == 0 || config.maxConcurrentRequests == 0) {
		return LinkResult::error(
		    LinkErrorCode::InvalidConfig,
		    "queue and concurrency must be nonzero"
		);
	}
	if (config.queueSize < config.maxConcurrentRequests) {
		return LinkResult::error(
		    LinkErrorCode::InvalidConfig,
		    "queue size must be at least max concurrent requests"
		);
	}
	if (config.connectionMode != LinkConnectionMode::PerRequest &&
	    config.connectionMode != LinkConnectionMode::PersistentPerWorker) {
		return LinkResult::error(LinkErrorCode::InvalidConfig, "connection mode is invalid");
	}
	if (!link_task_support::isValidStackSize(config.stackSizeBytes)) {
		return LinkResult::error(LinkErrorCode::InvalidConfig, "worker stack size is invalid");
	}
	if (config.defaultTimeoutMs == 0 || config.maxUrlSize == 0 || config.maxRequestBodySize == 0 ||
	    config.maxResponseBodySize == 0 || config.maxSerializedJsonSize == 0 ||
	    config.maxHeaderCount == 0 || config.maxHeaderNameSize == 0 ||
	    config.maxHeaderValueSize == 0 || config.maxTotalHeaderSize == 0 ||
	    config.streamChunkSize == 0) {
		return LinkResult::error(LinkErrorCode::InvalidConfig, "memory limits must be nonzero");
	}
	if (config.maxHeaderNameSize + config.maxHeaderValueSize > config.maxTotalHeaderSize) {
		return LinkResult::error(LinkErrorCode::InvalidConfig, "header total limit is too small");
	}
	UBaseType_t signalCapacity = 0;
	if (!link_internal::linkWorkerSignalCapacity(config, signalCapacity)) {
		return LinkResult::error(
		    LinkErrorCode::InvalidConfig,
		    "worker signal capacity is too large"
		);
	}
	return LinkResult::ok();
}

template <size_t CallbackStorageSize>
bool LinkClient<CallbackStorageSize>::shouldUsePsramStack() const {
	if (_config.stackType == LinkStackType::Psram) {
		return true;
	}
	return _config.stackType == LinkStackType::Auto && link_task_support::hasExternalStackSupport();
}

template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::init(const LinkConfig &config) {
	{
		LinkLock lock(_mutex);
		if (!lock) {
			return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
		}
		if (_state != LinkState::Uninitialized) {
			return LinkResult::error(
			    LinkErrorCode::AlreadyInitialized,
			    "link is already initialized"
			);
		}
		LinkResult configResult = validateConfig(config);
		if (!configResult) {
			return configResult;
		}

		_state = LinkState::Starting;
		_config = config;
		_diagnostics = LinkDiagnostics{};
		_slots = new (std::nothrow) QueuedRequest[config.queueSize];
		_slotUsed = new (std::nothrow) bool[config.queueSize];
		_queue = new (std::nothrow) size_t[config.queueSize];
		_workers = new (std::nothrow) WorkerRecord[config.maxConcurrentRequests];
		if (_slots == nullptr || _slotUsed == nullptr || _queue == nullptr || _workers == nullptr) {
			delete[] _slots;
			delete[] _slotUsed;
			delete[] _queue;
			delete[] _workers;
			_slots = nullptr;
			_slotUsed = nullptr;
			_queue = nullptr;
			_workers = nullptr;
			_state = LinkState::Uninitialized;
			return LinkResult::error(
			    LinkErrorCode::AllocationFailed,
			    "link storage allocation failed"
			);
		}
		for (size_t i = 0; i < config.queueSize; ++i) {
			_slotUsed[i] = false;
			_queue[i] = 0;
		}
		_queueHead = 0;
		_queueTail = 0;
		_queueCount = 0;
		_nextRequestId = 1;
		_stopWakeIssued = false;
	}

#if defined(ESP32)
	UBaseType_t signalCapacity = 0;
	if (!link_internal::linkWorkerSignalCapacity(config, signalCapacity)) {
		forceDeinitBlocking();
		return LinkResult::error(
		    LinkErrorCode::InvalidConfig,
		    "worker signal capacity is too large"
		);
	}
	_items = xSemaphoreCreateCounting(signalCapacity, 0);
	if (_items == nullptr) {
		delete[] _slots;
		delete[] _slotUsed;
		delete[] _queue;
		delete[] _workers;
		_slots = nullptr;
		_slotUsed = nullptr;
		_queue = nullptr;
		_workers = nullptr;
		_state = LinkState::Uninitialized;
		return LinkResult::error(LinkErrorCode::AllocationFailed, "link queue semaphore failed");
	}

	for (size_t i = 0; i < config.maxConcurrentRequests; ++i) {
		_workers[i].owner = this;
		_workers[i].index = i;
		_workers[i].active = true;
		_workers[i].http.eventContext.owner = this;
		char name[16]{};
		snprintf(name, sizeof(name), "link-%u", static_cast<unsigned>(i));
		const BaseType_t created = link_task_support::createTask(
		    &LinkClient::taskEntry,
		    name,
		    config.stackSizeBytes,
		    &_workers[i],
		    config.priority,
		    &_workers[i].handle,
		    config.coreId,
		    shouldUsePsramStack(),
		    _workers[i].createdWithCaps
		);
		if (created != pdPASS) {
			_workers[i].active = false;
			{
				LinkLock lock(_mutex);
				if (lock) {
					_state = LinkState::Stopping;
				}
			}
			forceDeinitBlocking();
			return LinkResult::error(
			    LinkErrorCode::AllocationFailed,
			    "worker task creation failed"
			);
		}
	}
#endif

	{
		LinkLock lock(_mutex);
		if (!lock) {
			forceDeinitBlocking();
			return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
		}
		_state = LinkState::Running;
	}
	return LinkResult::ok();
}

template <size_t CallbackStorageSize> LinkResult LinkClient<CallbackStorageSize>::deinit() {
	return deinitInternal(false);
}

template <size_t CallbackStorageSize> void LinkClient<CallbackStorageSize>::forceDeinitBlocking() {
	(void)deinitInternal(true);
}

template <size_t CallbackStorageSize> void LinkClient<CallbackStorageSize>::markStopping() {
	LinkLock lock(_mutex);
	if (lock && _state != LinkState::Uninitialized) {
		_state = LinkState::Stopping;
	}
}

template <size_t CallbackStorageSize> void LinkClient<CallbackStorageSize>::wakeWorkers() {
#if defined(ESP32)
	LinkLock lock(_mutex);
	if (!lock || _stopWakeIssued || _items == nullptr) {
		return;
	}
	_stopWakeIssued = true;
	for (size_t i = 0; i < _config.maxConcurrentRequests; ++i) {
		xSemaphoreGive(_items);
	}
#endif
}

template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::waitForWorkers(bool waitForever) {
#if defined(ESP32)
	uint32_t timeoutMs = _config.defaultTimeoutMs + 100;
	if (timeoutMs < _config.defaultTimeoutMs) {
		timeoutMs = UINT32_MAX;
	}
	const uint32_t started = millis();
	while (true) {
		bool workersRunning = false;
		{
			LinkLock lock(_mutex);
			if (!lock) {
				return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
			}
			if (_workers != nullptr) {
				for (size_t i = 0; i < _config.maxConcurrentRequests; ++i) {
					workersRunning = workersRunning || _workers[i].active;
				}
			}
		}
		if (!workersRunning) {
			return LinkResult::ok();
		}
		if (!waitForever && static_cast<uint32_t>(millis() - started) >= timeoutMs) {
			return LinkResult::error(LinkErrorCode::Timeout, "timed out waiting for link workers");
		}
		link_task_support::delayMs(10);
	}
#else
	(void)waitForever;
	return LinkResult::ok();
#endif
}

template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::freeRuntimeStorage() {
	{
		LinkLock lock(_mutex);
		if (!lock) {
			return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
		}
		if (_state == LinkState::Uninitialized) {
			return LinkResult::ok();
		}
#if defined(ESP32)
		if (_items != nullptr) {
			vSemaphoreDelete(_items);
			_items = nullptr;
		}
#endif
		delete[] _slots;
		delete[] _slotUsed;
		delete[] _queue;
		delete[] _workers;
		_slots = nullptr;
		_slotUsed = nullptr;
		_queue = nullptr;
		_workers = nullptr;
		_queueHead = 0;
		_queueTail = 0;
		_queueCount = 0;
		_stopWakeIssued = false;
		_config = LinkConfig{};
		_state = LinkState::Uninitialized;
	}
	return LinkResult::ok();
}
