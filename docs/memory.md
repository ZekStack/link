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

Queued requests own their URL, headers, serialized body, timeout, response mode, and callbacks before entering the worker queue.

Payload buffers prefer PSRAM when available and fall back to internal RAM. Worker stacks can optionally use PSRAM through `LinkStackType`; queue arrays, worker records, and header entry arrays use normal allocation.

## ESP-IDF parameter bounds

Some ESP-IDF HTTP client parameters use signed `int` values. Link validates every public value before narrowing it:

- `defaultTimeoutMs` must be between `1` and `INT_MAX`;
- an explicit per-request timeout must be between `1` and `INT_MAX`;
- `maxRequestBodySize` must not exceed `INT_MAX`;
- `streamChunkSize` must not exceed `INT_MAX`.

An invalid configuration is rejected by `init()`. An oversized individual request timeout is rejected before the request is queued.

## Explicit copy behavior

Allocation-backed response storage is move-oriented:

- `LinkOwnedBuffer`, `LinkHeaders`, `LinkBody`, and `LinkResponse` are not implicitly copyable;
- their move operations transfer ownership without allocation;
- explicit `copyFrom()` operations report allocation failure to the caller.

`LinkResponse::copyFrom()` provides a strong result contract: it first duplicates headers and body into temporary ownership. If either allocation fails, the destination response remains unchanged.

This prevents a copied response from reporting transport success while silently losing its body or headers.

## Callback storage

Callback storage is compile-time fixed:

```cpp
Link client;        // 64-byte callback storage
LinkClient<96> big; // 96-byte callback storage
```

If any copied field exceeds its configured limit, `fetch()` fails immediately and the request is not queued.

`maxSerializedJsonSize` is a serialized byte limit, not a cap on `JsonDocument` heap usage. While parsing a JSON response, peak memory includes the buffered response bytes plus ArduinoJson's parsed nodes and copied strings. Their size depends on the input structure and ArduinoJson's allocator.
