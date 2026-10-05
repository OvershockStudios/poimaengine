# Native gamepad input

Poima 0.0.20 adds SDL gamepad discovery and live input for **one assigned local player**, alongside the existing keyboard/mouse controls. Buttons, sticks and triggers resolve to the same native `RuntimeInput` used by the runtime, C# gameplay and semantic replay. The native evaluator also runs without SDL, a display or a GPU. Physical controller qualification remains outstanding; implementation and automated coverage do not establish device compatibility or input feel.

This extends [input profiles](INPUT_PROFILES.md). General action maps, UI navigation, rumble, gyro, per-player multiplayer routing and console platform integration remain unfinished.

## Discover devices and select one

`world.describe` schema 18 exposes `input.devices`:

```json
{"jsonrpc":"2.0","id":1,"method":"input.devices","params":{}}
```

The result reports `available` and a `devices` array. Each device has an `id`, `name`, `type`, supported `buttons` with control IDs/labels, and supported `axes`. A build without the SDL host returns `available:false` and an empty list. This differs from an available host that currently finds no devices.

Device IDs are SDL instance IDs valid within the current world-service process. Keep that process open between discovery and play. Do not save an enumeration index or assume an unplugged controller keeps its instance ID. Reconnection may require discovering and selecting a new ID.

Add one of these `gamepad` values to the existing `runtime.play` parameters:

| Value | Assignment policy |
| --- | --- |
| `{"mode":"disabled"}` | Ignore physical gamepads for this play call. |
| `{"mode":"only_connected"}` | Assign the sole connected gamepad; wait if none exist or selection is ambiguous. An existing assignment is retained when another device appears. |
| `{"mode":"explicit","id":7}` | Assign the specified live ID; replace `7` with discovery's ID. Initial absence fails play; later disconnect does not silently substitute a different device. |

The assigned gamepad controls the `controller` entity already specified by `runtime.play`; its local slot is 0. Other devices cannot inject gameplay or Start-button actions into that slot. Add/remove/remapping events are handled during play. Removal clears that pad's held controls and pending action edges while preserving keyboard/mouse input. With `only_connected`, an unassigned slot can adopt the sole remaining/arriving device; it must pass the neutral gate first.

The play result includes `gamepad` assignment, connected/armed/active state, policy, diagnostic detail and attachment/disconnection counts. This is a report of the completed play call. The call still blocks its service connection, so concurrent live discovery or reassignment commands are not yet available.

## Defaults and versioned profiles

The defaults deliberately differ between the live player and compatibility-oriented headless evaluation:

| Operation | Default behavior |
| --- | --- |
| `input.describe` → `defaults` | Original keyboard/mouse v1 profile. |
| `input.describe` → `gamepad_defaults` | v2 profile with keyboard/mouse plus gamepad configuration. |
| `input.evaluate`, no profile/options | Original v1 behavior. Set `gamepad_defaults:true` to evaluate v2 defaults. |
| `input.inspect`, missing file | Unpersisted v1 defaults at revision 0. |
| `runtime.play`, no profile | v2 defaults and `only_connected` selection for interactive play. |
| `runtime.play`, explicit v1 profile | Preserve that profile exactly; gamepad selection defaults to disabled. |
| `runtime.play`, replay mode | Consume recorded semantic controls, with physical gamepads inactive. |

A v2 profile is the complete existing profile plus a required `gamepad` object. Built-in v2 defaults are:

```json
{
  "bindings": {
    "forward": ["key.w"],
    "backward": ["key.s"],
    "left": ["key.a"],
    "right": ["key.d"],
    "jump": ["key.space", "gamepad.south"],
    "use": ["key.e", "gamepad.west"]
  },
  "sensitivity_x": 0.1,
  "sensitivity_y": 0.1,
  "invert_x": false,
  "invert_y": false,
  "gamepad": {
    "move": {"stick":"left", "inner_deadzone":0.15, "outer_deadzone":0.95, "response":1, "invert_x":false, "invert_y":false},
    "look": {"stick":"right", "inner_deadzone":0.15, "outer_deadzone":0.95, "response":1, "invert_x":false, "invert_y":false},
    "look_degrees_per_second": [180, 120],
    "trigger_press": 0.55,
    "trigger_release": 0.45
  }
}
```

