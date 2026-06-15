# Callbacks

Link stores callbacks in fixed inline storage. The default public type is `Link`, which is `LinkClient<64>`.

If a callback is too large, submission returns `LinkErrorCode::CallbackTooLarge` and the request is not queued.

## Supported Callback Forms

Function pointer:

```cpp
void onResponse(const LinkResponse &response) {
}

link.get(url, onResponse);
```

Capturing lambda:

```cpp
link.get(url, [this](const LinkResponse &response) {
    handleResponse(response);
});
```

Bindable class method:

```cpp
link.get(
    url,
    Link::ResponseCallback::bind(this, &ApiClient::onResponse)
);
```

Private methods can be bound when binding happens from inside the class.

## Execution Context

Callbacks run inside the Link worker task that handled the request. If `maxConcurrentRequests > 1`, multiple callbacks may run at the same time.

Do not hold long blocking work in Link callbacks. Forward large processing to another task when needed.
