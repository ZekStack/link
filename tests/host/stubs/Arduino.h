#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using String = const char *;

inline uint32_t millis() {
	static uint32_t now = 100;
	return now += 10;
}

class SerialClass {
  public:
	void begin(unsigned long) {
	}

	template <typename T> void println(T) {
	}
};

inline SerialClass Serial;
