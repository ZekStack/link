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

	LinkHeaders copy;
	assert(copy.copyFrom(headers));
	assert(copy.size() == headers.size());
	assert(std::strcmp(copy.get("accept"), "text/plain") == 0);
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

	LinkBody copy;
	assert(copy.copyFrom(text));
	assert(copy.valid());
	assert(copy.size() == 5);
	assert(std::strcmp(copy.c_str(), "hello") == 0);
}

void testOwnedBufferAppend() {
	LinkOwnedBuffer buffer;
	const uint8_t first[] = {'h', 'e'};
	const uint8_t second[] = {'l', 'l', 'o'};
	assert(buffer.append(first, sizeof(first), true));
	assert(std::strcmp(buffer.c_str(), "he") == 0);
	assert(buffer.append(second, sizeof(second), true));
	assert(buffer.size() == 5);
	assert(std::strcmp(buffer.c_str(), "hello") == 0);
}

void testRedirectDecisions() {
	LinkConfig config;
	LinkHeaders headers;
	assert(headers.add("location", "https://example.com/final"));

	const int followedStatuses[] = {301, 302, 303, 307, 308};
	for (int status : followedStatuses) {
		const link_internal::LinkRedirectDecision decision =
		    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, status, headers, 0);
		assert(decision.action == link_internal::LinkRedirectAction::Follow);
		assert(std::strcmp(decision.location, "https://example.com/final") == 0);
	}

	const int finalStatuses[] = {200, 300, 304, 305, 306, 404};
	for (int status : finalStatuses) {
		const link_internal::LinkRedirectDecision decision =
		    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, status, headers, 0);
		assert(decision.action == link_internal::LinkRedirectAction::None);
	}

	assert(
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Post, 302, headers, 0).action ==
	    link_internal::LinkRedirectAction::None
	);
	config.followRedirects = false;
	assert(
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, headers, 0).action ==
	    link_internal::LinkRedirectAction::None
	);

	config.followRedirects = true;
	LinkHeaders missingLocation;
	assert(
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, missingLocation, 0)
	        .action == link_internal::LinkRedirectAction::None
	);
	LinkHeaders relativeLocation;
	assert(relativeLocation.add("Location", "/final"));
	assert(
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, relativeLocation, 0)
	        .action == link_internal::LinkRedirectAction::None
	);

	config.maxRedirects = 3;
	const link_internal::LinkRedirectDecision limit =
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, headers, 3);
	assert(limit.action == link_internal::LinkRedirectAction::Error);
	assert(limit.error.code == LinkErrorCode::RedirectLimitReached);
	assert(
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, headers, 2).action ==
	    link_internal::LinkRedirectAction::Follow
	);

	config.maxUrlSize = 12;
	const link_internal::LinkRedirectDecision tooLarge =
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, headers, 0);
	assert(tooLarge.action == link_internal::LinkRedirectAction::Error);
	assert(tooLarge.error.code == LinkErrorCode::UrlTooLarge);
}

void freeFunctionResponse(const LinkResponse &) {
}

void testCallbacks() {
	LinkCallback<void(const LinkResponse &), 64> callback;
	assert(callback.assign(freeFunctionResponse));
	LinkResponse response;
	callback(response);

	int called = 0;
	assert(callback.assign([&called](const LinkResponse &) { called++; }));
	callback(response);
	assert(called == 1);

	struct Handler {
		int called = 0;
		void handle(const LinkResponse &) {
			called++;
		}
	};

	Handler handler;
	assert(callback.assign(
	    LinkCallback<void(const LinkResponse &), 64>::bind(&handler, &Handler::handle)
	));
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

void testInvalidQueueConcurrencyConfig() {
	Link link;
	LinkConfig config;
	config.queueSize = 1;
	config.maxConcurrentRequests = 2;
	LinkResult result = link.init(config);
	assert(result.code == LinkErrorCode::InvalidConfig);
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
	assert(
	    link.get("https://example.com/too-long", [](const LinkResponse &) {}).code ==
	    LinkErrorCode::UrlTooLarge
	);
	assert(link.deinit());
}

} // namespace

int main() {
	testResultAndStateStrings();
	testHeaders();
	testBody();
	testOwnedBufferAppend();
	testRedirectDecisions();
	testCallbacks();
	testLifecycleAndQueueLimits();
	testInvalidQueueConcurrencyConfig();
	testRequestValidation();
	return 0;
}
