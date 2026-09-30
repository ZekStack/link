# Streaming

Link supports streaming in both directions:

- `getStream()` streams a response without buffering the full response body.
- request-body streaming sends a known-length request body without copying the full payload into Link-owned queue storage.

## Streaming responses

`getStream()` downloads response data without buffering the full body.

```cpp
client.getStream(url, onStart, onChunk, onEnd);
```

The chunk callback receives data valid only during the callback. Return `LinkStreamAction::Cancel` to stop the request; `onEnd` then receives `LinkErrorCode::Cancelled`.

## Streaming request bodies

Use `postStreamBody()` for the common POST case:

```cpp
client.postStreamBody(
    url,
    contentLength,
    [](size_t offset, uint8_t *destination, size_t capacity) -> size_t {
        // Fill up to capacity bytes starting at offset.
        return produced;
    },
    [](const LinkResponse &response) {
        // Normal buffered response handling.
    }
);
```

For PUT, PATCH, DELETE, streamed responses, or JSON response parsing, configure `LinkRequestT::streamBody` and call `fetch()` directly:

```cpp
LinkRequest request;
request.method = LinkMethod::Put;
request.url = url;
request.streamBody.contentLength = contentLength;
request.streamBody.read.assign(readCallback);
request.onResponse.assign(responseCallback);
client.fetch(request);
```

The reader callback contract is:

- `offset` is the logical byte offset requested by Link.
- `capacity` is at most `LinkConfig::streamChunkSize` and never exceeds the remaining body length.
- return between 1 and `capacity` bytes while data remains;
- returning 0 before `contentLength` bytes are produced, or returning more than `capacity`, fails with `RequestBodyReadFailed`;
- a zero-length streamed body is valid when a reader callback is configured;
- the source should be seekable/re-readable because an HTTP attempt may start again from offset 0.

Link copies the reader callback into the queued request, but objects referenced by that callback remain application-owned and must outlive the request's terminal response callback.

`maxRequestBodySize` is the logical limit for buffered and streamed request bodies. Raising it for a streamed request does not allocate that amount of RAM. Link allocates only a `streamChunkSize` scratch buffer while the worker performs the upload.

Link owns HTTP `Content-Length` framing for streamed bodies from `streamBody.contentLength`.

## Request and response streaming together

Request-body streaming is independent from `LinkResponseMode`. A single request may stream its upload and also set `responseMode = LinkResponseMode::Stream`; the normal `onStreamStart`, `onStreamChunk`, and `onStreamEnd` callbacks then process the response.

## Cancellation and failures

Shutdown is checked between upload chunks and writes. An active streamed upload terminates with `Cancelled` when Link enters `Stopping`.

If the source cannot provide the declared bytes, Link reports `RequestBodyReadFailed`. HTTP transport write failures remain `SendFailed`.

`LinkConfig::streamChunkSize` controls both the intended response stream buffer size and the scratch buffer used for streamed uploads. Neither stream mode allocates storage for the full payload.

## Redirects

Streaming GET requests follow redirects when `LinkConfig::followRedirects` is enabled. Link supports `301`, `302`, `303`, `307`, and `308` responses with absolute `http://` or `https://` `Location` headers and enforces `maxRedirects` and `maxUrlSize`.

Same-origin redirects are followed by default. Cross-origin redirects require `allowCrossOriginRedirects`; when enabled, Link strips all caller-supplied request headers for the remainder of the redirect chain. HTTPS-to-HTTP redirects are rejected unless `allowHttpsToHttpRedirects` is also enabled.

Intermediate redirect responses are not exposed to stream callbacks. `onStart` runs once for the final response, `onChunk` receives only the final response body, and `LinkStreamResult::totalReceived` counts only final response bytes. Redirect bodies are discarded without being buffered.

If redirect following is disabled, or a redirect has a missing, relative, or invalid `Location`, Link treats that response as final and streams it normally. Redirect policy, redirect limit, and redirect URL size failures are reported to `onEnd` without starting the stream.
