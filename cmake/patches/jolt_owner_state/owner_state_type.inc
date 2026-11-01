// Poima extension, SPDX-License-Identifier: Apache-2.0
// Separate from Jolt SaveState: no existing recorder bytes or object layout change.
struct PoimaBodyOwnerState
{
	static constexpr uint32 cMaxInvalidationQueue = 65536;
	BodyIDVector mActiveRigidOrder;
	BodyIDVector mInvalidationQueue; // Exact native order, including retired full IDs.
	BodyIDVector mInvalidatedBodies; // Live flags, in body-slot order; includes off-broadphase bodies.
};

