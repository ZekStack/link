#include <Arduino.h>
#include <Link.h>
#include <WiFi.h>

Link client;

constexpr size_t kPayloadSize = 64U * 1024U;

void waitForWiFi() {
	while (WiFi.status() != WL_CONNECTED) {
		delay(250);
	}
}

void setup() {
	Serial.begin(115200);

	WiFi.begin("ssid", "password");
	waitForWiFi();

	LinkConfig config;
	config.maxRequestBodySize = kPayloadSize;
	config.streamChunkSize = 2048;

	if (!client.init(config)) {
		return;
	}

	client.postStreamBody(
	    "https://example.com/upload",
	    kPayloadSize,
	    [](size_t offset, uint8_t *destination, size_t capacity) -> size_t {
		    for (size_t i = 0; i < capacity; ++i) {
			    destination[i] = static_cast<uint8_t>((offset + i) & 0xFFU);
		    }
		    return capacity;
	    },
	    [](const LinkResponse &response) {
		    if (!response) {
			    Serial.println(response.error.message);
			    return;
		    }
		    Serial.println(response.httpStatus);
	    }
	);
}

void loop() {
	delay(1000);
}
