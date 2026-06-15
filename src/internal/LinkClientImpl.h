#pragma once

#if defined(ESP32)
#include <errno.h>
#include <esp_err.h>
#include <esp_http_client.h>
#if __has_include(<esp_crt_bundle.h>)
#include <esp_crt_bundle.h>
#define LINK_HAS_CRT_BUNDLE 1
#else
#define LINK_HAS_CRT_BUNDLE 0
#endif
#endif

template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::validateConfig(const LinkConfig &config) const {
	if (config.queueSize == 0 || config.maxConcurrentRequests == 0) {
		return LinkResult::error(LinkErrorCode::InvalidConfig, "queue and concurrency must be nonzero");
	}
	if (config.queueSize < config.maxConcurrentRequests) {
		return LinkResult::error(
		    LinkErrorCode::InvalidConfig,
		    "queue size must be at least max concurrent requests"
		);
	}
	if (!link_task_support::isValidStackSize(config.stackSizeBytes)) {
		return LinkResult::error(LinkErrorCode::InvalidConfig, "worker stack size is invalid");
	}
	if (config.defaultTimeoutMs == 0 || config.maxUrlSize == 0 ||
	    config.maxRequestBodySize == 0 || config.maxResponseBodySize == 0 ||
	    config.maxJsonDocumentSize == 0 || config.maxHeaderCount == 0 ||
	    config.maxHeaderNameSize == 0 || config.maxHeaderValueSize == 0 ||
	    config.maxTotalHeaderSize == 0 || config.streamChunkSize == 0) {
		return LinkResult::error(LinkErrorCode::InvalidConfig, "memory limits must be nonzero");
	}
	if (config.maxHeaderNameSize + config.maxHeaderValueSize > config.maxTotalHeaderSize) {
		return LinkResult::error(LinkErrorCode::InvalidConfig, "header total limit is too small");
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
			return LinkResult::error(LinkErrorCode::AlreadyInitialized, "link is already initialized");
		}
		LinkResult configResult = validateConfig(config);
		if (!configResult) {
			return configResult;
		}

		_state = LinkState::Starting;
		_config = config;
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
			return LinkResult::error(LinkErrorCode::AllocationFailed, "link storage allocation failed");
		}
		for (size_t i = 0; i < config.queueSize; ++i) {
			_slotUsed[i] = false;
			_queue[i] = 0;
		}
		_queueHead = 0;
		_queueTail = 0;
		_queueCount = 0;
		_nextRequestId = 1;
	}

#if defined(ESP32)
	_items = xSemaphoreCreateCounting(static_cast<UBaseType_t>(config.queueSize), 0);
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
			{
				LinkLock lock(_mutex);
				if (lock) {
					_state = LinkState::Stopping;
				}
			}
			for (size_t n = 0; n < config.maxConcurrentRequests; ++n) {
				xSemaphoreGive(_items);
			}
			forceDeinitBlocking();
			return LinkResult::error(LinkErrorCode::AllocationFailed, "worker task creation failed");
		}
		_workers[i].active = true;
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

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::forceDeinitBlocking() {
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
	if (_items != nullptr) {
		for (size_t i = 0; i < _config.maxConcurrentRequests; ++i) {
			xSemaphoreGive(_items);
		}
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
		_config = LinkConfig{};
		_state = LinkState::Uninitialized;
	}
	return LinkResult::ok();
}

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
	cancelPendingRequests();
	wakeWorkers();

	LinkResult waitResult = waitForWorkers(waitForever);
	if (!waitResult) {
		return waitResult;
	}

	return freeRuntimeStorage();
}

