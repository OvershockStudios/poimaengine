	/// Poima owner-thread/quiescent metadata; separate from original SaveState bytes.
	bool CapturePoimaOwnerState(PoimaBodyOwnerState &outState) const { return mBodyManager.CapturePoimaOwnerState(outState); }
	bool RestorePoimaOwnerState(const PoimaBodyOwnerState &inState) { return mBodyManager.RestorePoimaOwnerState(inState); }

