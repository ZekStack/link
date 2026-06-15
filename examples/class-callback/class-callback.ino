#include <Arduino.h>
#include <Link.h>
#include <WiFi.h>

class ApiClient {
  public:
	void begin() {
		LinkConfig config;
		config.maxConcurrentRequests = 2;

		if (!client.init(config)) {
			return;
		}

		client.get(
		    "https://example.com/status",
		    Link::ResponseCallback::bind(this, &ApiClient::onStatus)
		);
	}

  private:
	void onStatus(const LinkResponse &response) {
		if (!response) {
			Serial.println(response.error.message);
			return;
		}

		Serial.println(response.httpStatus);
		Serial.println(response.body.c_str());
	}

	Link client;
};

ApiClient api;

void waitForWiFi() {
	while (WiFi.status() != WL_CONNECTED) {
		delay(250);
	}
}

void setup() {
	Serial.begin(115200);
	WiFi.begin("ssid", "password");
	waitForWiFi();
	api.begin();
}

void loop() {
	delay(1000);
}