Submit that object as `profile` to `input.transact`, with a new `.poima-input.json` path, request ID and expected revision 0. The envelope format becomes `poima.input.v2`. All profile fields are required; the transaction accepts a complete candidate, not a patch. Existing revision, preview, hash and retry rules apply.

Existing v1 documents, hashes and receipts remain unchanged. Converting is explicit: inspect the old profile, retain its keyboard/mouse choices, add the complete gamepad configuration and desired alternatives, then transact to a **new destination path**. The new file starts its own revision/receipt history. The original stays intact. In-place format changes and mixed-format receipt histories are rejected, including preview requests. There is no automatic migration or dedicated conversion command.

An existing action may already have four alternatives; choose which to retain before adding a pad binding. To disable hardware input temporarily while retaining v2 configuration, use `gamepad:{"mode":"disabled"}` in play. Selecting physical gamepads with an explicitly supplied v1 profile fails instead of changing its meaning.

## Buttons, sticks and triggers

The same zero-to-four alternatives and no-sharing rules apply across keyboard, mouse and gamepad controls. Holding a second alternative does not retrigger an already-held action. Jump/use press edges retain their source so disconnecting a pad cannot consume a keyboard/mouse press that is awaiting a tick.

Face controls name physical positions: `gamepad.south`, `.east`, `.west` and `.north`. For example, south is normally Xbox A or PlayStation Cross. Device discovery asks SDL for actual face-button labels; the generic catalog retains position labels. This avoids baking Xbox lettering into game logic. Shoulder, stick-click, D-pad, paddle and additional buttons are listed by `input.describe`; not every device exposes every catalog control. Start and Guide are reserved from gameplay bindings.

Each analog role selects `left`, `right` or `none`; move and look cannot own the same enabled stick. Processing uses normalized radial magnitude, with symmetric signed endpoints. Magnitudes at/below `inner_deadzone` produce zero; `outer_deadzone` is the saturation magnitude, not an amount subtracted from the edge. The interval between them is rescaled to `[0,1]`, raised to `response`, then applied along the stick direction. Validate `0 <= inner_deadzone < outer_deadzone <= 1` and `0.1 <= response <= 8`. Inversion applies independently to each role and axis.

Move combines the processed stick with digital movement, clamps components to `[-1,1]`, and uses the runtime's existing unit-magnitude movement limit. Analog magnitude below one is retained. Look uses `[yaw,pitch]` **degrees per second**, each finite in `[0,1080]`, integrated once per committed 1/60-second tick. With defaults, full right-stick horizontal deflection turns 180 degrees over 60 ticks. Normal right/down deflection produces negative yaw/pitch under Poima's existing input convention. Mouse sensitivity remains degrees per relative mouse unit; it does not control stick speed.

Mouse and stick look combine under the existing 180-degree-per-axis tick limit. Mouse displacement retains its own bounded backlog. A stick contribution beyond the combined tick limit is clipped for that tick instead of becoming queued mouse motion. Repeated input peeks do not accumulate extra rotation or consume edges; only a successful simulation step commits consumption.

`gamepad.left_trigger` and `.right_trigger` are digital binding controls derived from analog trigger positions. They press at/above `trigger_press`, stay held inside the hysteresis band, and release at/below `trigger_release`. Require `0 <= release < press <= 1`. This avoids repeated action edges near a single threshold. Triggers are driven through axis events; directly injecting them as physical buttons is rejected.

## Neutral gating, pause and mouse capture

