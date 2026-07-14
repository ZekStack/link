# Memory

Link bounds accepted request and response payloads through `LinkConfig`.

```cpp
config.maxUrlSize = 512;
config.maxRequestBodySize = 8192;
config.maxResponseBodySize = 8192;
config.maxSerializedJsonSize = 8192;
config.maxHeaderCount = 16;
config.maxHeaderNameSize = 64;
config.maxHeaderValueSize = 512;
config.maxTotalHeaderSize = 4096;
config.streamChunkSize = 1024;
```

`queueSize` is the maximum number of accepted in-flight requests, including both queued and actively running requests. Active requests occupy their queue slot until completion, so `queueSize` must be at least `maxConcurrentRequests`.

Public `LinkBodyView` factories do not allocate. During submission, Link measures the body against `maxRequestBodySize` and, for JSON, `maxSerializedJsonSize` before copying it once into owned queue storage. The source text, bytes, or `JsonDocument` only needs to remain valid until the submission method returns.

Queued requests own URL, headers, serialized body, timeout, mode, and callbacks before entering the worker queue.

Payload buffers prefer PSRAM when available and fall back to internal RAM. Worker stacks can optionally use PSRAM through `LinkStackType`; queue arrays, worker records, and header entry arrays use normal allocation.

Callback storage is compile-time fixed:

```cpp
Link client;        // 64-byte callback storage
LinkClient<96> big; // 96-byte callback storage
```

If any copied field exceeds its configured limit, `fetch()` fails immediately and the request is not queued.

`maxSerializedJsonSize` is a serialized byte limit, not a cap on `JsonDocument` heap usage. While parsing a JSON response, peak memory includes the buffered response bytes plus ArduinoJson's parsed nodes and copied strings. Their size depends on the input structure and ArduinoJson's allocator.
