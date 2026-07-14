# Callbacks

Link stores callbacks in fixed inline storage. The default public type is `Link`, which is `LinkClient<64>`.

If a callback is too large, submission returns `LinkErrorCode::CallbackTooLarge` and the request is not queued.

## Supported Callback Forms

Function pointer:

```cpp
void onResponse(const LinkResponse &response) {
}

client.get(url, onResponse);
```

Capturing lambda:

```cpp
client.get(url, [this](const LinkResponse &response) {
    handleResponse(response);
});
```

Bindable class method:

```cpp
client.get(
    url,
    Link::ResponseCallback::bind(this, &ApiClient::onResponse)
);
```

Private methods can be bound when binding happens from inside the class.

## Execution Context

Callbacks run inside the Link worker task that handled the request. If `maxConcurrentRequests > 1`, multiple callbacks may run at the same time.

Queued cancellation callbacks also run inside Link worker tasks during shutdown. Do not call `deinit()` or destroy the `Link` instance from a Link callback: both shutdown paths wait for workers, including the worker currently executing that callback.

Do not hold long blocking work in Link callbacks. Forward large processing to another task when needed.
