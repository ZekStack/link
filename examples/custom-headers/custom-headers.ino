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

	if (!client.init()) {
		return;
	}

	LinkHeaders headers;
	headers.set("Authorization", "Bearer token");
	headers.set("Accept", "application/json");

	client.get("https://example.com/api/status", headers, [](const LinkResponse &response) {
		if (!response) {
			Serial.println(response.error.message);
			return;
		}

		Serial.println(response.httpStatus);
		Serial.println(response.headers.get("Content-Type"));
	});
}

void loop() {
	delay(1000);
}
