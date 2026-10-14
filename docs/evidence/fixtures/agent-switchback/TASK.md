# Switchback Range exercise rules

Create an original small Windows first-person game using an external agent and
Poima's public native service. Start from a fresh project with `poima project
create`. Supply the current SDK/API reference, discovery, configured development
profiles and the original CC0 Kenney weighted character/Idle/Run and two sound
inputs. Do not supply this fixture, earlier game implementations or verifier
source to the agent.

This portable task adapts the original exercise's gameplay rules. It omits the
original machine-specific execution transport, roots and receipt configuration.
Configure explicit process ownership, provider policy and time budgets for a new
run before launching it. Preserve failures; a provider exit is not game proof.

Use native controller input and public guarded authoring/import operations.
Game source and project files may be written normally. Do not patch authored
worlds, receipts, cooked assets, runtime poses or gameplay values to pass tests.

## Required world and role handoff

The level is a compact range on a solid floor whose top is Y=0. Keep the
rectangle X=-8..8, Z=-10..7 walkable except for the two stated obstacles. Visual
layout, materials, signs and UI typography remain the agent's choice. Use
consistent physical and visual dimensions; transform-scaled unit box colliders
must not multiply their dimensions twice.

| Role | Required geometry or behavior |
| --- | --- |
| Player and direct-child camera | Foot starts (0,0.1,5), heading toward -Z. Standing height 1.8 m, radius 0.3 m, speed 4 m/s; configured crouch height 1.0 m, speed 2 m/s, camera drop 0.6 m. Initial camera local eye height 1.6 m, world lens 70 degrees / 0.1 m / 80 m. Native locomotion uses the explicit virtual-v1 profile: step_height=0.4 m, ground_snap=0.5 m, max_slope_degrees=45. This selected profile requires full native snapshot version 9 for the checkpoint qualification. |
| Supply pad | Center (-3,0,0), inclusive XZ interaction radius 0.5 m. A lintel occupies X=-4..-2, Z=-0.8..0.8, underside Y=1.25 m. Pad Use succeeds only while committed crouch is true, foot lies inside this footprint and foot Y is within 0.05 m of the floor. |
| Static target A | Root (-3,0,-6), a visible query target centered 1.3 m above its root. It has a named `target` HitRegion with owner A. |
| Imported target B | Camera-free native controller root initially (3,0,-6). Import the supplied genuinely weighted character and original Idle/Run sources. During active play it patrols between X=2.5 and X=3.5 at Z=-6 using native character inputs, not transforms or teleportation. Select Idle/Run from observed committed displacement; stopping a cleared target selects Idle. |
| B's hit region | Attach a query region to one inspected upper-body rig node. Declare the source model node index, imported node-name/path evidence, rig-node entity and region role in the handoff. Do not guess a bone ID or invent an anatomy index before import inspection. The independent oracle checks the imported skeleton binding. |
| Occluding cover | Box center (-2,0.8,-2), full size (1.2,1.6,0.4). Its visible and physical surfaces agree. It blocks a shot from the start area toward A; walking to the supply-side lane permits a shot around it. |
| Hazard pad | Center (0,0,-4), inclusive XZ radius 0.4 m, floor-height bound 0.05 m. One damage occurs per outside-to-inside entry. Exiting is required before another entry can damage. |
| Finish console | Center (0,0,5), inclusive XZ Use radius 0.6 m, floor-height bound 0.05 m. Win requires supply collected and both targets cleared. |

Assign entity/schema IDs yourself and return a bounded handoff mapping these
roles and the required semantic state fields to their actual IDs/names. The
handoff identifies authored objects; it cannot substitute new geometry,
interaction constants, script-generated progress, or oracle expectations.

Supply B's actual imported node/skin/clip mapping, source hashes, declared
reference frame, source licenses and cooked content/provenance hashes. The
character must render with its real skin and both Idle and Run must execute.
Do not replace it with a static proxy or make animation depend on desired
velocity when the character is blocked. No navigation, root-motion, IK,
procedural crouch body-rig, or weapon-model importer claim is required.

## Frozen gameplay rules

Use one compiled `Game<TState>` with primitive saved authoritative fields.
Publish semantic observations for Phase, Supply, ClearedMask, Ammo, Shots,
Rejected, Health, Timer, MenuOpen, ReloadRemaining, and the saved ADS ramp.
Required phases are Ready=0, Running=1, Dead=2, Won=3. Initial values are Ready,
Supply=0, ClearedMask=0, Ammo=0, Shots=0, Rejected=0, Health=3, Timer=0,
MenuOpen=0, ReloadRemaining=0. No objectives, weapon timers, patrol or ADS phase
advance in Ready, Dead, Won or an open Menu. Native explicit stepping does not
bypass that game gate. During an active tick Timer advances exactly once,
including the tick entering Dead or Won. Gameplay reads committed prephysics
positions and poses.

For each Tick, determine active-at-entry exactly once (`Phase==Running` and
`MenuOpen==0`). If false, perform none of the active handlers below. If true,
increment Timer once and execute this fixed order:

1. Stage held stance and the ADS lens step; advance an existing reload by one
   (and refill at zero), then process a rising reload using the state at this
   point. A new reload starts at 12 and is not decremented that Tick. Invalid
   reload attempts increment Rejected once.
2. Evaluate the hazard entry from committed position; if Health reaches zero,
   enter Dead and skip all remaining handlers this Tick.
3. Process rising Use for supply or finish. If it wins, enter Won and skip all
   remaining handlers this Tick.
4. Process rising fire using the resulting logical state and the committed
   camera/target poses.
5. Stage target B locomotion and animation, stopping it if its bit is cleared.

Native setters remain staged: a simultaneous Crouch+Use cannot make committed
stance crouched before the same Tick's pickup check. A simultaneous Reload+Use
evaluates reload before pickup. A successful winning Use or lethal hazard entry
prevents a simultaneous Fire from consuming ammunition. These rules fix only
the game handlers; they do not alter the native physics/commit ordering.

Controls are ordinary native move/look/jump, legacy Use=E, and named actions
`fire`, `aim`, `reload`, `crouch`. Bind fire to the primary mouse button, aim to
the right mouse button (catalog `mouse.3`), reload to R and crouch to C. Declare
every SDK feature used. The oracle also supplies these semantic input records,
which qualifies simulated native input, not physical mouse/keyboard interaction.

- Crouch follows the held named action through `SetCharacterStance`; releasing
  under the lintel must retain actual crouch and blocked stand intent until
  native clearance allows standing. Supply pickup requires a rising Use and
  committed crouch/position; first valid pickup sets Supply=1 and Ammo=4.
- A rising fire with no ammunition or while reloading increments Rejected once
  and changes neither Shots nor Ammo. Otherwise it consumes one round and
  increments Shots once. Use the actual camera world origin/forward and
  `GameContext.HitTest`, ignoring the player owner, with a maximum 20 m ray.
  A new, enabled target region with Supply=1 sets its corresponding ClearedMask
  bit (A=1, B=2). Other results, occlusion, misses or already-cleared targets
  increment Rejected once without changing the mask. Held fire without another
  rising edge does not shoot automatically. A multi-tick input batch has a
  single initial pressed edge.
- Rising reload, when Supply=1, Ammo<4 and no reload is active, sets
  ReloadRemaining=12. This initiating tick does not decrement it. Each later
  active tick decrements by one; reaching zero sets Ammo=4. No reload, shot or
  objective progress is hidden in unmanaged or game-object instance fields.
- Aim independently controls world FOV 70->35 degrees and foreground FOV
  75->55 degrees over 12 active fixed ticks. Release returns to 70/75. Use
  `CameraFovTransition.Evaluate` with saved primitive phase/endpoints; a reversal
  starts from the current evaluated lens. A changed aim target seeds elapsed
  zero and publishes the current value on that initiating Tick; each subsequent
  active Tick advances elapsed by one up to 12. Other camera lens channels stay
  authored. Author a simple visible viewmodel primitive using the supported
  camera binding; no supplied weapon art or source implementation is needed.
- One damage per hazard entry reduces Health by one. At zero enter Dead and
  stop active logic. Retry must restore the real native checkpoint, not reset
  selected fields or teleport the actor. An initial checkpoint must be available
  after Begin; explicit later saves replace it through guarded native saves.
- Rising Use at the finish console wins only for Supply=1 and ClearedMask=3.
  Premature or out-of-range Use increments Rejected once. Won is terminal;
  repeated inputs must not replay objectives or the completion cue.

UI provides Welcome/Begin/Load, Running HUD/Menu, Menu/Save/Load/Resume,
Dead/Retry/Load and Won status. Compiled controls act at the current tick;
Begin and Resume request native resume, Menu requests pause, and game gating
also protects explicitly stepped ticks. Show ammo, target progress, health,
reload, stance/blocked stand and actual save completion status. Save/load use
`switchback-checkpoint`, native tickets and observed completion; Load is usable
from a fresh Welcome owner. Preserve exact gameplay/physics/rig/UI/audio state
and reconcile restored save tickets without re-enqueueing them. Do not persist
new random host identities merely to observe restore; exact cold continuation
must not need arbitrary gameplay-field exclusions.

Import the supplied licensed native WAV inputs through the documented API.
Play one pickup/hit cue at accepted events and one completion cue once on Win.
Use permanent cue emitters so clearing target logic does not retire their owner.
Record actual native playback requests and active audio/save state; verify the
exported asset/credit closure separately. Listening and audio-device quality need
independent hardware evidence.

## Publication and independent evidence

Use discovery and current guards for every native authoring mutation. Compile a
development image through the configured public job service, inspect diagnostics
and explicitly load it for repairs. Publish the final native game through the
selected development profile, require verified terminal success, then use
`project.gameplay.adopt` with the inspected project/artifact hashes and a fresh
request ID. Inspect the adopted closure and load that immutable native artifact
in a fresh owner; adoption alone does not activate gameplay.

Export through the configured runtime/profile and verify its full inventory.
Exclude C#, source project/FBX/OGG files, CoreCLR, managed bridges and debug symbols
from the shipping bundle. Retain engine/dependency licenses and selected asset
credits. Keep saves, captures and test output outside the immutable bundle.

Return a portable handoff with actual role/field/UI IDs, imported binding
inspection, source/content/provenance hashes and real compile/save/export
receipts. Keep failed attempts, recovery and passing checks distinct. Unknown
operation outcomes require observing the existing authoritative owner rather
than blindly retrying or replacing a process.

Use an actually pumped shared host for live player observation. `poima world`
stdio supports explicit headless stepping; shared `poima serve` or packaged
`poima game serve` plus `poima connect` supplies live pumping. Wait for real host
readiness before connecting, observe current-player capture ticks, stop the
player before host shutdown, and record natural owner completion.

Independent checks must drive actual native movement and action edges, including
occluded/empty shots, crouch and blocked standing, moving weighted target,
12-tick ADS/reload and reversals, hazard entry, death/retry, menu gates,
winning-handler order, native save/load and complete same/cold-owner snapshots.
Render observations, bundle relocation, source-path absence, physical controls,
audio-device listening and performance are separate evidence scopes. Do not
substitute a provider's claimed playthrough for independent qualification.