template <size_t CallbackStorageSize>
bool LinkClient<CallbackStorageSize>::isInitialized() const {
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

template <size_t CallbackStorageSize> void LinkClient<CallbackStorageSize>::cancelPendingRequests() {
	while (true) {
		size_t slotIndex = 0;
		if (!popRequest(slotIndex)) {
			break;
		}
		QueuedRequest request;
		{
			LinkLock lock(_mutex);
			if (lock && _slots != nullptr && slotIndex < _config.queueSize) {
				request = std::move(_slots[slotIndex]);
				_slotUsed[slotIndex] = false;
			}
		}
		invokeCancelled(request);
	}
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

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::taskEntry(void *arg) {
	WorkerRecord *worker = static_cast<WorkerRecord *>(arg);
	if (worker != nullptr && worker->owner != nullptr) {
		worker->owner->workerLoop(worker);
	}
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::workerLoop(WorkerRecord *worker) {
#if defined(ESP32)
	while (true) {
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
		processRequest(_slots[slotIndex]);
		releaseSlot(slotIndex);
	}
	if (worker != nullptr) {
		LinkLock lock(_mutex);
		if (lock) {
			worker->active = false;
			worker->handle = nullptr;
		}
		link_task_support::deleteCurrentTask(worker->createdWithCaps);
	}
#else
	(void)worker;
#endif
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::processRequest(QueuedRequest &request) {
	LinkState currentState = state();
	if (currentState == LinkState::Stopping) {
		invokeCancelled(request);
		return;
	}
	performHttpRequest(request);
}

template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::addJsonAccept(LinkHeaders &headers) const {
	if (!headers.has("Accept")) {
		return headers.set("Accept", "application/json");
	}
	return LinkResult::ok();
}

#if !defined(ESP32)
template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::performHttpRequest(QueuedRequest &request) {
	if (request.responseMode == LinkResponseMode::Stream) {
		LinkStreamResult result;
		result.error = {LinkErrorCode::InternalError, "http execution requires ESP32"};
		result.httpStatus = 0;
		result.totalReceived = 0;
		if (request.onStreamEnd) {
			request.onStreamEnd(result);
		}
		return;
	}
	if (request.parseJsonResponse) {
		LinkJsonResponse response;
		response.error = {LinkErrorCode::InternalError, "http execution requires ESP32"};
		if (request.onJsonResponse) {
			request.onJsonResponse(response);
		}
		return;
	}
	LinkResponse response;
	response.error = {LinkErrorCode::InternalError, "http execution requires ESP32"};
	if (request.onResponse) {
		request.onResponse(response);
	}
}
#endif

#if defined(ESP32)
namespace link_internal_http {

inline esp_http_client_method_t toEspMethod(LinkMethod method) {
	switch (method) {
	case LinkMethod::Get:
		return HTTP_METHOD_GET;
	case LinkMethod::Post:
		return HTTP_METHOD_POST;
	case LinkMethod::Put:
		return HTTP_METHOD_PUT;
	case LinkMethod::Patch:
		return HTTP_METHOD_PATCH;
	case LinkMethod::Delete:
		return HTTP_METHOD_DELETE;
	case LinkMethod::Head:
		return HTTP_METHOD_HEAD;
	}
	return HTTP_METHOD_GET;
}

inline bool isHttps(const char *url) {
	return url != nullptr && std::strncmp(url, "https://", 8) == 0;
}

inline LinkError mapEspError(esp_err_t err, esp_http_client_handle_t client, const char *url) {
	if (err == ESP_OK) {
		return {LinkErrorCode::Ok, "ok"};
	}
	if (client != nullptr && isHttps(url)) {
		int tlsError = 0;
		int tlsFlags = 0;
		const esp_err_t tlsResult =
		    esp_http_client_get_and_clear_last_tls_error(client, &tlsError, &tlsFlags);
		if (tlsResult != ESP_OK || tlsError != 0 || tlsFlags != 0) {
			return {LinkErrorCode::TlsFailed, "https request failed"};
		}
	}
#if defined(ESP_ERR_TIMEOUT)
	if (err == ESP_ERR_TIMEOUT) {
		return {LinkErrorCode::Timeout, "http request timed out"};
	}
#endif
#if defined(ESP_ERR_HTTP_READ_TIMEOUT)
	if (err == ESP_ERR_HTTP_READ_TIMEOUT || err == ESP_ERR_HTTP_EAGAIN ||
	    err == ESP_ERR_HTTP_CONNECTING) {
		return {LinkErrorCode::Timeout, "http request timed out"};
	}
#endif
#if defined(ESP_ERR_HTTP_CONNECT)
	if (err == ESP_ERR_HTTP_CONNECT) {
		return {LinkErrorCode::ConnectionFailed, "http connection failed"};
	}
#endif
#if defined(ESP_ERR_HTTP_WRITE_DATA)
	if (err == ESP_ERR_HTTP_WRITE_DATA) {
		return {LinkErrorCode::SendFailed, "http send failed"};
	}
#endif
#if defined(ESP_ERR_HTTP_FETCH_HEADER)
	if (err == ESP_ERR_HTTP_FETCH_HEADER || err == ESP_ERR_HTTP_CONNECTION_CLOSED ||
	    err == ESP_ERR_HTTP_INCOMPLETE_DATA) {
		return {LinkErrorCode::ReceiveFailed, esp_err_to_name(err)};
	}
#endif
	if (client != nullptr) {
		const int socketError = esp_http_client_get_errno(client);
		if (socketError != 0 && socketError != -1) {
			if (socketError == ETIMEDOUT) {
				return {LinkErrorCode::Timeout, "http request timed out"};
			}
			if (socketError == ECONNREFUSED || socketError == ENETUNREACH ||
			    socketError == EHOSTUNREACH || socketError == ENOTCONN) {
				return {LinkErrorCode::ConnectionFailed, "http connection failed"};
			}
			if (socketError == EPIPE) {
				return {LinkErrorCode::SendFailed, "http send failed"};
			}
			if (socketError == ECONNRESET) {
				return {LinkErrorCode::ReceiveFailed, "http receive failed"};
			}
		}
	}
	return {LinkErrorCode::ReceiveFailed, esp_err_to_name(err)};
}

inline bool isRedirectStatus(int status) {
	return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

} // namespace link_internal_http

template <size_t CallbackStorageSize>
esp_err_t LinkClient<CallbackStorageSize>::httpEventHandler(esp_http_client_event_t *event) {
	if (event == nullptr || event->user_data == nullptr) {
		return ESP_OK;
	}
	HttpEventContext *context = static_cast<HttpEventContext *>(event->user_data);
	if (context->owner == nullptr || context->request == nullptr) {
		return ESP_OK;
	}

	switch (event->event_id) {
	case HTTP_EVENT_ON_HEADER:
		if (event->header_key != nullptr && event->header_value != nullptr) {
			LinkResult result =
			    context->request->responseMode == LinkResponseMode::Stream
			        ? context->streamInfo.headers.add(event->header_key, event->header_value)
			        : context->response->headers.add(event->header_key, event->header_value);
			if (!result) {
				context->headerFailed = true;
				return ESP_FAIL;
			}
		}
		break;
	case HTTP_EVENT_ON_DATA:
		if (event->data == nullptr || event->data_len <= 0) {
			break;
		}
		if (context->owner->state() == LinkState::Stopping) {
			context->cancelled = true;
			return ESP_FAIL;
		}
		if (context->request->responseMode == LinkResponseMode::Stream) {
			if (!context->streamStarted) {
				context->streamInfo.httpStatus = esp_http_client_get_status_code(event->client);
				context->streamInfo.contentLength = esp_http_client_get_content_length(event->client);
				context->request->onStreamStart(context->streamInfo);
				context->streamStarted = true;
			}
			LinkStreamChunk chunk;
			chunk.data = static_cast<const uint8_t *>(event->data);
			chunk.size = static_cast<size_t>(event->data_len);
			chunk.totalReceived = context->totalReceived + chunk.size;
			const LinkStreamAction action = context->request->onStreamChunk(chunk);
			context->totalReceived = chunk.totalReceived;
			if (action == LinkStreamAction::Cancel) {
				context->cancelled = true;
				return ESP_FAIL;
			}
			break;
		}

		const size_t chunkSize = static_cast<size_t>(event->data_len);
		const size_t currentSize = context->response->body.size();
		if (chunkSize > context->owner->_config.maxResponseBodySize ||
		    currentSize > context->owner->_config.maxResponseBodySize - chunkSize) {
			context->responseTooLarge = true;
			return ESP_FAIL;
		}
		if (!context->response->body.append(
		        static_cast<const uint8_t *>(event->data),
		        chunkSize,
		        true
		    )) {
			context->response->error = {LinkErrorCode::AllocationFailed, "response body allocation failed"};
			return ESP_FAIL;
		}
		break;
	default:
		break;
	}
	return ESP_OK;
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::performHttpRequest(QueuedRequest &request) {
	char *currentUrl = link_memory::duplicateString(
	    request.url.c_str(),
	    std::strlen(request.url.c_str())
	);
	if (currentUrl == nullptr) {
		if (request.responseMode == LinkResponseMode::Stream) {
			LinkStreamResult result;
			result.error = {LinkErrorCode::AllocationFailed, "url allocation failed"};
			request.onStreamEnd(result);
		} else if (request.parseJsonResponse) {
			LinkJsonResponse response;
			response.error = {LinkErrorCode::AllocationFailed, "url allocation failed"};
			request.onJsonResponse(response);
		} else {
			LinkResponse response;
			response.error = {LinkErrorCode::AllocationFailed, "url allocation failed"};
			request.onResponse(response);
		}
		return;
	}

	uint8_t redirects = 0;
	while (true) {
		LinkResponse response;
		response.headers.configureLimits(
		    _config.maxHeaderCount,
		    _config.maxHeaderNameSize,
		    _config.maxHeaderValueSize,
		    _config.maxTotalHeaderSize
		);

		HttpEventContext context;
		context.owner = this;
		context.request = &request;
		context.response = &response;
		context.streamInfo.headers.configureLimits(
		    _config.maxHeaderCount,
		    _config.maxHeaderNameSize,
		    _config.maxHeaderValueSize,
		    _config.maxTotalHeaderSize
		);

		esp_http_client_config_t httpConfig = {};
		httpConfig.url = currentUrl;
		httpConfig.timeout_ms = static_cast<int>(request.timeoutMs);
		httpConfig.event_handler = &LinkClient::httpEventHandler;
		httpConfig.user_data = &context;
		httpConfig.disable_auto_redirect = true;
		httpConfig.buffer_size = static_cast<int>(_config.streamChunkSize);
#if LINK_HAS_CRT_BUNDLE
		if (link_internal_http::isHttps(currentUrl)) {
			httpConfig.crt_bundle_attach = esp_crt_bundle_attach;
		}
#endif

		esp_http_client_handle_t client = esp_http_client_init(&httpConfig);
		if (client == nullptr) {
			link_memory::release(currentUrl);
			LinkError error = {LinkErrorCode::AllocationFailed, "http client allocation failed"};
			if (request.responseMode == LinkResponseMode::Stream) {
				LinkStreamResult result;
				result.error = error;
				request.onStreamEnd(result);
			} else if (request.parseJsonResponse) {
				LinkJsonResponse jsonResponse;
				jsonResponse.error = error;
				request.onJsonResponse(jsonResponse);
			} else {
				response.error = error;
				request.onResponse(response);
			}
			return;
		}

		esp_http_client_set_method(client, link_internal_http::toEspMethod(request.method));
		for (size_t i = 0; i < request.headers.size(); ++i) {
			esp_http_client_set_header(client, request.headers.nameAt(i), request.headers.valueAt(i));
		}
		if (request.body.size() > 0) {
			esp_http_client_set_post_field(
			    client,
			    reinterpret_cast<const char *>(request.body.data()),
			    static_cast<int>(request.body.size())
			);
		}

		const esp_err_t err = esp_http_client_perform(client);
		response.httpStatus = esp_http_client_get_status_code(client);
		context.streamInfo.httpStatus = response.httpStatus;
		context.streamInfo.contentLength = esp_http_client_get_content_length(client);
		LinkError transportError = link_internal_http::mapEspError(err, client, currentUrl);
		esp_http_client_cleanup(client);

		if (context.headerFailed) {
			response.error = {LinkErrorCode::HeaderTooLarge, "response headers exceed limits"};
		} else if (context.responseTooLarge) {
			response.error = {LinkErrorCode::ResponseTooLarge, "response body is too large"};
		} else if (context.cancelled) {
			response.error = {LinkErrorCode::Cancelled, "request cancelled"};
		} else {
			response.error = transportError;
		}

		if (response.error.code == LinkErrorCode::Ok && request.method == LinkMethod::Get &&
		    request.responseMode == LinkResponseMode::Buffered && _config.followRedirects &&
		    link_internal_http::isRedirectStatus(response.httpStatus)) {
			const char *location = response.headers.get("Location");
			if (location != nullptr && link_internal::linkUrlLooksValid(location)) {
				if (redirects >= _config.maxRedirects) {
					response.error = {
					    LinkErrorCode::RedirectLimitReached,
					    "redirect limit reached"
					};
				} else if (std::strlen(location) > _config.maxUrlSize) {
					response.error = {LinkErrorCode::UrlTooLarge, "redirect url is too large"};
				} else {
					char *nextUrl = link_memory::duplicateString(location, std::strlen(location));
					if (nextUrl == nullptr) {
						response.error = {
						    LinkErrorCode::AllocationFailed,
						    "redirect url allocation failed"
						};
					} else {
						link_memory::release(currentUrl);
						currentUrl = nextUrl;
						redirects++;
						continue;
					}
				}
			}
		}

		link_memory::release(currentUrl);

		if (request.responseMode == LinkResponseMode::Stream) {
			if (!context.streamStarted && response.error.code == LinkErrorCode::Ok) {
				request.onStreamStart(context.streamInfo);
			}
			LinkStreamResult streamResult;
			streamResult.error = response.error;
			streamResult.httpStatus = response.httpStatus;
			streamResult.totalReceived = context.totalReceived;
			request.onStreamEnd(streamResult);
			return;
		}

		if (request.parseJsonResponse) {
			LinkJsonResponse jsonResponse;
			jsonResponse.error = response.error;
			jsonResponse.httpStatus = response.httpStatus;
			LinkResult headerCopyResult = jsonResponse.headers.copyFrom(response.headers);
			if (!headerCopyResult) {
				jsonResponse.error = {headerCopyResult.code, headerCopyResult.message};
			}
			if (jsonResponse.error.code == LinkErrorCode::Ok) {
				if (response.body.size() > _config.maxJsonDocumentSize) {
					jsonResponse.error = {
					    LinkErrorCode::JsonParseFailed,
					    "json document is too large"
					};
				} else {
					DeserializationError jsonError =
					    deserializeJson(jsonResponse.json, response.body.c_str(), response.body.size());
					if (jsonError) {
						jsonResponse.error = {
						    LinkErrorCode::JsonParseFailed,
						    "json parse failed"
						};
					}
				}
			}
			request.onJsonResponse(jsonResponse);
			return;
		}

		request.onResponse(response);
		return;
	}
}
#endif
