# Link

Link is an async HTTP client library for ESP32 with fetch-style requests and bounded memory.

Link helps you communicate with APIs and backend services in Arduino ESP32 projects. It is designed for production firmware that needs thread-safe request submission, predictable request/response limits, and clear result-based errors.

[![CI](https://github.com/ZekStack/link/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/link/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/link?sort=semver)](https://github.com/ZekStack/link/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Link?

* **Fetch-style requests** - submit `get`, `post`, `getJson`, `postJson`, or `getStream` work from normal FreeRTOS tasks.
* **Concurrent workers** - run more than one HTTP request at the same time with a bounded worker pool.
* **ESP32-friendly memory** - request bodies, response bodies, URLs, headers, JSON, callbacks, and stream buffers have explicit limits.
* **Clear API** - operations return `LinkResult`; HTTP status codes stay separate from transport failures.
* **Production-minded** - no exceptions, FreeRTOS mutex protection, bindable callbacks, and PSRAM-preferred allocation.

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

Link link;

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

    LinkResult initResult = link.init(config);
    if (!initResult) {
        Serial.println(initResult.message);
        return;
    }

    link.get("https://example.com", onResponse);
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
* User callbacks are not called while Link internal mutexes are held.
* `LinkJsonResponse::json` is valid only during the callback unless the user copies the needed data.
* HTTPS uses the ESP-IDF certificate bundle when available. If the project/core does not provide usable certificate bundle support, verified HTTPS fails with `TlsFailed`.
* `deinit()` cancels queued requests and waits for active worker requests to exit. If a request is blocked inside the HTTP client, timeout settings define the maximum wait.

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
| [`docs/api.md`](docs/api.md) | Public classes, result types, and callback aliases. |
| [`docs/callbacks.md`](docs/callbacks.md) | Callback storage, binding, and execution context. |
| [`docs/concurrency.md`](docs/concurrency.md) | Queue, worker pool, lifecycle, and completion ordering. |
| [`docs/errors.md`](docs/errors.md) | Error codes and HTTP status behavior. |
| [`docs/json.md`](docs/json.md) | ArduinoJson helpers and JSON lifetime rules. |
| [`docs/streaming.md`](docs/streaming.md) | Streaming downloads and cancellation. |
| [`docs/memory.md`](docs/memory.md) | Bounded memory configuration. |

## API overview

```cpp
Link link;

LinkResult init(const LinkConfig &config);
LinkResult deinit();
LinkResult fetch(const LinkRequest &request);

link.get(url, callback);
link.post(url, body, callback);
link.getJson(url, callback);
link.postJson(url, json, callback);
link.getStream(url, onStart, onChunk, onEnd);
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
| PSRAM | Preferred for internal allocations and optional worker stacks |
| Dependencies | `bblanchon/ArduinoJson >= 7.0.0` |
| Exceptions | Not used |
| Status | Early-stage `0.0.1` |

## Configuration

```cpp
LinkConfig config;
config.queueSize = 10;
config.maxConcurrentRequests = 3;
config.defaultTimeoutMs = 15000;
config.maxUrlSize = 512;
config.maxRequestBodySize = 8192;
config.maxResponseBodySize = 8192;
config.maxJsonDocumentSize = 8192;
config.maxTotalHeaderSize = 4096;
config.streamChunkSize = 1024;
```

For all options, see [`docs/memory.md`](docs/memory.md).

## Error handling

```cpp
LinkResult result = link.get(url, callback);

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
