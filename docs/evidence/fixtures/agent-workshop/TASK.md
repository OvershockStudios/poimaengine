# Workshop Relay: sanitized authoring assignment

Create and actually playtest a small first-person workshop delivery game in a
fresh Poima workspace. Use the headless engine's guarded authoring operations,
native controller/physics, typed runtime UI and compiled C# gameplay. Do not
modify engine source, write the world document directly, patch live gameplay
fields or teleport transforms to manufacture successful tests. Use primitive
geometry and local SDK/analyzer references; no external art or provider service
is required by the game.

## Gameplay requirements

- Provide a CharacterController, child Camera, collidable floor and two recipe
  stations. Use must follow the actual camera's closest collider hit within
  three meters, ignoring the player collider. Native movement/look must provide
  the test route.
- Spawn six collectible props through compiled gameplay: three kind A and
  three kind B. Give each its real kind component and collision geometry. Keep
  the player's ordered inventory in a native-backed capacity-three int32
  component buffer rather than a mutable managed collection.
- Reject collecting while full without removing the targeted pickup or changing
  inventory contents. Wrong recipes and premature use of the second station
  must preserve resources. Station one consumes A A B; station two then consumes
  A B B. Completing both deliveries wins.
- A compiled Drop control queues intent; the next Tick removes the last
  inventory item and spawns a native pickup of the matching kind. Verify
  reachable open-space placement and recollection. Do not claim reliable
  placement near walls without testing or implementing it.
- Provide a responsive HUD for inventory, delivery progress and feedback, plus
  a styled menu with Drop, Pause, Resume, Save and Load controls. Use authored
  layout/style metadata and a wine/neutral palette. Pause/Resume return native
  playback intents; a manually ticking headless host must respect them.
- Save a checkpoint after the first delivery with nonempty A B inventory.
  Restore ordered inventory, gameplay state, player and pickup poses/identities,
  and logical UI, then complete the second recipe. Demonstrate restore and
  continuation in a fresh process as well as compiled Save/Load controls.
- After victory, neutral ticks, Use and Drop must preserve the completed outcome
  and resources.

## Authoring and verification

Keep authoritative mutable state in native-backed scalar game state or registered
components so rollback and saves include it. Respect committed-read/queued-write
and spawn lifecycle rules. Discover the relevant scoped engine contracts before
editing, and use real compiler/runtime diagnostics to fix failures.

Compile and load the matching local module, configure external checkpoint
storage, and exercise the mechanics through native controller inputs and
compiled UI controls. Check camera misses/range rejection, capacity rejection,
recipe/order rejection, deferred drop, both deliveries, pause intents and durable
restore. Preserve per-kind resource conservation through pickups, drops and
deliveries. An independent input-based verifier follows the authoring session;
an agent's self-reported success is not sufficient evidence.

Provide a portable manifest naming the game assembly/type, player/camera/stations,
component fields, spawn templates, UI controls, recipes and save slot. Report
limitations candidly. This assignment describes one bounded exercise, not a
production game, general inventory framework, navigation system, physical-input
qualification or performance benchmark.
