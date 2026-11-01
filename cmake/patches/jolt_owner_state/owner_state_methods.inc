// Poima extension, SPDX-License-Identifier: Apache-2.0
// These operations require an external owner/quiescence barrier. The locks
// preserve Jolt lock ranking, not permission to race PhysicsSystem::Update.
bool BodyManager::ValidatePoimaOwnerStateLocked(const PoimaBodyOwnerState &inState,
	Array<uint8> &active_seen, Array<uint8> &flagged_seen, Array<uint8> &queued_live_seen) const
{
	if (inState.mActiveRigidOrder.size() > GetMaxBodies()
		|| inState.mInvalidatedBodies.size() > GetMaxBodies()
		|| inState.mInvalidationQueue.size() > PoimaBodyOwnerState::cMaxInvalidationQueue
		|| mNumActiveBodies[int(EBodyType::SoftBody)].load(memory_order_relaxed) != 0)
		return false;
	for (const Body *body : mBodies)
		if (sIsValidBodyPointer(body) && !body->IsRigidBody())
			return false;
	const uint32 count = mNumActiveBodies[int(EBodyType::RigidBody)].load(memory_order_relaxed);
	if (inState.mActiveRigidOrder.size() != count)
		return false;
	if (active_seen.size() != GetMaxBodies() || flagged_seen.size() != GetMaxBodies()
		|| queued_live_seen.size() != GetMaxBodies())
		return false;
	for (uint8 &value : active_seen) value = 0;
	for (uint8 &value : flagged_seen) value = 0;
	for (uint8 &value : queued_live_seen) value = 0;
	for (const BodyID id : inState.mActiveRigidOrder)
	{
		const Body *body = TryGetBody(id);
		if (body == nullptr || !body->IsRigidBody() || body->IsStatic()
			|| !body->IsInBroadPhase() || !body->IsActive() || active_seen[id.GetIndex()] != 0)
			return false;
		const uint32 index = body->GetIndexInActiveBodiesInternal();
		if (index >= count || mActiveBodies[int(EBodyType::RigidBody)][index] != id)
			return false;
		active_seen[id.GetIndex()] = 1;
	}
	uint32 previous_slot = 0;
	bool first = true;
	for (const BodyID id : inState.mInvalidatedBodies)
	{
		const Body *body = TryGetBody(id);
		if (body == nullptr || (!first && id.GetIndex() <= previous_slot))
			return false;
		first = false;
		previous_slot = id.GetIndex();
		flagged_seen[id.GetIndex()] = 1;
	}
	for (const BodyID id : inState.mInvalidationQueue)
	{
		const uint32 raw = id.GetIndexAndSequenceNumber();
		if (id.IsInvalid() || (raw & BodyID::cBroadPhaseBit) != 0 || id.GetIndex() >= GetMaxBodies())
			return false;
		// Retired full IDs are retained, never resolved by slot alone. Duplicate
		// retired entries are harmless; duplicate LIVE entries would clear a flag twice.
		if (TryGetBody(id) != nullptr)
		{
			if (flagged_seen[id.GetIndex()] == 0 || queued_live_seen[id.GetIndex()] != 0)
				return false;
			queued_live_seen[id.GetIndex()] = 1;
		}
	}
	for (const BodyID id : inState.mInvalidatedBodies)
		if (queued_live_seen[id.GetIndex()] == 0)
			return false;
	return true;
}

