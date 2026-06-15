#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "internal/LinkCallback.h"
#include "internal/LinkMemory.h"
#include "internal/LinkMutex.h"
#include "internal/LinkTaskSupport.h"

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#if defined(ESP32)
#include <esp_err.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

enum class LinkErrorCode : uint8_t {
	Ok,
	NotInitialized,
	AlreadyInitialized,
	InvalidConfig,
	Stopping,
	InvalidUrl,
	UrlTooLarge,
	QueueFull,
	AllocationFailed,
	CallbackTooLarge,
	RequestTooLarge,
	ResponseTooLarge,
	HeaderTooLarge,
	TooManyHeaders,
	Timeout,
	DnsFailed,
	ConnectionFailed,
	TlsFailed,
	SendFailed,
	ReceiveFailed,
	RedirectLimitReached,
	JsonSerializeFailed,
	JsonParseFailed,
	Cancelled,
	InternalError
};

enum class LinkState : uint8_t {
	Uninitialized,
	Starting,
	Running,
	Stopping
};

enum class LinkStackType : uint8_t {
	Internal,
	Psram,
	Auto
};

enum class LinkMethod : uint8_t {
	Get,
	Post,
	Put,
	Patch,
	Delete,
	Head
};

enum class LinkResponseMode : uint8_t {
	Buffered,
	Stream
};

enum class LinkBodyType : uint8_t {
	None,
	Text,
	Json,
	Binary
};

enum class LinkStreamAction : uint8_t {
	Continue,
	Cancel
};

struct LinkError {
	LinkErrorCode code = LinkErrorCode::Ok;
	const char *message = "ok";

	explicit operator bool() const {
		return code == LinkErrorCode::Ok;
	}
};

struct LinkResult {
	LinkErrorCode code = LinkErrorCode::Ok;
	const char *message = "ok";

	explicit operator bool() const {
		return code == LinkErrorCode::Ok;
	}

	static LinkResult ok(const char *message = "ok") {
		return {LinkErrorCode::Ok, message};
	}

	static LinkResult error(LinkErrorCode code, const char *message) {
		return {code, message};
	}
};

struct LinkConfig {
	uint32_t stackSizeBytes = 4096;
	UBaseType_t priority = 1;
	BaseType_t coreId = tskNO_AFFINITY;
	LinkStackType stackType = LinkStackType::Auto;

	size_t queueSize = 10;
	size_t maxConcurrentRequests = 3;

	uint32_t defaultTimeoutMs = 15000;

	size_t maxUrlSize = 512;
	size_t maxRequestBodySize = 8192;
	size_t maxResponseBodySize = 8192;
	size_t maxJsonDocumentSize = 8192;
	size_t maxHeaderCount = 16;
	size_t maxHeaderNameSize = 64;
	size_t maxHeaderValueSize = 512;
	size_t maxTotalHeaderSize = 4096;
	size_t streamChunkSize = 1024;

	bool followRedirects = true;
	uint8_t maxRedirects = 3;
};

class LinkBody;
namespace link_internal {
LinkResult linkBodyFromJson(const JsonDocument &json, size_t maxBytes, LinkBody &out);
}

class LinkHeaders {
  public:
	LinkHeaders() = default;
	~LinkHeaders();

	LinkHeaders(const LinkHeaders &other);
	LinkHeaders &operator=(const LinkHeaders &other);

	LinkHeaders(LinkHeaders &&other) noexcept;
	LinkHeaders &operator=(LinkHeaders &&other) noexcept;

	void configureLimits(
	    size_t maxHeaderCount,
	    size_t maxHeaderNameSize,
	    size_t maxHeaderValueSize,
	    size_t maxTotalHeaderSize
	);

	LinkResult add(const char *name, const char *value);
	LinkResult set(const char *name, const char *value);
	bool has(const char *name) const;
	const char *get(const char *name) const;
	const char *nameAt(size_t index) const;
	const char *valueAt(size_t index) const;
	size_t size() const;
	size_t totalSize() const;
	void clear();
	LinkResult copyFrom(const LinkHeaders &other);

  private:
	struct Entry {
		char *name = nullptr;
		char *value = nullptr;
		size_t nameSize = 0;
		size_t valueSize = 0;
	};

