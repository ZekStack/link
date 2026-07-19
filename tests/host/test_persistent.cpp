#include <Link.h>

#include <cassert>

namespace {

void testDefaultConnectionMode() {
	LinkConfig config;
	assert(config.connectionMode == LinkConnectionMode::PerRequest);
	assert(config.persistentIdleTimeoutMs == 5U * 60U * 1000U);
	assert(config.persistentMaxRequestsPerHandle == 0);
}

void testPersistentReuseDecisions() {
	using Decision = link_internal::LinkPersistentReuseDecision;

	assert(
	    link_internal::linkEvaluatePersistentReuse(false, false, false, 0, 0, 0, 0, 0) ==
	    Decision::Create
	);
	assert(
	    link_internal::linkEvaluatePersistentReuse(true, false, true, 100, 50, 1000, 1, 0) ==
	    Decision::Reuse
	);
	assert(
	    link_internal::linkEvaluatePersistentReuse(true, true, true, 100, 50, 1000, 1, 0) ==
	    Decision::Poisoned
	);
	assert(
	    link_internal::linkEvaluatePersistentReuse(true, false, false, 100, 50, 1000, 1, 0) ==
	    Decision::OriginChanged
	);
	assert(
	    link_internal::linkEvaluatePersistentReuse(true, false, true, 1050, 50, 1000, 1, 0) ==
	    Decision::IdleExpired
	);
	assert(
	    link_internal::linkEvaluatePersistentReuse(true, false, true, 100, 50, 0, 5, 5) ==
	    Decision::RequestLimitReached
	);
	assert(
	    link_internal::linkEvaluatePersistentReuse(
	        true,
	        false,
	        true,
	        25,
	        UINT32_MAX - 25,
	        50,
	        0,
	        0
	    ) == Decision::IdleExpired
	);
}

void testPersistentOriginMatching() {
	const link_internal::LinkUrlOrigin first =
	    link_internal::linkParseOrigin("https://EXAMPLE.com:443/a");
	const link_internal::LinkUrlOrigin second =
	    link_internal::linkParseOrigin("https://example.com/b");
	const link_internal::LinkUrlOrigin changedPort =
	    link_internal::linkParseOrigin("https://example.com:444/b");
	const link_internal::LinkUrlOrigin changedScheme =
	    link_internal::linkParseOrigin("http://example.com/b");
	const link_internal::LinkUrlOrigin ipv6Default =
	    link_internal::linkParseOrigin("https://[2001:db8::1]/a");
	const link_internal::LinkUrlOrigin ipv6Explicit =
	    link_internal::linkParseOrigin("https://[2001:DB8::1]:443/b");
	const link_internal::LinkUrlOrigin invalid =
	    link_internal::linkParseOrigin("https://example.com:70000/a");

	assert(link_internal::linkSameOrigin(first, second));
	assert(!link_internal::linkSameOrigin(first, changedPort));
	assert(!link_internal::linkSameOrigin(first, changedScheme));
	assert(link_internal::linkSameOrigin(ipv6Default, ipv6Explicit));
	assert(!invalid.valid);
}

} // namespace

int main() {
	testDefaultConnectionMode();
	testPersistentReuseDecisions();
	testPersistentOriginMatching();
	return 0;
}
