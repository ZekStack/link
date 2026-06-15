#include <Arduino.h>
#include <Link.h>
#include <WiFi.h>

Link client;

void waitForWiFi() {
	while (WiFi.status() != WL_CONNECTED) {
		delay(250);
	}
}

void onResponse(const LinkResponse &response) {
	if (!response) {
		Serial.println(response.error.message);
		return;
	}

	Serial.println(response.httpStatus);
	Serial.println(response.body.c_str());
}

void setup() {
	Serial.begin(115200);

	WiFi.begin("ssid", "password");
	waitForWiFi();

	LinkConfig config;
	config.maxConcurrentRequests = 2;

	LinkResult initResult = client.init(config);
	if (!initResult) {
		Serial.println(initResult.message);
		return;
	}

	LinkResult result = client.get("https://example.com", onResponse);
	if (!result) {
		Serial.println(result.message);
	}
}

void loop() {
	delay(1000);
}
