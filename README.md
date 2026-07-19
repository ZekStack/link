# Link

Link is an async HTTP client library for ESP32 with fetch-style requests and bounded memory.

Link helps you communicate with APIs and backend services in Arduino ESP32 projects. It is designed for production firmware that needs thread-safe request submission, predictable request/response limits, and clear result-based errors.

[![CI](https://github.com/ZekStack/link/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/link/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/link?sort=semver)](https://github.com/ZekStack/link/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Link?

* **Fetch-style requests** - submit `get`, `post`, `getJson`, `postJson`, or `getStream` work from normal FreeRTOS tasks.
* **Concurrent workers** - run more than one HTTP request at the same time with a bounded worker pool.
* **ESP32-friendly memory** - accepted request bodies, response bodies, URLs, headers, serialized JSON, callbacks, and stream buffers have explicit limits.
* **Clear API** - operations return `LinkResult`; HTTP status codes stay separate from transport failures.
* **Production-minded** - no exceptions, serialized lifecycle transitions, atomic queue publication, bindable callbacks, and PSRAM-preferred payload buffers.

## Install

### PlatformIO

Link is built for Arduino ESP32 and depends on ArduinoJson v7.

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  https://github.com/ZekStack/link.git
  bblanchon/ArduinoJson@>=7.0.0

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

### Arduino IDE

Link is not published to Arduino Library Manager yet.

Install it by downloading the repository ZIP or cloning it into your Arduino libraries folder.

```txt
Arduino/libraries/Link
```

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

## Important notes

> [!IMPORTANT]
> Link callbacks run inside Link worker tasks. If `maxConcurrentRequests > 1`, multiple callbacks may run at the same time.

* Protect shared application state touched from callbacks.
* Requests are started in queue order, but may complete out of order when more than one worker is enabled.
* Submission preparation, queue publication, and worker signaling form one runtime critical section. A successful submission always owns a slot and has a corresponding worker permit.
* User callbacks are not called while Link's runtime mutex is held. A callback may submit another request; if shutdown has started, the submission returns `Stopping`.
* Every accepted request receives exactly one terminal callback before a successful `deinit()` returns.
* `LinkJsonResponse::json` is valid only during the callback unless the user copies the needed data.
* Allocation-backed response storage is move-only. Use explicit result-returning `copyFrom()` operations when duplication is required.
* HTTPS uses the ESP-IDF certificate bundle when available. If the project/core does not provide usable certificate bundle support, verified HTTPS fails with `TlsFailed`.
* `deinit()` lets worker tasks cancel queued requests and waits for active workers to exit. If the public wait times out, Link stays in `Stopping` and keeps worker-owned storage alive so a later `deinit()` can finish cleanup.
* The destructor performs blocking shutdown. It assumes active HTTP operations eventually return through their configured nonzero request timeout.
* Do not call `deinit()` or destroy a `Link` instance from one of its callbacks; shutdown waits for that callback's worker task to exit.
* Redirect following is limited to GET requests with absolute `http://` or `https://` `Location` headers. Same-origin redirects are allowed by default; cross-origin and HTTPS-to-HTTP redirects require explicit opt-in. Caller-supplied headers are stripped after an origin change. Intermediate redirect bodies are discarded.

## Examples

The repository includes topic-focused Arduino sketches in the `examples/` folder.

| Example | Description |
| --- | --- |
| `basic-get` | Initialize Link and run one buffered GET request. |
| `post-json` | Send a JSON request body and parse a JSON response. |
| `custom-headers` | Add custom request headers and inspect response headers. |
| `stream-download` | Download a large response in bounded chunks. |
| `class-callback` | Bind a private class method as a response callback. |

Start with:

```txt
examples/basic-get
```

## Documentation

Detailed documentation is available in the `docs/` folder.

| Document | Description |
| --- | --- |
| [`docs/api.md`](docs/api.md) | Public classes, result types, ownership, and callback aliases. |
| [`docs/callbacks.md`](docs/callbacks.md) | Callback storage, binding, and execution context. |
| [`docs/concurrency.md`](docs/concurrency.md) | Queue publication, worker pool, lifecycle, and completion guarantees. |
| [`docs/errors.md`](docs/errors.md) | Error codes and HTTP status behavior. |
| [`docs/json.md`](docs/json.md) | ArduinoJson helpers and JSON lifetime rules. |
| [`docs/streaming.md`](docs/streaming.md) | Streaming downloads and cancellation. |
| [`docs/memory.md`](docs/memory.md) | Bounded memory, ESP-IDF ranges, and explicit copy behavior. |
| [`docs/persistent-http.md`](docs/persistent-http.md) | Optional per-worker persistent HTTP clients. |
| [`docs/release-validation.md`](docs/release-validation.md) | Automated gates and physical v0.1.0 qualification. |

## API overview

```cpp
Link client;

LinkResult init(const LinkConfig &config);
LinkResult deinit();
LinkResult fetch(const LinkRequest &request);
LinkDiagnostics diagnostics() const;

client.get(url, callback);
client.post(url, body, callback);
client.getJson(url, callback);
client.postJson(url, json, callback);
client.getStream(url, onStart, onChunk, onEnd);
```

For the full API, see [`docs/api.md`](docs/api.md).

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Platform | `espressif32` |
| Language | C++20 |
| Networking | ESP-IDF `esp_http_client` |
| HTTPS | ESP-IDF certificate bundle when available |
| PSRAM | Payload buffers prefer PSRAM; worker stacks can optionally use PSRAM |
| Dependencies | `bblanchon/ArduinoJson >= 7.0.0` |
| Exceptions | Not used |
| Status | `0.1.0` |

## Configuration

```cpp
LinkConfig config;
config.queueSize = 10;
config.maxConcurrentRequests = 3;
config.defaultTimeoutMs = 15000;
config.connectionMode = LinkConnectionMode::PerRequest;
config.persistentIdleTimeoutMs = 5U * 60U * 1000U;
config.persistentMaxRequestsPerHandle = 0;
config.maxUrlSize = 512;
config.maxRequestBodySize = 8192;
config.maxResponseBodySize = 8192;
config.maxSerializedJsonSize = 8192;
config.maxTotalHeaderSize = 4096;
config.streamChunkSize = 1024;
config.allowCrossOriginRedirects = false;
config.allowHttpsToHttpRedirects = false;
```

`queueSize` is the maximum number of accepted in-flight requests, including queued and actively running requests. It must be at least `maxConcurrentRequests`.

Request body factories return non-owning `LinkBodyView` values. Link validates a view against the active configuration and copies it into owned queue storage before submission returns. The source text, bytes, or `JsonDocument` therefore only needs to remain valid until `fetch()`, `post()`, or `postJson()` returns.

Timeouts, request body limits, and stream buffer sizes are validated before being narrowed to signed ESP-IDF parameters. An oversized explicit request timeout returns `InvalidTimeout` before queue publication.

`maxSerializedJsonSize` limits serialized JSON request and response bytes. ArduinoJson's parsed document uses additional heap memory based on the JSON structure.

For all options, see [`docs/memory.md`](docs/memory.md).

## Error handling

```cpp
LinkResult result = client.get(url, callback);

if (!result) {
    Serial.println(result.message);
}
```

HTTP status codes are not Link transport failures. A valid server response with `404` still produces a successful `LinkResponse` with `response.httpStatus == 404`.

## License

MIT - see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack.

ZekStack libraries are designed to provide small, reusable building blocks for ESP32 applications.
