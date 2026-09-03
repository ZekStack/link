#pragma once

#include <Arduino.h>

#if defined(ESP32)
#include <strata/freertos/Mutex.h>
#else
using TickType_t = uint32_t;
constexpr TickType_t portMAX_DELAY = 0xffffffffu;
#endif

class LinkMutex {
  public:
	LinkMutex() {
#if defined(ESP32)
		_mutex = Strata::FreeRTOS::RecursiveMutex::create();
#endif
	}

	LinkMutex(const LinkMutex &) = delete;
	LinkMutex &operator=(const LinkMutex &) = delete;

	bool lock(TickType_t timeout = portMAX_DELAY) {
#if defined(ESP32)
		return _mutex.lock(timeout);
#else
		(void)timeout;
		return true;
#endif
	}

	void unlock() {
#if defined(ESP32)
		_mutex.unlock();
#endif
	}

	bool ready() const {
#if defined(ESP32)
		return _mutex.valid();
#else
		return true;
#endif
	}

  private:
#if defined(ESP32)
	Strata::FreeRTOS::RecursiveMutex _mutex;
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
