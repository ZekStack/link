# Link API

Include Link with:

```cpp
#include <Link.h>
```

The default type is:

```cpp
template<size_t CallbackStorageSize = 64>
class LinkClient;

using Link = LinkClient<64>;
```

Use a larger callback store when a capturing callback does not fit:

```cpp
LinkClient<96> link;
```

## Core Types

`LinkConfig` controls worker tasks, queue size, timeouts, redirects, and memory limits.

`LinkResult` is returned by setup and submission calls. A false result means the request was not accepted or the lifecycle operation failed.

`LinkResponse` represents a buffered HTTP response. Transport success is separate from HTTP status:

```cpp
if (!response) {
    // DNS, TLS, timeout, allocation, cancellation, or transport failure.
}

if (!response.isHttpOk()) {
    // Server responded, but not with 2xx.
}
```

`LinkRequestT<CallbackStorageSize>` is the public request builder type. `LinkRequest` aliases the default 64-byte callback storage version.

## Request Methods

```cpp
LinkResult init(const LinkConfig &config = LinkConfig());
LinkResult deinit();
bool isInitialized() const;
LinkState state() const;
LinkResult fetch(const LinkRequest &request);
```

Convenience methods create a `LinkRequestT` internally and forward to `fetch()`:

```cpp
link.get(url, callback);
link.get(url, headers, callback);
link.post(url, body, callback);
link.post(url, headers, body, callback);
link.getJson(url, callback);
link.getJson(url, headers, callback);
link.postJson(url, json, callback);
link.postJson(url, headers, json, callback);
link.getStream(url, onStart, onChunk, onEnd);
```

## Headers And Bodies

`LinkHeaders` supports `add`, `set`, `has`, `get`, `size`, and `clear`. Header lookup is case-insensitive.

`LinkBody` supports:

```cpp
LinkBody::none();
LinkBody::text("hello");
LinkBody::json(jsonDocument);
LinkBody::bytes(data, size);
```

Queued requests own copied URL, header, body, and callback data.
