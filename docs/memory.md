# Memory

Link `v0.2.0` routes Link-owned dynamic memory and owned FreeRTOS storage through Strata `v0.1.2`.

## Policy

`LinkConfig` uses the shared ZekStack memory vocabulary:

```cpp
LinkConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

The defaults are both `PreferExternal`, preserving the practical v0.1.1 behavior of preferring PSRAM while falling back to internal memory.

Available placements are:

- `Default` - use the Strata backend default;
- `Internal` - require internal memory;
- `PreferExternal` - prefer external memory and fall back internally;
- `RequireExternal` - require external memory and fail instead of falling back.

`RequireExternal` is intentionally strict. If external memory is unavailable or exhausted, Link reports `AllocationFailed`; it does not silently consume internal RAM.

## What `memory.allocation` controls

The general allocation policy applies to Link-owned movable storage:

- queued request records and slot bookkeeping;
- worker-record storage;
- Strata dispatch-queue item storage;
- copied request URLs and redirect URLs;
- request/response headers and their names/values;
- serialized request bodies;
- buffered response bodies;
- persistent per-worker origin host storage;
- Link-created parsed `LinkJsonResponse::json` storage through `Strata::ArduinoJson::Allocator`.

Strata keeps the control blocks for its static FreeRTOS tasks, queues, and mutexes in internal memory even when their movable storage uses another placement.

## What `memory.taskStack` controls

Each Link worker owns a `Strata::FreeRTOS::Task`. `memory.taskStack` controls the task stack placement. Strata owns the stack and task control block; Link owns the worker lifecycle and HTTP behavior.

A worker never self-deletes its Strata task. On shutdown it cleans up its HTTP session, marks itself ready for external deletion, and suspends. The caller executing `deinit()` then resets the Strata task owner, which deletes the task and releases the static stack/control storage safely.

## Ownership boundaries

Not every allocation observed while a request is running belongs to Link.

A caller-provided request `JsonDocument` remains caller-owned. Link measures and serializes it during submission but does not replace its allocator.

ESP-IDF's internal `esp_http_client` allocations also remain owned by ESP-IDF. Link controls the lifetime of the HTTP client handle, but Strata does not intercept ESP-IDF's private allocations.

Parsed JSON responses are different: Link creates `LinkJsonResponse::json`, so its ArduinoJson allocator is Strata-backed and follows `memory.allocation`.

## Bounded request and response data

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

`queueSize` is the maximum number of accepted in-flight requests, including queued and active requests. Active requests retain their slot until completion, so `queueSize` must be at least `maxConcurrentRequests`.

Public `LinkBodyView` factories do not allocate. During submission, Link validates the body limits and copies the body into owned queue storage before returning. The source text, bytes, or request `JsonDocument` only needs to remain valid until the submission call completes.

## Explicit copy behavior

Allocation-backed response storage remains move-oriented:

- `LinkOwnedBuffer`, `LinkHeaders`, `LinkBody`, and `LinkResponse` are not implicitly copyable;
- moves transfer ownership without allocating;
- explicit `copyFrom()` operations return allocation failures.

`LinkResponse::copyFrom()` first duplicates headers and body into temporary ownership. If either operation fails, the destination response remains unchanged.

The host tests retain an allocation-failure injection layer in front of Strata specifically to validate these error paths. Production allocation still delegates directly to Strata.

## Diagnostics

`LinkDiagnostics` separates requested policy from observed storage:

```cpp
LinkDiagnostics d = client.diagnostics();

Strata::toString(d.allocationPlacement);
Strata::toString(d.workerStackPlacement);
Strata::toString(d.requestSlotRegion);
Strata::toString(d.workerStorageRegion);
Strata::toString(d.dispatchQueueStorageRegion);

d.workerStacksInternal;
d.workerStacksExternal;
d.workerStacksUnknown;
```

Worker stack regions are counted rather than represented by one value because `PreferExternal` can place different worker stacks in different regions as memory availability changes during initialization.

## ESP-IDF parameter bounds

Some ESP-IDF HTTP client parameters use signed `int` values. Link validates them before narrowing:

- `defaultTimeoutMs` must be between `1` and `INT_MAX`;
- an explicit per-request timeout must be between `1` and `INT_MAX`;
- `maxRequestBodySize` must not exceed `INT_MAX`;
- `streamChunkSize` must not exceed `INT_MAX`.

An invalid configuration is rejected by `init()`. An oversized request-specific timeout is rejected before queue publication.

## v0.1.1 migration

| v0.1.1 | v0.2.0 |
| --- | --- |
| `LinkStackType::Auto` | `Strata::Placement::PreferExternal` |
| `LinkStackType::Internal` | `Strata::Placement::Internal` |
| `LinkStackType::Psram` | `Strata::Placement::RequireExternal` |
| `config.stackType` | `config.memory.taskStack` |
| implicit payload PSRAM preference | `config.memory.allocation` |

`LinkStackType` is removed in v0.2.0; no compatibility alias is retained.
