#include <Arduino.h>
#include <Link.h>
#include <WiFi.h>

Link client;

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
	config.streamChunkSize = 2048;

	if (!client.init(config)) {
		return;
	}

	client.getStream(
	    "https://example.com/firmware.bin",
	    [](const LinkStreamInfo &info) {
		    Serial.println(info.httpStatus);
		    Serial.println(info.contentLength);
	    },
	    [](const LinkStreamChunk &chunk) {
		    Serial.println(chunk.totalReceived);
		    return LinkStreamAction::Continue;
	    },
	    [](const LinkStreamResult &result) {
		    if (!result) {
			    Serial.println(result.error.message);
			    return;
		    }
		    Serial.println(result.totalReceived);
	    }
	);
}

void loop() {
	delay(1000);
}
