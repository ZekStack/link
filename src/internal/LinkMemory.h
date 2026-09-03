#pragma once

#include <Arduino.h>
#include <Strata.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace link_memory {

#if !defined(ESP32)
inline size_t &testAllocationBudget() {
	static size_t budget = std::numeric_limits<size_t>::max();
	return budget;
}

inline void testFailAllocationsAfter(size_t successfulAllocations) {
	testAllocationBudget() = successfulAllocations;
}

inline void testResetAllocationFailures() {
	testAllocationBudget() = std::numeric_limits<size_t>::max();
}

inline bool consumeTestAllocation() {
	size_t &budget = testAllocationBudget();
	if (budget == 0) {
		return false;
	}
	if (budget != std::numeric_limits<size_t>::max()) {
		budget--;
	}
	return true;
}
#endif

inline void *allocate(
    size_t bytes,
    Strata::Placement placement = Strata::Placement::PreferExternal
) {
	if (bytes == 0) {
		return nullptr;
	}
#if !defined(ESP32)
	if (!consumeTestAllocation()) {
		return nullptr;
	}
#endif
	return Strata::allocate(bytes, placement);
}

inline void release(void *memory) {
	Strata::free(memory);
}

inline char *duplicateString(
    const char *value,
    size_t length,
    Strata::Placement placement = Strata::Placement::PreferExternal
) {
	if (length == std::numeric_limits<size_t>::max()) {
		return nullptr;
	}
	char *copy = static_cast<char *>(allocate(length + 1, placement));
	if (copy == nullptr) {
		return nullptr;
	}
	if (length > 0 && value != nullptr) {
		std::memcpy(copy, value, length);
	}
	copy[length] = '\0';
	return copy;
}

template <typename T>
T *allocateArray(size_t count, Strata::Placement placement) {
	static_assert(std::is_nothrow_default_constructible_v<T>);
	static_assert(std::is_nothrow_destructible_v<T>);
	if (count == 0 || count > std::numeric_limits<size_t>::max() / sizeof(T)) {
		return nullptr;
	}
#if !defined(ESP32)
	if (!consumeTestAllocation()) {
		return nullptr;
	}
#endif
	void *storage = Strata::allocate(Strata::AllocationRequest{
	    .sizeBytes = count * sizeof(T),
	    .placement = placement,
	    .alignment = alignof(T),
	});
	if (storage == nullptr) {
		return nullptr;
	}
	T *items = static_cast<T *>(storage);
	for (size_t i = 0; i < count; ++i) {
		std::construct_at(items + i);
	}
	return items;
}

template <typename T> void releaseArray(T *items, size_t count) {
	if (items == nullptr) {
		return;
	}
	for (size_t i = 0; i < count; ++i) {
		std::destroy_at(items + i);
	}
	Strata::free(items);
}

} // namespace link_memory

class LinkOwnedBuffer {
  public:
	explicit LinkOwnedBuffer(
	    Strata::Placement placement = Strata::Placement::PreferExternal
	) noexcept
	    : _placement(placement) {
	}

	~LinkOwnedBuffer() {
		clear();
	}

	LinkOwnedBuffer(const LinkOwnedBuffer &) = delete;
	LinkOwnedBuffer &operator=(const LinkOwnedBuffer &) = delete;

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

	void setPlacement(Strata::Placement placement) {
		if (_data == nullptr) {
			_placement = placement;
		}
	}

	Strata::Placement placement() const {
		return _placement;
	}

	Strata::Region region() const {
		return Strata::regionOf(_data);
	}

	bool assign(const uint8_t *data, size_t size) {
		clear();
		if (size == 0) {
			return true;
		}
		if (data == nullptr) {
			return false;
		}
		_data = static_cast<uint8_t *>(link_memory::allocate(size, _placement));
		if (_data == nullptr) {
			return false;
		}
		std::memcpy(_data, data, size);
		_size = size;
		_capacity = size;
		_nulTerminated = false;
		return true;
	}

	bool assignText(const char *value, size_t size) {
		clear();
		if (value == nullptr) {
			value = "";
			size = 0;
		}
		_data = reinterpret_cast<uint8_t *>(
		    link_memory::duplicateString(value, size, _placement)
		);
		if (_data == nullptr) {
			return false;
		}
		_size = size;
		_capacity = size + 1;
		_nulTerminated = true;
		return true;
	}

	bool allocateForWrite(size_t size, bool nulTerminate) {
		clear();
		const size_t capacity = size + (nulTerminate ? 1 : 0);
		if (capacity < size) {
			return false;
		}
		if (capacity == 0) {
			return true;
		}
		_data = static_cast<uint8_t *>(link_memory::allocate(capacity, _placement));
		if (_data == nullptr) {
			return false;
		}
		_size = size;
		_capacity = capacity;
		_nulTerminated = nulTerminate;
		if (nulTerminate) {
			_data[size] = '\0';
		}
		return true;
	}

	bool reserve(size_t capacity) {
		if (capacity <= _capacity) {
			return true;
		}
		uint8_t *next = static_cast<uint8_t *>(link_memory::allocate(capacity, _placement));
		if (next == nullptr) {
			return false;
		}
		if (_data != nullptr && _size > 0) {
			std::memcpy(next, _data, _size);
		}
		if (_nulTerminated) {
			next[_size] = '\0';
		}
		link_memory::release(_data);
		_data = next;
		_capacity = capacity;
		return true;
	}

	bool append(const uint8_t *data, size_t size, bool nulTerminate = false) {
		if (size == 0) {
			if (nulTerminate && _data == nullptr) {
				return assignText("", 0);
			}
			return true;
		}
		if (data == nullptr) {
			return false;
		}
		const size_t extra = nulTerminate ? 1 : 0;
		if (_size > std::numeric_limits<size_t>::max() - size ||
		    _size + size > std::numeric_limits<size_t>::max() - extra) {
			return false;
		}
		const size_t required = _size + size + extra;
		size_t nextCapacity = _capacity == 0 ? required : _capacity;
		while (nextCapacity < required) {
			const size_t grown = nextCapacity * 2;
			if (grown <= nextCapacity) {
				nextCapacity = required;
				break;
			}
			nextCapacity = grown;
		}
		if (!reserve(nextCapacity)) {
			return false;
		}
		std::memcpy(_data + _size, data, size);
		if (nulTerminate) {
			_data[_size + size] = '\0';
		}
		_size += size;
		_nulTerminated = nulTerminate;
		return true;
	}

	void clear() {
		link_memory::release(_data);
		_data = nullptr;
		_size = 0;
		_capacity = 0;
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

	bool copyFrom(const LinkOwnedBuffer &other) {
		if (this == &other) {
			return true;
		}
		clear();
		if (other._data == nullptr) {
			return true;
		}
		if (other._nulTerminated) {
			return assignText(reinterpret_cast<const char *>(other._data), other._size);
		}
		return assign(other._data, other._size);
	}

  private:
	void moveFrom(LinkOwnedBuffer &other) {
		_data = other._data;
		_size = other._size;
		_capacity = other._capacity;
		_nulTerminated = other._nulTerminated;
		_placement = other._placement;
		other._data = nullptr;
		other._size = 0;
		other._capacity = 0;
		other._nulTerminated = false;
	}

	uint8_t *_data = nullptr;
	size_t _size = 0;
	size_t _capacity = 0;
	bool _nulTerminated = false;
	Strata::Placement _placement = Strata::Placement::PreferExternal;
	inline static uint8_t _empty[1] = {0};
};
