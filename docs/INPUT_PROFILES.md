# Keyboard and mouse profiles

Poima 0.0.19 adds native, persisted bindings for the current six player actions. The CLI, headless evaluator and SDL player share the same profile and evaluation code. Version 0.0.20 adds [gamepads and v2 profiles](GAMEPADS.md). General action maps/contexts, live rebinding UI and comprehensive graphics/audio preferences remain unfinished.

## Discover, preview and apply

`world.describe` schema 18 exposes `input.describe`, `input.inspect`, `input.transact` and `input.evaluate`. These work in the headless build. `input.describe` returns v1 `defaults`, v2 `gamepad_defaults`, supported formats, complete profile schemas, physical control catalog, reserved controls, units and persistence/application rules. `input.inspect` takes a `path`; a missing profile reports v1 defaults, revision 0 and `persisted:false` without creating it. `input.devices` separately reports whether the SDL device host is available and its discovered gamepads.

Profile files end in `.poima-input.json`. Their parent directory must already exist. Relative paths resolve beside the world document. They are independent of the authored world and can be shared by several worlds. Paths inside the world's immutable asset store or reserved document paths are rejected. Main files and sidecars reject symbolic links and hard-link aliases. Explicit paths are the initial development interface; automatic per-user storage selection and packaged-game configuration are unfinished.

The runnable [input profile example](../examples/input-profile.jsonl) saves an arrow-key profile and observes its actions without opening a window.

Submit a complete profile, rather than a partial field patch:

```json
{"jsonrpc":"2.0","id":1,"method":"input.transact","params":{"path":"personal.poima-input.json","request_id":"00000000000000000000000000000001","expected_revision":0,"preview":true,"profile":{"bindings":{"forward":["key.up","key.w"],"backward":["key.s"],"left":["key.a"],"right":["key.d"],"jump":["key.space"],"use":["mouse.3"]},"sensitivity_x":0.15,"sensitivity_y":0.1,"invert_x":false,"invert_y":false}}}
```

Preview validates the candidate and reports its profile/hash, whether values change, and proposed revision; it creates neither profile nor receipt. Set `preview:false` or omit it to commit. A preview request ID can subsequently be used for that commit because previews do not persist receipts. Every accepted commit increments the profile revision, including a set with unchanged values (`changed:false`). Reset by submitting the matching complete defaults from discovery: `defaults` for v1 or `gamepad_defaults` for v2. An existing file cannot change format through a reset/transaction.

The last 32 successful request IDs and their normalized parameters/results survive restart. Retrying the same committed operation returns its original result with `replayed:true`; reusing the ID with different parameters fails. Retry receipts are checked before the expected revision, so a successful request can be retried after later edits. Once a receipt expires, a stale expected revision still fails. Profile revisions are separate from authored-world and runtime revisions.

The content hash is SHA-256 of canonical compact profile JSON, excluding receipts/revision. Keyboard/mouse-only profiles retain the `poima.input.v1` envelope and original hashes; profiles containing the complete `gamepad` configuration use `poima.input.v2`. Profiles plus receipts are limited to 64 KiB. Existing files cannot change format in place: explicitly copy a converted complete profile to a new path, preserving the original file/receipts. Parsing rejects unknown/missing fields, duplicate JSON keys, invalid types, unsupported format versions and inconsistent or mixed-format receipt history. Edit through the transaction API; manually changing a profile while keeping stale receipts produces a corruption diagnostic.

## Binding semantics

The six actions are `forward`, `backward`, `left`, `right`, `jump` and `use`. Each accepts zero to four alternatives. Holding either alternative holds the action; releasing one does not cancel another that remains held. Duplicate controls and sharing a control across actions are rejected atomically. To swap two bindings, change both in one profile transaction. Empty bindings explicitly disable an action.

Keyboard IDs name physical positions, using the pinned SDL scancode mapping. Catalog labels are descriptive labels, not localized keyboard-layout glyphs. Mouse controls use SDL numbering: `mouse.1` left, `mouse.2` middle, `mouse.3` right, then buttons 4 and 5. Escape and Tab remain reserved recovery controls. Modifiers can be standalone controls; chords, wheel bindings and context-dependent sharing are not yet implemented.

