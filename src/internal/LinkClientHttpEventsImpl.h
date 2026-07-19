#if defined(ESP32)
template <size_t CallbackStorageSize>
LinkResult LinkClient<CallbackStorageSize>::preparePersistentHttpClient(
    WorkerRecord &worker, const char *url, uint32_t timeoutMs, esp_http_client_handle_t &client
) {
	WorkerHttpSession &session = worker.http;
	const link_internal::LinkUrlOrigin origin = link_internal::linkParseOrigin(url);
	if (!origin.valid) {
		return LinkResult::error(LinkErrorCode::InvalidUrl, "url origin is invalid");
	}

	const bool sameOrigin = persistentSessionMatchesUrl(session, url);
	const uint32_t nowMs = millis();
	const link_internal::LinkPersistentReuseDecision decision =
	    link_internal::linkEvaluatePersistentReuse(
	        session.client != nullptr,
	        session.poisoned,
	        sameOrigin,
	        nowMs,
	        session.lastUsedAtMs,
	        _config.persistentIdleTimeoutMs,
	        session.requestCount,
	        _config.persistentMaxRequestsPerHandle
	    );

	switch (decision) {
	case link_internal::LinkPersistentReuseDecision::Reuse:
		client = session.client;
		recordHttpClientReused();
		return LinkResult::ok();
	case link_internal::LinkPersistentReuseDecision::OriginChanged:
		cleanupPersistentHttpClient(worker, HttpSessionCleanupReason::OriginChanged);
		break;
	case link_internal::LinkPersistentReuseDecision::IdleExpired:
		cleanupPersistentHttpClient(worker, HttpSessionCleanupReason::IdleExpired);
		break;
	case link_internal::LinkPersistentReuseDecision::RequestLimitReached:
		cleanupPersistentHttpClient(worker, HttpSessionCleanupReason::RequestLimitReached);
		break;
	case link_internal::LinkPersistentReuseDecision::Poisoned:
		cleanupPersistentHttpClient(worker, HttpSessionCleanupReason::Poisoned);
		break;
	case link_internal::LinkPersistentReuseDecision::Create:
		break;
	}

	if (!session.originHost.assignText(origin.host, origin.hostSize)) {
		return LinkResult::error(LinkErrorCode::AllocationFailed, "http origin allocation failed");
	}
	session.originPort = origin.port;
	session.originHttps = origin.https;
	session.eventContext.owner = this;
	session.client = createHttpClient(session.eventContext, url, timeoutMs);
	if (session.client == nullptr) {
		session.originHost.clear();
		session.originPort = 0;
		session.originHttps = false;
		return LinkResult::error(LinkErrorCode::AllocationFailed, "http client allocation failed");
	}
	session.createdAtMs = nowMs;
	session.lastUsedAtMs = nowMs;
	session.requestCount = 0;
	session.poisoned = false;
	client = session.client;
	return LinkResult::ok();
}

template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::resetHttpEventContext(
    HttpEventContext &context,
    QueuedRequest &request,
    LinkResponse &response,
    const char *currentUrl,
    uint8_t redirectCount
) {
	context.owner = this;
	context.request = &request;
	context.response = &response;
	context.streamInfo.httpStatus = 0;
	context.streamInfo.contentLength = -1;
	context.streamInfo.headers.clear();
	context.streamInfo.headers.configureLimits(
	    _config.maxHeaderCount,
	    _config.maxHeaderNameSize,
	    _config.maxHeaderValueSize,
	    _config.maxTotalHeaderSize
	);
	context.streamStarted = false;
	context.streamDispositionSet = false;
	context.suppressStreamCallbacks = false;
	context.eventError = LinkError{};
	context.currentUrl = currentUrl;
	context.redirectCount = redirectCount;
	context.totalReceived = 0;
}

