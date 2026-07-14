# Errors

Link uses `LinkErrorCode` for lifecycle, validation, allocation, HTTP transport, JSON, and cancellation failures.

Common submission errors:

| Code | Meaning |
| --- | --- |
| `NotInitialized` | Link is not running. |
| `Stopping` | Link is shutting down. |
| `QueueFull` | The request queue has no free slot. |
| `InvalidUrl` | URL is missing or not HTTP/HTTPS. |
| `UrlTooLarge` | URL exceeds `maxUrlSize`. |
| `RequestTooLarge` | Request body exceeds `maxRequestBodySize`. |
| `CallbackTooLarge` | Callback does not fit inline storage. |
| `InvalidConfig` | Configuration is internally inconsistent, such as `queueSize < maxConcurrentRequests`. |

Common response errors:

| Code | Meaning |
| --- | --- |
| `Timeout` | HTTP operation timed out. |
| `ConnectionFailed` | ESP reported a connection or socket failure before a valid response. |
| `TlsFailed` | Verified HTTPS could not be established. |
| `SendFailed` | ESP reported request write failure. |
| `ReceiveFailed` | ESP reported response read, header fetch, closed connection, incomplete data, or another unmapped transport failure. |
| `ResponseTooLarge` | Buffered response exceeded `maxResponseBodySize`. |
| `HeaderTooLarge` | Response headers exceeded configured limits. |
| `TooManyHeaders` | A response supplied more headers than configured. |
| `AllocationFailed` | Request setup or response storage allocation failed. |
| `RedirectRejected` | A redirect violated the cross-origin or HTTPS downgrade policy. |
| `RedirectLimitReached` | A buffered or streaming GET exceeded `maxRedirects`. |
| `JsonParseFailed` | JSON parsing failed or exceeded JSON limits. |
| `Cancelled` | Request was cancelled during shutdown or by stream callback. |

Transport diagnostics are best-effort and depend on the ESP-IDF error, socket errno, and TLS details exposed by `esp_http_client`.

HTTP status codes are not Link errors. A valid `404` response is still a successful transport response.
