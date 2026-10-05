# Runtime saves in the editor

Open **Window > Saves** or **File > Runtime saves** to save and restore the supported runtime state. This window uses the same native `save.*` service as CLI and agent clients. Authored scene edits are already persisted separately; a runtime checkpoint preserves the simulation rather than replacing the scene file.

## Save a checkpoint

1. Choose an existing storage directory, then select **Use folder**. Keep it outside the cooked asset directory; packaged hosts also protect the entire game bundle. Configuration lasts for this editor session.
2. Start Play, then pause at the state to preserve.
3. Enter a slot name and inspect it with the refresh icon beside the field. Names contain lowercase letters, digits, underscores or hyphens. Inspection captures both the current slot generation and runtime state.
4. Select **Save checkpoint**. The result reports its committed generation and byte count. Inspect again before the next operation to deliberately observe its new state.

The **Details** section shows observed generations, session IDs and trusted gameplay information.

The window does not silently refresh a stale observation just before saving or loading. If an agent advances the runtime, replaces a save or changes the storage root, the native guards reject the outdated operation. Drafts and the observed state remain available so the conflict can be understood.

## Load a checkpoint

Pause the running simulation or stop it, inspect the intended slot, then load. Apply or explicitly revert Inspector and C# Gameplay drafts first. Loading stages a separate runtime; success opens it paused under a new session ID and clears old gameplay input. Failure keeps the running world intact. Current authored objects, revision and history remain unchanged, even when the checkpoint contains an older scene definition.

An active runtime supplies its trusted gameplay module. When stopped, choose the observed C# launch configuration or no gameplay. A save made with C# requires its original backend, assembly image, type and schema. Configure the matching prebuilt assembly in **Window > C# Gameplay** before a stopped load. An old save does not automatically migrate after a code rebuild, and saved data never chooses executable paths. A save without gameplay can be loaded after stopping and choosing no gameplay.

The game assembly's ordinary constructors/static initialization may run while staging, but gameplay `Initialize` is skipped. The saved typed values resume at their recorded tick. The [snapshot contract](RUNTIME.md#portable-runtime-snapshot-foundation) lists supported state and physics/audio reconstruction limits.

## Failed operations and recovery

A failed mutation retains its original request and ID. Retry uses that same operation; dismiss it explicitly before issuing a different one. This handles a lost response after a write has already committed without writing another generation. Refreshing the window does not discard a pending request. A write retry can refresh the session's routing counter only when it still points at the same storage root.

If the newest payload is damaged, inspection identifies the verified prior generation. Loading that generation requires explicit recovery permission. Writing afterward requires a separate acknowledgement and preserves the failed generation. A damaged manifest permits read recovery only; repeated corruption while a failed generation is already preserved also blocks recovery writes. The window shows those conditions rather than deleting failed data. See [publication and recovery](RUNTIME.md#publication-and-recovery).

Changing the selected root/slot or inspecting a new observation clears recovery choices. The window and editor preserve unapplied root drafts and pending requests on ordinary close.

## Current limits

The window operates on named slots; it does not yet provide a slot browser, thumbnails, autosave scheduling, persistent root preferences or cloud/platform storage. Saves and loads are synchronous and require paused editor playback. The current format binds a fixed supported entity set and external assets, with a 64 MiB total bound. It is not an arbitrary-object serializer or a guarantee of identical future physics trajectories. [Typed gameplay requests](GAMEPLAY_SAVES.md) can also save after committed ticks; gameplay loads pause the editor and clear old input.

Qualification uses actual editor textboxes and routed button events, fresh editor processes, real C# state and deliberately damaged test-owned files. Visual-content captures render the attached Avalonia window; they do not qualify OS chrome, a desktop compositor, physical input or general accessibility. [Recorded evidence](evidence/m2-editor-saves.json).