	static bool namesEqual(const char *left, const char *right);
	static bool validName(const char *name);
	static size_t safeLength(const char *value);
	int findIndex(const char *name) const;
	LinkResult ensureStorage();
	LinkResult validate(const char *name, const char *value, size_t replacingIndex) const;
	bool copyEntry(Entry &entry, const char *name, const char *value);
	void freeEntry(Entry &entry);
	void moveFrom(LinkHeaders &other);

	Entry *_entries = nullptr;
	size_t _count = 0;
	size_t _capacity = 0;
	size_t _totalSize = 0;
	size_t _maxHeaderCount = 16;
	size_t _maxHeaderNameSize = 64;
	size_t _maxHeaderValueSize = 512;
	size_t _maxTotalHeaderSize = 4096;
};

class LinkBody {
  public:
	LinkBody() = default;
	LinkBody(const LinkBody &other);
	LinkBody &operator=(const LinkBody &other);
	LinkBody(LinkBody &&other) noexcept = default;
	LinkBody &operator=(LinkBody &&other) noexcept = default;

	static LinkBody none();
	static LinkBody text(const char *value);
	static LinkBody json(const JsonDocument &json);
	static LinkBody bytes(const uint8_t *data, size_t size);

	LinkBodyType type() const;
	const uint8_t *data() const;
	const char *c_str() const;
	size_t size() const;
	LinkErrorCode status() const;
	bool valid() const;
	void clear();
	LinkResult copyFrom(const LinkBody &other);

  private:
	friend struct LinkBodyBuilder;
	friend LinkResult link_internal::linkBodyFromJson(const JsonDocument &json, size_t maxBytes, LinkBody &out);

	LinkBodyType _type = LinkBodyType::None;
	LinkOwnedBuffer _buffer;
	LinkErrorCode _status = LinkErrorCode::Ok;
};

struct LinkResponse {
	LinkError error;
	int httpStatus = 0;
	LinkHeaders headers;
	LinkOwnedBuffer body;

	explicit operator bool() const {
		return error.code == LinkErrorCode::Ok;
	}

	bool succeeded() const {
		return error.code == LinkErrorCode::Ok;
	}

	bool isHttpOk() const {
		return httpStatus >= 200 && httpStatus <= 299;
	}

	bool isRedirect() const {
		return httpStatus >= 300 && httpStatus <= 399;
	}

	bool isClientError() const {
		return httpStatus >= 400 && httpStatus <= 499;
	}

	bool isServerError() const {
		return httpStatus >= 500 && httpStatus <= 599;
	}
};

struct LinkJsonResponse {
	LinkError error;
	int httpStatus = 0;
	LinkHeaders headers;
	JsonDocument json;

	explicit operator bool() const {
		return error.code == LinkErrorCode::Ok;
	}

	bool isHttpOk() const {
		return httpStatus >= 200 && httpStatus <= 299;
	}
};

struct LinkStreamInfo {
	int httpStatus = 0;
	int contentLength = -1;
	LinkHeaders headers;
};

struct LinkStreamChunk {
	const uint8_t *data = nullptr;
	size_t size = 0;
	size_t totalReceived = 0;
};

struct LinkStreamResult {
	LinkError error;
	int httpStatus = 0;
	size_t totalReceived = 0;

	explicit operator bool() const {
		return error.code == LinkErrorCode::Ok;
	}
};

template <size_t CallbackStorageSize> struct LinkRequestT {
	using ResponseCallback = LinkCallback<void(const LinkResponse &), CallbackStorageSize>;
	using JsonResponseCallback = LinkCallback<void(const LinkJsonResponse &), CallbackStorageSize>;
	using StreamStartCallback = LinkCallback<void(const LinkStreamInfo &), CallbackStorageSize>;
	using StreamChunkCallback =
	    LinkCallback<LinkStreamAction(const LinkStreamChunk &), CallbackStorageSize>;
	using StreamEndCallback = LinkCallback<void(const LinkStreamResult &), CallbackStorageSize>;

	LinkMethod method = LinkMethod::Get;
	const char *url = nullptr;
	LinkHeaders headers;
	LinkBody body;
	uint32_t timeoutMs = 0;
	LinkResponseMode responseMode = LinkResponseMode::Buffered;
	bool parseJsonResponse = false;

	ResponseCallback onResponse;
	JsonResponseCallback onJsonResponse;
	StreamStartCallback onStreamStart;
	StreamChunkCallback onStreamChunk;
	StreamEndCallback onStreamEnd;
};

using LinkRequest = LinkRequestT<64>;

const char *linkErrorCodeToString(LinkErrorCode code);
const char *linkStateToString(LinkState state);

