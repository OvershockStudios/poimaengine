# DEPOT RUN exercise rules

Create a new small first-person 3D game using an external agent and Poima's
native service. Supply the SDK/API reference and licensed character Idle and
completion-sound inputs, but no sample game source or scene.

The workflow is import → guarded scene/UI authoring → development compilation
and controller tests → genuine Native AOT publication → verified native export.
Preserve failed attempts and actual diagnostics. A successful provider exit is
not independent game verification. Unknown operation outcomes require inspecting
the authoritative owner; never blindly resend them.

## Frozen gameplay expectations

Player foot starts at `(0,0,3)`. Use an unparented, unscaled native
CharacterController with a direct-child Camera. Marker roots have these exact
centers; their visual children may sit above the ground:

| Marker | Center |
| --- | --- |
| Start | `(0,0,3)` |
| Pickup A | `(-3,0,0)` |
| Delivery A | `(3,0,0)` |
| Pickup B | `(-3,0,-5)` |
| Delivery B | `(3,0,-5)` |
| Finish | `(0,0,-8)` |

Interactions use an inclusive 0.8 m XZ radius and foot Y from -0.6 to 0.6 m.
Supply a solid floor, lighting/sky, distinguishable pads/parcels, off-route
cover and clear walking routes through all markers and outer lanes at X ±5.
Keep the licensed animated marshal away from those routes.

Phase is Ready=0, Running=1, Won=2. Begin changes Phase immediately at the
unchanged tick and requests Resume in the same compiled callback. Running
transitions on Use are:

| Current stage/cargo | Region | Result |
| --- | --- | --- |
| `0/0` | Pickup A | `1/1` |
| `1/1` | Delivery A | `2/0` |
| `2/0` | Pickup B | `3/2` |
| `3/2` | Delivery B | `4/0` |
| `4/0` | Finish | Won, unchanged `4/0` |

Wrong stage/region, early finish and out-of-range rising Use increment Rejected
once and preserve progress/cargo. Neutral input never interacts. No objectives
or rejections advance while Ready, Menu is open or Won. The timer increments
once for each tick initially Running with Menu closed, including the winning
tick. The game observes committed prephysics player position. There is no
timeout. A multi-tick native input batch produces Use only on its first tick.

## Controls, persistence and content

Provide Welcome/Begin/Load, Running HUD/Menu, Menu/Save/Load/Resume/status and
Won UI. Menu requests Pause and gates game logic even during explicitly stepped
ticks. Resume requests Resume. Save uses `depot-checkpoint` and reports actual
native ticket completion. Load is reachable from fresh Welcome as well as Menu.
Restore every mutable scalar, pending ticket, controller/animation/UI/audio
state before another tick; reconcile restored tickets without re-enqueuing.

Use wine `#722F37` primary actions, light text `#F7E8EB` and dark-background
accent `#D46A7E`. Import the supplied genuinely weighted character and native
Idle clip. Play the supplied completion sound once on an actual win. Preserve
licensed original notices, declared input hashes and selected native provenance.

Test wrong order, empty delivery, early/outside Use, first-tick input batching,
complete playthrough, terminal stability and partial stage3/cargo2 save/load in
both same and fresh owners. Move only with genuine native controller inputs;
do not teleport, patch gameplay fields or edit runtime poses to pass checks.

## Engine and artifact boundaries

Use discovery/current guards/request IDs for every scene/entity/component/UI
edit. Do not write world/history/receipts/cooked assets directly. Only authorized
build jobs may execute the configured native toolchain. No engine changes,
policy widening, tool installation or another provider invocation is allowed.

Inspect build diagnostics and explicitly load a newly compiled development
assembly. Complete development repairs before selecting the final native image.
Require verified publication success, copy its complete artifact to the project
and load that immutable path/hash. Native libraries cannot be replaced within a
process. Test fresh checkpoints against the native image; managed checkpoints
alone are insufficient native proof.

Create a portable version-2 project manifest and export its actual native
descriptor/closure. Require a verified bundle inventory; exclude C#, project
files, FBX sources, CoreCLR and managed bridges from the shipped payload. Leave
the owner available for independent inspection. Report real paths/hashes,
failed attempts, repairs and limitations without claiming an oracle result.
