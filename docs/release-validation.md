# Release validation

A successful GitHub Actions run verifies host logic, source constraints, metadata, formatting, and compilation across the supported ESP32 targets. It does not replace physical-device qualification of FreeRTOS scheduling, network behavior, TLS reuse, or heap integrity.

The following validation is required before publishing `v0.1.0`.

## Automated release gates

The tagged commit must pass:

- release metadata validation;
- clang-format validation;
- embedded source audit;
- general host logic tests;
- persistent-client host logic tests;
- all examples under PIOArduino on ESP32, ESP32-S3, ESP32-C3, and ESP32-P4;
- all examples under Arduino CLI on the same target families;
- compilation of the concurrent lifecycle stress sketch;
- one build lane against ArduinoJson `7.0.0` and one against the current ArduinoJson v7 release.

The release workflow must depend on every listed job on the exact tagged commit.

## Concurrent lifecycle stress

Flash and run `tests/esp32/shutdown-stress` on a physical ESP32 target.

The test runs concurrent producer tasks while repeatedly alternating `deinit()` and `init()` in both `PerRequest` and `PersistentPerWorker` modes. A passing run must report:

- no crash, watchdog reset, invalid semaphore access, or heap corruption;
- no unexpected submission errors;
- exactly one terminal callback per accepted request;
- no callback after successful `deinit()`;
- `requestsSubmitted == requestsCompleted` after every stopped generation;
- `activeHttpClients == 0` after every stopped generation;
- `httpClientCreates == httpClientCleanups` after every stopped generation;
- successful `heap_caps_check_integrity_all(true)` after every generation.

Record the final free heap, minimum free heap, and largest free block.

## Persistent HTTPS soak

Run a same-origin HTTPS workload in both connection modes for an extended period. The server or test harness should exercise:

- normal keep-alive reuse;
- server-initiated connection close;
- connection timeout;
- incomplete or malformed response;
- same-origin redirect;
- allowed and rejected cross-origin redirect;
- HTTPS-to-HTTP downgrade rejection;
- changing custom headers between requests;
- alternating GET and body-bearing methods;
- idle and request-count eviction.

Record at regular intervals:

- accepted, completed, successful, and failed request counts;
- `httpClientCreates`, `httpClientReuses`, and `httpClientCleanups`;
- transport connect and disconnect events;
- origin, idle, request-limit, and poisoned evictions;
- free heap, minimum free heap, and largest free block;
- Link worker stack high-water marks.

Required invariants after final shutdown:

```cpp
LinkDiagnostics d = client.diagnostics();
assert(d.requestsSubmitted == d.requestsCompleted);
assert(d.activeHttpClients == 0);
assert(d.httpClientCreates == d.httpClientCleanups);
```

Link must not automatically replay a failed request. In particular, a body-bearing request must never be duplicated after a stale persistent connection fails.

## Compatibility smoke tests

Run at least one real HTTP and one verified HTTPS request on every hardware family intended for the initial release. Confirm:

- certificate-bundle HTTPS succeeds on the actual firmware configuration;
- buffered text and JSON responses work;
- streaming start, chunk, cancellation, and end callbacks work;
- cross-origin authorization headers are stripped;
- final response limits are enforced;
- large intermediate redirect bodies do not prevent an otherwise valid redirect.

## Release evidence

Attach the following to the release pull request or a linked issue:

- GitHub Actions URL for the final commit;
- device and core versions used;
- lifecycle stress serial output;
- persistent soak duration and request count;
- heap and stack measurements;
- any known limitations or deviations.

Do not tag `v0.1.0` until the automated gates and physical qualification are both complete.
