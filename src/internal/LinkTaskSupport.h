#pragma once

#include <Arduino.h>
#include <cstddef>

#if defined(ESP32)
#include <strata/freertos/Task.h>

extern "C" {
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
}
#else
using UBaseType_t = unsigned int;
using BaseType_t = int;
constexpr BaseType_t tskNO_AFFINITY = -1;
#endif

namespace link_task_support {
constexpr size_t kMinStackSizeBytes = 1024;

inline bool isValidStackSize(size_t stackBytes) {
#if defined(ESP32)
	return stackBytes >= kMinStackSizeBytes && (stackBytes % sizeof(StackType_t)) == 0;
#else
	return stackBytes >= kMinStackSizeBytes;
#endif
}

inline void delayMs(uint32_t durationMs) {
#if defined(ESP32)
	vTaskDelay(pdMS_TO_TICKS(durationMs));
#else
	(void)durationMs;
#endif
}

[[noreturn]] inline void suspendCurrentTask() {
#if defined(ESP32)
	vTaskSuspend(nullptr);
	for (;;) {
		vTaskDelay(portMAX_DELAY);
	}
#else
	for (;;) {
	}
#endif
}

} // namespace link_task_support
