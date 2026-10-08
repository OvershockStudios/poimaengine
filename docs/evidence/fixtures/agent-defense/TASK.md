# Relay Defense: sanitized authoring assignment

Create and actually playtest a fresh small first-person wave-defense game named
**Relay Defense** in a disposable Poima workspace. Author new C# gameplay, a
native scene, spawn templates and logical UI. Use guarded Poima transactions for
scene/template/UI edits; do not write the world document directly or modify
engine source. Use primitive geometry and local SDK references, with no external
art, package dependencies, network requests, GUI or GPU captures.

## Gameplay requirements

- Provide a CharacterController with a child Camera, collidable floor, reactor
  and cover. Support movement/look and a Use action that fires hitscan from the
  actual camera, ignoring the player collider.
- Run three sequential waves of three native spawned kinematic box drones.
  Each advances toward the reactor along an unobstructed straight-line lane and
  requires two successful camera-ray hits to eliminate. Remove defeated drones
  natively and clear dead references.
- Eliminating all nine drones wins. Each drone reaching its reactor boundary
  despawns and removes one reactor health point. Start with three health; zero
  health loses without also reporting victory. Unattended loss must occur within
  1,800 ticks. Gameplay progression stops after either terminal outcome.
- Use a six-round magazine and twelve-tick cooldown. Accepted shots spend one
  round even on a miss or cover hit. Cooldown and empty-magazine rejections spend
  no ammunition. A compiled Reload control refills the magazine without advancing
  the simulation tick.
- At least one initial drone must be occluded by authored cover from the initial
  camera. Native controller movement sideways must reveal it. Cover blocks rays;
  drone lanes do not intersect cover. Allow enough time to test gameplay gates
  before incidental reactor breaches.
- Provide a logical HUD for wave, kills, ammunition, reactor health, terminal
  outcome and pause state. Compiled Pause/Resume controls return native playback
  intents. A manually ticking headless host is responsible for respecting pause.
- Compiled Save/Load controls use a durable checkpoint slot in external storage.
  A mid-wave checkpoint must restore player pose, drone identities/poses/health,
  wave/kills/ammunition/cooldown and UI, then permit continued play.

## Authoring and state constraints

Keep mutable authoritative gameplay state in native-backed scalar game state or
components. Do not depend on mutable globals, wall-clock time or an unsaved
random generator. Respect the supported scalar field kinds and lifecycle
ordering: reserved spawn IDs are not immediately readable; reads/raycasts see
committed state, while component writes, movement and removal are queued.
Kinematic lane motion is prescribed movement, not obstacle navigation.

Discover the compact catalog, invariants and relevant method/component schemas
before authoring. Compile/load the module with the matching local SDK and bridge,
configure checkpoint storage, and test through controller inputs and compiled UI
activation. Do not use live field edits or transform teleportation to fake tests.

Exercise moving drones, an occluded shot, misses, cooldown, finite ammunition,
reload, a complete nine-kill victory and a separate unattended loss. Test
mid-wave checkpoint restoration and continuation. Report failures and repair
source or authored data through the permitted tools before repeating checks.
Independent input-based verification follows the authoring exercise; an agent's
printed success statement or self-written test is not sufficient evidence.

Provide a portable manifest naming the assembly/type, scene entities, reflected
state fields, drone handles, logical UI controls and gameplay constants. Record
observed ticks, checks, limitations and known bugs in a concise result report.
Keep provider credentials, machine-specific paths and execution receipts out of
the public fixture. The single fresh session has a twenty-minute limit; avoid
repeated full discovery and unnecessary one-tick idle polling.

Do not claim production readiness, physical-input qualification, general
navigation/AI, projectile simulation or graphics quality from this exercise.
