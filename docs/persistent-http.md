# Persistent HTTP clients

Link normally creates and cleans up one ESP-IDF HTTP client for every request. This remains the default and preserves existing behavior.

Applications that repeatedly call the same API can opt into one persistent HTTP client per Link worker:

```cpp
LinkConfig config;
config.maxConcurrentRequests = 2;
config.connectionMode = LinkConnectionMode::PersistentPerWorker;
config.persistentIdleTimeoutMs = 5U * 60U * 1000U;
config.persistentMaxRequestsPerHandle = 0;

Link client;
LinkResult result = client.init(config);
```

`persistentIdleTimeoutMs == 0` disables idle eviction. `persistentMaxRequestsPerHandle == 0` disables request-count eviction.

## Ownership and concurrency

A persistent handle belongs to exactly one worker. A worker processes one request at a time, so the handle is never used concurrently. The maximum number of retained HTTP clients is therefore bounded by `maxConcurrentRequests`.

Link does not use a global lease pool and does not move handles between workers.

## Same-origin reuse

A worker reuses its handle only when the next URL has the same:

- scheme
- case-insensitive host
- effective port

Explicit default ports are normalized, so `https://example.com` and `https://EXAMPLE.com:443` are the same origin.

A cross-origin redirect or later request replaces the worker's retained handle. Existing redirect policy still controls whether the origin change is allowed, and caller-supplied headers are stripped after an allowed cross-origin redirect.

## Request isolation

Before and after every transfer, Link resets request-specific state on the ESP-IDF handle. Caller-supplied headers and POST data are removed before the queued request storage is released.

If setup, transfer, callback processing, response buffering, or state scrubbing fails, the retained handle is marked unusable and cleaned up. The failed operation is returned to the caller normally.

Link never automatically replays a request. This prevents duplicate POST, PUT, PATCH, or DELETE operations.

## Lifetime

Persistent handles are cleaned up when:

- their origin changes
- their idle timeout expires
- their configured request limit is reached
- a request poisons the handle
- the owning worker exits during Link shutdown

Idle expiration is lazy: it is checked when the worker receives its next request. No maintenance task or timer is created.

## Diagnostics

`LinkClient::diagnostics()` returns a snapshot containing request, handle, eviction, and transport-event counters.

Useful invariants are:

```cpp
LinkDiagnostics diagnostics = client.diagnostics();

// While running:
assert(diagnostics.activeHttpClients <= config.maxConcurrentRequests);

// After successful deinit():
assert(diagnostics.activeHttpClients == 0);
assert(diagnostics.httpClientCreates == diagnostics.httpClientCleanups);
```

`httpClientReuses` counts reuse of an ESP-IDF client handle. Transport connection and disconnection events are tracked separately because a retained handle may reconnect its underlying socket.
