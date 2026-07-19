# Concurrency

Link uses a thread-safe request queue, a fixed worker pool, and separate synchronization for lifecycle transitions and runtime state.

`LinkConfig::maxConcurrentRequests` controls how many worker tasks are created during `init()`. Each worker processes one active request at a time.

`LinkConfig::queueSize` is the maximum number of accepted in-flight requests, including both queued and actively running requests. It must be greater than or equal to `maxConcurrentRequests`.

Requests are started in queue order, but may complete out of order when `maxConcurrentRequests > 1`.

## Submission

`fetch()` and the convenience request methods are safe to call from multiple normal FreeRTOS tasks while Link is running.

Submission preparation and publication are serialized under Link's runtime mutex. The same critical section validates the active generation, copies bounded request ownership, reserves a slot, publishes the queue entry, and signals a worker. A concurrent lifecycle transition cannot free or replace runtime storage during that operation.

Queue publication is one atomic operation:

1. validate that Link is still `Running`;
2. copy the URL, headers, body, timeout, and callbacks into bounded owned storage;
3. reserve a free request slot;
4. move the owned request into that slot;
5. append its slot index to the queue;
6. signal one worker;
7. count the request as submitted.

If worker signaling fails, Link rolls the queue operation back and reports an error. A successful submission therefore always has a corresponding worker permit.

Submission does not hold the lifecycle mutex. This matters during shutdown: a callback that attempts another request can acquire the runtime mutex, observe `Stopping`, and return immediately instead of blocking the worker that `deinit()` is waiting for.

## Lifecycle

Link tracks:

```cpp
Uninitialized -> Starting -> Running -> Stopping -> Uninitialized
```

Complete `init()` and `deinit()` transitions are serialized by a dedicated lifecycle mutex. Runtime storage and synchronization primitives cannot be freed while submission holds the runtime mutex.

`fetch()` accepts requests only while Link is `Running`.

During `deinit()`:

- new submissions return `LinkErrorCode::Stopping` once shutdown begins;
- workers drain accepted queued requests and complete them with `Cancelled`;
- active requests observe stopping between blocking HTTP operations where possible;
- request and callback storage is released only after workers exit;
- every accepted request receives exactly one terminal callback before a successful `deinit()` returns.

If public `deinit()` times out, Link remains in `Stopping` and keeps worker-owned storage, the request semaphore, and configuration alive. Calling `deinit()` again continues waiting and cleanup safely.

A failed partial `init()` stops any workers already created, releases partial storage, restores `Uninitialized`, and leaves the instance reusable.

The destructor uses blocking shutdown and does not return until workers have exited. This assumes active HTTP operations eventually return through their configured nonzero request timeout.

Do not call `deinit()` or destroy Link from a Link callback. The callback executes on a worker that shutdown must wait for, so either operation would deadlock on itself.

User callbacks are never invoked while Link's runtime mutex is held. A callback may submit another request; if shutdown has started, that submission returns `Stopping`. Long callback work should still be forwarded to an application task.