template <size_t CallbackStorageSize = 64> class LinkClient;
using Link = LinkClient<64>;

namespace link_internal {

bool linkUrlLooksValid(const char *url);
LinkResult linkBodyFromJson(const JsonDocument &json, size_t maxBytes, LinkBody &out);

template <size_t CallbackStorageSize> struct QueuedLinkRequest {
	using Request = LinkRequestT<CallbackStorageSize>;

	uint32_t id = 0;
	LinkMethod method = LinkMethod::Get;
	LinkResponseMode responseMode = LinkResponseMode::Buffered;
	bool parseJsonResponse = false;
	uint32_t timeoutMs = 0;
	LinkOwnedBuffer url;
	LinkHeaders headers;
	LinkBody body;

	typename Request::ResponseCallback onResponse;
	typename Request::JsonResponseCallback onJsonResponse;
	typename Request::StreamStartCallback onStreamStart;
	typename Request::StreamChunkCallback onStreamChunk;
	typename Request::StreamEndCallback onStreamEnd;

	void reset() {
		id = 0;
		method = LinkMethod::Get;
		responseMode = LinkResponseMode::Buffered;
		parseJsonResponse = false;
		timeoutMs = 0;
		url.clear();
		headers.clear();
		body.clear();
		onResponse.reset();
		onJsonResponse.reset();
		onStreamStart.reset();
		onStreamChunk.reset();
		onStreamEnd.reset();
	}

	LinkResult copyFrom(const Request &request, const LinkConfig &config, uint32_t requestId) {
		reset();
		if (request.url == nullptr || !linkUrlLooksValid(request.url)) {
			return LinkResult::error(LinkErrorCode::InvalidUrl, "url is invalid");
		}
		const size_t urlSize = std::strlen(request.url);
		if (urlSize > config.maxUrlSize) {
			return LinkResult::error(LinkErrorCode::UrlTooLarge, "url is too large");
		}
		if (!url.assignText(request.url, urlSize)) {
			return LinkResult::error(LinkErrorCode::AllocationFailed, "url allocation failed");
		}
		if (request.body.size() > config.maxRequestBodySize) {
			return LinkResult::error(LinkErrorCode::RequestTooLarge, "request body is too large");
		}

		headers.configureLimits(
		    config.maxHeaderCount,
		    config.maxHeaderNameSize,
		    config.maxHeaderValueSize,
		    config.maxTotalHeaderSize
		);
		// Copy manually so queued headers are revalidated against the active LinkConfig limits.
		for (size_t i = 0; i < request.headers.size(); ++i) {
			LinkResult headerResult =
			    headers.add(request.headers.nameAt(i), request.headers.valueAt(i));
			if (!headerResult) {
				return headerResult;
			}
		}

		LinkResult bodyResult = body.copyFrom(request.body);
		if (!bodyResult) {
			return bodyResult;
		}

		id = requestId;
		method = request.method;
		responseMode = request.responseMode;
		parseJsonResponse = request.parseJsonResponse;
		timeoutMs = request.timeoutMs == 0 ? config.defaultTimeoutMs : request.timeoutMs;
		onResponse = request.onResponse;
		onJsonResponse = request.onJsonResponse;
		onStreamStart = request.onStreamStart;
		onStreamChunk = request.onStreamChunk;
		onStreamEnd = request.onStreamEnd;

		if (responseMode == LinkResponseMode::Buffered && !parseJsonResponse && !onResponse) {
			return LinkResult::error(LinkErrorCode::CallbackTooLarge, "response callback is required");
		}
		if (parseJsonResponse && !onJsonResponse) {
			return LinkResult::error(LinkErrorCode::CallbackTooLarge, "json callback is required");
		}
		if (responseMode == LinkResponseMode::Stream &&
		    (!onStreamStart || !onStreamChunk || !onStreamEnd)) {
			return LinkResult::error(LinkErrorCode::CallbackTooLarge, "stream callbacks are required");
		}
		return LinkResult::ok();
	}
};

} // namespace link_internal

