# Link

Link is an async HTTP client library for ESP32 with fetch-style requests and bounded memory.

Link helps Arduino ESP32 firmware communicate with APIs and backend services using thread-safe request submission, a bounded worker pool, explicit request/response limits, and result-based errors. Link owns HTTP/request lifecycle policy while [Strata](https://github.com/ZekStack/strata) owns Link's memory placement and low-level FreeRTOS storage.

[![CI](https://github.com/ZekStack/link/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/link/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/link?sort=semver)](https://github.com/ZekStack/link/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Link?

* **Fetch-style requests** - submit `get`, `post`, `getJson`, `postJson`, or `getStream` work from normal FreeRTOS tasks.
* **Concurrent workers** - run more than one HTTP request at a time with a bounded worker pool.
* **Consistent memory policy** - `Strata::MemoryPolicy` controls Link-owned allocations and worker task stacks.
* **Bounded payloads** - accepted URLs, bodies, headers, serialized JSON, callbacks, and streaming behavior have explicit limits.
* **Strata-owned FreeRTOS storage** - worker task stacks/TCBs, dispatch queue storage, and mutex control blocks use static Strata ownership.
* **Clear errors** - operations return `LinkResult`; HTTP status codes remain separate from transport failures.

## Dependency

Link `v0.2.0` requires Strata `v0.1.2` and ArduinoJson v7.

### PlatformIO

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  https://github.com/ZekStack/link.git#v0.2.0
  bblanchon/ArduinoJson@>=7.0.0

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

Link's `library.json` pins Strata `v0.1.2`, so PlatformIO resolves it as a transitive dependency.

### Arduino IDE

Link and Strata are not published to Arduino Library Manager yet. Install both repositories and ArduinoJson v7:

```text
Arduino/libraries/Strata
Arduino/libraries/Link
```

Use Strata `v0.1.2` or a compatible later release.

## Quick start

```cpp
#include <Arduino.h>
#include <Link.h>

Link client;

void onResponse(const LinkResponse &response) {
    if (!response) {
        Serial.println(response.error.message);
        return;
    }

    Serial.println(response.httpStatus);
    Serial.println(response.body.c_str());
}

void setup() {
    Serial.begin(115200);

    LinkConfig config;
    config.maxConcurrentRequests = 2;
    config.maxResponseBodySize = 8192;

    LinkResult initResult = client.init(config);
    if (!initResult) {
        Serial.println(initResult.message);
        return;
    }

    client.get("https://example.com", onResponse);
}

void loop() {
    delay(1000);
}
```

## Memory policy

Link uses the ZekStack-standard Strata configuration shape:

```cpp
LinkConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

`memory.allocation` controls movable Link-owned storage, including queued request slots, worker records, dispatch queue item storage, URLs, headers, request/response bodies, persistent origin data, redirect URLs, and Link-created parsed JSON response storage.

`memory.taskStack` controls worker task stack placement. Strata keeps task control blocks, queue control blocks, and recursive-mutex control blocks in internal memory.

The defaults preserve Link `v0.1.1` behavior:

```cpp
allocation = Strata::Placement::PreferExternal;
taskStack  = Strata::Placement::PreferExternal;
```

`PreferExternal` uses external memory when available and falls back to internal memory. `RequireExternal` fails with `AllocationFailed` rather than consuming internal memory.

Strict external-memory configuration is therefore explicit:

```cpp
config.memory.allocation = Strata::Placement::RequireExternal;
config.memory.taskStack = Strata::Placement::RequireExternal;
```

## v0.1.1 to v0.2.0 migration

`v0.2.0` intentionally removes the Link-specific stack enum instead of carrying compatibility aliases.

| Link v0.1.1 | Link v0.2.0 |
| --- | --- |
| `LinkStackType::Auto` | `Strata::Placement::PreferExternal` |
| `LinkStackType::Internal` | `Strata::Placement::Internal` |
| `LinkStackType::Psram` | `Strata::Placement::RequireExternal` |
| `config.stackType` | `config.memory.taskStack` |
| implicit PSRAM-preferred payload allocation | `config.memory.allocation` |

## Concurrency and shutdown

> [!IMPORTANT]
> Link callbacks run inside Link worker tasks. If `maxConcurrentRequests > 1`, multiple callbacks may run concurrently.

Link uses a Strata task-only dispatch queue. A successful submission owns a request slot and has a corresponding queue message. During shutdown, stop messages are appended after accepted request messages. Workers cancel/drain accepted requests, clean up persistent HTTP state, publish that they are ready for deletion, and suspend. `deinit()` then resets each `Strata::FreeRTOS::Task` from the caller task so Strata can safely release its static stack and task control block.

Other lifecycle rules remain unchanged:

* Protect shared application state touched from callbacks.
* Requests start in queue order but may complete out of order with multiple workers.
* User callbacks are never called while Link's runtime mutex is held.
* New submissions return `Stopping` after shutdown begins.
* Every accepted request receives exactly one terminal callback before successful `deinit()` returns.
* A timed-out `deinit()` leaves Link in `Stopping` with worker-owned storage intact so a later call can continue cleanup.
* Do not call `deinit()` or destroy a `Link` instance from one of its callbacks.

## Ownership boundaries

Link routes memory it owns through Strata. Two allocation domains remain intentionally outside this boundary:

* a caller-provided request `JsonDocument`, which Link only reads/serializes during submission;
* allocations internal to ESP-IDF's `esp_http_client` implementation.

`LinkJsonResponse::json`, by contrast, is created by Link and uses Strata's ArduinoJson allocator with `memory.allocation`.

Allocation-backed response storage is move-only. Use explicit result-returning `copyFrom()` methods when duplication is required.

## Diagnostics

`LinkDiagnostics` retains the existing request/HTTP counters and also reports requested Strata policy plus observed storage regions:

```cpp
LinkDiagnostics d = client.diagnostics();
Serial.println(Strata::toString(d.allocationPlacement));
Serial.println(Strata::toString(d.workerStackPlacement));
Serial.println(Strata::toString(d.requestSlotRegion));
Serial.println(Strata::toString(d.dispatchQueueStorageRegion));
Serial.printf(
    "worker stacks: internal=%u external=%u unknown=%u\n",
    static_cast<unsigned>(d.workerStacksInternal),
    static_cast<unsigned>(d.workerStacksExternal),
    static_cast<unsigned>(d.workerStacksUnknown));
```

## Important notes

* HTTPS uses the ESP-IDF certificate bundle when available. If the project/core does not provide usable certificate bundle support, verified HTTPS fails with `TlsFailed`.
* Redirect following is limited to GET requests with absolute `http://` or `https://` `Location` headers. Same-origin redirects are allowed by default; cross-origin and HTTPS-to-HTTP redirects require explicit opt-in.
* Caller-supplied headers are stripped after an origin change. Intermediate redirect bodies are discarded.
* Request body views are copied into owned storage before submission returns, so the source only needs to remain valid for the submission call.
* `LinkJsonResponse::json` and streaming chunk data are callback-scoped unless copied by the application.

## Documentation

| Document | Description |
| --- | --- |
| [`docs/api.md`](docs/api.md) | Public classes, configuration, result types, diagnostics, and ownership. |
| [`docs/callbacks.md`](docs/callbacks.md) | Callback storage, binding, and execution context. |
| [`docs/concurrency.md`](docs/concurrency.md) | Dispatch queue, worker pool, lifecycle, and completion guarantees. |
| [`docs/errors.md`](docs/errors.md) | Error codes and HTTP status behavior. |
| [`docs/json.md`](docs/json.md) | ArduinoJson helpers, Strata allocation, and JSON lifetime rules. |
| [`docs/streaming.md`](docs/streaming.md) | Streaming downloads and cancellation. |
| [`docs/memory.md`](docs/memory.md) | Strata policy, bounded memory, diagnostics, and explicit copy behavior. |
| [`docs/persistent-http.md`](docs/persistent-http.md) | Optional per-worker persistent HTTP clients. |
| [`docs/release-validation.md`](docs/release-validation.md) | Automated gates and physical v0.2.0 qualification. |

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Platform | `espressif32` |
| Language | C++20 |
| Networking | ESP-IDF `esp_http_client` |
| HTTPS | ESP-IDF certificate bundle when available |
| Memory policy | Strata `v0.1.2` |
| JSON | ArduinoJson `>= 7.0.0` |
| Exceptions | Not used by Link |
| Status | `0.2.0` |

## License

MIT - see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack.
