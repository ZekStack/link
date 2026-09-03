# Concurrency

Link uses a thread-safe request-slot pool, a `Strata::FreeRTOS::Queue` dispatch channel, a fixed worker pool, and separate synchronization for lifecycle transitions and runtime state.

`LinkConfig::maxConcurrentRequests` controls how many Strata-owned worker tasks are created during `init()`. Each worker processes one active request at a time.

`LinkConfig::queueSize` is the maximum number of accepted in-flight requests, including queued and active requests. It must be greater than or equal to `maxConcurrentRequests`.

Requests are dispatched in queue order, but may complete out of order when `maxConcurrentRequests > 1`.

## Submission

`fetch()` and the convenience request methods are safe to call from multiple normal FreeRTOS tasks while Link is running.

Submission preparation and publication are serialized under Link's runtime mutex. The critical section validates the active generation, copies bounded request ownership, reserves a request slot, publishes a `WorkerSignal` into the Strata queue, and records the submission.

Publication is atomic from Link's perspective:

1. verify that Link is `Running`;
2. copy URL, headers, body, timeout, and callbacks into Link-owned Strata-backed storage;
3. reserve a free request slot;
4. move the request into that slot;
5. send the slot index through the Strata dispatch queue;
6. count the request as submitted.

If queue publication fails, Link resets the slot and reports an error. A successful submission therefore always has a corresponding dispatch message.

The dispatch queue capacity is `queueSize + maxConcurrentRequests`. The extra entries are reserved logically for one shutdown message per worker, so shutdown cannot be prevented by a full request queue.

Submission does not hold the lifecycle mutex. A callback that attempts another request during shutdown can acquire the runtime mutex, observe `Stopping`, and return immediately rather than blocking the worker that `deinit()` is waiting for.

## Lifecycle

Link tracks:

```cpp
Uninitialized -> Starting -> Running -> Stopping -> Uninitialized
```

Complete `init()` and `deinit()` transitions are serialized by a dedicated recursive lifecycle mutex. Link's recursive mutexes are backed by `Strata::FreeRTOS::RecursiveMutex` on ESP32.

`fetch()` accepts requests only while Link is `Running`.

During `deinit()`:

- new submissions return `LinkErrorCode::Stopping` once shutdown begins;
- one stop signal per configured worker is appended to the dispatch queue;
- accepted request messages already in the queue remain ahead of those stop messages;
- workers cancel accepted work after observing `Stopping` and still deliver exactly one terminal callback;
- each worker cleans up persistent HTTP state;
- each worker marks itself ready for external task deletion and suspends;
- the caller executing `deinit()` resets each `Strata::FreeRTOS::Task` owner;
- request/worker/queue storage is released only after all worker tasks have been externally deleted.

This external-deletion handoff is required because the Strata task owner owns static stack and task-control-block storage. A worker cannot safely free that storage from inside itself.

If public `deinit()` times out, Link remains in `Stopping` and keeps worker-owned storage, the Strata dispatch queue, configuration, and JSON allocator alive. Calling `deinit()` again continues waiting and cleanup safely.

A failed partial `init()` transitions to stopping, wakes any workers already created, reaps their Strata task owners, releases partial storage, restores `Uninitialized`, and leaves the instance reusable.

The destructor uses blocking shutdown and does not return until workers have exited. This assumes active ESP-IDF HTTP operations eventually return through their configured nonzero request timeout.

Do not call `deinit()` or destroy Link from a Link callback. The callback executes on a worker that shutdown must wait for, so either operation would deadlock on itself.

User callbacks are never invoked while Link's runtime mutex is held. Long callback work should still be forwarded to an application task.
