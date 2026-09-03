# JSON

Link uses ArduinoJson v7.

`getJson()` sets `Accept: application/json` unless the request already has an `Accept` header.

`postJson()` serializes the request body, sets `Accept: application/json`, and sets `Content-Type: application/json` unless the request already has a content type.

JSON request serialization is bounded by:

* `maxSerializedJsonSize`
* `maxRequestBodySize`

JSON response parsing is bounded by:

* `maxResponseBodySize`
* `maxSerializedJsonSize`

If the HTTP body exceeds the response body limit, Link returns `ResponseTooLarge`. If the buffered body fits but cannot be parsed as JSON or exceeds the serialized JSON limit, Link returns `JsonParseFailed`.

## Allocation policy

A caller-provided request `JsonDocument` remains caller-owned. Link reads and serializes that document during submission but does not replace its allocator.

A parsed response document is different: `LinkJsonResponse::json` is created by Link, so v0.2.0 constructs it with `Strata::ArduinoJson::Allocator` using `LinkConfig::memory.allocation`.

This means parsed ArduinoJson nodes and copied strings follow the same Strata placement policy as Link-owned response buffers and headers.

`maxSerializedJsonSize` is still a serialized byte limit rather than a fixed cap on parsed-document memory. Peak JSON response memory includes the buffered serialized body plus structure-dependent ArduinoJson allocations; both Link-owned domains now follow Strata placement.

`LinkJsonResponse::json` is valid only during the callback unless copied by the application.
