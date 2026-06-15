# JSON

Link uses ArduinoJson v7.

`getJson()` sets `Accept: application/json` unless the request already has an `Accept` header.

`postJson()` serializes the request body, sets `Accept: application/json`, and sets `Content-Type: application/json` unless the request already has a content type.

JSON request serialization is bounded by:

* `maxJsonDocumentSize`
* `maxRequestBodySize`

JSON response parsing is bounded by:

* `maxResponseBodySize`
* `maxJsonDocumentSize`

If the HTTP body exceeds the response body limit, Link returns `ResponseTooLarge`. If the buffered body fits but cannot be parsed as JSON or exceeds the JSON document limit, Link returns `JsonParseFailed`.

`LinkJsonResponse::json` is valid only during the callback unless copied by the user.
