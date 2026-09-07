#include <Link.h>

#include <cassert>
#include <cstdint>

int main() {
	LinkConfig defaults;
	assert(defaults.memory.allocation == Strata::Placement::PreferExternal);
	assert(defaults.memory.taskStack == Strata::Placement::PreferExternal);

	Link internalLink;
	LinkConfig internal;
	internal.queueSize = 1;
	internal.maxConcurrentRequests = 1;
	internal.memory.allocation = Strata::Placement::Internal;
	internal.memory.taskStack = Strata::Placement::Internal;
	assert(internalLink.init(internal));
	const LinkDiagnostics internalDiag = internalLink.diagnostics();
	assert(internalDiag.allocationPlacement == Strata::Placement::Internal);
	assert(internalDiag.workerStackPlacement == Strata::Placement::Internal);
	assert(internalLink.deinit());

	Link strictLink;
	LinkConfig strict;
	strict.queueSize = 1;
	strict.maxConcurrentRequests = 1;
	strict.memory.allocation = Strata::Placement::RequireExternal;
	strict.memory.taskStack = Strata::Placement::RequireExternal;
	assert(strictLink.init(strict).code == LinkErrorCode::AllocationFailed);

	Link invalidLink;
	LinkConfig invalid;
	invalid.queueSize = 1;
	invalid.maxConcurrentRequests = 1;
	invalid.memory.allocation = static_cast<Strata::Placement>(UINT8_MAX);
	assert(invalidLink.init(invalid).code == LinkErrorCode::InvalidConfig);

	return 0;
}
