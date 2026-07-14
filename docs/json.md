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

`maxSerializedJsonSize` limits the serialized input bytes passed to ArduinoJson. It does not cap the heap used by the parsed `JsonDocument`; parsed nodes and copied strings add structure-dependent overhead. Peak response memory includes both the buffered input and the parsed document.

`LinkJsonResponse::json` is valid only during the callback unless copied by the user.