bool BodyManager::CapturePoimaOwnerState(PoimaBodyOwnerState &outState) const
{
	// All capacity and validation scratch is allocated BEFORE taking any lock.
	// Jolt's production profile disables exceptions; upstream allocation failure
	// is not advertised as recoverable. No allocating operation follows LockAllBodies.
	PoimaBodyOwnerState candidate;
	candidate.mActiveRigidOrder.reserve(GetMaxBodies());
	candidate.mInvalidatedBodies.reserve(GetMaxBodies());
	candidate.mInvalidationQueue.reserve(PoimaBodyOwnerState::cMaxInvalidationQueue);
	Array<uint8> active_seen(GetMaxBodies(), uint8(0));
	Array<uint8> flagged_seen(GetMaxBodies(), uint8(0));
	Array<uint8> queued_live_seen(GetMaxBodies(), uint8(0));
	LockAllBodies(); // PerBody then BodiesList, released after later-ranked locks.
	JPH_SCOPE_EXIT([this] { UnlockAllBodies(); });
	UniqueLock active_lock(mActiveBodiesMutex JPH_IF_ENABLE_ASSERTS(, this, EPhysicsLockTypes::ActiveBodiesList));
	lock_guard invalid_lock(mBodiesCacheInvalidMutex);
	if (mBodiesCacheInvalid.size() > PoimaBodyOwnerState::cMaxInvalidationQueue)
		return false;
	const uint32 count = mNumActiveBodies[int(EBodyType::RigidBody)].load(memory_order_relaxed);
	if (count > GetMaxBodies())
		return false;
	candidate.mActiveRigidOrder.assign(mActiveBodies[int(EBodyType::RigidBody)], mActiveBodies[int(EBodyType::RigidBody)] + count);
	candidate.mInvalidationQueue.assign(mBodiesCacheInvalid.begin(), mBodiesCacheInvalid.end());
	for (const Body *body : mBodies)
		if (sIsValidBodyPointer(body) && body->IsCollisionCacheInvalid())
			candidate.mInvalidatedBodies.push_back(body->GetID());
	if (!ValidatePoimaOwnerStateLocked(candidate, active_seen, flagged_seen, queued_live_seen))
		return false;
	outState.mActiveRigidOrder.swap(candidate.mActiveRigidOrder);
	outState.mInvalidationQueue.swap(candidate.mInvalidationQueue);
	outState.mInvalidatedBodies.swap(candidate.mInvalidatedBodies);
	return true;
}

bool BodyManager::RestorePoimaOwnerState(const PoimaBodyOwnerState &inState)
{
	// All input ownership/allocation precedes native mutation. The copied queue
	// is swapped into place only after full ID/membership/flag consistency checks.
	if (inState.mInvalidationQueue.size() > PoimaBodyOwnerState::cMaxInvalidationQueue
		|| inState.mActiveRigidOrder.size() > GetMaxBodies() || inState.mInvalidatedBodies.size() > GetMaxBodies())
		return false;
	BodyIDVector prepared_queue = inState.mInvalidationQueue;
	Array<uint8> active_seen(GetMaxBodies(), uint8(0));
	Array<uint8> flagged_seen(GetMaxBodies(), uint8(0));
	Array<uint8> queued_live_seen(GetMaxBodies(), uint8(0));
	LockAllBodies();
	JPH_SCOPE_EXIT([this] { UnlockAllBodies(); });
	UniqueLock active_lock(mActiveBodiesMutex JPH_IF_ENABLE_ASSERTS(, this, EPhysicsLockTypes::ActiveBodiesList));
	lock_guard invalid_lock(mBodiesCacheInvalidMutex);
	if (!ValidatePoimaOwnerStateLocked(inState, active_seen, flagged_seen, queued_live_seen))
		return false;
	// Replace the flag/list pair together. Calling ValidateContactCacheInternal
	// alone is unsafe when an old queue contains a now-recreated exact BodyID.
	for (Body *body : mBodies)
		if (sIsValidBodyPointer(body))
			body->mFlags.fetch_and(uint8(~uint8(Body::EFlags::InvalidateContactCache)), memory_order_relaxed);
	for (const BodyID id : inState.mInvalidatedBodies)
		TryGetBody(id)->mFlags.fetch_or(uint8(Body::EFlags::InvalidateContactCache), memory_order_relaxed);
	mBodiesCacheInvalid.swap(prepared_queue);
	uint32 index = 0, ccd_count = 0;
	for (const BodyID id : inState.mActiveRigidOrder)
	{
		Body *body = TryGetBody(id);
		mActiveBodies[int(EBodyType::RigidBody)][index] = id;
		body->mMotionProperties->mIndexInActiveBodies = index++;
		if (body->mMotionProperties->GetMotionQuality() == EMotionQuality::LinearCast)
			++ccd_count;
	}
	mNumActiveCCDBodies = ccd_count;
	mNumActiveBodies[int(EBodyType::RigidBody)].store(index, memory_order_release);
	return true;
}