Connection, reassignment, remapping and activation snapshot current device state. Gameplay remains disarmed until all physical buttons are released, both triggers are at/below the release threshold, and both sticks lie within the smaller configured inner deadzone. This conservative gate includes unused sticks and reserved buttons. A zero inner deadzone therefore requires exact centering. Center the sticks and release buttons after connecting or resuming; the neutral transition itself does not create a press.

Focus loss/minimize clears pending input and pauses simulation. Regaining focus does not automatically resume. The assigned pad's Start rising edge toggles pause/resume while focused; Tab pauses and releases mouse capture; a left click recaptures the mouse and resumes. Escape/window close exits the player. Guide is not a gameplay action.

Start can resume without capturing the mouse. Keyboard and gamepad input then work while the cursor is free; mouse look/buttons resume only after capture. The recapture click is consumed rather than firing a gameplay binding, and recapturing an already-active session preserves the pad's state. Pause/resume is local player control, not a game menu framework. Disconnect clears pad input but does not automatically pause the whole simulation.

## Desktop editor

The editor shares the native profile evaluator and device adapter with the standalone player. Open **Game Input** with the Game toolbar’s input-profile icon. Select built-in keyboard/mouse/gamepad bindings or a saved v2 profile, then choose an assignment policy. Apply while stopped stages preferences for the next Game capture; Apply during an active runtime validates and acquires the proposed device before replacing the current configuration. A failed change preserves existing input. Settings are session-local.

Click the Game view to take control. Center sticks and release buttons to arm an assigned pad. **Start releases existing Game capture; it does not pause simulation or acquire editor focus.** Escape and Tab also release capture. Pause, focus loss, Game camera changes, resizing or detaching Game clear input. Resume requires another deliberate click. Scene detachment alone does not clear Game input. A capture job suspends gamepad delivery and requires a fresh neutral snapshot afterward, while pending keyboard/mouse input is retained.

Agents use `desktop.input.devices` (or `input.devices`) to discover process-session IDs. Configure a runtime controller with:

```json
{
  "session_id": "<current runtime session>",
  "controller": "<CharacterController entity ID>",
  "defaults": "keyboard_mouse_gamepad",
  "gamepad": { "mode": "only_connected" }
}
```

Pass this object as `params` to `desktop.input.configure`. Replace `defaults` with `input_profile` and optional `input_revision` to freeze a saved profile. Built-in `defaults` accepts `keyboard_mouse` or `keyboard_mouse_gamepad`; omission selects v1 keyboard/mouse bindings. Enabling a pad with v1 is an error. Selection accepts `{"mode":"disabled"}`, `{"mode":"only_connected"}`, or `{"mode":"explicit","id":123}`. Only explicit mode takes an ID. A missing explicit ID fails initial configuration; later disconnection waits for that identity and never substitutes a different controller.

`desktop.input.inspect` and the compact `input` field in editor polling include a `gamepad` object: availability, policy/requested ID, assigned ID/name, connected/armed/active flags, reason, attachment/disconnection counts and any device error. Focus remains a separate explicit gate. Device errors release input and require reconfiguration; they do not undo committed simulation. Stop or save-load session replacement releases the borrowed input/device binding while keeping the host’s SDL subsystem reference for later discovery.

Hosted device polling updates SDL gamepads directly and consumes their events without pumping the editor’s Windows message loop. It removes at most 256 gamepad events and 256 duplicate raw joystick events per poll, preserving unrelated events. Raw-event filtering still traverses the queue; this bound is not a constant-time or frame-budget guarantee. The profiler records `input.gamepad.poll`. Event processing does not advance simulation: each successful automatic fixed tick consumes one evaluated frame, so stick look applies on every catch-up tick and button edges fire once.

The editor does not yet provide physical-controller qualification, rumble, general action maps or controller-only editor navigation. Automated virtual-device and semantic-control tests are distinct from physical focus and cursor behavior. Editor audio output remains a separate unfinished integration.

