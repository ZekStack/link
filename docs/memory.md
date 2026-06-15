# Memory

Link keeps request and response memory bounded through `LinkConfig`.

```cpp
config.maxUrlSize = 512;
config.maxRequestBodySize = 8192;
config.maxResponseBodySize = 8192;
config.maxJsonDocumentSize = 8192;
config.maxHeaderCount = 16;
config.maxHeaderNameSize = 64;
config.maxHeaderValueSize = 512;
config.maxTotalHeaderSize = 4096;
config.streamChunkSize = 1024;
```

Queued requests copy URL, headers, body, timeout, mode, and callbacks into owned storage before entering the worker queue.

Internal allocations prefer PSRAM when available and fall back to internal RAM.

Callback storage is compile-time fixed:

```cpp
Link client;        // 64-byte callback storage
LinkClient<96> big; // 96-byte callback storage
```

If any copied field exceeds its configured limit, `fetch()` fails immediately and the request is not queued.
