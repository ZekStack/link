template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::validateConfig(const LinkConfig &config) const {
	if (!Strata::validMemoryPolicy(config.memory)) {
		return LinkResult::error(LinkErrorCode::InvalidConfig, "memory policy is invalid");
	}
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
	if (config.defaultTimeoutMs > static_cast<uint32_t>(INT_MAX)) {
		return LinkResult::error(
		    LinkErrorCode::InvalidConfig,
		    "default timeout exceeds ESP-IDF limit"
		);
	}
	if (config.maxRequestBodySize > static_cast<size_t>(INT_MAX)) {
		return LinkResult::error(
		    LinkErrorCode::InvalidConfig,
		    "request body limit exceeds ESP-IDF limit"
		);
	}
	if (config.streamChunkSize > static_cast<size_t>(INT_MAX)) {
		return LinkResult::error(
		    LinkErrorCode::InvalidConfig,
		    "stream chunk size exceeds ESP-IDF limit"
		);
	}
	if (config.maxHeaderNameSize > config.maxTotalHeaderSize ||
	    config.maxHeaderValueSize > config.maxTotalHeaderSize - config.maxHeaderNameSize) {
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
	}

	LinkLock lifecycleLock(_lifecycleMutex);
	if (!lifecycleLock) {
		return LinkResult::error(LinkErrorCode::InternalError, "lifecycle mutex lock failed");
	}

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
		_diagnostics.allocationPlacement = config.memory.allocation;
		_diagnostics.workerStackPlacement = config.memory.taskStack;
		_slots = link_memory::allocateArray<QueuedRequest>(config.queueSize, config.memory.allocation);
		_slotUsed = link_memory::allocateArray<bool>(config.queueSize, config.memory.allocation);
		_workers = link_memory::allocateArray<WorkerRecord>(
		    config.maxConcurrentRequests,
		    config.memory.allocation
		);
		if (_slots == nullptr || _slotUsed == nullptr || _workers == nullptr) {
			link_memory::releaseArray(_slots, config.queueSize);
			link_memory::releaseArray(_slotUsed, config.queueSize);
			link_memory::releaseArray(_workers, config.maxConcurrentRequests);
			_slots = nullptr;
			_slotUsed = nullptr;
			_workers = nullptr;
			_config = LinkConfig{};
			_state = LinkState::Uninitialized;
			return LinkResult::error(
			    LinkErrorCode::AllocationFailed,
			    "link storage allocation failed"
			);
		}
		_diagnostics.requestSlotRegion = Strata::regionOf(_slots);
		_diagnostics.workerStorageRegion = Strata::regionOf(_workers);
		for (size_t i = 0; i < config.queueSize; ++i) {
			_slotUsed[i] = false;
		}
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
	_dispatchQueue = Strata::FreeRTOS::Queue<WorkerSignal>::create(
	    Strata::FreeRTOS::QueueConfig{
	        .length = static_cast<size_t>(signalCapacity),
	        .storagePlacement = config.memory.allocation,
	        .usage = Strata::FreeRTOS::QueueUsage::TaskOnly,
	    }
	);
	if (!_dispatchQueue) {
		forceDeinitBlocking();
		return LinkResult::error(LinkErrorCode::AllocationFailed, "link dispatch queue failed");
	}
	_jsonAllocator.emplace(config.memory.allocation);
	{
		LinkLock lock(_mutex);
		if (!lock) {
			forceDeinitBlocking();
			return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
		}
		_diagnostics.dispatchQueueStorageRegion = _dispatchQueue.storageRegion();
	}

	for (size_t i = 0; i < config.maxConcurrentRequests; ++i) {
		WorkerRecord &worker = _workers[i];
		{
			LinkLock lock(_mutex);
			if (!lock) {
				forceDeinitBlocking();
				return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
			}
			worker.owner = this;
			worker.index = i;
			worker.active = true;
			worker.readyForDelete = false;
			worker.http.originHost.setPlacement(config.memory.allocation);
			worker.http.eventContext.owner = this;
			worker.http.eventContext.streamInfo.headers.configurePlacement(config.memory.allocation);
		}

		char name[16]{};
		snprintf(name, sizeof(name), "link-%u", static_cast<unsigned>(i));
		worker.task = Strata::FreeRTOS::Task::create(
		    &LinkClient::taskEntry,
		    &worker,
		    Strata::FreeRTOS::TaskConfig{
		        .name = name,
		        .stackBytes = config.stackSizeBytes,
		        .stackPlacement = config.memory.taskStack,
		        .priority = config.priority,
		        .affinity = static_cast<int32_t>(config.coreId),
		    }
		);
		if (!worker.task) {
			{
				LinkLock lock(_mutex);
				if (lock) {
					worker.active = false;
					_state = LinkState::Stopping;
				}
			}
			forceDeinitBlocking();
			return LinkResult::error(
			    LinkErrorCode::AllocationFailed,
			    "worker task creation failed"
			);
		}
		{
			LinkLock lock(_mutex);
			if (lock) {
				switch (worker.task.stackRegion()) {
				case Strata::Region::Internal:
					_diagnostics.workerStacksInternal++;
					break;
				case Strata::Region::External:
					_diagnostics.workerStacksExternal++;
					break;
				case Strata::Region::Unknown:
					_diagnostics.workerStacksUnknown++;
					break;
				}
			}
		}
	}
#endif

	{
		LinkLock lock(_mutex);
		if (!lock) {
			forceDeinitBlocking();
			return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
		}
		if (_state != LinkState::Starting) {
			forceDeinitBlocking();
			return LinkResult::error(LinkErrorCode::InternalError, "link startup state changed");
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
	size_t workerCount = 0;
	{
		LinkLock lock(_mutex);
		if (!lock || _stopWakeIssued || !_dispatchQueue) {
			return;
		}
		_stopWakeIssued = true;
		workerCount = _config.maxConcurrentRequests;
	}
	const WorkerSignal stopSignal{0, true};
	for (size_t i = 0; i < workerCount; ++i) {
		(void)_dispatchQueue.send(stopSignal, portMAX_DELAY);
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
		bool ownedTasksRemain = false;
		if (_workers != nullptr) {
			for (size_t i = 0; i < _config.maxConcurrentRequests; ++i) {
				bool readyForDelete = false;
				{
					LinkLock lock(_mutex);
					if (!lock) {
						return LinkResult::error(
						    LinkErrorCode::InternalError,
						    "link mutex lock failed"
						);
					}
					workersRunning = workersRunning || _workers[i].active;
					readyForDelete = _workers[i].readyForDelete;
					if (readyForDelete) {
						_workers[i].readyForDelete = false;
					}
				}
				if (readyForDelete) {
					_workers[i].task.reset();
				}
				ownedTasksRemain = ownedTasksRemain || _workers[i].task.valid();
			}
		}
		if (!workersRunning && !ownedTasksRemain) {
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
	LinkLock lock(_mutex);
	if (!lock) {
		return LinkResult::error(LinkErrorCode::InternalError, "link mutex lock failed");
	}
	if (_state == LinkState::Uninitialized) {
		return LinkResult::ok();
	}
	const size_t queueSize = _config.queueSize;
	const size_t workerCount = _config.maxConcurrentRequests;
#if defined(ESP32)
	_dispatchQueue.reset();
	_jsonAllocator.reset();
	if (_workers != nullptr) {
		for (size_t i = 0; i < workerCount; ++i) {
			_workers[i].task.reset();
		}
	}
#endif
	link_memory::releaseArray(_slots, queueSize);
	link_memory::releaseArray(_slotUsed, queueSize);
	link_memory::releaseArray(_workers, workerCount);
	_slots = nullptr;
	_slotUsed = nullptr;
	_workers = nullptr;
	_stopWakeIssued = false;
	_config = LinkConfig{};
	_state = LinkState::Uninitialized;
	return LinkResult::ok();
}
