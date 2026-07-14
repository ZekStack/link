#include <Link.h>

#include <cassert>
#include <cstring>
#include <limits>

namespace {

void testResultAndStateStrings() {
	LinkResult ok = LinkResult::ok();
	assert(ok);
	assert(std::strcmp(linkErrorCodeToString(LinkErrorCode::Stopping), "Stopping") == 0);
	assert(
	    std::strcmp(linkErrorCodeToString(LinkErrorCode::RedirectRejected), "RedirectRejected") == 0
	);
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
	LinkConfig config;
	config.maxRequestBodySize = 5;
	config.maxSerializedJsonSize = 5;

	LinkBody text;
	assert(link_internal::linkBodyFromView(LinkBodyView::text("hello"), config, text));
	assert(text.valid());
	assert(text.size() == 5);
	assert(std::strcmp(text.c_str(), "hello") == 0);

	const uint8_t data[] = {1, 2, 3};
	LinkBody bytes;
	assert(link_internal::linkBodyFromView(LinkBodyView::bytes(data, sizeof(data)), config, bytes));
	assert(bytes.valid());
	assert(bytes.size() == 3);
	assert(bytes.data()[1] == 2);
	assert(
	    link_internal::linkBodyFromView(LinkBodyView::bytes(nullptr, 1), config, bytes).code ==
	    LinkErrorCode::InternalError
	);

	LinkBody copy;
	assert(copy.copyFrom(text));
	assert(copy.valid());
	assert(copy.size() == 5);
	assert(std::strcmp(copy.c_str(), "hello") == 0);

	LinkBody oversized;
	assert(
	    link_internal::linkBodyFromView(LinkBodyView::text("123456"), config, oversized).code ==
	    LinkErrorCode::RequestTooLarge
	);
	assert(oversized.size() == 0);

	JsonDocument json;
	json.setRaw("12345");
	LinkBody jsonBody;
	assert(link_internal::linkBodyFromView(LinkBodyView::json(json), config, jsonBody));
	assert(jsonBody.type() == LinkBodyType::Json);
	assert(jsonBody.size() == 5);
	assert(std::strcmp(jsonBody.c_str(), "12345") == 0);
	json.setRaw("123456");
	assert(
	    link_internal::linkBodyFromView(LinkBodyView::json(json), config, jsonBody).code ==
	    LinkErrorCode::RequestTooLarge
	);
}

void testWorkerSignalCapacity() {
	LinkConfig config;
	config.queueSize = 3;
	config.maxConcurrentRequests = 3;
	UBaseType_t capacity = 0;
	assert(link_internal::linkWorkerSignalCapacity(config, capacity));
	assert(capacity == 6);

	// A full queue consumes all request permits. Shutdown must still be able to add one
	// independent permit per worker, including when workers race for the final request.
	UBaseType_t permits = static_cast<UBaseType_t>(config.queueSize);
	for (size_t i = 0; i < config.maxConcurrentRequests; ++i) {
		assert(permits < capacity);
		permits++;
	}
	for (size_t i = 0; i < config.queueSize; ++i) {
		assert(permits > 0);
		permits--;
	}
	for (size_t i = 0; i < config.maxConcurrentRequests; ++i) {
		assert(permits > 0);
		permits--;
	}
	assert(permits == 0);

	config.queueSize = std::numeric_limits<size_t>::max();
	config.maxConcurrentRequests = 1;
	assert(!link_internal::linkWorkerSignalCapacity(config, capacity));
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

void testOperationErrorsTakePrecedence() {
	const LinkError transportError = {LinkErrorCode::ReceiveFailed, "transport failed"};
	const LinkError allocationError = {
	    LinkErrorCode::AllocationFailed,
	    "response body allocation failed"
	};
	const LinkError selected =
	    link_internal::linkPreserveOperationError(allocationError, transportError);
	assert(selected.code == LinkErrorCode::AllocationFailed);
	assert(std::strcmp(selected.message, "response body allocation failed") == 0);

	const LinkError noOperationError;
	assert(
	    link_internal::linkPreserveOperationError(noOperationError, transportError).code ==
	    LinkErrorCode::ReceiveFailed
	);
}

void testRedirectDecisions() {
	LinkConfig config;
	LinkHeaders headers;
	assert(headers.add("location", "https://example.com/final"));
	constexpr const char *currentUrl = "https://example.com/start";

	const int followedStatuses[] = {301, 302, 303, 307, 308};
	for (int status : followedStatuses) {
		const link_internal::LinkRedirectDecision decision = link_internal::linkEvaluateRedirect(
		    config,
		    LinkMethod::Get,
		    status,
		    headers,
		    0,
		    currentUrl
		);
		assert(decision.action == link_internal::LinkRedirectAction::Follow);
		assert(!decision.stripRequestHeaders);
		assert(std::strcmp(decision.location, "https://example.com/final") == 0);
	}

	const int finalStatuses[] = {200, 300, 304, 305, 306, 404};
	for (int status : finalStatuses) {
		const link_internal::LinkRedirectDecision decision = link_internal::linkEvaluateRedirect(
		    config,
		    LinkMethod::Get,
		    status,
		    headers,
		    0,
		    currentUrl
		);
		assert(decision.action == link_internal::LinkRedirectAction::None);
	}

	assert(
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Post, 302, headers, 0, currentUrl)
	        .action == link_internal::LinkRedirectAction::None
	);
	config.followRedirects = false;
	assert(
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, headers, 0, currentUrl)
	        .action == link_internal::LinkRedirectAction::None
	);

