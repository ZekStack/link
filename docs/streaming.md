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
