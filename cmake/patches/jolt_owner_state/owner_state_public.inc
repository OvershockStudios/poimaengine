	/// Poima extension: owner-thread/quiescent rigid-world metadata omitted by SaveState.
	/// No Update, body creation/destruction, queries or authoring may run concurrently.
	/// Capture leaves output unchanged on validation failure. All bounded storage
	/// is prepared before locks. Allocation failure follows upstream Jolt policy;
	/// production disables C++ exceptions: no recoverable-OOM guarantee is made.
	bool CapturePoimaOwnerState(PoimaBodyOwnerState &outState) const;
	/// Apply AFTER Global|Bodies|Contacts restore and structural reconstruction.
	/// Requires the same active set, restores its exact order and invalidation flags/list.
	/// Entire metadata is validated before mutation; no callbacks, velocity/sleep resets.
	bool RestorePoimaOwnerState(const PoimaBodyOwnerState &inState);

