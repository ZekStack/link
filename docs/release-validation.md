# Release validation

A successful GitHub Actions run verifies host logic, source ownership constraints, metadata, formatting, and compilation across the supported ESP32 targets. It does not replace physical-device qualification of FreeRTOS scheduling, Strata placement, network behavior, TLS reuse, or heap integrity.

The following validation is required before publishing `v0.2.0`.

## Automated release gates

The tagged commit must pass:

- release metadata validation for `0.2.0`;
- clang-format validation;
- embedded source audit preventing direct Link-owned allocation and dynamic FreeRTOS ownership paths;
- general host logic tests linked against Strata `v0.1.2` generic backend;
- persistent-client host logic tests linked against Strata `v0.1.2`;
- all examples under PIOArduino on ESP32, ESP32-S3, ESP32-C3, and ESP32-P4 with Strata `v0.1.2`;
- all examples under Arduino CLI on the same target families with Strata installed in the sketchbook;
- compilation of the concurrent lifecycle stress sketch;
- ArduinoJson `7.0.0` coverage in PIOArduino and current ArduinoJson v7 coverage in Arduino CLI.

The source audit must reject direct heap-capability allocation, raw owned allocation, dynamic task/queue/semaphore creation, and ESP-IDF allocation-internal headers from production Link sources.

## Strata policy qualification

On hardware with PSRAM, qualify at least these configurations:

```cpp
// Default v0.2.0 policy
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;

// Internal-only control case
config.memory.allocation = Strata::Placement::Internal;
config.memory.taskStack = Strata::Placement::Internal;

// Strict external-memory case
config.memory.allocation = Strata::Placement::RequireExternal;
config.memory.taskStack = Strata::Placement::RequireExternal;
```

For each successful configuration, inspect `LinkDiagnostics` and verify that requested placements are reported separately from observed storage regions. Under `RequireExternal`, an allocation or task-stack failure must surface as `AllocationFailed`; Link must not silently fall back to internal memory.

Also run a strict external-memory configuration on a target without external RAM and confirm initialization fails predictably rather than consuming internal memory.

## Concurrent lifecycle stress

Flash and run `tests/esp32/shutdown-stress` on a physical ESP32 target.

The test runs concurrent producer tasks while repeatedly alternating `deinit()` and `init()` in both `PerRequest` and `PersistentPerWorker` modes. A passing run must report:

- no crash, watchdog reset, invalid queue/mutex access, or heap corruption;
- no task returning into released Strata stack/control storage;
- no unexpected submission errors;
- exactly one terminal callback per accepted request;
- no callback after successful `deinit()`;
- `requestsSubmitted == requestsCompleted` after every stopped generation;
- `activeHttpClients == 0` after every stopped generation;
- `httpClientCreates == httpClientCleanups` after every stopped generation;
- successful `heap_caps_check_integrity_all(true)` after every generation.

Run the stress test under both `Internal` and `PreferExternal` task-stack placement. On a PSRAM target, also exercise `RequireExternal`.

## Worker deletion handoff

The v0.2.0 task lifecycle must specifically be observed under repeated shutdown:

1. a worker receives its stop signal only after accepted request messages ahead of it;
2. the worker cleans persistent HTTP state;
3. the worker marks itself ready for deletion;
4. the worker suspends instead of self-deleting;
5. `deinit()` resets the `Strata::FreeRTOS::Task` from another task context;
6. worker-record and dispatch-queue storage is not released until every Strata task owner has been reset.

No worker stack or task control block may leak across repeated init/deinit cycles.

## Persistent HTTPS soak

Run a same-origin HTTPS workload in both connection modes for an extended period. Exercise:

- normal keep-alive reuse;
- server-initiated connection close;
- connection timeout;
- incomplete or malformed response;
- same-origin redirect;
- allowed and rejected cross-origin redirect;
- HTTPS-to-HTTP downgrade rejection;
- changing custom headers between requests;
- alternating GET and body-bearing methods;
- absent and changing `Content-Type` headers while request state is cleared;
- idle and request-count eviction;
- buffered JSON responses large enough to exercise the Strata ArduinoJson allocator.

Record at regular intervals:

- accepted, completed, successful, and failed request counts;
- `httpClientCreates`, `httpClientReuses`, and `httpClientCleanups`;
- transport connect and disconnect events;
- origin, idle, request-limit, and poisoned evictions;
- free internal/external heap, minimum free heap, and largest free block;
- requested Link placements and observed storage regions;
- worker task stack regions and high-water marks where available.

Required invariants after final shutdown:

```cpp
LinkDiagnostics d = client.diagnostics();
assert(d.requestsSubmitted == d.requestsCompleted);
assert(d.activeHttpClients == 0);
assert(d.httpClientCreates == d.httpClientCleanups);
```

Link must not automatically replay a failed request. In particular, a body-bearing request must never be duplicated after a stale persistent connection fails.

## Compatibility smoke tests

Run at least one real HTTP and one verified HTTPS request on every hardware family intended for the release. Confirm:

- certificate-bundle HTTPS succeeds on the actual firmware configuration;
- buffered text and JSON responses work;
- parsed `LinkJsonResponse::json` follows the configured Strata allocation policy;
- streaming start, chunk, cancellation, and end callbacks work;
- cross-origin authorization headers are stripped;
- final response limits are enforced;
- large intermediate redirect bodies do not prevent an otherwise valid redirect;
- fresh GET and JSON POST requests do not fail while clearing absent request state.

## Release evidence

Attach to the release pull request or a linked issue:

- GitHub Actions URL for the final commit;
- Strata version and ESP32 core versions used;
- device/PSRAM configuration;
- lifecycle stress serial output;
- persistent soak duration and request count;
- internal/external heap and task-stack measurements;
- diagnostics showing requested vs observed placement;
- any known limitations or deviations.

Do not tag `v0.2.0` until the automated gates and physical qualification are complete.