Sensitivity is finite, in `[0,10]` degrees per relative mouse unit, independently per axis. Zero disables that axis. Normal mouse movement preserves the existing player signs; inversion reverses the selected axis. The existing bounded pending-look accumulator retains sub-tick motion and limits each consumed look axis to 180 degrees. Opposing movement actions cancel. Jump/use latch an aggregate rising edge until the next successful tick; multiple presses before that tick collapse to one Boolean in the current runtime input ABI.

Focus loss and minimization clear pending input. SDL keyboard repeat events do not create new presses. The click used to recapture the mouse is consumed by the window interaction and does not also trigger a gameplay action. Interactive play peeks at a pending frame, steps simulation, and consumes it only after the step succeeds. C# still receives the existing semantic `GameInput`; profiles do not change that ABI or add C# settings editing APIs.

## Observe without a window

`input.evaluate` runs a bounded, isolated event sequence through the same native evaluator as the player:

```json
{"jsonrpc":"2.0","id":2,"method":"input.evaluate","params":{"path":"personal.poima-input.json","events":[{"control":"key.up","down":true},{"motion":[10,-5]},{"consume":true},{"control":"key.up","down":false},{"consume":true}]}}
```

Supply at most 256 events. Keyboard/mouse events are a catalog `control` plus Boolean `down`, a two-number `motion` in relative units (each in `[-1e6,1e6]`), `consume:true`, or `clear:true`. Each consume produces an indexed frame with movement, look, jump/use and gamepad connected/armed flags. Omit `path` to use v1 defaults; a supplied path must name an existing valid profile. Opt into built-in v2 evaluation with `gamepad_defaults:true`, mutually exclusive with `path`; additional gamepad events are documented [here](GAMEPADS.md#headless-observations). The evaluator changes no runtime, profile or world data, and each request starts with clear input. Existing-profile inspection/loading may create a cooperative `.lock` sidecar.

## Connect to play

Add `input_profile:"personal.poima-input.json"` to `runtime.play`. Optional `input_revision` requires that exact profile revision. The player loads and validates a profile snapshot before opening the window; changes made elsewhere affect the next play call. Invalid or missing requested profiles fail explicitly. In 0.0.20, omission uses v2 engine defaults with gamepad support; an explicit v1 file preserves its keyboard/mouse behavior and disables gamepad selection by default. Unlike live play, `input.evaluate` still defaults to v1.

Play reports `input_profile` with `source` (`defaults` or `profile`), format, revision, content hash (null for built-in defaults) and `applied`. Interactive play applies bindings. Replay validates an explicitly supplied profile but consumes its existing semantic movement/look/action sequence directly (`applied:false`), with physical gamepads inactive. Changing bindings therefore does not reinterpret a recorded replay. A retried play request returns the original play receipt and profile metadata.

## Storage and qualification

Writes use a cooperative lock, bounded validation, flushed staging, a previous valid copy (`.previous`) and atomic file replacement. Failures before publication preserve the current profile. Corrupt/newer-format files are rejected without automatic overwrite or migration; recovery from a backup is manual in this slice. Atomic replacement does not establish universal power-loss durability, and a separate process that ignores the cooperative lock is outside the concurrency guarantee.

`tests/input_profile.cpp` checks the native evaluator, while `tests/input_profile_contract.py` covers persisted profiles, retries, errors and native event observations. `tests/player_contract.py --input-profile` compares replay against ordinary runtime stepping despite changed bindings. `tests/player_window.py --input-profile` exercises a remapped control using targeted Windows messages when the desktop permits foreground input. These messages are synthetic; physical input feel, layout-specific labels and broader device qualification remain separate acceptance work. Recorded keyboard/mouse results belong in [input evidence](evidence/m2-input-profiles.json); [gamepad validation](GAMEPADS.md#validation-and-limits) describes the added test surfaces and remaining hardware limits.
