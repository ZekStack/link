#include <Arduino.h>
#include <Link.h>

#include <atomic>
#include <esp_heap_caps.h>

namespace {

constexpr size_t kRoundsPerMode = 100;
constexpr size_t kProducerCount = 3;
constexpr const char *kTarget = "http://192.0.2.1/link-lifecycle-stress";

Link link;
std::atomic_bool producersRunning{false};
std::atomic_size_t producersActive{0};
std::atomic_size_t callbacks{0};
std::atomic_size_t cancelled{0};
std::atomic_size_t unexpectedSubmissionErrors{0};

void onResponse(const LinkResponse &response) {
	callbacks.fetch_add(1, std::memory_order_relaxed);
	if (response.error.code == LinkErrorCode::Cancelled) {
		cancelled.fetch_add(1, std::memory_order_relaxed);
	}
}

void producerTask(void *) {
	producersActive.fetch_add(1, std::memory_order_relaxed);
	while (producersRunning.load(std::memory_order_relaxed)) {
		const LinkResult result = link.get(kTarget, onResponse);
		if (!result && result.code != LinkErrorCode::QueueFull &&
		    result.code != LinkErrorCode::Stopping &&
		    result.code != LinkErrorCode::NotInitialized) {
			unexpectedSubmissionErrors.fetch_add(1, std::memory_order_relaxed);
		}
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	producersActive.fetch_sub(1, std::memory_order_relaxed);
	vTaskDelete(nullptr);
}

bool startProducers() {
	producersRunning.store(true, std::memory_order_relaxed);
	for (size_t i = 0; i < kProducerCount; ++i) {
		char name[20]{};
		snprintf(name, sizeof(name), "link-producer-%u", static_cast<unsigned>(i));
		if (xTaskCreate(producerTask, name, 4096, nullptr, 2, nullptr) != pdPASS) {
			producersRunning.store(false, std::memory_order_relaxed);
			return false;
		}
	}
	while (producersActive.load(std::memory_order_relaxed) != kProducerCount) {
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	return true;
}

void stopProducers() {
	producersRunning.store(false, std::memory_order_relaxed);
	while (producersActive.load(std::memory_order_relaxed) != 0) {
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

bool verifyStoppedGeneration(size_t callbackBase, LinkConnectionMode mode, size_t round) {
	const LinkDiagnostics diagnostics = link.diagnostics();
	const size_t callbackDelta = callbacks.load(std::memory_order_relaxed) - callbackBase;
	if (diagnostics.requestsSubmitted != diagnostics.requestsCompleted ||
	    callbackDelta != diagnostics.requestsSubmitted || diagnostics.activeHttpClients != 0 ||
	    diagnostics.httpClientCreates != diagnostics.httpClientCleanups) {
		Serial.printf(
		    "generation mismatch mode=%u round=%u submitted=%u completed=%u callbacks=%u "
		    "creates=%u cleanups=%u active=%u\n",
		    static_cast<unsigned>(mode),
		    static_cast<unsigned>(round),
		    static_cast<unsigned>(diagnostics.requestsSubmitted),
		    static_cast<unsigned>(diagnostics.requestsCompleted),
		    static_cast<unsigned>(callbackDelta),
		    static_cast<unsigned>(diagnostics.httpClientCreates),
		    static_cast<unsigned>(diagnostics.httpClientCleanups),
		    static_cast<unsigned>(diagnostics.activeHttpClients)
		);
		return false;
	}
	if (!heap_caps_check_integrity_all(true)) {
		Serial.printf(
		    "heap integrity failed mode=%u round=%u\n",
		    static_cast<unsigned>(mode),
		    static_cast<unsigned>(round)
		);
		return false;
	}
	return true;
}

bool runMode(LinkConnectionMode mode) {
	LinkConfig config;
	config.queueSize = 8;
	config.maxConcurrentRequests = 2;
	config.defaultTimeoutMs = 250;
	config.connectionMode = mode;
	config.persistentIdleTimeoutMs = 1000;
	config.persistentMaxRequestsPerHandle = 32;

	if (!link.init(config) || !startProducers()) {
		(void)link.deinit();
		stopProducers();
		return false;
	}

	size_t callbackBase = callbacks.load(std::memory_order_relaxed);
	for (size_t round = 0; round < kRoundsPerMode; ++round) {
		vTaskDelay(pdMS_TO_TICKS(10));
		if (!link.deinit()) {
			stopProducers();
			return false;
		}
		if (!verifyStoppedGeneration(callbackBase, mode, round)) {
			stopProducers();
			return false;
		}

		const size_t callbacksAfterStop = callbacks.load(std::memory_order_relaxed);
		vTaskDelay(pdMS_TO_TICKS(5));
		if (callbacks.load(std::memory_order_relaxed) != callbacksAfterStop) {
			Serial.println("callback arrived after successful deinit");
			stopProducers();
			return false;
		}

		if (round + 1 < kRoundsPerMode) {
			if (!link.init(config)) {
				stopProducers();
				return false;
			}
			callbackBase = callbacks.load(std::memory_order_relaxed);
		}
	}

	stopProducers();
	return unexpectedSubmissionErrors.load(std::memory_order_relaxed) == 0;
}

} // namespace

void setup() {
	Serial.begin(115200);
	delay(500);

	const bool perRequestPassed = runMode(LinkConnectionMode::PerRequest);
	const bool persistentPassed = perRequestPassed && runMode(LinkConnectionMode::PersistentPerWorker);
	if (!persistentPassed) {
		Serial.printf(
		    "link lifecycle stress failed: callbacks=%u cancelled=%u unexpected=%u\n",
		    static_cast<unsigned>(callbacks.load(std::memory_order_relaxed)),
		    static_cast<unsigned>(cancelled.load(std::memory_order_relaxed)),
		    static_cast<unsigned>(unexpectedSubmissionErrors.load(std::memory_order_relaxed))
		);
		return;
	}

	Serial.printf(
	    "link lifecycle stress passed: callbacks=%u cancelled=%u free=%u largest=%u minimum=%u\n",
	    static_cast<unsigned>(callbacks.load(std::memory_order_relaxed)),
	    static_cast<unsigned>(cancelled.load(std::memory_order_relaxed)),
	    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
	    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
	    static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT))
	);
}

void loop() {
	delay(1000);
}
