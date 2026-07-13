#pragma once

#include <Arduino.h>
#include <cstddef>

#if defined(ESP32)
extern "C" {
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

#if __has_include("freertos/idf_additions.h")
extern "C" {
#include "freertos/idf_additions.h"
}
#define LINK_HAS_IDF_TASK_CAPS 1
#else
#define LINK_HAS_IDF_TASK_CAPS 0
#endif

#if LINK_HAS_IDF_TASK_CAPS && defined(configSUPPORT_STATIC_ALLOCATION) &&                          \
    (configSUPPORT_STATIC_ALLOCATION == 1) && defined(MALLOC_CAP_SPIRAM)
#define LINK_CAN_USE_EXTERNAL_STACKS 1
#else
#define LINK_CAN_USE_EXTERNAL_STACKS 0
#endif
#else
using TaskHandle_t = void *;
using TaskFunction_t = void (*)(void *);
using UBaseType_t = unsigned int;
using BaseType_t = int;
constexpr BaseType_t tskNO_AFFINITY = -1;
constexpr BaseType_t pdPASS = 1;
constexpr BaseType_t pdFAIL = 0;
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

inline bool hasExternalStackSupport() {
#if defined(ESP32) && LINK_CAN_USE_EXTERNAL_STACKS
	return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
	return false;
#endif
}

inline BaseType_t createTask(
    TaskFunction_t entry,
    const char *name,
    size_t stackBytes,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t coreId,
    bool usePsramStack,
    bool &createdWithCaps
) {
	createdWithCaps = false;
	if (!isValidStackSize(stackBytes)) {
		return pdFAIL;
	}
#if defined(ESP32)
	if (usePsramStack) {
#if LINK_CAN_USE_EXTERNAL_STACKS
		if (!hasExternalStackSupport()) {
			return pdFAIL;
		}
		const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
		    entry,
		    name,
		    static_cast<configSTACK_DEPTH_TYPE>(stackBytes),
		    arg,
		    priority,
		    handle,
		    coreId,
		    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
		);
		createdWithCaps = created == pdPASS;
		return created;
#else
		return pdFAIL;
#endif
	}
	if (coreId == tskNO_AFFINITY) {
		return xTaskCreate(entry, name, static_cast<uint32_t>(stackBytes), arg, priority, handle);
	}
	return xTaskCreatePinnedToCore(
	    entry,
	    name,
	    static_cast<uint32_t>(stackBytes),
	    arg,
	    priority,
	    handle,
	    coreId
	);
#else
	(void)entry;
	(void)name;
	(void)stackBytes;
	(void)arg;
	(void)priority;
	(void)coreId;
	(void)usePsramStack;
	if (handle != nullptr) {
		*handle = nullptr;
	}
	return pdPASS;
#endif
}

inline void delayMs(uint32_t durationMs) {
#if defined(ESP32)
	vTaskDelay(pdMS_TO_TICKS(durationMs));
#else
	(void)durationMs;
#endif
}

inline void deleteCurrentTask(bool createdWithCaps) {
#if defined(ESP32)
#if LINK_CAN_USE_EXTERNAL_STACKS
	if (createdWithCaps) {
		vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
		return;
	}
#endif
	(void)createdWithCaps;
	vTaskDelete(nullptr);
#else
	(void)createdWithCaps;
#endif
}

} // namespace link_task_support
