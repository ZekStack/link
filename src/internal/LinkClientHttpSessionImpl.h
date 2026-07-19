#if !defined(ESP32)
template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::performHttpRequest(
    WorkerRecord &worker, QueuedRequest &request
) {
	(void)worker;
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

inline bool getSocketError(esp_http_client_handle_t client, int &socketError) {
#if defined(ESP_IDF_VERSION) && defined(ESP_IDF_VERSION_VAL) &&                                    \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
	if (client == nullptr) {
		return false;
	}
	socketError = esp_http_client_get_errno(client);
	return socketError != 0 && socketError != -1;
#else
	(void)client;
	(void)socketError;
	return false;
#endif
}

inline bool hasTlsError(esp_http_client_handle_t client) {
#if defined(ESP_IDF_VERSION) && defined(ESP_IDF_VERSION_VAL) &&                                    \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
	if (client == nullptr) {
		return false;
	}
	int tlsError = 0;
	int tlsFlags = 0;
	const esp_err_t tlsResult =
	    esp_http_client_get_and_clear_last_tls_error(client, &tlsError, &tlsFlags);
	return tlsResult != ESP_OK || tlsError != 0 || tlsFlags != 0;
#else
	(void)client;
	return false;
#endif
}

inline LinkError mapEspError(esp_err_t err, esp_http_client_handle_t client, const char *url) {
	if (err == ESP_OK) {
		return {LinkErrorCode::Ok, "ok"};
	}
	if (isHttps(url) && hasTlsError(client)) {
		return {LinkErrorCode::TlsFailed, "https request failed"};
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
	int socketError = 0;
	if (getSocketError(client, socketError)) {
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
	return {LinkErrorCode::ReceiveFailed, esp_err_to_name(err)};
}

inline LinkError mapSetupError(esp_err_t err, const char *message) {
	if (err == ESP_OK)
		return {LinkErrorCode::Ok, "ok"};
	if (err == ESP_ERR_NO_MEM) {
		return {LinkErrorCode::AllocationFailed, "http request setup allocation failed"};
	}
	return {LinkErrorCode::InternalError, message};
}

} // namespace link_internal_http

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::recordHttpClientCreated() {
	LinkLock lock(_mutex);
	if (!lock)
		return;
	_diagnostics.httpClientCreates++;
	_diagnostics.activeHttpClients++;
	if (_diagnostics.activeHttpClients > _diagnostics.peakHttpClients) {
		_diagnostics.peakHttpClients = _diagnostics.activeHttpClients;
	}
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::recordHttpClientReused() {
	LinkLock lock(_mutex);
	if (lock) {
		_diagnostics.httpClientReuses++;
	}
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::recordTransportConnected() {
	LinkLock lock(_mutex);
	if (lock) {
		_diagnostics.transportConnectedEvents++;
	}
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::recordTransportDisconnected() {
	LinkLock lock(_mutex);
	if (lock) {
		_diagnostics.transportDisconnectedEvents++;
	}
}

template <size_t CallbackStorageSize>
esp_http_client_handle_t LinkClient<CallbackStorageSize>::createHttpClient(
    HttpEventContext &context, const char *url, uint32_t timeoutMs
) {
	esp_http_client_config_t httpConfig = {};
	httpConfig.url = url;
	httpConfig.timeout_ms = static_cast<int>(timeoutMs);
	httpConfig.event_handler = &LinkClient::httpEventHandler;
	httpConfig.user_data = &context;
	httpConfig.disable_auto_redirect = true;
	httpConfig.buffer_size = static_cast<int>(_config.streamChunkSize);
#if LINK_HAS_CRT_BUNDLE
	if (link_internal_http::isHttps(url)) {
		httpConfig.crt_bundle_attach = esp_crt_bundle_attach;
	}
#endif

	esp_http_client_handle_t client = esp_http_client_init(&httpConfig);
	if (client != nullptr) {
		recordHttpClientCreated();
	}
	return client;
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::cleanupHttpClient(esp_http_client_handle_t client) {
	if (client == nullptr)
		return;
	(void)esp_http_client_cleanup(client);
	LinkLock lock(_mutex);
	if (!lock)
		return;
	_diagnostics.httpClientCleanups++;
	if (_diagnostics.activeHttpClients > 0) {
		_diagnostics.activeHttpClients--;
	}
}

template <size_t CallbackStorageSize>
bool LinkClient<CallbackStorageSize>::persistentSessionMatchesUrl(
    const WorkerHttpSession &session, const char *url
) const {
	if (session.client == nullptr || session.originHost.empty())
		return false;
	const link_internal::LinkUrlOrigin next = link_internal::linkParseOrigin(url);
	link_internal::LinkUrlOrigin stored;
	stored.host = session.originHost.c_str();
	stored.hostSize = session.originHost.size();
	stored.port = session.originPort;
	stored.https = session.originHttps;
	stored.valid = true;
	return link_internal::linkSameOrigin(stored, next);
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::cleanupPersistentHttpClient(
    WorkerRecord &worker, HttpSessionCleanupReason reason
) {
	WorkerHttpSession &session = worker.http;
	if (session.client == nullptr)
		return;

	cleanupHttpClient(session.client);
	session.client = nullptr;
	session.originHost.clear();
	session.originPort = 0;
	session.originHttps = false;
	session.createdAtMs = 0;
	session.lastUsedAtMs = 0;
	session.requestCount = 0;
	session.poisoned = false;
	session.eventContext.request = nullptr;
	session.eventContext.response = nullptr;
	session.eventContext.currentUrl = nullptr;
	session.eventContext.streamInfo.headers.clear();

	LinkLock lock(_mutex);
	if (!lock)
		return;
	switch (reason) {
	case HttpSessionCleanupReason::OriginChanged:
		_diagnostics.originReuseMisses++;
		break;
	case HttpSessionCleanupReason::IdleExpired:
		_diagnostics.idleEvictions++;
		break;
	case HttpSessionCleanupReason::RequestLimitReached:
		_diagnostics.requestLimitEvictions++;
		break;
	case HttpSessionCleanupReason::Poisoned:
		_diagnostics.poisonedEvictions++;
		break;
	case HttpSessionCleanupReason::None:
	case HttpSessionCleanupReason::Shutdown:
		break;
	}
}

#endif