## Headless observations

This request needs no physical controller:

```json
{"jsonrpc":"2.0","id":2,"method":"input.evaluate","params":{"gamepad_defaults":true,"events":[{"gamepad_connect":{}},{"gamepad_axis":{"axis":"left_y","value":-32768}},{"gamepad_axis":{"axis":"right_x","value":32767}},{"control":"gamepad.south","down":true},{"consume":true},{"consume":true},{"gamepad_disconnect":true},{"consume":true}]}}
```

The first two frames move forward and look by `[-3,0]` degrees each; only the first has `jump:true`. The disconnected frame has neutral pad input. Frames include their event index and connected/armed flags; the result also includes final gamepad status. A `path` can select a persisted v2 profile instead; `path` and `gamepad_defaults` cannot both appear, even when the latter is false.

Additional event forms are:

| Event | Meaning |
| --- | --- |
| `{"gamepad_connect":{"axes":[0,0,0,0,0,0],"buttons":0}}` | Replace the simulated connection with a full initial snapshot. Both fields are optional. Axis order is left X/Y, right X/Y, left/right trigger; button bits use physical SDL codes 0–25. |
| `{"gamepad_axis":{"axis":"right_trigger","value":20000}}` | Update an axis. Stick values are integers −32768…32767; trigger values are integers 0…32767. Names are `left_x`, `left_y`, `right_x`, `right_y`, `left_trigger`, `right_trigger`. |
| `{"gamepad_button":{"control":"gamepad.start","down":false}}` | Raw physical button event, including reserved Start/Guide, for neutral-gate traces. This does not toggle headless simulation pause. Trigger controls are excluded. |
| `{"gamepad_disconnect":true}` | Disconnect and clear only pad-owned input. |

The existing generic `control` event accepts non-reserved physical pad buttons, while raw `gamepad_button` additionally permits reserved ones. `clear:true` clears all pending input and disarms the pad while retaining its last raw snapshot; subsequent neutral updates can re-arm it. Requests remain isolated and bounded to 256 events. They do not advance a runtime or modify profiles/worlds. Booleans are not accepted as axis/button-mask numbers.

`runtime.play` replay continues to consume resolved movement/look/actions directly, independent of these bindings or device attachment. C# receives the existing semantic `GameInput`, with no ABI change or C# device-management API introduced by this slice.

## Validation and limits

Native tests in `tests/input_profile.cpp` cover stick shaping, endpoints, fixed-tick rates, repeated peeks, trigger hysteresis, neutral gating, source-preserving disconnects and unchanged v1 controls. `tests/gamepad_contract.py` checks the public API, versioned persistence and malformed input. `tests/gamepad_sdl.cpp` exercises the actual SDL adapter using virtual devices, including assignment, removal/reconnection and device-state handling. [Recorded milestone evidence](evidence/m2-gamepads.json) includes 18 headless and 21 runtime CTest suites, 13 gamepad contract cases on Linux and Windows, the native Windows evaluator and SDL virtual-device test, and matching player replays on both laptop GPUs. A targeted Windows window test also verifies keyboard remapping and lifecycle behavior.

Physical Xbox/DualSense/other controllers, Bluetooth behavior, actual deadzone tuning, latency and prolonged foreground use still need qualification. Device labels reported by SDL are not an implemented prompt/glyph UI. There is one local assigned controller, no runtime rebinding menu, no rumble/gyro/touch integration, no general joystick mapping editor, and no console backend. Using a console-branded gamepad on Windows is not Xbox/PlayStation platform support.

The [0.0.38 editor checkpoint](evidence/m2-editor-gamepads.json) records hosted-device ownership, virtual-controller routing, capture suspension, device cleanup, managed integration and actual Game Input controls on both tested GPUs. The [settings image](evidence/m2-editor-gamepads.png) is an attached Avalonia client render, not a desktop screenshot.
