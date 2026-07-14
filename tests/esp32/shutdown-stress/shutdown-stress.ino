#include <Arduino.h>
#include <Link.h>

#include <atomic>

namespace {

constexpr size_t kRounds = 100;
std::atomic_size_t callbacks{0};
std::atomic_size_t cancelled{0};

void onResponse(const LinkResponse &response) {
	callbacks.fetch_add(1, std::memory_order_relaxed);
	if (response.error.code == LinkErrorCode::Cancelled) {
		cancelled.fetch_add(1, std::memory_order_relaxed);
	}
}

bool runRound() {
	Link link;
	LinkConfig config;
	config.queueSize = 4;
	config.maxConcurrentRequests = 2;
	config.defaultTimeoutMs = 250;
	if (!link.init(config)) {
		return false;
	}

	const size_t callbacksBefore = callbacks.load(std::memory_order_relaxed);
	size_t accepted = 0;
	for (size_t i = 0; i < config.queueSize; ++i) {
		if (link.get("http://192.0.2.1/shutdown-stress", onResponse)) {
			accepted++;
		}
	}

	const LinkResult result = link.deinit();
	return result && callbacks.load(std::memory_order_relaxed) - callbacksBefore == accepted;
}

} // namespace

void setup() {
	Serial.begin(115200);
	delay(500);
	for (size_t round = 0; round < kRounds; ++round) {
		if (!runRound()) {
			Serial.printf("shutdown stress failed at round %u\n", static_cast<unsigned>(round));
			return;
		}
	}
	Serial.printf(
	    "shutdown stress passed: callbacks=%u cancelled=%u\n",
	    static_cast<unsigned>(callbacks.load(std::memory_order_relaxed)),
	    static_cast<unsigned>(cancelled.load(std::memory_order_relaxed))
	);
}

void loop() {
	delay(1000);
}
