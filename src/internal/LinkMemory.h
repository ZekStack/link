#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

namespace link_memory {

inline void *allocate(size_t bytes, bool preferPsram = true) {
	if (bytes == 0) {
		return nullptr;
	}
#if defined(ESP32) && defined(MALLOC_CAP_8BIT)
	void *memory = nullptr;
	if (preferPsram) {
#if defined(MALLOC_CAP_SPIRAM)
		memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
	}
	if (memory == nullptr) {
		memory = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
	}
	return memory;
#else
	(void)preferPsram;
	return ::operator new(bytes, std::nothrow);
#endif
}

inline void release(void *memory) {
	if (memory == nullptr) {
		return;
	}
#if defined(ESP32)
	heap_caps_free(memory);
#else
	::operator delete(memory);
#endif
}

inline char *duplicateString(const char *value, size_t length, bool preferPsram = true) {
	char *copy = static_cast<char *>(allocate(length + 1, preferPsram));
	if (copy == nullptr) {
		return nullptr;
	}
	if (length > 0 && value != nullptr) {
		std::memcpy(copy, value, length);
	}
	copy[length] = '\0';
	return copy;
}

} // namespace link_memory

class LinkOwnedBuffer {
  public:
	LinkOwnedBuffer() = default;

	~LinkOwnedBuffer() {
		clear();
	}

	LinkOwnedBuffer(const LinkOwnedBuffer &other) {
		copyFrom(other);
	}

	LinkOwnedBuffer &operator=(const LinkOwnedBuffer &other) {
		if (this != &other) {
			clear();
			copyFrom(other);
		}
		return *this;
	}

	LinkOwnedBuffer(LinkOwnedBuffer &&other) noexcept {
		moveFrom(other);
	}

	LinkOwnedBuffer &operator=(LinkOwnedBuffer &&other) noexcept {
		if (this != &other) {
			clear();
			moveFrom(other);
		}
		return *this;
	}

	bool assign(const uint8_t *data, size_t size, bool preferPsram = true) {
		clear();
		if (size == 0) {
			return true;
		}
		if (data == nullptr) {
			return false;
		}
		_data = static_cast<uint8_t *>(link_memory::allocate(size, preferPsram));
		if (_data == nullptr) {
			return false;
		}
		std::memcpy(_data, data, size);
		_size = size;
		_nulTerminated = false;
		return true;
	}

	bool assignText(const char *value, size_t size, bool preferPsram = true) {
		clear();
		if (value == nullptr) {
			value = "";
			size = 0;
		}
		_data = reinterpret_cast<uint8_t *>(link_memory::duplicateString(value, size, preferPsram));
		if (_data == nullptr) {
			return false;
		}
		_size = size;
		_nulTerminated = true;
		return true;
	}

	bool append(const uint8_t *data, size_t size, bool nulTerminate = false, bool preferPsram = true) {
		if (size == 0) {
			if (nulTerminate && _data == nullptr) {
				return assignText("", 0, preferPsram);
			}
			return true;
		}
		if (data == nullptr) {
			return false;
		}
		const size_t extra = nulTerminate ? 1 : 0;
		uint8_t *next = static_cast<uint8_t *>(link_memory::allocate(_size + size + extra, preferPsram));
		if (next == nullptr) {
			return false;
		}
		if (_data != nullptr && _size > 0) {
			std::memcpy(next, _data, _size);
		}
		std::memcpy(next + _size, data, size);
		if (nulTerminate) {
			next[_size + size] = '\0';
		}
		link_memory::release(_data);
		_data = next;
		_size += size;
		_nulTerminated = nulTerminate;
		return true;
	}

	void clear() {
		link_memory::release(_data);
		_data = nullptr;
		_size = 0;
		_nulTerminated = false;
	}

	uint8_t *data() {
		return _data;
	}

	const uint8_t *data() const {
		return _data;
	}

	char *c_str() {
		return reinterpret_cast<char *>(_data != nullptr ? _data : _empty);
	}

	const char *c_str() const {
		return reinterpret_cast<const char *>(_data != nullptr ? _data : _empty);
	}

	size_t size() const {
		return _size;
	}

	bool empty() const {
		return _size == 0;
	}

  private:
	void copyFrom(const LinkOwnedBuffer &other) {
		if (other._data == nullptr) {
			return;
		}
		if (other._nulTerminated) {
			assignText(reinterpret_cast<const char *>(other._data), other._size);
			return;
		}
		assign(other._data, other._size);
	}

	void moveFrom(LinkOwnedBuffer &other) {
		_data = other._data;
		_size = other._size;
		_nulTerminated = other._nulTerminated;
		other._data = nullptr;
		other._size = 0;
		other._nulTerminated = false;
	}

	uint8_t *_data = nullptr;
	size_t _size = 0;
	bool _nulTerminated = false;
	inline static uint8_t _empty[1] = {0};
};