	config.followRedirects = true;
	LinkHeaders missingLocation;
	assert(
	    link_internal::linkEvaluateRedirect(
	        config,
	        LinkMethod::Get,
	        302,
	        missingLocation,
	        0,
	        currentUrl
	    )
	        .action == link_internal::LinkRedirectAction::None
	);
	LinkHeaders relativeLocation;
	assert(relativeLocation.add("Location", "/final"));
	assert(
	    link_internal::linkEvaluateRedirect(
	        config,
	        LinkMethod::Get,
	        302,
	        relativeLocation,
	        0,
	        currentUrl
	    )
	        .action == link_internal::LinkRedirectAction::None
	);

	config.maxRedirects = 3;
	const link_internal::LinkRedirectDecision limit =
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, headers, 3, currentUrl);
	assert(limit.action == link_internal::LinkRedirectAction::Error);
	assert(limit.error.code == LinkErrorCode::RedirectLimitReached);
	assert(
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, headers, 2, currentUrl)
	        .action == link_internal::LinkRedirectAction::Follow
	);

	config.maxUrlSize = 12;
	const link_internal::LinkRedirectDecision tooLarge =
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, headers, 0, currentUrl);
	assert(tooLarge.action == link_internal::LinkRedirectAction::Error);
	assert(tooLarge.error.code == LinkErrorCode::UrlTooLarge);

	config.maxUrlSize = 512;
	LinkHeaders crossOrigin;
	assert(crossOrigin.add("Location", "https://api.example.com/final"));
	const link_internal::LinkRedirectDecision rejectedCrossOrigin =
	    link_internal::linkEvaluateRedirect(
	        config,
	        LinkMethod::Get,
	        302,
	        crossOrigin,
	        0,
	        currentUrl
	    );
	assert(rejectedCrossOrigin.action == link_internal::LinkRedirectAction::Error);
	assert(rejectedCrossOrigin.error.code == LinkErrorCode::RedirectRejected);

	config.allowCrossOriginRedirects = true;
	const link_internal::LinkRedirectDecision allowedCrossOrigin =
	    link_internal::linkEvaluateRedirect(
	        config,
	        LinkMethod::Get,
	        302,
	        crossOrigin,
	        0,
	        currentUrl
	    );
	assert(allowedCrossOrigin.action == link_internal::LinkRedirectAction::Follow);
	assert(allowedCrossOrigin.stripRequestHeaders);

	LinkHeaders downgrade;
	assert(downgrade.add("Location", "http://example.com/final"));
	const link_internal::LinkRedirectDecision rejectedDowngrade =
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, downgrade, 0, currentUrl);
	assert(rejectedDowngrade.action == link_internal::LinkRedirectAction::Error);
	assert(rejectedDowngrade.error.code == LinkErrorCode::RedirectRejected);

	config.allowHttpsToHttpRedirects = true;
	const link_internal::LinkRedirectDecision allowedDowngrade =
	    link_internal::linkEvaluateRedirect(config, LinkMethod::Get, 302, downgrade, 0, currentUrl);
	assert(allowedDowngrade.action == link_internal::LinkRedirectAction::Follow);
	assert(allowedDowngrade.stripRequestHeaders);

	LinkHeaders explicitDefaultPort;
	assert(explicitDefaultPort.add("Location", "https://EXAMPLE.com:443/final"));
	const link_internal::LinkRedirectDecision sameDefaultPort = link_internal::linkEvaluateRedirect(
	    config,
	    LinkMethod::Get,
	    302,
	    explicitDefaultPort,
	    0,
	    currentUrl
	);
	assert(sameDefaultPort.action == link_internal::LinkRedirectAction::Follow);
	assert(!sameDefaultPort.stripRequestHeaders);

	LinkHeaders differentPort;
	assert(differentPort.add("Location", "https://example.com:444/final"));
	const link_internal::LinkRedirectDecision changedPort = link_internal::linkEvaluateRedirect(
	    config,
	    LinkMethod::Get,
	    302,
	    differentPort,
	    0,
	    currentUrl
	);
	assert(changedPort.action == link_internal::LinkRedirectAction::Follow);
	assert(changedPort.stripRequestHeaders);

	LinkHeaders invalidPort;
	assert(invalidPort.add("Location", "https://example.com:999999999999999999999/final"));
	const link_internal::LinkRedirectDecision rejectedInvalidPort =
	    link_internal::linkEvaluateRedirect(
	        config,
	        LinkMethod::Get,
	        302,
	        invalidPort,
	        0,
	        currentUrl
	    );
	assert(rejectedInvalidPort.action == link_internal::LinkRedirectAction::Error);
	assert(rejectedInvalidPort.error.code == LinkErrorCode::RedirectRejected);
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
	testWorkerSignalCapacity();
	testOwnedBufferAppend();
	testOperationErrorsTakePrecedence();
	testRedirectDecisions();
	testCallbacks();
	testLifecycleAndQueueLimits();
	testInvalidQueueConcurrencyConfig();
	testRequestValidation();
	return 0;
}
