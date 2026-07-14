# Streaming

`getStream()` downloads response data without buffering the full body.

```cpp
client.getStream(
    url,
    onStart,
    onChunk,
    onEnd
);
```

The chunk callback receives data valid only during the callback.

```cpp
LinkStreamAction onChunk(const LinkStreamChunk &chunk) {
    return LinkStreamAction::Continue;
}
```

Return `LinkStreamAction::Cancel` to stop the request. The end callback then receives `LinkErrorCode::Cancelled`.

`LinkConfig::streamChunkSize` controls the intended worker read buffer size. Stream mode does not allocate a full response body.

## Redirects

Streaming GET requests follow redirects when `LinkConfig::followRedirects` is enabled. Link supports `301`, `302`, `303`, `307`, and `308` responses with absolute `http://` or `https://` `Location` headers and enforces `maxRedirects` and `maxUrlSize`.

Same-origin redirects are followed by default. Cross-origin redirects require `allowCrossOriginRedirects`; when enabled, Link strips all caller-supplied request headers for the remainder of the redirect chain. HTTPS-to-HTTP redirects are rejected unless `allowHttpsToHttpRedirects` is also enabled.

Intermediate redirect responses are not exposed to stream callbacks. `onStart` runs once for the final response, `onChunk` receives only the final response body, and `LinkStreamResult::totalReceived` counts only final response bytes. Redirect bodies are discarded without being buffered.

If redirect following is disabled, or a redirect has a missing, relative, or invalid `Location`, Link treats that response as final and streams it normally. Redirect policy, redirect limit, and redirect URL size failures are reported to `onEnd` without starting the stream.