template <size_t CallbackStorageSize>
bool LinkClient<CallbackStorageSize>::scrubHttpClientRequest(
    esp_http_client_handle_t client, const LinkHeaders &headers, size_t appliedHeaderCount
) {
	bool clean = true;
	for (size_t i = 0; i < appliedHeaderCount; ++i) {
		const char *headerName = headers.nameAt(i);
		if (headerName == nullptr) {
			continue;
		}

		bool alreadyDeleted = false;
		const size_t headerNameSize = std::strlen(headerName);
		for (size_t previousIndex = 0; previousIndex < i; ++previousIndex) {
			const char *previousName = headers.nameAt(previousIndex);
			if (previousName != nullptr && link_internal::linkAsciiEqual(
			                                   headerName,
			                                   headerNameSize,
			                                   previousName,
			                                   std::strlen(previousName)
			                               )) {
				alreadyDeleted = true;
				break;
			}
		}
		if (!alreadyDeleted && esp_http_client_delete_header(client, headerName) != ESP_OK) {
			clean = false;
		}
	}
	if (esp_http_client_set_post_field(client, nullptr, 0) != ESP_OK) {
		clean = false;
	}
	return clean;
}

template <size_t CallbackStorageSize>
esp_err_t LinkClient<CallbackStorageSize>::httpEventHandler(esp_http_client_event_t *event) {
	if (event == nullptr || event->user_data == nullptr) {
		return ESP_OK;
	}
	HttpEventContext *context = static_cast<HttpEventContext *>(event->user_data);
	if (context->owner == nullptr) {
		return ESP_OK;
	}

	switch (event->event_id) {
	case HTTP_EVENT_ON_CONNECTED:
		context->owner->recordTransportConnected();
		return ESP_OK;
	case HTTP_EVENT_DISCONNECTED:
		context->owner->recordTransportDisconnected();
		return ESP_OK;
	default:
		break;
	}

	if (context->request == nullptr || context->response == nullptr) {
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
				context->eventError = {result.code, result.message};
				return ESP_FAIL;
			}
		}
		break;
	case HTTP_EVENT_ON_DATA: {
		if (event->data == nullptr || event->data_len <= 0) {
			break;
		}
		if (context->owner->state() == LinkState::Stopping) {
			context->eventError = {LinkErrorCode::Cancelled, "request cancelled"};
			return ESP_FAIL;
		}

		const bool streaming = context->request->responseMode == LinkResponseMode::Stream;
		if (!context->streamDispositionSet) {
			const int httpStatus = esp_http_client_get_status_code(event->client);
			if (streaming) {
				context->streamInfo.httpStatus = httpStatus;
				context->streamInfo.contentLength =
				    esp_http_client_get_content_length(event->client);
			}
			const LinkHeaders &headers =
			    streaming ? context->streamInfo.headers : context->response->headers;
			const link_internal::LinkRedirectDecision redirect =
			    link_internal::linkEvaluateRedirect(
			        context->owner->_config,
			        context->request->method,
			        httpStatus,
			        headers,
			        context->redirectCount,
			        context->currentUrl
			    );
			context->suppressStreamCallbacks =
			    redirect.action != link_internal::LinkRedirectAction::None;
			context->streamDispositionSet = true;
		}
		if (context->suppressStreamCallbacks) {
			break;
		}

		if (streaming) {
			if (!context->streamStarted) {
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
				context->eventError = {LinkErrorCode::Cancelled, "request cancelled"};
				return ESP_FAIL;
			}
			break;
		}

		const size_t chunkSize = static_cast<size_t>(event->data_len);
		const size_t currentSize = context->response->body.size();
		if (chunkSize > context->owner->_config.maxResponseBodySize ||
		    currentSize > context->owner->_config.maxResponseBodySize - chunkSize) {
			context->eventError = {LinkErrorCode::ResponseTooLarge, "response body is too large"};
			return ESP_FAIL;
		}
		if (!context->response->body
		         .append(static_cast<const uint8_t *>(event->data), chunkSize, true)) {
			context->eventError = {
			    LinkErrorCode::AllocationFailed,
			    "response body allocation failed"
			};
			return ESP_FAIL;
		}
		break;
	}
	default:
		break;
	}
	return ESP_OK;
}

#endif
