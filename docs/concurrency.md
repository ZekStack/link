# Concurrency

Link uses a thread-safe request queue and a fixed worker pool.

`LinkConfig::maxConcurrentRequests` controls how many worker tasks are created during `init()`. Each worker processes one active request at a time.

Requests are started in queue order, but may complete out of order when `maxConcurrentRequests > 1`.

## Lifecycle

Link tracks:

```cpp
Uninitialized -> Starting -> Running -> Stopping -> Uninitialized
```

`fetch()` accepts requests only while Link is `Running`.

During `deinit()`:

* new requests return `LinkErrorCode::Stopping`
* pending queued requests complete with `Cancelled`
* active requests observe stopping between blocking HTTP operations where possible
* request and callback storage is released only after workers exit

User callbacks are never invoked while Link internal mutexes are held.
