# Link API

Include Link with:

```cpp
#include <Link.h>
```

The default type is:

```cpp
template<size_t CallbackStorageSize = 64>
class LinkClient;

using Link = LinkClient<64>;
```

Use a larger callback store when a capturing callback does not fit:

```cpp
LinkClient<96> link;
```

## Core types

`LinkConfig` controls Strata memory policy, worker tasks, in-flight request capacity, connection reuse, timeouts, redirects, and payload limits.

The v0.2.0 memory configuration is:

```cpp
LinkConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

`memory.allocation` applies to Link-owned movable storage. `memory.taskStack` applies to Link worker stacks. `LinkStackType` was removed in v0.2.0.

`LinkResult` is returned by setup and submission calls. A false result means the request was not accepted or the lifecycle operation failed.

`LinkResponse` represents a buffered HTTP response. Transport success is separate from HTTP status:

```cpp
if (!response) {
    // DNS, TLS, timeout, allocation, cancellation, or transport failure.
}

if (!response.isHttpOk()) {
    // Server responded, but not with 2xx.
}
```

`LinkRequestT<CallbackStorageSize>` is the public request builder type. `LinkRequest` aliases the default 64-byte callback storage version.

## Request methods

```cpp
LinkResult init(const LinkConfig &config = LinkConfig());
LinkResult deinit();
bool isInitialized() const;
LinkState state() const;
LinkDiagnostics diagnostics() const;
LinkResult fetch(const LinkRequest &request);
```

Convenience methods create a `LinkRequestT` internally and forward to `fetch()`:

```cpp
client.get(url, callback);
client.get(url, headers, callback);
client.post(url, body, callback);
client.post(url, headers, body, callback);
client.getJson(url, callback);
client.getJson(url, headers, callback);
client.postJson(url, json, callback);
client.postJson(url, headers, json, callback);
client.getStream(url, onStart, onChunk, onEnd);
```

An explicit `LinkRequestT::timeoutMs` value of zero selects `LinkConfig::defaultTimeoutMs`. Every effective timeout must fit the ESP-IDF signed `int` timeout range. An oversized request-specific value returns `LinkErrorCode::InvalidTimeout` before queue publication.

## Headers and bodies

`LinkHeaders` supports `add`, `set`, `has`, `get`, `size`, `clear`, and `copyFrom`. Header lookup is case-insensitive.

Public headers default to `Strata::Placement::PreferExternal`. `configurePlacement()` can select another placement before the header object allocates storage. Internal request/response headers are configured from the active client's `memory.allocation` policy.

`LinkBodyView` supports:

```cpp
LinkBodyView::none();
LinkBodyView::text("hello");
LinkBodyView::json(jsonDocument);
LinkBodyView::bytes(data, size);
```

Body views do not allocate and do not own their source data. Link validates and copies a body into Strata-backed queue storage before `fetch()`, `post()`, or `postJson()` returns. Queued requests own copied URL, header, serialized body, and callback data.

## Response ownership

Allocation-backed public response storage is move-only by default. `LinkHeaders`, `LinkBody`, `LinkOwnedBuffer`, and `LinkResponse` do not provide implicit copy construction or assignment.

Application-owned response objects can transfer existing ownership without allocation:

```cpp
LinkResponse stored = std::move(ownedResponse);
```

Callbacks receive a `const LinkResponse &`, so application code normally extracts or explicitly duplicates required data during the callback. Use the result-returning duplication API when a full response copy is needed:

```cpp
LinkResponse stored;
LinkResult result = stored.copyFrom(response);
if (!result) {
    // The destination remains unchanged.
}
```

`LinkJsonResponse::json` is Link-owned and uses Strata's ArduinoJson v7 allocator with `LinkConfig::memory.allocation`. Its lifetime remains callback-scoped unless the application copies the needed values.

A caller-provided request `JsonDocument` is not reallocated by Link; Link only reads and serializes it while the submission call is active.

## Diagnostics

`diagnostics()` returns a mutex-protected snapshot of request, HTTP-client, eviction, transport, and Strata placement information.

The memory-specific fields are:

```cpp
Strata::Placement allocationPlacement;
Strata::Placement workerStackPlacement;
Strata::Region requestSlotRegion;
Strata::Region workerStorageRegion;
Strata::Region dispatchQueueStorageRegion;
size_t workerStacksInternal;
size_t workerStacksExternal;
size_t workerStacksUnknown;
```

The requested placements and observed regions are intentionally separate. `PreferExternal` may fall back to internal memory, and worker stack regions are counted independently.

After successful shutdown, useful runtime invariants include:

```cpp
LinkDiagnostics d = client.diagnostics();
assert(d.requestsSubmitted == d.requestsCompleted);
assert(d.activeHttpClients == 0);
assert(d.httpClientCreates == d.httpClientCleanups);
```

Diagnostics are retained after successful `deinit()` so shutdown/leak invariants can be inspected. A later successful `init()` resets them for the next runtime generation.

## Worker ownership

Link v0.2.0 creates worker tasks with `Strata::FreeRTOS::Task` and dispatches request-slot indices through `Strata::FreeRTOS::Queue`.

On shutdown a worker does not delete itself. It cleans up persistent HTTP state, marks its task ready for external deletion, and suspends. `deinit()` then resets the Strata task owner from the caller context, allowing Strata to delete the FreeRTOS task and release its static stack and task control block safely.

## Redirects

When `followRedirects` is enabled, automatic redirects are limited to GET requests with absolute `http://` or `https://` `Location` headers. Buffered and streaming requests both enforce `maxRedirects` and `maxUrlSize`.

Only same-origin redirects are followed by default. Set `allowCrossOriginRedirects` to opt into origin changes. Link strips all caller-supplied request headers on an allowed origin change and does not restore them later in the redirect chain. Set `allowHttpsToHttpRedirects` as well to explicitly permit a TLS downgrade.

Intermediate bodies for responses that Link will redirect are discarded in both buffered and streaming modes. Final response size limits remain enforced normally.
