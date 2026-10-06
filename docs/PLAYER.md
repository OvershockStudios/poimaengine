# Continuous native player

Poima keeps a Vulkan window open while its native runtime advances. `runtime.play` connects SDL keyboard/mouse input or a deterministic input sequence to the same 60 Hz simulation used by `runtime.step`. It keeps one renderer/context alive and updates presentation snapshots each frame. The [Collection Room sample](../examples/collection-game) combines this player with compiled C# interaction, native UI and durable saves. Broader production qualification remains incomplete.

## Run the example on Windows

Build `windows-runtime` as described in [BUILD.md](BUILD.md). From a PowerShell prompt at `D:\poimaengine`, run:

```powershell
Get-Content examples/physics-room.jsonl | .\build\windows-runtime\poima.exe world build/physics.world.json
Get-Content examples/play-room.jsonl | .\build\windows-runtime\poima.exe world build/physics.world.json
```

Use a new world path for the first command, or the existing unmodified revision-1 physics room. The second command starts its runtime, opens the player's attached camera, and closes the service after you exit the window. Play never changes authored spawn positions. On a foreground desktop, controls are:

| Input | Action |
| --- | --- |
| W / A / S / D | Move forward / left / backward / right relative to character yaw. |
| Mouse | Look; initially 0.1 degrees per relative mouse unit. |
| Space | Jump on a new press, while supported by the ground. |
| E | Use on a new press; consumed by a loaded [C# gameplay module](MANAGED_GAMEPLAY.md). |
| Gamepad left / right stick | Move / look with configurable radial deadzones and response. |
| Gamepad south / west face button | Jump / use (typically A/X on Xbox, Cross/Square on PlayStation controllers). |
| Gamepad Start | Pause or resume while the window is focused. |
| Tab | While captured, release the mouse and pause. With native UI focus available, move to the next control; Shift+Tab moves back. |
| Left click inside the focused window | Activate a native UI control, or capture the mouse and resume when clicking outside consumed UI in a controller scene. |
| Escape or window close | Return to the world service with live runtime state retained. |

Focus loss and minimization clear held movement, queued jump/use and pending look. Simulation pauses rather than catching up elapsed inactive time. Click or press the assigned gamepad's Start button to resume after returning. Start can resume without capturing the mouse; a later click captures it. [Saved input profiles](INPUT_PROFILES.md) configure keyboard/mouse alternatives, sensitivity and inversion through the same native evaluator used by the player. Version 0.0.20 adds [gamepads](GAMEPADS.md), including sticks, trigger bindings and device discovery. Native [game UI controls](GAME_UI.md) route pointer, keyboard and assigned-gamepad input through the displayed layout. General action contexts, broader accessibility settings and a general UI authoring toolkit remain unfinished.

Without a saved profile, interactive play uses the combined keyboard/mouse/gamepad defaults and assigns a controller only when exactly one is connected. An existing `poima.input.v1` profile retains keyboard/mouse-only behavior. Optional `gamepad: {"mode":"explicit","id":123}` selects a session device ID from `input.devices`; `only_connected` and `disabled` are the other modes. Multiple devices are never resolved by choosing an arbitrary first controller. Reports include assignment, connection and neutral-gate status. Return sticks/triggers to rest and release buttons after attachment or resume to arm gameplay input. See the gamepad contract for precise limits and v2 profile conversion.

## Agent operation

Call `runtime.start` first. Discover this method and its schemas through `world.describe` (schema revision 19). Interactive play requires:

```json
{"jsonrpc":"2.0","id":2,"method":"runtime.play","params":{"session_id":"00000000000000000000000000000384","request_id":"000000000000000000000000000007d0","expected_tick":0,"controller":"00000000000000000000000000000064","camera":"00000000000000000000000000000065","mode":"interactive"}}
```

`camera` identifies a runtime camera. Interactive mode may omit `controller` for menu-only scenes; when supplied, it must identify a runtime CharacterController. Replay mode requires both `controller` and `sequence`. Menu-only scenes do not fabricate a movement controller or capture the mouse for gameplay. Optional `max_frames` limits interactive presentations to 1–36,000; omitted/zero runs until exit. Optional `path`, `width`, `height`, `gpu` and `samples` use the [capture bounds](SCENE_CAPTURE.md). Optional `culling` (default true) and `profile` (default false) control [frustum rejection and CPU/GPU diagnostics](RENDER_DIAGNOSTICS.md); results include `render_diagnostics`. `path` must be a new file in an existing directory, outside the world's reserved paths. If supplied, an extra presentation/readback captures the exact final runtime state without advancing another tick.

Replay replaces window input with explicit segments:

```json
{"jsonrpc":"2.0","id":2,"method":"runtime.play","params":{"session_id":"00000000000000000000000000000384","request_id":"000000000000000000000000000007d1","expected_tick":0,"controller":"00000000000000000000000000000064","camera":"00000000000000000000000000000065","mode":"replay","sequence":[{"ticks":120},{"ticks":180,"move":[0,1]},{"ticks":10,"jump":true},{"ticks":1,"look":[45,20]}],"path":"build/replay-final.bmp"}}
```

Segments can also start [timed kinematic targets](PHYSICS_INTERACTIONS.md) through `motions`; targets begin once on the segment's first tick and persist into later segments.

A replay has 1–256 segments, each 1–600 ticks, and at most 36,000 total ticks. Movement is held throughout a segment; look/jump/use occur on its first tick. Omitted controls are neutral. Each replay tick gets a render attempt, independent of wall time or focus. Minimized surfaces wait for restoration. A replay can still be interrupted by Escape or window close. `max_frames` is interactive-only; `sequence` is replay-only. Agents can use the headless `runtime.step` path for faster simulation checks and this path when actual continuous presentation matters.

The operation requires the exact current `expected_tick` and a caller-generated `request_id`. Play and step share a 32-receipt window. Repeating identical parameters returns the original receipt with `replayed: true`, without another window or simulation. Reusing an ID across methods or with changed parameters fails. Validation and missing-build failures occur before advancing state. Receipts remain process-local.

The result identifies the source world/revision, initial/final ticks, camera/world matrix, stop reason, frame count, GPU, final dimensions, rebuild count, dropped wall time and capture status. `success` indicates execution without an engine error; `stop_reason: "replay_complete"` identifies a fully consumed replay. A normal user exit can occur before that. A device/window failure returns `success: false`, `stop_reason: "error"`, a diagnostic and the actual current tick. Successful earlier ticks remain live; the whole play session is not an atomic transaction. That failed receipt is also cached. Inspect state before resuming with a fresh request ID. No renderer recreation or replay rollback is silently represented as success.

`runtime.play` currently blocks other requests on this world-service connection until it returns. It is an initial player operation, not the planned independent player process plus concurrently editable viewport. The authored/runtime separation remains intact. This limitation must be removed for the full iterative editor/agent workflow.

## Timing and presentation

Human play accumulates wall time into fixed 1/60-second updates. Look, jump and use edges wait for a simulation tick, so a fast render frame cannot lose or repeat them. A slow frame catches up at most eight ticks; extra wall time is discarded and reported in `dropped_wall_seconds`. The timestep never expands to compensate. Inactive time is excluded, and inactive drawing is throttled. Render interpolation remains unimplemented.

[Runtime rigs](RUNTIME_ANIMATION.md) advance on these same simulation ticks and publish live joint palettes to the compute skinning path. Initial clip settings come from the authored `AnimationRig`; `runtime.step.animations` can change playback before opening the player. Interactive/replay play currently accepts no separate animation cue track, and the blocking service prevents concurrent live commands. A loaded C# gameplay module can query and queue animation changes on simulation ticks through `GetAnimation` and `SetAnimation`. A cinematic sequencer remains future work.

The window is resizable. Swapchain images, MSAA/depth attachments, semaphores and readback storage are recreated after GPU completion; shared geometry and the pipeline remain alive when the format is unchanged. A zero-sized Vulkan surface during minimize/restore suspends presentation. This explicitly handles the race where the surface becomes empty before SDL's queued minimize notification arrives. Changed surface formats and actual device loss still end the player with a diagnostic.

Rendering remains serialized, with authored lighting, optional [shadow maps](SHADOWS.md) and 1×/4× MSAA. Poima 0.0.9 also draws [imported static geometry and textured PBR materials](ASSETS.md). It waits for the GPU after each frame; this is not a shipping frame-pacing design or a game-performance result. Poima 0.0.14 also runs a loaded [C# gameplay module](MANAGED_GAMEPLAY.md) during play/replay, including use-driven door interactions. Broader glTF support, animation tooling, environmental audio, general UI authoring and the complete gameplay SDK remain unfinished. Native game bundles have separate [packaging qualification](PROJECTS.md).

## Evidence and limits

[Recorded evidence](evidence/m2-player.json) includes the [final replay view](evidence/m2-player-replay.png) and [resized window capture](evidence/m2-player-resized.png). It covers:

- Platform-independent input tests on native Linux and Windows: sub-tick edges, held/opposed controls, repeat suppression, focus reset, bounded look, 30/144 Hz presentation schedules and stall handling.
- A 371-tick continuous replay on NVIDIA and AMD Windows GPUs. Inspected character, camera and crate states equal independent headless stepping; final image pixels equal a separate runtime capture.
- Receipt retries, cross-method ID conflicts, stale ticks, invalid sequences, failed GPU selection, recovery and unchanged authored spawn data.
- A native Windows window resized, minimized/restored and closed using messages targeted only to the child process. The final capture matches the new 804×481 client extent and live state remains inspectable.

The automated desktop declined the foreground request. That run therefore does **not** qualify physical keyboard/mouse control, nor does it claim targeted keyboard messages moved the character. Those checks are explicitly false in its evidence. The input adapter and timing logic are unit-tested; a foreground human session is still needed for end-to-end input/feel qualification. Native Linux graphics, other machines, multi-window operation, extended device-loss/recovery and sustained resource behavior remain unqualified.

The SDL integration follows its main-thread [relative mouse mode](https://wiki.libsdl.org/SDL3/SDL_SetWindowRelativeMouseMode) and [drawable pixel size](https://wiki.libsdl.org/SDL3/SDL_GetWindowSizeInPixels) contracts. Dependencies remain pinned as in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
