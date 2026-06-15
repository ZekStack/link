#include <Link.h>

#include <cassert>
#include <cstring>

namespace {

void testResultAndStateStrings() {
	LinkResult ok = LinkResult::ok();
	assert(ok);
	assert(std::strcmp(linkErrorCodeToString(LinkErrorCode::Stopping), "Stopping") == 0);
	assert(std::strcmp(linkStateToString(LinkState::Running), "Running") == 0);
}

void testHeaders() {
	LinkHeaders headers;
	headers.configureLimits(2, 8, 16, 32);
	assert(headers.add("Accept", "application/json"));
	assert(headers.has("accept"));
	assert(std::strcmp(headers.get("ACCEPT"), "application/json") == 0);
	assert(headers.set("Accept", "text/plain"));
	assert(headers.size() == 1);
	assert(std::strcmp(headers.get("accept"), "text/plain") == 0);
	assert(headers.add("X-Test", "1"));
	assert(headers.add("Extra", "2").code == LinkErrorCode::TooManyHeaders);
	assert(headers.add("Long-Header-Name", "2").code == LinkErrorCode::HeaderTooLarge);
}

void testBody() {
	LinkBody text = LinkBody::text("hello");
	assert(text.valid());
	assert(text.size() == 5);
	assert(std::strcmp(text.c_str(), "hello") == 0);

	const uint8_t data[] = {1, 2, 3};
	LinkBody bytes = LinkBody::bytes(data, sizeof(data));
	assert(bytes.valid());
	assert(bytes.size() == 3);
	assert(bytes.data()[1] == 2);
}

void freeFunctionResponse(const LinkResponse &) {
}

void testCallbacks() {
	LinkCallback<void(const LinkResponse &), 64> callback;
	assert(callback.assign(freeFunctionResponse));
	LinkResponse response;
	callback(response);

	int called = 0;
	assert(callback.assign([&called](const LinkResponse &) {
		called++;
	}));
	callback(response);
	assert(called == 1);

	struct Handler {
		int called = 0;
		void handle(const LinkResponse &) {
			called++;
		}
	};

	Handler handler;
	assert(callback.assign(LinkCallback<void(const LinkResponse &), 64>::bind(&handler, &Handler::handle)));
	callback(response);
	assert(handler.called == 1);

	struct LargeCallback {
		char data[128]{};
		void operator()(const LinkResponse &) {
			data[0] = 1;
		}
	};
	LinkCallback<void(const LinkResponse &), 16> small;
	assert(!small.assign(LargeCallback{}));
}

void testLifecycleAndQueueLimits() {
	Link link;
	LinkConfig config;
	config.queueSize = 1;
	config.maxConcurrentRequests = 1;
	assert(link.init(config));
	assert(link.isInitialized());

	LinkResult first = link.get("https://example.com", [](const LinkResponse &) {});
	assert(first);
	LinkResult full = link.get("https://example.com/again", [](const LinkResponse &) {});
	assert(full.code == LinkErrorCode::QueueFull);
	assert(link.deinit());
	assert(!link.isInitialized());
}

void testRequestValidation() {
	Link link;
	LinkConfig config;
	config.queueSize = 1;
	config.maxConcurrentRequests = 1;
	config.maxUrlSize = 12;
	assert(link.init(config));
	assert(link.get("ftp://bad", [](const LinkResponse &) {}).code == LinkErrorCode::InvalidUrl);
	assert(link.get("https://example.com/too-long", [](const LinkResponse &) {}).code == LinkErrorCode::UrlTooLarge);
	assert(link.deinit());
}

} // namespace

int main() {
	testResultAndStateStrings();
	testHeaders();
	testBody();
	testCallbacks();
	testLifecycleAndQueueLimits();
	testRequestValidation();
	return 0;
}
