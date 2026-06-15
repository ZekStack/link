#pragma once

#include <cstddef>
#include <cstring>

class JsonDocument {
  public:
	void setRaw(const char *value) {
		_raw = value != nullptr ? value : "{}";
	}

	const char *raw() const {
		return _raw != nullptr ? _raw : "{}";
	}

  private:
	const char *_raw = "{}";
};

class DeserializationError {
  public:
	explicit DeserializationError(bool failed = false) : _failed(failed) {
	}

	explicit operator bool() const {
		return _failed;
	}

  private:
	bool _failed = false;
};

inline size_t measureJson(const JsonDocument &json) {
	return std::strlen(json.raw());
}

inline size_t serializeJson(const JsonDocument &json, char *buffer, size_t size) {
	const char *raw = json.raw();
	const size_t length = std::strlen(raw);
	if (buffer == nullptr || size == 0 || size <= length) {
		return 0;
	}
	std::memcpy(buffer, raw, length + 1);
	return length;
}

inline DeserializationError deserializeJson(JsonDocument &json, const char *buffer, size_t size) {
	if (buffer == nullptr && size != 0) {
		return DeserializationError(true);
	}
	json.setRaw(buffer != nullptr ? buffer : "{}");
	return DeserializationError(false);
}
