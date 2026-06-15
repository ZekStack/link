#include <Arduino.h>
#include <ArduinoJson.h>
#include <Link.h>
#include <WiFi.h>

Link link;

void waitForWiFi() {
	while (WiFi.status() != WL_CONNECTED) {
		delay(250);
	}
}

void setup() {
	Serial.begin(115200);

	WiFi.begin("ssid", "password");
	waitForWiFi();

	if (!link.init()) {
		return;
	}

	JsonDocument payload;
	payload["device"] = "esp32";
	payload["online"] = true;

	link.postJson("https://example.com/api/devices", payload, [](const LinkJsonResponse &response) {
		if (!response) {
			Serial.println(response.error.message);
			return;
		}

		Serial.println(response.httpStatus);
		serializeJson(response.json, Serial);
		Serial.println();
	});
}

void loop() {
	delay(1000);
}