template <size_t CallbackStorageSize> class LinkClient {
	static_assert(CallbackStorageSize > 0, "CallbackStorageSize must be greater than zero");

  public:
	using Request = LinkRequestT<CallbackStorageSize>;
	using ResponseCallback = typename Request::ResponseCallback;
	using JsonResponseCallback = typename Request::JsonResponseCallback;
	using StreamStartCallback = typename Request::StreamStartCallback;
	using StreamChunkCallback = typename Request::StreamChunkCallback;
	using StreamEndCallback = typename Request::StreamEndCallback;

	LinkClient() = default;

	~LinkClient() {
		forceDeinitBlocking();
	}

	LinkClient(const LinkClient &) = delete;
	LinkClient &operator=(const LinkClient &) = delete;

	LinkResult init(const LinkConfig &config = LinkConfig());
	LinkResult deinit();

	bool isInitialized() const;
	LinkState state() const;

	LinkResult fetch(const Request &request);

	template <typename Callback> LinkResult get(const char *url, Callback &&callback) {
		LinkHeaders headers;
		return get(url, headers, std::forward<Callback>(callback));
	}

	template <typename Callback>
	LinkResult get(const char *url, const LinkHeaders &headers, Callback &&callback) {
		Request request;
		request.method = LinkMethod::Get;
		request.url = url;
		LinkResult headerResult = request.headers.copyFrom(headers);
		if (!headerResult) {
			return headerResult;
		}
		if (!request.onResponse.assign(std::forward<Callback>(callback))) {
			return LinkResult::error(LinkErrorCode::CallbackTooLarge, "response callback is too large");
		}
		return fetch(request);
	}

	template <typename Callback>
	LinkResult post(const char *url, const LinkBody &body, Callback &&callback) {
		LinkHeaders headers;
		return post(url, headers, body, std::forward<Callback>(callback));
	}

	template <typename Callback>
	LinkResult post(
	    const char *url,
	    const LinkHeaders &headers,
	    const LinkBody &body,
	    Callback &&callback
	) {
		Request request;
		request.method = LinkMethod::Post;
		request.url = url;
		LinkResult headerResult = request.headers.copyFrom(headers);
		if (!headerResult) {
			return headerResult;
		}
		LinkResult bodyResult = request.body.copyFrom(body);
		if (!bodyResult) {
			return bodyResult;
		}
		if (!request.onResponse.assign(std::forward<Callback>(callback))) {
			return LinkResult::error(LinkErrorCode::CallbackTooLarge, "response callback is too large");
		}
		return fetch(request);
	}

	template <typename Callback> LinkResult getJson(const char *url, Callback &&callback) {
		LinkHeaders headers;
		return getJson(url, headers, std::forward<Callback>(callback));
	}

	template <typename Callback>
	LinkResult getJson(const char *url, const LinkHeaders &headers, Callback &&callback) {
		Request request;
		request.method = LinkMethod::Get;
		request.url = url;
		LinkResult headerResult = request.headers.copyFrom(headers);
		if (!headerResult) {
			return headerResult;
		}
		request.parseJsonResponse = true;
		LinkResult acceptResult = addJsonAccept(request.headers);
		if (!acceptResult) {
			return acceptResult;
		}
		if (!request.onJsonResponse.assign(std::forward<Callback>(callback))) {
			return LinkResult::error(LinkErrorCode::CallbackTooLarge, "json callback is too large");
		}
		return fetch(request);
	}

	template <typename Callback>
	LinkResult postJson(const char *url, const JsonDocument &json, Callback &&callback) {
		LinkHeaders headers;
		return postJson(url, headers, json, std::forward<Callback>(callback));
	}

	template <typename Callback>
	LinkResult postJson(
	    const char *url,
	    const LinkHeaders &headers,
	    const JsonDocument &json,
	    Callback &&callback
	) {
		LinkBody body;
		LinkConfig configSnapshot;
		LinkResult configResult = getConfigSnapshot(configSnapshot);
		if (!configResult) {
			return configResult;
		}
		size_t maxJsonBody = configSnapshot.maxRequestBodySize;
		if (configSnapshot.maxJsonDocumentSize < maxJsonBody) {
			maxJsonBody = configSnapshot.maxJsonDocumentSize;
		}
		LinkResult bodyResult = link_internal::linkBodyFromJson(json, maxJsonBody, body);
		if (!bodyResult) {
			return bodyResult;
		}

		Request request;
		request.method = LinkMethod::Post;
		request.url = url;
		LinkResult headerResult = request.headers.copyFrom(headers);
		if (!headerResult) {
			return headerResult;
		}
		LinkResult requestBodyResult = request.body.copyFrom(body);
		if (!requestBodyResult) {
			return requestBodyResult;
		}
		request.parseJsonResponse = true;
		LinkResult acceptResult = addJsonAccept(request.headers);
		if (!acceptResult) {
			return acceptResult;
		}
		if (!request.headers.has("Content-Type")) {
			LinkResult headerResult = request.headers.set("Content-Type", "application/json");
			if (!headerResult) {
				return headerResult;
			}
		}
		if (!request.onJsonResponse.assign(std::forward<Callback>(callback))) {
			return LinkResult::error(LinkErrorCode::CallbackTooLarge, "json callback is too large");
		}
		return fetch(request);
	}

	template <typename StartCallback, typename ChunkCallback, typename EndCallback>
	LinkResult getStream(
	    const char *url,
	    StartCallback &&onStart,
	    ChunkCallback &&onChunk,
	    EndCallback &&onEnd
	) {
		LinkHeaders headers;
		return getStream(
		    url,
		    headers,
		    std::forward<StartCallback>(onStart),
		    std::forward<ChunkCallback>(onChunk),
		    std::forward<EndCallback>(onEnd)
		);
	}

	template <typename StartCallback, typename ChunkCallback, typename EndCallback>
	LinkResult getStream(
	    const char *url,
	    const LinkHeaders &headers,
	    StartCallback &&onStart,
	    ChunkCallback &&onChunk,
	    EndCallback &&onEnd
	) {
		Request request;
		request.method = LinkMethod::Get;
		request.url = url;
		LinkResult headerResult = request.headers.copyFrom(headers);
		if (!headerResult) {
			return headerResult;
		}
		request.responseMode = LinkResponseMode::Stream;
		if (!request.onStreamStart.assign(std::forward<StartCallback>(onStart)) ||
		    !request.onStreamChunk.assign(std::forward<ChunkCallback>(onChunk)) ||
		    !request.onStreamEnd.assign(std::forward<EndCallback>(onEnd))) {
			return LinkResult::error(LinkErrorCode::CallbackTooLarge, "stream callback is too large");
		}
		return fetch(request);
	}

  private:
	using QueuedRequest = link_internal::QueuedLinkRequest<CallbackStorageSize>;

	struct WorkerRecord {
		LinkClient *owner = nullptr;
		size_t index = 0;
		TaskHandle_t handle = nullptr;
		bool createdWithCaps = false;
		bool active = false;
	};

#if defined(ESP32)
	struct HttpEventContext {
		LinkClient *owner = nullptr;
		QueuedRequest *request = nullptr;
		LinkResponse *response = nullptr;
		LinkStreamInfo streamInfo;
		bool streamStarted = false;
		bool cancelled = false;
		bool responseTooLarge = false;
		bool headerFailed = false;
		size_t totalReceived = 0;
	};

	static esp_err_t httpEventHandler(esp_http_client_event_t *event);
#endif

	static void taskEntry(void *arg);
	void workerLoop(WorkerRecord *worker);
	bool popRequest(size_t &slotIndex);
	void releaseSlot(size_t slotIndex);
	void cancelPendingRequests();
	void invokeCancelled(QueuedRequest &request);
	void processRequest(QueuedRequest &request);
	LinkResult validateConfig(const LinkConfig &config) const;
	LinkResult getConfigSnapshot(LinkConfig &out) const;
	bool shouldUsePsramStack() const;
	LinkResult addJsonAccept(LinkHeaders &headers) const;
	LinkResult deinitInternal(bool waitForever);
	void forceDeinitBlocking();
	void markStopping();
	void wakeWorkers();
	LinkResult waitForWorkers(bool waitForever);
	LinkResult freeRuntimeStorage();

#if defined(ESP32)
	void performHttpRequest(QueuedRequest &request);
#else
	void performHttpRequest(QueuedRequest &request);
#endif

	mutable LinkMutex _mutex;
	LinkConfig _config{};
	// When deinit() times out, Stopping means workers may still own slots.
	// Do not free task-owned storage until all workers become inactive.
	LinkState _state = LinkState::Uninitialized;
	QueuedRequest *_slots = nullptr;
	bool *_slotUsed = nullptr;
	size_t *_queue = nullptr;
	size_t _queueHead = 0;
	size_t _queueTail = 0;
	size_t _queueCount = 0;
	WorkerRecord *_workers = nullptr;
	uint32_t _nextRequestId = 1;

#if defined(ESP32)
	SemaphoreHandle_t _items = nullptr;
#endif
};

#include "internal/LinkClientImpl.h"
