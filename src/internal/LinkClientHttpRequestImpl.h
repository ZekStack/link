#if defined(ESP32)
template <size_t CallbackStorageSize>
void LinkClient<CallbackStorageSize>::performHttpRequest(
    WorkerRecord &worker, QueuedRequest &request
) {
	char *currentUrl =
	    link_memory::duplicateString(request.url.c_str(), std::strlen(request.url.c_str()));
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
	bool includeRequestHeaders = true;
	while (true) {
		LinkResponse response;
		response.headers.configureLimits(
		    _config.maxHeaderCount,
		    _config.maxHeaderNameSize,
		    _config.maxHeaderValueSize,
		    _config.maxTotalHeaderSize
		);

		HttpEventContext temporaryContext;
		HttpEventContext *context = &temporaryContext;
		esp_http_client_handle_t client = nullptr;
		bool persistent = _config.connectionMode == LinkConnectionMode::PersistentPerWorker;
		LinkError setupError;

		if (persistent) {
			LinkResult persistentResult =
			    preparePersistentHttpClient(worker, currentUrl, request.timeoutMs, client);
			if (!persistentResult) {
				setupError = {persistentResult.code, persistentResult.message};
			} else {
				context = &worker.http.eventContext;
			}
		} else {
			temporaryContext.owner = this;
			client = createHttpClient(temporaryContext, currentUrl, request.timeoutMs);
			if (client == nullptr) {
				setupError = {LinkErrorCode::AllocationFailed, "http client allocation failed"};
			}
		}

		if (client == nullptr) {
			link_memory::release(currentUrl);
			if (request.responseMode == LinkResponseMode::Stream) {
				LinkStreamResult result;
				result.error = setupError;
				request.onStreamEnd(result);
			} else if (request.parseJsonResponse) {
				LinkJsonResponse jsonResponse;
				jsonResponse.error = setupError;
				request.onJsonResponse(jsonResponse);
			} else {
				response.error = setupError;
				request.onResponse(response);
			}
			return;
		}

		resetHttpEventContext(*context, request, response, currentUrl, redirects);

		size_t appliedHeaderCount = 0;
		setupError = link_internal_http::mapSetupError(
		    esp_http_client_set_url(client, currentUrl),
		    "http url setup failed"
		);
		if (setupError.code == LinkErrorCode::Ok) {
			setupError = link_internal_http::mapSetupError(
			    esp_http_client_set_timeout_ms(client, static_cast<int>(request.timeoutMs)),
			    "http timeout setup failed"
			);
		}
		if (setupError.code == LinkErrorCode::Ok) {
			setupError = link_internal_http::mapSetupError(
			    esp_http_client_set_method(client, link_internal_http::toEspMethod(request.method)),
			    "http method setup failed"
			);
		}
		for (size_t i = 0; includeRequestHeaders && setupError.code == LinkErrorCode::Ok &&
		                   i < request.headers.size();
		     ++i) {
			const char *headerName = request.headers.nameAt(i);
			setupError = link_internal_http::mapSetupError(
			    esp_http_client_set_header(client, headerName, request.headers.valueAt(i)),
			    "http header setup failed"
			);
			if (setupError.code == LinkErrorCode::Ok) {
				appliedHeaderCount++;
			}
		}
		if (setupError.code == LinkErrorCode::Ok && request.body.size() > 0) {
			setupError = link_internal_http::mapSetupError(
			    esp_http_client_set_post_field(
			        client,
			        reinterpret_cast<const char *>(request.body.data()),
			        static_cast<int>(request.body.size())
			    ),
			    "http request body setup failed"
			);
		}

		const esp_err_t err =
		    setupError.code == LinkErrorCode::Ok ? esp_http_client_perform(client) : ESP_FAIL;
		response.httpStatus = esp_http_client_get_status_code(client);
		context->streamInfo.httpStatus = response.httpStatus;
		context->streamInfo.contentLength = esp_http_client_get_content_length(client);
		LinkError transportError = setupError.code == LinkErrorCode::Ok
		                               ? link_internal_http::mapEspError(err, client, currentUrl)
		                               : setupError;
		const bool scrubbed = !link_internal::linkShouldScrubHttpClientRequest(persistent) ||
		                      scrubHttpClientRequest(client, request.headers, appliedHeaderCount);

		response.error =
		    link_internal::linkPreserveOperationError(context->eventError, transportError);
		if (!scrubbed && response.error.code == LinkErrorCode::Ok) {
			response.error = {LinkErrorCode::InternalError, "http request cleanup failed"};
		}
		const bool poisoned = response.error.code != LinkErrorCode::Ok;

		if (persistent) {
			worker.http.lastUsedAtMs = millis();
			worker.http.requestCount++;
			worker.http.poisoned = poisoned;
			if (poisoned) {
				cleanupPersistentHttpClient(worker, HttpSessionCleanupReason::Poisoned);
			}
		} else {
			cleanupHttpClient(client);
		}

		context->request = nullptr;
		context->response = nullptr;
		context->currentUrl = nullptr;

		if (response.error.code == LinkErrorCode::Ok) {
			const LinkHeaders &responseHeaders = request.responseMode == LinkResponseMode::Stream
			                                         ? context->streamInfo.headers
			                                         : response.headers;
			const link_internal::LinkRedirectDecision redirect =
			    link_internal::linkEvaluateRedirect(
			        _config,
			        request.method,
			        response.httpStatus,
			        responseHeaders,
			        redirects,
			        currentUrl
			    );
			if (redirect.action == link_internal::LinkRedirectAction::Error) {
				response.error = redirect.error;
			} else if (redirect.action == link_internal::LinkRedirectAction::Follow) {
				char *nextUrl =
				    link_memory::duplicateString(redirect.location, std::strlen(redirect.location));
				if (nextUrl == nullptr) {
					response.error = {
					    LinkErrorCode::AllocationFailed,
					    "redirect url allocation failed"
					};
				} else {
					if (redirect.stripRequestHeaders)
						includeRequestHeaders = false;
					link_memory::release(currentUrl);
					currentUrl = nextUrl;
					redirects++;
					continue;
				}
			}
		}

		link_memory::release(currentUrl);

		if (request.responseMode == LinkResponseMode::Stream) {
			if (!context->streamStarted && response.error.code == LinkErrorCode::Ok) {
				request.onStreamStart(context->streamInfo);
			}
			LinkStreamResult streamResult;
			streamResult.error = response.error;
			streamResult.httpStatus = response.httpStatus;
			streamResult.totalReceived = context->totalReceived;
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
				if (response.body.size() > _config.maxSerializedJsonSize) {
					jsonResponse.error = {
					    LinkErrorCode::JsonParseFailed,
					    "serialized json response is too large"
					};
				} else {
					DeserializationError jsonError = deserializeJson(
					    jsonResponse.json,
					    response.body.c_str(),
					    response.body.size()
					);
					if (jsonError) {
						jsonResponse.error = {LinkErrorCode::JsonParseFailed, "json parse failed"};
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
