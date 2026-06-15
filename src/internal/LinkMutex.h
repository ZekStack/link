#pragma once

#include <Arduino.h>

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#else
using TickType_t = uint32_t;
constexpr TickType_t portMAX_DELAY = 0xffffffffu;
#endif

class LinkMutex {
  public:
	LinkMutex() {
#if defined(ESP32)
		_handle = xSemaphoreCreateRecursiveMutex();
#endif
	}

	~LinkMutex() {
#if defined(ESP32)
		if (_handle != nullptr) {
			vSemaphoreDelete(_handle);
		}
#endif
	}

	LinkMutex(const LinkMutex &) = delete;
	LinkMutex &operator=(const LinkMutex &) = delete;

	bool lock(TickType_t timeout = portMAX_DELAY) {
#if defined(ESP32)
		return _handle != nullptr && xSemaphoreTakeRecursive(_handle, timeout) == pdTRUE;
#else
		(void)timeout;
		return true;
#endif
	}

	void unlock() {
#if defined(ESP32)
		if (_handle != nullptr) {
			xSemaphoreGiveRecursive(_handle);
		}
#endif
	}

	bool ready() const {
#if defined(ESP32)
		return _handle != nullptr;
#else
		return true;
#endif
	}

  private:
#if defined(ESP32)
	SemaphoreHandle_t _handle = nullptr;
#endif
};

class LinkLock {
  public:
	explicit LinkLock(LinkMutex &mutex) : _mutex(mutex), _locked(mutex.lock()) {
	}

	~LinkLock() {
		if (_locked) {
			_mutex.unlock();
		}
	}

	LinkLock(const LinkLock &) = delete;
	LinkLock &operator=(const LinkLock &) = delete;

	explicit operator bool() const {
		return _locked;
	}

  private:
	LinkMutex &_mutex;
	bool _locked = false;
};
