#pragma once

#include <limits>

#if defined(ESP32)
#include <errno.h>
#include <esp_err.h>
#include <esp_http_client.h>
#if __has_include(<esp_idf_version.h>)
#include <esp_idf_version.h>
#endif
#if __has_include(<esp_crt_bundle.h>)
#include <esp_crt_bundle.h>
#define LINK_HAS_CRT_BUNDLE 1
#else
#define LINK_HAS_CRT_BUNDLE 0
#endif
#endif

namespace link_internal {

enum class LinkRedirectAction : uint8_t { None, Follow, Error };

enum class LinkPersistentReuseDecision : uint8_t {
	Create,
	Reuse,
	OriginChanged,
	IdleExpired,
	RequestLimitReached,
	Poisoned
};

inline bool
linkHttpRequestStateMutationSucceeded(int result, int successResult, int alreadyClearResult) {
	return result == successResult || result == alreadyClearResult;
}

inline bool linkShouldScrubHttpClientRequest(bool persistent) {
	return persistent;
}

struct LinkRedirectDecision {
	LinkRedirectAction action = LinkRedirectAction::None;
	const char *location = nullptr;
	bool stripRequestHeaders = false;
	LinkError error;
};

struct LinkUrlOrigin {
	const char *host = nullptr;
	size_t hostSize = 0;
	uint16_t port = 0;
	bool https = false;
	bool valid = false;
};

inline char linkLowerAscii(char value) {
	return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

inline bool linkAsciiEqual(const char *left, size_t leftSize, const char *right, size_t rightSize) {
	if (left == nullptr || right == nullptr || leftSize != rightSize)
		return false;
	for (size_t i = 0; i < leftSize; ++i) {
		if (linkLowerAscii(left[i]) != linkLowerAscii(right[i]))
			return false;
	}
	return true;
}

inline LinkUrlOrigin linkParseOrigin(const char *url) {
	LinkUrlOrigin origin;
	if (url == nullptr)
		return origin;

	const char *authority = nullptr;
	if (std::strncmp(url, "https://", 8) == 0) {
		origin.https = true;
		origin.port = 443;
		authority = url + 8;
	} else if (std::strncmp(url, "http://", 7) == 0) {
		origin.port = 80;
		authority = url + 7;
	} else {
		return origin;
	}

	const char *authorityEnd = authority;
	while (*authorityEnd != '\0' && *authorityEnd != '/' && *authorityEnd != '?' &&
	       *authorityEnd != '#') {
		if (*authorityEnd == '@')
			return origin;
		authorityEnd++;
	}
	if (authority == authorityEnd)
		return origin;

	const char *hostBegin = authority;
	const char *hostEnd = authorityEnd;
	const char *portBegin = nullptr;
	if (*hostBegin == '[') {
		hostBegin++;
		hostEnd = hostBegin;
		while (hostEnd < authorityEnd && *hostEnd != ']')
			hostEnd++;
		if (hostEnd == authorityEnd || hostEnd == hostBegin)
			return origin;
		const char *afterBracket = hostEnd + 1;
		if (afterBracket < authorityEnd) {
			if (*afterBracket != ':')
				return origin;
			portBegin = afterBracket + 1;
		}
	} else {
		for (const char *cursor = authority; cursor < authorityEnd; ++cursor) {
			if (*cursor == ':') {
				if (portBegin != nullptr)
					return origin;
				hostEnd = cursor;
				portBegin = cursor + 1;
			}
		}
	}
	if (hostEnd == hostBegin)
		return origin;

	if (portBegin != nullptr) {
		if (portBegin == authorityEnd)
			return origin;
		uint32_t port = 0;
		for (const char *cursor = portBegin; cursor < authorityEnd; ++cursor) {
			if (*cursor < '0' || *cursor > '9')
				return origin;
			const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
			if (port > 6553U || (port == 6553U && digit > 5U))
				return origin;
			port = (port * 10U) + digit;
		}
		if (port == 0)
			return origin;
		origin.port = static_cast<uint16_t>(port);
	}

	origin.host = hostBegin;
	origin.hostSize = static_cast<size_t>(hostEnd - hostBegin);
	origin.valid = true;
	return origin;
}

inline bool linkSameOrigin(const LinkUrlOrigin &left, const LinkUrlOrigin &right) {
	return left.valid && right.valid && left.https == right.https && left.port == right.port &&
	       linkAsciiEqual(left.host, left.hostSize, right.host, right.hostSize);
}

inline LinkPersistentReuseDecision linkEvaluatePersistentReuse(
    bool hasClient,
    bool poisoned,
    bool sameOrigin,
    uint32_t nowMs,
    uint32_t lastUsedAtMs,
    uint32_t idleTimeoutMs,
    uint32_t requestCount,
    uint32_t maxRequestsPerHandle
) {
	if (!hasClient)
		return LinkPersistentReuseDecision::Create;
	if (poisoned)
		return LinkPersistentReuseDecision::Poisoned;
	if (!sameOrigin)
		return LinkPersistentReuseDecision::OriginChanged;
	if (idleTimeoutMs != 0 && static_cast<uint32_t>(nowMs - lastUsedAtMs) >= idleTimeoutMs)
		return LinkPersistentReuseDecision::IdleExpired;
	if (maxRequestsPerHandle != 0 && requestCount >= maxRequestsPerHandle)
		return LinkPersistentReuseDecision::RequestLimitReached;
	return LinkPersistentReuseDecision::Reuse;
}

inline LinkError
linkPreserveOperationError(const LinkError &operationError, const LinkError &transportError) {
	return operationError.code == LinkErrorCode::Ok ? transportError : operationError;
}

inline bool linkIsRedirectStatus(int status) {
	return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

inline LinkRedirectDecision linkEvaluateRedirect(
    const LinkConfig &config,
    LinkMethod method,
    int status,
    const LinkHeaders &headers,
    uint8_t redirectCount,
    const char *currentUrl
) {
	LinkRedirectDecision decision;
	if (!config.followRedirects || method != LinkMethod::Get || !linkIsRedirectStatus(status)) {
		return decision;
	}

	const char *location = headers.get("Location");
	if (location == nullptr || !linkUrlLooksValid(location)) {
		return decision;
	}

	decision.location = location;
	if (redirectCount >= config.maxRedirects) {
		decision.action = LinkRedirectAction::Error;
		decision.error = {LinkErrorCode::RedirectLimitReached, "redirect limit reached"};
		return decision;
	}

	if (std::strlen(location) > config.maxUrlSize) {
		decision.action = LinkRedirectAction::Error;
		decision.error = {LinkErrorCode::UrlTooLarge, "redirect url is too large"};
		return decision;
	}

	const LinkUrlOrigin currentOrigin = linkParseOrigin(currentUrl);
	const LinkUrlOrigin redirectOrigin = linkParseOrigin(location);
	if (!currentOrigin.valid || !redirectOrigin.valid) {
		decision.action = LinkRedirectAction::Error;
		decision.error = {LinkErrorCode::RedirectRejected, "redirect origin is invalid"};
		return decision;
	}
	if (currentOrigin.https && !redirectOrigin.https && !config.allowHttpsToHttpRedirects) {
		decision.action = LinkRedirectAction::Error;
		decision.error = {LinkErrorCode::RedirectRejected, "https to http redirect rejected"};
		return decision;
	}
	const bool sameOrigin = linkSameOrigin(currentOrigin, redirectOrigin);
	if (!sameOrigin && !config.allowCrossOriginRedirects) {
		decision.action = LinkRedirectAction::Error;
		decision.error = {LinkErrorCode::RedirectRejected, "cross-origin redirect rejected"};
		return decision;
	}

	decision.action = LinkRedirectAction::Follow;
	decision.stripRequestHeaders = !sameOrigin;
	return decision;
}

inline bool linkWorkerSignalCapacity(const LinkConfig &config, UBaseType_t &out) {
	constexpr UBaseType_t maximum = std::numeric_limits<UBaseType_t>::max();
	if (config.queueSize > maximum || config.maxConcurrentRequests > maximum) {
		return false;
	}
	const UBaseType_t queueSize = static_cast<UBaseType_t>(config.queueSize);
	const UBaseType_t workers = static_cast<UBaseType_t>(config.maxConcurrentRequests);
	if (queueSize > maximum - workers) {
		return false;
	}
	out = queueSize + workers;
	return true;
}

} // namespace link_internal
