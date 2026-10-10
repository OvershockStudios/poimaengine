# Implementation status

Updated 2026-10-10 for **0.0.89**. Capability discovery reports what is available in each build. The desktop editor and CLI are first-class interfaces over shared native authoring, a Vulkan renderer with forward and optional deferred opaque paths, a configurable physics runtime and a continuous native player. Headless builds run without the editor or its managed runtime. The gameplay SDK and broader production qualification remain incomplete.

## Current limits

Version 0.0.89 fixes Native AOT publication for games consuming the distributed
`Poima.Gameplay.dll` through a binary SDK reference. Generated entry/binding
code explicitly references the SDK copied by the selected game build; projects
that disable copying receive an actionable rejection before native generation.
The game project remains a project reference, preserving its source/dependencies.
This changes build tooling, not authoring-core v1, services epoch 7 or artifacts.

Matching-host Windows and Linux regressions publish the same unchanged compiled
UI fixture through DLL and project references, execute real native Tick/Control
callbacks, verify exact SDK/artifact hashes and reject a noncopying SDK reference.
These checks cover matched SDK/tooling cohorts, not arbitrary SDK version mixes,
clean-machine toolchain installation or complete autonomous build workflows.
See the [publishing contract](NATIVE_GAMEPLAY.md#publish),
[regression runner](../tests/native_gameplay_sdk_reference.py) and
[recorded evidence](evidence/m2-native-sdk-reference.json).

Version 0.0.88 integrates licensed audio into Relay Yard. Five original Kenney
CC0 clips are preserved with licenses, hashes and a reproducible offline
conversion recipe; native import supplies mono 48 kHz cooked audio. Eight
permanent emitters carry pickup, denied interaction, courier arrival, completion
and alternating footsteps. The pickup voice survives collectible removal.
Per-character scalar cadence uses committed displacement, a short flat-floor
support ray and bounded phase; it is not animation-driven foot contact or a
general terrain/material gait model.

Windows and Linux genuine Native AOT playthroughs each verify 5,962 native
operations, including negative interaction, all cue events, exact voice/cadence
checkpoint restoration, inert retries and completion. A newly authored silent
profile also completes with the full three-schema module manifest. The 50 global
state fields, their persistent IDs and services epoch 7/256-byte prefix are
unchanged. The audio scene is a separate content cohort; retained two-schema
0.0.86 worlds and artifacts are not silently converted.

Real controller inputs independently reproduce two footsteps and a pickup
through offline capture and continuous WASAPI playback on both laptop GPUs.
Each Windows run uses five native owners and 1,066 operations. A saved active
pickup resumes at its exact logical cursor in fresh owners; 20 neutral ticks
finish it without another allocation. Device submissions of 72,800 and 16,000
stereo frames match fresh offline mixer peaks/voice starts, drain cleanly and
report zero timeline resets and over-range samples. This qualifies submission
and mixer continuity, not listening, hardware loopback or an uninterrupted DSP
waveform across restore. Linux headless coverage uses three owners/743 operations.

Source-free Windows exports carry all five cooked audio assets, provenance,
credits and redistribution notices. Both GPU runs remove the owned source copy,
relocate the immutable bundle, restore exact voice/cadence/native game state in a
fresh service and finish the objective. Selected protocol, audio, save and build
metadata regressions pass across the three native build configurations.
[Evidence](evidence/m2-relay-yard-audio.json) and
[licensed-audio playbook](../examples/relay-yard/README.md#optional-licensed-audio).
Physical input, listening, representative performance, fresh-project agent
workflows and clean-machine delivery remain separate Alpha gates.

Version 0.0.87 fixes saves after removing played runtime emitters. Emitter
retirement legitimately leaves holes, removes the newest handle or empties
logical voice history while preserving its allocator. Restore now accepts those
states while retaining positive ordered ID bounds, trusted clip bindings,
chronology, capacity limits and atomic load validation. Sound-state format 1,
authoring-core v1, schema revision 70 and services epoch 7 are unchanged.

Windows and Linux reproduce the previous build's isolated save rejection, then
pass real hierarchical spawn/play/despawn, relocated fresh-owner restoration,
failed sound-batch rollback and allocation after complete retirement. Each
corrected run checks three scenarios through nine clean native owners and 93
operations. Native tests additionally cover retirement after history pruning
and malformed ID/allocator rejection. Both Windows GPUs submit and drain actual
WASAPI output with matching offline mixer statistics; this is diagnostic content
and device submission, not listening or physical loopback qualification.
The retained real-game upgrade also passes on the corrected host without
changing its compiled artifacts or content. [Evidence](evidence/m2-sound-retirement.json)
and [lifetime/save task](AUDIO_EVENTS.md#removing-emitters-and-saving).

That checkpoint closes a sound/save lifecycle defect; its diagnostic audio
checks do not establish whole Alpha, general acoustic realism, representative
performance or audibility. Licensed game integration follows in 0.0.88.

Version 0.0.86 gives Relay Yard persistent global field IDs and a meaningful
checkpoint version. All 49 old fields keep their kinds; the new field defaults
to 2, the actual sequential layout grows from 328 to 336 bytes, and the artifact
adds `gameplay_persistence_v1` without changing its 256-byte services prefix.
The [gameplay/content/save profile](ALPHA_GAMEPLAY_PROFILE.md) separates released
stable authoring, negotiated gameplay features, bounded content intake and exact
or explicitly upgraded checkpoints. It does not freeze every SDK API or format.

The [retained-game playbook](../examples/relay-yard/UPGRADING.md) consumes actual
0.0.85 Native AOT artifacts and version-5 root-prop checkpoints, not regenerated
source saves. Windows and Linux checks exact-restore the old game, reject
changed-image and invalid-plan loads without mutation, then preserve every old
global value and all native state through one explicit hash-bound upgrade.
Immediate observations verify the new default, target schema/image and every
reported edge identity before target callbacks. Real remaining pickups, native
courier/animation travel and terminal Use complete the objective. A deliberately
written new target slot exact-reopens in a fresh process and runs real callbacks.
Native restoration bypasses `Initialize` by the reviewed dispatch path; state
preservation is not a dynamic constructor/static-initializer trace.

Both laptop GPUs compare source/target captures at the exact saved tick and
authored overview camera. Native pixels and draw counts match, with eight draws,
1,688 triangles, one skinned instance and 1,029 weighted vertices submitted.
The checkpoint menu overlaps part of that overview; this is native output
preservation, not an independent source-pose or unobscured-character oracle.
Each Windows capture cohort passes 4,218 operations; Linux headless passes 4,198.
The updated source-free exported-game continuation also passes on both GPUs,
with 4,580 operations and seven native captures per run.
[Evidence](evidence/m2-relay-yard-upgrade.json) retains the initial verifier's
incorrect version-6 assumption as a failed attempt; actual root-prop snapshots
require version 5. The save format/admission policy was not loosened. Protocol 1,
authoring-core v1, schema revision 70 and services epoch 7 are unchanged.

This qualifies a real global-only Native AOT update, not arbitrary component or
geometry updates, upgrade chains, automatic compiled-menu migration, CoreCLR
Relay Yard upgrade, physical input, audibility, representative performance,
clean-machine delivery or whole Alpha. Ordinary menu loads remain exact and
host-selected upgrades do not silently overwrite source checkpoints.

Version 0.0.85 adds [Relay Yard](../examples/relay-yard/README.md), a complete
small compiled first-person integration game. Three real camera-ray pickups
unlock an imported Idle/Run courier's native route around cover; an in-range
terminal Use after arrival completes the objective. Compiled menus provide
checkpoints, visible save errors and current native preferences. The sample
reuses the exact original licensed FBX intake and reviewed locomotion component
layouts. It adds no engine API or schema surface.

Actual Native AOT publication is qualified on Windows and Linux. Both semantic
playthroughs pass four groups and 5,492 native operations each: welcome gating,
missing-save and negative-use outcomes, premature terminal rejection, repeated
compiled saves, guarded retries, exact partial restoration, native courier
collision/animation and completion. The partial save contains one collected
cell and a stationary courier; this checkpoint does not establish active-motion
save coverage. [Evidence](evidence/m2-relay-yard.json) preserves earlier failed
attempts and their recovery rather than relabelling them as passes.

Windows exported-game qualification runs on both laptop GPUs. Each removes the
owned source copy, relocates the immutable bundle outside the checkout and
launches from an unrelated directory with a system-only path. One native
process collects a cell and saves; a distinct native process uses Welcome Load,
restores the exact partial snapshot under its independent current player
configuration, and completes the remaining game. Seven native frame captures
per GPU observe welcome, menus, restore, Run and final Idle. Public player
identities/configuration are observed; hidden preference epochs are not claimed.
These checks do not establish physical input, audibility, representative
performance, clean-machine deployment or full Alpha readiness. Protocol 1,
authoring-core v1, schema revision 70 and services epoch 7 remain unchanged.

Version 0.0.84 removes the native player's artificial exit after 32 interactive
checkpoint replacements. Restores retain the same window and current preference
owner while clearing old input, pausing the restored game and revalidating its
controller/camera. The replacement counter saturates at `UINT32_MAX`; recording
a session change remains independent of that diagnostic count. Recorded replay
still stops when its Runtime is replaced. World/session/module resource budgets
remain separate; this is not an unlimited-lifetime guarantee.

The [checkpoint-cycle verifier](LIVE_PLAYER.md#edits-replay-and-replacement)
uses actual controller movement and look before every load, exact restored
controller/camera state, fresh sessions, guarded retries and current live
preferences. Its captures compare the same native context around restore 32.
Forty restores pass on each of the NVIDIA and AMD laptop GPUs, with 21 selected
native regressions across Windows, Linux and the authoring-only build. The
retained exported C# menu checks also pass on both GPUs.
Recorded player/audio replay checks pass on both GPUs with a freshly published
Native AOT save fixture; replay still stops on Runtime replacement.
[Recorded evidence](evidence/m2-player-checkpoint-cycles.json) retains the
reproduced 0.0.83 exit and separates corrected runs from that negative result.
The fixture does not establish physical input, audible output, memory-growth
behavior, representative performance or whole-game/Alpha qualification. Protocol
1, authoring-core v1, schema revision 70 and services epoch 7 are unchanged.

Version 0.0.83 adds the [exported game service](GAME_SERVICE.md): `game serve`
verifies the complete bundle, opens its read-only world, starts the declared
Runtime and loads its optional native gameplay at tick zero. It acquires the
local endpoint before initialization and reuses the existing owner-thread host
loop. Clients discover the current runtime, step it, activate guarded compiled
menus, adjust preferences, service external checkpoints and capture a live
player through the same native API. No editor, new action-driver language,
automatic player window or initial simulation tick is introduced.

The no-window Windows contract exercises source deletion, relocation, native
controller movement, client detach/reconnect, exact receipts, external save/load,
MCP initialization/tools, rejected authoring and malformed bundle/endpoint
recovery. The compiled service runs the retained genuine 0.0.81 Native AOT
settings consumer on both laptop GPUs. Each performs 10 menu actions and three
Vulkan captures, verifies one actual Runtime replacement and retains the same
preference owner/ticket while current application/presentation reach revision 6.
The two client connections observe the same authoritative game. These are
semantic UI operations; physical pointer hit testing remains a separate check.

Three native configurations pass 21 selected regression executions. The initial
Windows launcher failure is retained separately from the corrected remaining
checks; suites that had already passed were not repeated.

Protocol 1, authoring-core v1, schema revision 70 and gameplay services epoch 7
are unchanged. `game inspect` adds the optional bundle-root-relative input-profile
path; resolve it before explicit `player.start` parameters. Current bundle export
still requires simulation and rendering. Linux headless builds compile this
service and retain their authoring/simulation regressions; native Linux game
bundles/rendering are not newly qualified. Tiny fixtures do not establish full
Alpha, clean-machine installation, audible output, broad failure recovery or
representative performance. See [recorded evidence](evidence/m2-game-service.json).

Version 0.0.82 qualifies the [exported settings/checkpoint workflow](EXPORTED_PLAYER_SETTINGS.md)
using the retained genuine 0.0.81 Windows Native AOT consumer. The source project
is removed before launching the relocated immutable bundle from an unrelated
working directory with a system-only PATH. Native window input opens the modal,
stages preferences, saves and restores the actual player Runtime. The restored
compiled callback queries its original acceptance ticket while the same current
player retains later FOV/UI/gain configuration and successful presentation.

The verifier observes initial/modal/restored visible UI pixels and checks exact
compiled-state, durable-save, ownership, revision, native backend and final Vulkan
menu pixels. Both the RTX 4070 Laptop and AMD integrated GPU pass five check
groups, with 10 menu actions per run; three native configurations pass 15
selected regression executions. It retains seven failed debug-symbol export
and input/verifier attempts;
stripped installation keeps the shipping executable within the exporter's
256 MiB per-file budget. The development binary retains its symbols. Protocol 1,
authoring-core v1, schema revision 70 and gameplay services epoch 7 are unchanged.

This is one small exported settings fixture, not complete-game or whole Alpha
qualification. Physical devices, audible output, representative performance,
clean-machine installation and fresh-process menu/save continuation remain
separate work. Input spacing is explicit and does not itself establish redraw;
changed OS header pixels supplement the exact final semantic assertions. See
[recorded evidence](evidence/m2-exported-player-settings.json).

Version 0.0.81 adds [compiled player settings and a native menu fixture](COMPILED_PLAYER_SETTINGS.md).
C# Tick/Control callbacks read committed configuration and cached observations,
stage a guarded typed patch and query its process-local acceptance ticket. They
use the same native preference owner as the external live API. At most one patch
is prepared for an entire native step or Control boundary; failed callbacks,
entity validation and later batch failure consume no preference revision or
ticket. Successful publication precedes queued save/load replacement.

The independent `player_preferences_v1` extension uses a named 256-byte service
view with callbacks at 232/240/248, preserving epoch 7 and every earlier prefix.
A larger table grants no undeclared intermediate feature. Host negotiation is
separate from attached-owner availability. Replay permits observations and
rejects mutations; stop detaches availability, while a fresh player gets a new
owner. CoreCLR compatible reload retains that owner; Native AOT keeps its
existing process-lifetime replacement restriction. Schema revision is 70;
protocol 1 and authoring-core v1 are unchanged.

The rendered menu opens a native modal, requests pause, tunes FOV/mouse/UI/gain,
resets sparse intent, refreshes status and closes with resume. Current graphics
remain frozen while presets report next-launch intent. Preferences do not enter
authored documents or gameplay saves; saving diagnostic owner/ticket words does
not restore the authority or ledger. Profile persistence remains explicit.

Windows CoreCLR and genuine Native AOT pass attached checks on both laptop GPUs,
including independent FOV/UI image calculations, accepted-versus-staged reads,
exact retry, external stale guards, rollback of earlier completed physics,
same-window save replacement and fresh-owner/process continuation. Linux
headless callbacks pass with both backends. The unchanged original 0.0.78
Windows consumers also pass under Native AOT/CoreCLR. Managed guard-page ABI
probes pass on Windows/Linux, and the guide's C# example compiles without warnings.

These checks do not qualify physical input, audible sound, injected Jolt or
device failures, same-event-batch ordering, production performance or a new
source-free exported settings menu. The native menu capture was inspected;
logical Control activation is separate from physical mouse/controller testing.
See [recorded evidence](evidence/m2-compiled-player-settings.json).

Version 0.0.80 adds a shared native [live player-preference owner](PLAYER_SETTINGS.md).
`player.settings.inspect` and guarded `player.settings.transact` support preview,
set/reset, revision conflicts and process-local retry receipts. FOV, pointer
sensitivity/inversion, absolute UI scale and master output gain apply to the
current interactive player. Inspection distinguishes accepted configuration,
native application and successful presentation. Queued semantic look and held
input survive pointer tuning; scaled UI invalidates old gestures/hit regions
until redraw. Graphics samples/frame slots remain explicit next-player intent.

Stored profiles now admit nine keys, including output gain. Profile persistence
is independent of live edits: changing a file does not alter a running window,
and gameplay save/load does not replace its preferences. Historical launch-report
outcomes remain compatible; the live channel supplies separate revision and
next-player observations. Protocol 1, authoring-core v1 and gameplay services
epoch 7 remain unchanged; discovery schema revision is 69. No gameplay SDK or
compiled settings-menu extension is added in this checkpoint.

Three native configurations pass 67 selected regression checks; both new CTest
registrations also pass on all three. Live fixtures on both laptop GPUs pass
268 recorded RPCs with six clean owners and 18 readbacks. Independent perspective
and UI-bound calculations check actual FOV/scale changes. Preview/rejection,
reset, profile relaunch, same-window restore and semantic replay are checked;
a deferred player's invalid combined graphics/UI patch preserves state and pixels.
Actual SDL streams verify numeric gain, including mute and timeline replacement.
Both live fixtures observe an accepted change while genuinely unpaused; this is
not physical input or audible-output qualification.

Preserved shared-player tests pass 277 RPCs with eight clean owners. Stored/launch
fixtures pass 224 RPCs with 138 clean owners, independent projection/reference
images and zero-frame failure outcomes. Separately compiled original hierarchical
games pass 206 RPCs with four clean owners under Windows Native AOT and CoreCLR.
Each births two imported characters, submits 2,058 weighted vertices, continues
native movement/animation and restores exact version-6 state and same-tick output
with current preferences intact. Original worlds, cooked content and artifacts
remain unchanged. Draw submission and same-owner output equality do not provide
an independent raw-FBX/visibility pixel oracle.

The documented native Python task also executes with the checked-out client,
producing independently checked before/after images without advancing simulation
or authored revision. All these are bounded fixtures, not a full game, performance
benchmark or GUI qualification. Post-acceptance device-failure recovery is
source-reviewed, without forced device-failure injection. Retained failures cover
test include/setup/guard errors, initial SDL surface settling and the corrected
legacy launch-outcome regression. [Evidence](evidence/m2-live-player-settings.json).

Version 0.0.79 extends [explicit save upgrades](SAVE_UPGRADES.md) to complete
hierarchy recipes, version-6 runtime snapshots and bounded collection fields.
Stable field IDs preserve saved values across renames and layout changes;
retired fields and added scalar/empty-array defaults require a complete approval
plan. Plan version 2 can explicitly change retained array capacity, preserving
length/order and rejecting overflow rather than truncating. Plan version 1 and
the scalar-only helper retain their strict existing contracts.

The mapper visits authored, template root/child and live-instance fields.
Ordinary source validation checks every saved field and entity reference before
retirement; approved transformation and normal target restoration finish before
publication. Native physics, animation, sound, UI, complete local-to-live maps,
structure revisions and allocator history remain unchanged. Exact loads remain
the default; upgrade plans are host-selected, identity-bound one-edge approvals.
Schema revision is 68; authoring-core v1, protocol 1 and gameplay service epoch 7
are unchanged. No gameplay SDK or service layout change is required.

Three native configurations pass 71 selected regression checks. Refined
snapshot tests also check growth, fitting shrink and runtime-only overflow with
valid source/target native reconstruction on Windows/Linux simulation builds;
the authoring-only build checks mapping without simulation. Four separately
compiled retained-save cohorts pass 1,040 RPCs with 28 clean owners under
Windows/Linux CoreCLR and Native AOT. They verify legacy-global ID binding,
renamed/reordered component fields, changed defaults, entity-array growth,
compiled callbacks, same/fresh-owner continuation, whole-root retirement/rebirth,
retry recovery and rejection isolation. A real compiled capacity-one target
rejects authored-recipe overflow; the native fixture separately covers saved
runtime overflow. The original source slots, artifacts and cooked assets remain
unchanged. An earlier scalar compiled consumer still passes 49 RPCs.

Optional readbacks on both laptop GPUs match source/target output at the same
tick, but their retained origin camera draws zero character geometry. They
qualify capture/context/output preservation, not character visual preservation.
Exact saved transforms, instance maps, animation and unchanged cooked geometry
provide separate state checks. Retained verifier failures document raw-native
JSON checksum handling and a corrected publisher-wrapper feature expectation;
no engine checksum or compatibility check was weakened. These are bounded
correctness fixtures, not automatic migrations, game-scale performance,
physical-input, GUI or an exported upgraded-game qualification.
[Evidence](evidence/m2-hierarchical-save-upgrades.json) and
[compiled fixture instructions](../tests/fixtures/instance_save_evolution/README.md).

Version 0.0.78 adds [hierarchical runtime instances](RUNTIME_INSTANCES.md).
Frozen recipes contain complete local entity graphs; each spawn allocates fresh
IDs and remaps local native references and registered custom entity/array fields.
Characters, cameras, imported rigs/skins, lights and audio join atomic membership
publication. Whole-root removal requires surviving reference repairs. Failed
batches restore membership, physics, animation, sound, typed gameplay and ID
allocation; successful canceled births consume their reservations. Version 6
checkpoints retain complete instance maps and allocator history. Exact restoration
and the explicit schema upgrades described above preserve that graph state.

The independent `hierarchical_instances_v1` extension uses a 232-byte service
prefix at epoch 7. C# `TemplateNodeId` and `ResolveNode` distinguish local recipe
identities from live handles. Tick can resolve reserved members and stage supported
commands; ordinary reads still see committed membership. Control resolves only
committed members. Older 176–224-byte profiles remain supported. Schema revision
is 67; authoring-core v1 and protocol 1 remain unchanged.

Three native builds pass 56 selected regression checks, including complete
membership, animation rebind, rollback, references, saves, nested asset closure
and simulation-disabled authoring. Both managed ABI cohorts pass 75 checks each.
Four real compiled original-FBX instance cohorts pass 8,944 RPCs with eight clean
owners under Windows/Linux CoreCLR and Native AOT. Independent quaternion-chain,
joint-transform and weighted-geometry calculations check two physically moving
actors with different playback rates; save continuation, cancellation, failure,
reference repair, deletion and reload policy are also checked. Original source
sampling uses the native FBX importer; this is not an independent FBX decoder.

Generated player controller/camera admission passes on both laptop GPUs. A
relocated Windows Native AOT game runs after removing its owned source project,
spawns two complete characters from its compiled first Tick and submits 2,058
weighted vertices. Its readbacks match exactly with a separate
source-free runtime using the same compiled artifact. This comparison is separate
from the original-source pose oracle. Retained 176-byte Windows UI/save and
224-byte Linux locomotion artifacts still pass on the new host. Initial fixture,
build-path and missing UI-guard failures are retained with their corrections.
These are bounded correctness and scripted graphics checks, not crowd,
physical-input, GUI, clean-machine or game-scale performance qualification.
[Evidence](evidence/m2-runtime-instances.json). The [fixture instructions](../tests/managed_instance_gameplay/README.md)
provide reproducible compiled and exported checks; desktop prefab/variant authoring
and arbitrary child or authored-entity deletion remain separate work.

Version 0.0.77 adds a [frame-driven shared native player](LIVE_PLAYER.md).
`player.start` acknowledges a deferred graphics lifetime; `player.inspect`
reports readiness separately. Two independent clients can inspect, pause,
perform guarded edits or steps on an interactive runtime, and request fresh
captures through the same graphics context. Capture does not advance simulation.
Generation, identity and control guards protect owner changes; 32 bounded retry
receipts retain original command outcomes. Replay supports observation,
pause/resume and cancellation without accepting intervening runtime edits.
Save load resolves the replacement runtime through the stable owner adapter,
clears stale input/UI/audio state and pauses interactive presentation.
Stopping presentation retains the runtime and terminal report. Mutation APIs
require a shared headless host; standalone and desktop discovery omit them.

The owner finishes its accepted request batch before one player frame, without
recursive request dispatch inside simulation. The existing blocking player uses
the same native frame driver. GPU initialization, readback, audio, imports and
storage can still stall serialized requests; this is not a nonblocking or hard
latency contract. On-demand captures reserve swapchain transfer capability and
allocate readback staging lazily; ordinary player frames skip that copy.
Schema revision is 66; authoring-core v1, protocol 1 and gameplay service epoch 7
remain unchanged.

Three native builds pass 47 selected authoring, input, shared-session, discovery,
MCP, lifecycle and metadata regression checks. Two shared-player and two retained
Windows Native AOT UI cohorts pass 558 RPCs with 14 clean owners. Twenty-two
Windows Vulkan readbacks across AMD and NVIDIA cover the new lifecycle fixtures
and runnable observation recipe, including changed pixels after actual stepping
or compiled UI edits and exact saved/restored images. The separate native fixture
checks deferred cancellation, replay tick consumption, replacement and device
failure recovery. Existing blocking replay, targeted window-message, audio,
settings and runtime-replacement checks pass, as does the preserved 0.0.75 Linux
Native AOT locomotion game with 5,064 RPCs. Initial capture preparation and mock
camera failures are retained with their corrections. These are small scripted
fixtures, not physical-input, response-time or game-scale performance claims.
[Evidence](evidence/m2-live-player.json).

The [documentation coverage inventory](DOCUMENTATION_COVERAGE.md) maps supported
mechanisms to manual/reference entries, task recipes and checks. It explicitly
records missing human workflows, generated native/C# reference, searchable
versioned documentation and evaluated agent skills. Current documentation is
not a complete developer manual.

Version 0.0.76 adds [portable player settings](PLAYER_SETTINGS.md): eight
sparse, typed overrides for vertical FOV, pointer sensitivity/inversion, absolute
UI scale, MSAA and outstanding graphics submissions. Profiles have independent
revisions, preview/reset, persisted retry receipts and guarded file replacement.
They store intent outside authored worlds, input bindings and saved gameplay.
Player launch resolves inherited/profile/session/explicit-option precedence,
applies the existing native owners and reports effective values, sources and
presentation outcomes. Inherited FOV follows the final native camera; explicit
FOV does not mutate camera data. Semantic replay validates pointer preferences
without reinterpreting recorded controls. Packaged games load external profiles
without writer sidecars; live settings and a C# settings/menu API remain separate
work. Schema revision is 65; authoring-core v1, protocol 1 and gameplay service
epoch 7 remain unchanged.

Three native builds pass 33 selected input, world-session, discovery, authoring,
MCP and metadata regression checks. Four retained settings cohorts pass 28
protocol groups, 394 RPCs and 246 clean owners; the repeatable CTest protocol
gate passes in all three builds. Eight Windows Vulkan readbacks across AMD and
NVIDIA verify independent camera projection, exact authored-reference pixels,
absolute UI pixel bounds and actual sample/submission limits. A relocated
source-free Windows game consumes external preferences, honors explicit CLI
choices and rejects stale revisions and duplicate override keys without changing
its bundle or profile sidecars. The existing 0.0.75 Linux Native AOT locomotion
game passes 5,064 RPCs against the new runtime. These fixtures do not qualify
physical input, live menus, clean-machine installation or game-scale performance.
[Evidence](evidence/m2-player-settings.json).

Version 0.0.75 adds [Locomotion Yard](../examples/locomotion-yard/README.md),
a compiled character game using an untouched original CC0 FBX body and separate
Idle/Run takes. Explicit rotation retargeting preserves original target geometry,
rest data and inverse binds, with declared HipsCtrl/Hips position deltas.
The native capsule owns travel and navigation; a child visual wrapper aligns
forward axes and standing height without rescaling the source body. Both clips
advance and loop. Run speed follows horizontal displacement from the previous
committed tick, divided by an authored 3 m/s playback reference. Physical corner
slowdown exercises real rate changes. Rate-only corrections preserve the observed
clock and wait for active inertial fades to complete, because the existing
playback command replaces the full animation state.

Windows/Linux CoreCLR and Native AOT pass the original-source joint/weighted
geometry oracle, cumulative playback/wrap checks, native physical detour,
compiled controls and exact midfade same/fresh-owner checkpoint continuation.
CoreCLR compatible reload preserves live state; Native AOT replacement retains
its documented rejection policy. Windows captures observe idle, run and delivered
idle on both laptop GPUs. A relocated Windows Native AOT player starts at tick
zero after its owned authoring project is removed; a separate bundled-runtime
contract then verifies native courier poses, clocks and saves. The first player
report exposes courier observations through compiled state and renderer counts,
not direct courier joint/clock output. [Evidence](evidence/m2-locomotion-game.json).

The 3.76 m imported standing height is retained, with a 0.42 m capsule radius.
The 3 m/s rate reference is sample configuration, not automatic stride fitting.
The original Run closes its pose but is not velocity-continuous at the seam;
foot locking, IK, contact correction, loop repair, root-controller extraction,
motion matching and a broad character-exporter profile remain unfinished.
The original source decoder is the native importer; the rotation/FK/skin oracle
is separate math, and weighted vertices are calculated from actual runtime
joints rather than read directly from GPU buffers. These small fixtures do not
qualify physical input, editor usability, clean-machine installation or game-scale
performance. Authoring-core v1, protocol 1, schema revision 64 and gameplay
service epoch 7 remain unchanged. The new task playbook documents the verified
profile without changing the historical Character Yard example.

Version 0.0.74 adds [explicit rotation retargeting](ANIMATION_RETARGETING.md)
for matching named ancestry with differing proportions, reference stances and
positive nonuniform scales. Original reference quaternion chains define the
orientation transfer; target-reference local positions/scales stay fixed unless
selected source-local position deltas are explicitly requested. Target geometry,
rest defaults and inverse binds remain unchanged. Read-only `asset.source.inspect`
exposes original nodes, takes, reference poses and normalized-model fingerprints,
including geometryless donors. Required fingerprint guards reject changed
original models before filtering or retargeting. Focused discovery reports
schema revision 64; authoring-core v1, protocol 1 and native gameplay services
epoch 7 remain unchanged.

Each of three native builds passes eight independent retargeting groups and
six API tests, including original Kenney Idle, Run and Jump files. The combined
2,379 RPCs and 21 clean owners check quaternion chains, target FK and original
weighted geometry, strict policies, unpublished inspection, failure atomicity
and source-independent cooked persistence. Windows Vulkan checks pass 22
readbacks across AMD and NVIDIA using the original 1,029-vertex weighted body,
separate Run/Idle takes and 15 ordinary runtime ticks. The Run pose and runtime
match the independent normalized-source reference exactly on AMD; NVIDIA differs
at one pixel above channel error two, with maximum channel error 17.
One AMD Idle GPU/CPU pixel has channel error 13. These are bounded readback
comparisons, not universal rendering tolerances. Existing exact composition,
frame conversion, discovery, authoring and MCP checks pass, as does the preserved
0.0.71 Linux Native AOT character game. Automatic bone-name mapping, contact/IK,
stride and loop repair and root-motion extraction remain unfinished. The
compiled FBX locomotion game is qualified separately in 0.0.75. The source oracle uses original native-normalized
observations with independent retarget/FK/skin math, not an independent FBX
parser. [Evidence](evidence/m2-animation-rotation-retarget.json).

Version 0.0.73 adds opt-in [reference-pose bone-frame conversion](ANIMATION_FRAME_TRANSFER.md)
for separate clips with matching named ancestry, coincident reference joint
origins and exactly uniform mapped scales. Choose original rest or sampled
reference poses and an optional rigid component alignment. The converter
re-expresses translation/rotation curves and cubic tangents, synthesizes missing
constant properties where needed, and preserves target geometry, hierarchy,
rest transforms and inverse binds. Default exact-skeleton composition stays
unchanged. Focused discovery reports schema revision 63; authoring-core v1,
protocol 1 and native gameplay services epoch 7 remain unchanged.

Each of the three builds passes eight independent native conversion groups
and five API tests. The combined 570 RPCs and 18 clean owners compare known
joint matrices and original skin weights, check original reference selections
before take filtering, reject unsupported conversions without publication and
reopen cooked motion without source files. Windows Vulkan checks pass 16
readbacks across AMD and NVIDIA: GPU/CPU preview, independently calculated
geometry/normals and 30-tick runtime playback agree exactly in this analytic
triangle fixture. Existing exact-composition diagnostics, ASCII/binary FBX,
focused discovery, stable authoring and MCP contracts pass. The existing 0.0.71
Linux Native AOT Character Yard artifact also passes against the new runtime.
Different proportions, contact/stance retargeting and a representative locomotion
library remain unfinished; these checks do not establish game-scale performance
or physical input. [Evidence](evidence/m2-animation-frame-transfer.json).

Version 0.0.72 gives exact-skeleton animation-composition failures structured
`error.data`: the first incompatible donor, source-local node identities,
missing-node roles, component mismatch flags and local TRS measurements. The
report keeps complete counts and at most 64 issue records. Focused discovery
publishes its strict schema at revision 62, and the Python client preserves
the native payload. The [recovery guide](FBX_IMPORT.md#inspect-a-composition-failure)
explains the existing thresholds, normalized local-rest comparison and safe
next steps. It does not infer bind poses or retarget clips.

Linux runtime, native Windows runtime and simulation-disabled authoring each
pass eight native diagnostic check groups and six protocol tests. The combined
306 RPCs and 21 clean owners cover original analytic TRS inputs, missing nodes,
bounded records, same-owner recovery and fresh-owner persisted state. Failed
imports preserve authored state, history and the populated asset store.
Successful cooked composition remains unchanged. Existing ASCII/binary FBX,
scoped discovery and stable authoring-contract checks also pass. Preserved
0.0.71 Linux Native AOT Character Yard gameplay passes against the new runtime.
Eight real stdio MCP tests on each OS verify complete text/structured error
payloads, atomic rejection and same-owner recovery alongside existing contracts.
This checkpoint does not add locomotion clips or qualify retargeting, new GUI
controls or graphics performance. [Evidence](evidence/m2-animation-composition-diagnostics.json).

Version 0.0.71 adds [Character Yard](../examples/character-yard/README.md), a
compiled C# game using the original imported figure. Its camera-free capsule
plans around solid cover with native navigation; the attached rig retains all
22 source nodes and 19 joints. An explicit wrapper aligns the model's forward
axis with the controller. Movement advances the single source take, while
Gesture replays it once in place. The source motion opens the arms; it is not
a locomotion clip. Pause/resume, dispatch and save/load use real compiled
callbacks and native results. The sample authors a compact wine-styled footer.

Five matching-version cohorts pass on Linux CoreCLR/Native AOT and Windows
CoreCLR/Native AOT, with both laptop GPUs used for the native Windows captures:
7,772 RPCs, 40 check groups, ten clean owners and eight Vulkan readbacks.
Each cohort observes 21 stable source poses, with maximum joint-matrix error
1.065e-8 against an independent GLB calculation. Per-tick physics checks verify
detour, speed and arrival. Immediate checkpoints, grouped/unequally partitioned
ticks and fresh-owner continuation retain complete compared native, typed,
gameplay and UI state. Continued comparisons exclude only two named host
save-epoch identity fields in addition to session IDs. Compatible unchanged-DLL
reload passes; Native AOT replacement rejects without altering state.

Native UI layout checks establish all seven buttons' bounds and hittability at
960×640 and 512×288 in presenter-enabled Windows builds. Linux headless builds
exercise logical controls without claiming presentation. These are virtual
layout and scripted command checks, not physical mouse/controller qualification.

A separately published Windows Native AOT artifact runs from a relocated
61-file exported bundle after its owned source project is removed. The player
starts with empty gameplay values and completes delivery over 1,400 ticks on
the NVIDIA GPU, with no reported NVRHI errors or runtime replacements. Its
source-free run precedes a separate 1,552-RPC pose/save contract using the bundled
runtime and newly authored content. The six bundle-verifier commands and three
owned native processes exit cleanly; bundle bytes and source attribution remain
intact. The host has .NET installed, so environment isolation and relocation do
not establish clean-machine distribution. This small flat yard does not qualify
locomotion libraries, retargeting, general save migrations or game-scale
performance. [Source, hashes and scope](evidence/m2-character-game.json).

Version 0.0.70 accepts bounded matrix-authored glTF nodes by decomposing them
into the existing positive-scale TRS profile. It preserves hierarchy and inverse
bind frames, reports converted nodes and rejects unsupported transforms or
matrix/TRS animation conflicts. The [transform policy](ASSETS.md#matrix-authored-transforms)
includes exact affine constraints and explicit numerical tolerances.

The [imported-character playbook](IMPORTED_CHARACTER.md) qualifies an unchanged,
licensed 19-joint human figure with independent source calculations, authored
rest frames, four native frozen-playback seeks and fresh-owner cooked closure.
Linux runtime, Windows runtime and simulation-disabled Linux pass 694 RPCs with
eight clean owner exits. Both laptop GPUs pass 24 Vulkan captures; the largest
GPU/reference error is one edge pixel. Native tests include unequal scales,
180-degree rotations, nested static/skinned frames, source-independent decode
and numerical-boundary admission. Existing asset, animation, FBX and stable
authoring-contract regressions pass separately. [Evidence](evidence/m2-gltf-matrix-intake.json).

This is bounded character intake and observation. Compiled character gameplay,
save/export continuation, locomotion clips, retargeting and broader exporter
compatibility are not established by this checkpoint.

Version 0.0.69 integrates a shared [native job executor](JOBS.md) into animation
sampling and procedural material baking. Each creating owner lazily obtains a
bounded pool, with two workers by default and an explicit 0–8 worker policy.
Frame/background admission is separate, and one worker is reserved for frame
work when at least two exist. Registry writes, Jolt physics, C# callbacks,
material file publication and renderer recording remain on their owners.

Animation workers sample independent rigs into private candidate histories and
poses. Owner assembly preserves stable ordering; failure or cancellation cannot
commit a partial sample. Material callbacks own frozen inputs and CPU outputs;
owner cancellation can veto an already-completed result before publication.
Terminal groups fence capture cleanup before releasing admission capacity.
Named worker intervals preserve source/session/tick/parent and actual OS thread
IDs in profiler inspection and trace export. Late records cannot enter a new
recording.

Five integrated native consumer suites pass on Windows and Linux, including
15 scheduler groups and exact serial/0/1/2/4/8-worker animation comparisons.
Linux ASan/UBSan and TSan pass scheduler and worker-profiler checks. Both OSes
pass the existing compiled layer fixtures under CoreCLR and preserved Native
AOT: 2,028 RPCs, 32 check groups and 16 clean owner exits in total. The NVIDIA
GPU passes 36 original-fixture Vulkan captures against 18 exact independent pose
references, with zero reported NVRHI errors. The simulation-disabled build also
runs the installed-client guarded-editing playbook and the manual's C++ DAG
example. These are bounded correctness checks, not production game-scale or
physical-input qualification. [Jobs evidence](evidence/m2-native-jobs.json).

The [task playbook index](PLAYBOOKS.md) names current prerequisites, completion
checks and recovery guides. Agent skills remain a later compact entry point to
qualified workflows; comprehensive manual/reference coverage is a release
requirement rather than a claim of present completeness.

Version 0.0.68 adds bounded [FBX model import](FBX_IMPORT.md), including static
and linear-skinned geometry, metric/axis normalization and transform takes.
Separate animation sources can select and rename clips through exact normalized
hierarchy/rest-frame matching. This is not retargeting; mismatched ancestors or
bone bases reject. The profile rejects unsupported deformation/materials and
invalid attribute indices, reports narrowly permitted collapsed-UV tangent
repairs, and preserves the default glTF cooked identities.

Windows and Linux each pass 36 original fixtures in both ASCII and binary
encodings, 27 asset/animation/authoring-core regression tests, and 1,067 scoped
schema responses. FBX protocol checks pass seven Linux tests over 194 RPCs and
six Windows tests over 183 RPCs; the POSIX invalid-byte filename test is skipped
on Windows. All 17 recorded FBX owners exit cleanly. Staging recovery preserves
pre-existing files/links, and filesystem errors remain bounded UTF-8 diagnostics.
Six Windows Vulkan captures on the AMD integrated GPU match CPU and independent
geometry references, including 30 runtime ticks after source removal and a fresh
owner. General exporter compatibility, retargeting, compiled FBX-character
gameplay and game-scale import/render performance remain unqualified.
[Recorded evidence](evidence/m2-fbx-import.json).

Version 0.0.67 separates base API schemas from the world owner. Both runtime-enabled
Linux and renderer/UI-enabled Windows builds preserve all 1,067 scoped discovery
responses byte for byte, including read-only and shared-editor filtering. Fresh
standalone/shared CLI sessions preserve complete replies, state, history and
world bytes, then shut down and reopen cleanly. Nine selected Linux CTest cases
and seven Windows native/contract commands pass. Compilation graph probes
isolate each source to its own unit without changing compiler flags. Recorded
command timings are observations, not a controlled speedup benchmark.
[Build details](BUILD.md#world-service-and-schema-compilation),
[evidence](evidence/m2-world-schema-extraction.json).

The 0.0.66 Windows desktop passes 232 scripted editor actions and four restart
checks, plus 97 independent dual-view actions and three restart checks. Both
Scene and Game render on the RTX 4070 Laptop GPU with zero reported NVRHI
errors. The README now shows an inspected OS capture of this editor. These are
semantic-input checks; physical-device testing and wine branding remain pending.
[Desktop evidence](evidence/m2-desktop-current.json).

Version 0.0.66 adds immutable [asset source/license records](ASSET_PROVENANCE.md)
and guarded world selections. Frozen runtime/save sources retain their selected
records; component upgrades preserve those selections. Exports carry checked
records and deterministic credits, reject notice tampering and require a
runtime advertising provenance support. Explicit unused mappings retain their
records without adding unused cooked assets. Unmapped legacy bundles retain
their earlier shape.

Actual Windows and Linux checks each pass seven authoring/save groups over
238 RPCs with 12 clean owners, six export groups over 53 RPCs with six clean
owners, and five native programs. Linux additionally passes ten selected
regressions; Windows passes the stable authoring-core and identity suites.
Records are caller declarations, not verified permission or importer-captured
source traces. The export fixtures deliberately use non-executable runtimes;
this does not qualify player deployment, online acquisition or graphics
performance. [Recorded evidence](evidence/m2-asset-provenance.json).

Version 0.0.65 records qualification of the original compiled navigation fixture
in an exported, relocated Windows Native AOT game using the 0.0.64 runtime.
The player loads gameplay at tick zero and
completes 1,400 Vulkan replay ticks: one complete six-corner plan, NPC arrival,
zero NVRHI errors and no runtime replacement. The owned source is removed and
the bundle's exact runtime, artifact and navigation inventories remain unchanged.
The original fixture has no visible meshes; this qualifies the hardware player
lifecycle and compiled planning/arrival, rather than a visible-NPC demonstration.

Separate fresh worlds use the same bundled runtime and artifact for eight checks
over 987 RPCs, including native pose detours, query quotas, rollback, saves,
fresh-owner continuation and replan. All native commands and owners exit cleanly.
The recorded runtime and Native AOT artifact are both from 0.0.64. This does not
qualify clean-machine deployment, physical input, crowds,
performance, GUI authoring or in-player save UX. [Shipping evidence](evidence/m2-navigation-shipping.json),
[reproduction](../tests/managed_navigation_gameplay/README.md#exported-windows-player).

A default-off [Linux development option](BUILD.md#local-linux-thin-archives)
changes only the core static archive's representation. It retains compiler
optimization/debug settings and dependency archives. The speed measurements
cover archive creation/indexing on one filesystem, not compilation, general
iteration latency or game performance. Automatic online acquisition and importer-captured source traces remain
planned; [owned-file imports](ASSETS.md) are available for the documented formats.

Version 0.0.64 binds static navigation into compiled NPC gameplay. A guarded
`navigation.set` transaction associates the world with a checked `.pnav` asset.
Binding, runtime freeze, export and saved-source restore validate the relevant
static geometry and package bytes. Unbound worlds retain their earlier content
shape; binding changes participate in preview, retry, history and undo/redo.

C# `INavigationGame` enables live-character route queries during Tick. The
independent named extension appends a callback to the 224-byte services-7 view,
preserving the existing 176/192/208/216-byte profiles. Queries use an immutable
mesh and bounded scratch, with eight native attempts per Tick and explicit
incomplete-route statuses. Games own their route/cursor in existing components
and steer through ordinary character input; navigation does not teleport actors.

CoreCLR and actual published Native AOT each pass ten compiled follower checks
over 1,009 RPCs with five clean owner exits on Linux, and nine checks over 998
RPCs with four clean exits on Windows. The fixture physically detours
around cover, reaches its goal, enforces query limits, rolls back a late failed
batch, and resumes a persisted route in a fresh owner. CoreCLR checks compatible
reload; Native AOT checks replacement rejection. Six project tests verify content
and license closure using metadata-only runtime/artifact fixtures, separately
from real compiled gameplay execution on both operating systems. [Navigation contract](NAVIGATION.md),
[compiled fixture](../tests/managed_navigation_gameplay/README.md),
[evidence](evidence/m2-runtime-navigation.json).

Navigation remains a static ground-planning foundation. Dynamic obstacles,
crowds, traversal, streaming and editor navigation tools are unfinished; these
fixtures do not establish game-scale performance or clean-machine deployment.

Version 0.0.63 adds optional static navigation through pinned Recast/Detour.
Revision-guarded native baking, inspection and path queries use authored static
box and indexed mesh colliders, including physical holes. Profiles control
capsule clearance, slope, climb and voxel resolution. Routes distinguish complete,
partial, unreachable, output-budget and node-budget results, with explicit
endpoint projections. Content-addressed packages contain checked neutral arrays;
loaded meshes are immutable and source fingerprints reject stale topology.

Windows and Linux each pass native navigation checks and six protocol groups
over 70 RPCs, with eight clean native-owner exits per platform. A disabled build
passes availability checks while intentionally skipping five enabled-only groups.
Selected regressions cover eleven Linux groups, ten authoring-only groups and
ten Windows execution groups; the existing CoreCLR patrol sample also passes
its four gameplay checks. Native regressions reproduce and fix partial mesh
initialization cleanup, malformed-path diagnostics and the reserved neighbor-bit
polygon boundary. Discovery revision 57 retains protocol 1, authoring-core v1,
existing gameplay service profiles and save formats.

The 0.0.63 checkpoint covers authored ground-navigation infrastructure; runtime
binding and compiled queries were added in 0.0.64. Patrol Room still uses its
authored route and direct steering. Moving obstacles, avoidance,
crowds, traversal links and streaming remain separate work. Endpoint projection
uses a bounded linear scan; bake/query memory admission limits are not a game-scale
CPU or latency guarantee. [Navigation API](NAVIGATION.md),
[evidence](evidence/m2-navigation.json).

Version 0.0.62 adds a compiled CPU recipe baker for tileable brick and plaster.
Versioned recipes produce correlated base-color, tangent-normal and packed
metallic/roughness images with existing mip cooking. Bounded jobs support
inspection, cancellation and forgetting; generation leaves authored revisions
and history unchanged. Applying the suggested material uses the ordinary guarded
transaction API. Runtime bundles retain image dependencies, while recipe
manifests remain authoring data.

Windows and Linux runtime builds pass six detailed protocol groups each, covering
channel encoding, job retention, reopening, guarded application, undo, references,
export closure and shared/read-only sessions. Native checks cover independent
flat-channel and slope oracles, periodic surfaces, odd mips, failed publication,
UTF-8 diagnostics and worker cleanup. AMD and NVIDIA each pass seven rendered
captures and eight fixture assertions, including a flat normal control, generated
normal products, repeated pixels, two viewing distances and a curved tangent
fixture, with zero reported NVRHI errors.

This is a small built-in material generator, not a general graph or an
artist-replacement benchmark. UVs and tangent frames must be authored; normal
mips are not variance-aware. Recipe inspection validates descriptor/package
coherence rather than authenticating derivation. Same-profile regeneration
checks actual generated output; universal cross-compiler byte identity and
power-loss durability are not claimed. Discovery advances to revision 56;
protocol 1, authoring-core v1, gameplay service profiles and save formats are
unchanged. [Material workflow](PROCEDURAL_MATERIALS.md),
[evidence](evidence/m2-procedural-materials.json).

Version 0.0.61 adds camera-free CharacterControllers and compiled per-tick
movement, look and jump through `ICharacterInputGame` and `SetCharacterInput`.
Commands stage before physics, do not appear as caller inputs and do not change
same-tick reads. Caller input owns its target for the whole requested batch;
conflicts and later failures restore the batch's complete observed state.
Interactive player selection still requires a camera-bound controller.

Windows and Linux each pass six actual CoreCLR checks over 237 RPCs and five
published Native AOT checks over 213 RPCs. They cover wall contact, neutral
ticks, jump, invalid commands, second-tick rollback and durable fresh-process
continuation; CoreCLR also covers compatible and failed reload. Each OS passes
65 SDK/bridge ABI guard groups using protected allocations and mock callbacks.
These guard tests do not qualify arbitrary malformed Runtime callback storage.

The independent named 216-byte services-7 profile retains the original
176/192/208-byte profiles. Preserved 176-byte Native AOT Tick/Control fixtures
and original 208-byte layer artifacts execute on both OSes. Actual 0.0.60 versus
0.0.61 player input and save comparisons pass 108 RPCs per OS. Discovery advances
to revision 55 for nullable camera bindings; protocol 1, selected authoring-core
v1 and save formats remain unchanged. Navigation, acceleration and production
locomotion remain separate work. Native AOT libraries remain process-pinned.
[Character API](CHARACTER_INPUT.md), [evidence](evidence/m2-character-input.json).

The accompanying Patrol Room sample passes four native-input groups over
1,483 RPCs on each OS, with two clean owned exits per cohort. Checks exercise
actual capsule patrol, physics-ray occlusion, noise investigation, visual chase,
earned extraction, capture, compatible reload and exact fresh-process save
continuation. AMD and NVIDIA Vulkan cohorts each pass four rendered readbacks
over 1,531 RPCs, with zero reported NVRHI errors. These are primitive-room
gameplay checks, not general navigation, physical-device or performance evidence.
[Sample](../examples/managed/PatrolGame), [evidence](evidence/m2-patrol-game.json).

Version 0.0.60 adds `world.asset.references`: typed model/image/audio bindings
by authored entity or template, with inverse asset lookup and revision-pinned
pages. Queries can inspect missing or corrupt package references without loading
assets. They read the current authored document while gameplay retains its frozen
content, and support shared and read-only sessions.

Windows and Linux runtime builds, plus an authoring-only Linux build, each pass
eight protocol groups with clean owned exits. Selected native/MCP/core checks
and stable authoring request, response and identity gates pass. Discovery moves
to revision 54; protocol 1 and selected authoring-core v1 are unchanged. The new
query is outside that stable contract. Effective material graphs, live spawned
references, source-file tracking and editor reference panels remain separate work.
[Reference API](ASSET_REFERENCES.md), [evidence](evidence/m2-asset-references.json).

Version 0.0.59 isolates generated build metadata in one native translation unit.
Version-only dependency probes schedule one compile instead of seven; ordinary
source changes and relinking retain their costs. Compiler optimization, warnings
and debug settings are unchanged. Native metadata checks verify typed CLI data
against actual ELF/PE targets and match MCP initialization and exported profiler
trace versions on Windows, Linux and an authoring-only Linux build.

Selected native/protocol checks pass after a stale profiler schema pin is
corrected. Preserved Native AOT layer fixtures pass eight groups over 501 RPCs
with four clean exits on each OS. Stable discovery/identity and response gates
pass. Protocol 1, discovery revision 53, authoring-core v1, gameplay service
profiles and save formats remain unchanged. The dependency probes are scheduled
compile counts, not general elapsed-time or game-performance measurements.
[Build workflow](BUILD.md#incremental-version-builds),
[evidence](evidence/m2-build-metadata.json).

The 0.0.58 checkpoint exposes masked animation layers to compiled C# gameplay.
`IMaskedAnimationGame` opts into the named `animation_layers_v1` extension;
`GetAnimationLayer` reads committed state and `SetAnimationLayer` stages complete
clip/weight commands. Override/additive definitions, masks and reference poses
remain frozen at runtime start. Commands share the existing 64-command tick
budget with base animation and participate in whole-batch rollback.

Windows and Linux each pass eight real CoreCLR checks over 513 RPCs and eight
published Native AOT checks over 501 RPCs, with four clean owned exits per
cohort. Checks cover analytic layered motion, conflict/budget rejection, active
transition save/restore, immediate compiled re-interruption and complete-payload
rollback. CoreCLR additionally covers compatible/failed reload. Native AOT
libraries remain process-pinned. Both platforms pass 59 SDK/bridge ABI guard
groups; these use mock native callbacks and do not qualify malformed raw Runtime
callback storage.

The original 176-byte and inertial 192-byte services-7 tables remain intact;
layer access uses a separate 208-byte named extension and dedicated versioned
command/state records. Existing 192-byte Native AOT animation artifacts pass
435 RPCs per OS; an unchanged 176-byte Windows inventory artifact passes 151
RPCs. Actual 0.0.57-versus-0.0.58 unlayered comparisons pass 1,036 RPCs per OS.
Twenty-two selected Linux groups and sixteen Windows groups pass. Authoring-only
checks and all three stable discovery/identity gates pass. Protocol 1, discovery
revision 53, authoring-core v1 and animation save formats remain unchanged.

Requirement validation precedes game construction in supported matched
bridge/SDK cohorts. Arbitrarily mixing an older bridge with a newer SDK is not
supported and is outside that guarantee. Desktop layer widgets, IK, root motion,
retargeting and game-scale performance remain unfinished. This checkpoint adds
no installed desktop, GPU, physical-input, console or browser qualification.
[Managed layer API](MANAGED_GAMEPLAY.md#control-masked-layers-from-c),
[evidence](evidence/m2-managed-animation-layers.json).

The 0.0.57 checkpoint adds up to four frozen masked animation layers per rig,
with ordered override/additive composition, independent playback and motion
history, clip transitions and linear weight ramps. Native commands and joined
observations expose the layers; rollback and nested animation-state v3 saves
retain their clocks, transitions, weights and exact mask/reference identity.
Unlayered v1/v2 saves retain their previous bytes and behavior.

Windows and Linux each pass eight layer protocol checks over 146 RPCs and nine
clean owned exits. An actual 0.0.56-versus-0.0.57 comparison passes 1,036 RPCs on
each OS, including byte-identical unlayered saves and fresh-process continuation
with immediate re-interruption. The unchanged compiled C# fixtures pass ten
CoreCLR checks (486 RPCs) and seven Native AOT checks (435 RPCs) per platform;
these fixtures remain base-only and do not qualify C# layer access.

Both laptop GPUs pass 36 exact reference pairs across 72 captures at 640×480
with 1×/4× MSAA and zero reported NVRHI errors. Sixteen selected Windows checks
and seven authoring-only Linux groups pass. The initial Linux cohort passed 17
of 19 groups; two legacy discovery-shape failures were fixed, and all six affected
groups passed their final rerun. A fresh final layer and legacy comparison also
pass. This is not a fresh simultaneous rerun of all nineteen groups.

Discovery advances to revision 53. Protocol 1, selected authoring-core v1 and the
176/192-byte gameplay services-7 tables remain unchanged. Bounds of four slots,
128 rigs and 20,000 aggregate full-model layer nodes are admission limits, not
measured throughput. Maximum configurations and the 64 MiB outer-save ceiling
are not jointly qualified. At that checkpoint, compiled C# layer controls,
desktop layer widgets, IK, root motion and retargeting remained unfinished.
That checkpoint added no physical-input or game-scale performance qualification.
[Layer contract](RUNTIME_ANIMATION.md#masked-animation-layers),
[evidence](evidence/m2-animation-layers.json).

The 0.0.56 checkpoint adds opt-in C# inertial animation through
`IInertialAnimationGame`, typed mode selection and extended committed-state
queries. A named 192-byte services-7 extension preserves the original 176-byte
baseline and seven-parameter animation setter. Numeric-time calls, including
`0` and `default`, still compile against the old overload. Negotiated requirements
remain outside the state schema and its save fingerprint. Unsupported declared
requirements reject before game construction, `Initialize` and `Tick`; arbitrary
assembly/module initialization and native loader side effects are outside that
guarantee.

Windows and Linux each pass 54 SDK/bridge ABI guard groups, ten real CoreCLR
checks over 486 RPCs with six clean owned exits, and seven actual Native AOT
checks over 435 RPCs with four clean owned exits. These cover analytic motion,
staged writes, limits/conflicts, whole-batch rollback and fresh durable restore
with immediate compiled re-interruption. CoreCLR additionally checks compatible
and failed reload, preserved older compiled gameplay and old-host rejection.
Native AOT checks descriptor omission and retains process-pinned libraries;
compatible native reload is not supplied. An unchanged 0.0.52 Windows inventory
artifact also passes its 151-RPC contract on the new runtime.

Eighteen selected Linux native groups and four authoring-only groups pass. Stable
authoring discovery/identity gates pass for Windows/Linux runtime and Linux
authoring-only builds; selected response baselines remain compatible. Protocol 1, discovery revision 52 and authoring-core
v1 remain unchanged. Mock ABI fixtures do not qualify malformed raw Runtime
callbacks. There is no new GPU, installed desktop, physical-input, clean-machine
or production performance qualification. Desktop animation controls still select
crossfades; layers, IK, root motion and retargeting remain unfinished.
[Managed example](MANAGED_GAMEPLAY.md#control-animation-from-c),
[native artifact contract](NATIVE_GAMEPLAY.md#artifact-contents),
[evidence](evidence/m2-managed-inertial.json).

The 0.0.55 checkpoint adds opt-in native inertial animation transitions. Recent
output motion and analytic incoming clip derivatives initialize bounded,
finite-time translation, quaternion and positive-scale corrections. Same-tick
samples retain distinct-tick history; checkpoints and nested animation-state v2
preserve it through rollback, restoration and immediate re-interruption.
Crossfade-only saves retain version-1 bytes, and an actual 0.0.54 interrupted
crossfade save continues unchanged on the new Windows host.

Native/math, save and protocol checks pass on Windows and Linux. The Linux
regression cohort passed nineteen groups initially; a stale discovery-revision
assertion was updated and passed its focused rerun. Seven authoring-only groups
also pass. Across both laptop GPUs, 96 captures produce 24 exact inertial reference
pairs and 24 legacy crossfade pairs at 640×480 with 1×/4× MSAA and zero reported
NVRHI errors. Existing compiled C# crossfade/reload/rollback checks pass in twelve
checks per platform. Discovery advances to revision 52; authoring-core v1 and the
176-byte gameplay services ABI 7 remained unchanged. At that checkpoint C# and
desktop controls selected crossfades; inertial access used C++ or the world protocol.
Measured secant continuity does not guarantee smooth clip discontinuities, foot
contacts, phase matching or zero overshoot. No game-scale performance, new
installed desktop or physical-input qualification is claimed.
[Animation contract](RUNTIME_ANIMATION.md#inertial-transitions),
[evidence](evidence/m2-animation-inertial.json).

The 0.0.54 checkpoint adds `runtime.observe`: a bounded, exact-tick read joining
selected live entity state and native custom-component fields in one serialized
owner dispatch. Metadata is returned once per selected type; optional revisions
reject stale observations and guard query continuations. Nine protocol tests pass
on both Linux and Windows, alongside shared/read-only scope checks and an actual
authoring-only build. Discovery advances to revision 51; the selected stable
authoring-core v1 contract, gameplay ABI and save formats remain unchanged.

One provider-free replay comparison uses the same compiled Workshop Relay game
and final Windows host for both read paths. Native RPCs decrease from 2,549 to
1,292 (49.3%); compact reserialized request/result JSON bodies decrease by 11.7%
and 16.9%, respectively. Thirteen game checks, thirteen route events and six
complete checked snapshots match, with only opaque runtime session IDs
normalized. Each mode closes both owned native processes cleanly. This measures
request count and defined JSON-body size, not provider tokens, latency or game
performance. Byte-budget overflow, stale nonzero compiled gameplay/control pins and
physical input remain unqualified.
[Observation protocol and reproduction](RUNTIME_OBSERVATION.md),
[evidence](evidence/m2-runtime-observation.json).

The 0.0.53 checkpoint retains a third external-agent game exercise: Codex authors,
compiles and playtests Workshop Relay with generated capacity-three int32
inventory, six native spawned parts, ordered recipes, deferred Drop, typed styled
UI and compiled Pause/Resume/Save/Load controls. Independent original-binary
verification passes 2,550 requests; a fresh Windows public-source rebuild passes
2,549 requests on the final 0.0.53 host. Both restore a nonempty `[A,B]` checkpoint,
complete reflected state, remaining native identities/poses and logical modal UI
through compiled Load and a fresh process, then finish the second delivery.
Two inspected NVIDIA readbacks show the wine-styled checkpoint menu and completed
HUD. This is one bounded exercise with primitive geometry, not general autonomy,
physical controls or performance qualification. The generated game's fixed Drop
position lacks wall/overlap validation, and its Save label acknowledges acceptance
before storage completion; independent tests check actual durable completion.
True interaction occlusion, nonzero pending-intent saves and Linux/Native AOT
replay of this game remain unqualified.
[Retained source and replay](evidence/fixtures/agent-workshop/README.md),
[evidence](evidence/m2-agent-workshop.json).

The 0.0.52 checkpoint qualifies the existing generated capacity-four entity/int32
component buffers on Windows Native AOT. Publication uses the real fixture and
extracts the same component manifest as the CoreCLR build. Its unchanged 151-RPC
contract passes compiled mutation, capacity rejection, byte-exact rollback and two
fresh-process save continuations. Exported games run seven ticks after relocation
outside the checkout on AMD and NVIDIA, with zero reported NVRHI errors and an
unchanged 56-file bundle. Separate save checks also pass through each bundled
runtime; these use their own fixture world and do not demonstrate in-player save
UX. Native PE payload checks and disabled dynamic-code flags are recorded; the
host has .NET installed, so clean-machine deployment remains unqualified. No
runtime implementation, gameplay ABI or catalog change is introduced. [Native collection evidence](evidence/m2-component-collections-native-windows.json).

The 0.0.51 checkpoint separately qualifies these buffers on Windows CoreCLR with
matching SDK/generator/bridge builds, compiled writes and two fresh-process save
continuations. Global-state collections, arbitrary managed arrays and capacity
migration remain unqualified. [Collection contract](COMPONENT_COLLECTIONS.md),
[CoreCLR evidence](evidence/m2-component-collections-windows.json).

The 0.0.50 checkpoint adds bounded typed runtime UI layout/style authoring and
`world.ui.layout` for virtual-viewport geometry inspection without a window or GPU.
Explicit layouts support anchored HUDs and responsive flex menus; optional colors,
font/border settings and state colors override the compatibility fallback.
Frozen metadata participates in save content identity; absent metadata stays absent.
Presentation input follows visual order and rejects confirmation through an
occluding panel. Theme inheritance, images, widgets, animation, accessibility and
visual UI authoring remain unfinished. These UI interfaces are outside the selected
stable authoring contract. [UI contract](GAME_UI.md#authored-layout-and-styling).
Fifteen selected Linux headless and ten native-layout groups, seventeen Windows
check commands and both GPU capture suites pass. Seven old/new default UI
capture pairs on AMD are pixel-identical to 0.0.49; eighteen styled captures
verify three extents at density 1 on AMD and NVIDIA. Existing compiled Windows
CoreCLR/Native AOT control fixtures also pass. This is native readback and
synthetic-input evidence, without physical-device qualification. [Evidence](evidence/m2-authored-ui.json).

The 0.0.49 checkpoint releases a selected **authoring-core v1** boundary: nine world/entity methods, entity create/rename/reparent/delete and Transform edits. Native discovery identifies the contract and mode across full/focused views. Canonical request and response artifacts retain the earlier candidate schemas; their paths and hashes are pinned separately. A dedicated output-compatibility gate proves recognized producer-to-consumer restrictions, while native conformance covers RPC IDs/errors, retained retries after edits/restart, true hierarchical shear, query boundaries and failed undo/redo storage. Read-only scopes reject even valid mutations and retained receipts before replay. This does not freeze runtime/rendering/gameplay/save APIs or world formats, or establish alpha readiness. Windows/Linux service and installed-client qualification are recorded in [release evidence](evidence/m2-authoring-core-v1.json). GUI/GPU qualification is outside this checkpoint. [Contract](AUTHORING_API_COMPATIBILITY.md).

The 0.0.48 discovery view selects a mutation operation and optional component type, keeping the full transaction envelope and applicable session metadata. Native tests compare every advertised operation and component selection with full discovery, reject malformed/unavailable selectors without editing state/history/files, preserve mixed transactions, and enforce read-only restrictions in standalone and both shared scopes. MCP exposes these selectors; the updated Python client validates selection echoes, writable metadata and envelope shape. The candidate baseline stays unchanged. A conservative closed-object extension proof qualifies additive discovery requests, and reference-bearing schemas retain their full operation unions to preserve indexed targets. Windows wire measurements show a Transform mutation response of 1,318 bytes versus 27,779 for the complete method, and MCP 2,910 versus 60,276. Counts exclude line delimiters and do not measure provider tokens or development speed. Rebuilt Windows/Linux hosts and installed Python wheels have bounded qualification; broader API stability, fresh provider turns and GUI/GPU presentation are outside this checkpoint. [Discovery contract](WORLD_SERVICE.md#focused-discovery-development), [client](PYTHON_CLIENT.md), [evidence](evidence/m2-scoped-mutation-discovery.json).

The 0.0.47 headless owner waits for local transport progress after dispatch instead of repeatedly sleeping. Windows servers use persistent overlapped operations; Linux servers use eligible socket readiness. Native regressions cover fragmented frames, synchronous completion, budget continuation, deferred pipelined requests, all eight occupied client slots, slow-reader fairness and pending-operation destruction/reuse. Windows shared-host, SDK, MCP and ten native desktop ABI checks pass; seven selected Linux contract groups and both candidate request gates pass. Four matched Windows trial pairs reduce shared query median from 16.03 ms to 0.159 ms (p95 16.59 ms to 0.256 ms). Single five-second idle intervals record Windows host CPU time 0.046875 s before and zero delta after, and Linux 0.07 s before and 0.01 s after. Zero accounting deltas do not prove zero CPU work or energy use. The unchanged compiled defense game passes 3,208 calls in 2.28 s, preserving all seven tick and gameplay-value summaries and exact checkpoint restoration and clean exits from six owned processes. This is bounded same-machine evidence, not game FPS, GUI responsiveness, battery life or general agent success. Windows cancellation/reaping can exceed wait deadlines; the headless owner retains a 100 ms maintenance ceiling. Desktop dispatch cadence is unchanged. [Transport and probes](SHARED_SESSIONS.md#transport-and-limits), [checkpoint evidence](evidence/m2-host-readiness.json).

The 0.0.46 Windows client waits on overlapped pipe completion instead of sleeping and retrying during exchanges. One absolute deadline covers partial reads and writes; failure cancels and reaps pending I/O before releasing buffers and closes the connection without replay. Kernel cancellation cleanup can exceed that deadline. Native Windows tests identify pending read/write timeouts directly and cover disconnects, fragmented responses and fresh endpoint recovery; shared-host, Python SDK, MCP and ten native desktop ABI checks also pass. Four matched Windows trial pairs measure shared read median 31.4 ms before versus 15.9 ms after, with an old-build repeat at 32.2 ms. The unchanged compiled defense game passes 3,208 shared-transport calls and reproduces every previously recorded checkpoint and win/loss outcome; all six owned processes exit cleanly. These are one-machine authoring reads, not game FPS or GUI responsiveness. Seven selected Linux transport/client/core-contract test groups and the Windows/Linux candidate request gates pass. Server polling, startup retries, Linux socket waits and editor dispatch cadence are unchanged. [Transport contract](SHARED_SESSIONS.md#transport-and-limits), [checkpoint evidence](evidence/m2-client-completion-waits.json).

The 0.0.45 checkpoint attempts bounded reply delivery immediately after an owner dispatch, retaining partial writes for later polls. Native transport checks pass on Windows and Linux, including immediate small/empty frames, slow-reader progress and pipelined ordering. A new regression rejects the old queued-only backend. Four alternating paired Windows authoring trials measure shared query median 47.6 ms before versus 31.8 ms after; this is one-machine read latency, not game FPS or GUI responsiveness. SDK, shared-host and MCP checks pass; the native desktop ABI passes ten checks without GPU attachment. Client retry sleeps and editor dispatch cadence remain unchanged. [Transport contract and benchmark](SHARED_SESSIONS.md#transport-and-limits).

The 0.0.44 checkpoint adds a second external-agent game exercise: Claude Code creates, compiles and headlessly playtests a three-wave defense game through native MCP authoring. Independent native controller inputs check cover occlusion, two-hit drones, finite ammunition, cooldown, reload, victory, unattended loss and complete checkpoint continuation in a separate process. The [public fixture](evidence/fixtures/agent-defense/README.md) retains the generated C# source unchanged and provides a Python replay runner. This is Windows CoreCLR headless qualification with primitive geometry and prescribed kinematic lanes; graphical presentation, physical input, navigation, Native AOT and Linux execution of this fixture remain unqualified. [Exercise and replay evidence](evidence/m2-agent-defense.json).

The 0.0.43 checkpoint adds an installable [Python automation client](PYTHON_CLIENT.md), with explicit owned-process/endpoint transport, guarded mutation helpers and revision-pinned pagination. A separate candidate response manifest validates the nine core authoring methods without rejecting legacy retained receipts or equating a replay revision with current state. Native standalone/shared-session behavior, synthetic transport failures and independent malformed-response checks pass on Windows and Linux. Local installed-wheel checks qualify packaging; this does not qualify GUI/GPU/provider integration or freeze broader runtime/gameplay/save contracts. [Evidence](evidence/m2-python-client.json).

The 0.0.42 checkpoint adds a [candidate authoring-core API gate](AUTHORING_API_COMPATIBILITY.md). Its nine selected methods and Transform preserve the schema46 request baseline on rebuilt schema47 Windows and Linux hosts. Eleven compatibility tests and nine scope-projection tests pass, including overlapping exclusive branches and unresolved references. Native checks cover identity defaults, isolated previews, guarded revisions and retry behavior. This is preparation for a stable authoring contract, not a released guarantee for the full engine. [Evidence](evidence/m2-authoring-api-compatibility.json).

The retained [agent-built escape game](evidence/fixtures/agent-escape/README.md#rendered-checkpoints) now has three inspected Windows hardware Vulkan readbacks: locked exit, completed escape and restored checkpoint. Controller input and compiled save callbacks pass, with zero reported NVRHI errors and unchanged capture state. The slideshow is recorded agent authoring followed by fresh rendered replay, not live authoring or a real-time video. Primitive geometry and mid-word HUD wrapping remain visible limits. [Evidence](evidence/m2-agent-game-rendered.json).

[Structured compiler feedback](DEVELOPMENT_JOBS.md#structured-compiler-feedback-development), introduced in 0.0.41 with discovery schema47, now passes real SDK success/failure checks through rebuilt native world hosts on both platforms. Twelve focused Linux test groups pass; Windows world/history/UI checks and native read-only/shared-scope checks pass. All six development methods are hidden and rejected in read-only worlds before parameter validation, without modifying bundle files. Newly exported player qualification is not implied. [Host evidence](evidence/m2-development-host.json); [earlier standalone evidence](evidence/m2-development-diagnostics.json). Earlier changelog entries retain their original contents and qualification scope.

The [0.0.40 changelog](../CHANGELOG.md) collects development since the earlier 0.0.39 editor package. That source checkpoint and its editor project metadata identified 0.0.40. The full Linux rebuild passes after header dependency cleanup; 71 of 73 Linux test groups pass, including gameplay packaging. At that checkpoint, `local_session_native` and `shared_session_contract` could not bind sockets under the restricted environment. Both subsequently pass after filesystem/process restrictions were lifted; this is a focused rerun, not a new full-suite result. After rebuilding all optional baseline objects and confirming a clean Ninja dry run, renderer-header dependencies fall from 77 to 17 across the same Linux objects (60 removed). This measures dependency fanout; no compile-time speedup is claimed. [Dependency evidence](evidence/m2-build-dependencies.json). The Windows engine/desktop-bridge rebuild and optional graphics fixtures compile successfully. Its renderer-header dependency count falls from 85 to 31 objects (54 removed), with a real Ninja no-op build confirming freshness. Experimental [ambient occlusion](AMBIENT_OCCLUSION.md) has passing Linux API/schedule contracts and bounded hardware capture, reconstruction lifecycle and UI checks on both laptop GPUs under Vulkan synchronization validation. The default forward renderer matches 16 saved reference captures per device exactly; general quality and performance remain unqualified. [AO evidence](evidence/m2-ambient-occlusion.json). Earlier evidence below retains its original checkpoint scope and does not constitute a complete 0.0.40 release qualification.

The subsequent [focused discovery](WORLD_SERVICE.md#focused-discovery-development) change passes Linux protocol and native authoring/read-only scope tests. Clients can list names or request one schema/contract section; full discovery remains the default. Its Windows engine, desktop bridge and native test fixture rebuild also passes; Windows execution remains unqualified. [Evidence](evidence/m2-focused-discovery.json).

A fresh external Codex-authored escape-room exercise now passes compiled C# gameplay and independent headless replay: locked exit, three raycast pickups, pause/resume, checkpoint restoration and a second completion. The portable source fixture separately passes Windows CoreCLR replay and durable checkpoint continuation in a fresh native process, using its rebuilt DLL and cached compatible engine. This is one game with logical UI; rendering, physical input, Linux/Native AOT fixture replay and performance remain unqualified. [Original exercise](evidence/m2-agent-game.json), [public replay and source](evidence/m2-agent-game-replay.json).

The development [native compile-worker foundation](DEVELOPMENT_JOBS.md) passes standalone process contracts on Linux and Windows, plus standalone adapter retry/diagnostic contracts on both operating systems and real Linux SDK builds. World-service source integration is staged; full host and packaged read-only execution remain pending. [Initial worker evidence](evidence/m2-development-jobs.json), [current adapter and diagnostic evidence](evidence/m2-development-diagnostics.json).

The [native MCP interface](MCP.md) passes Linux protocol, real stdio authoring/persistence and CLI/world regression checks. Real stdio authoring and shared headless endpoint tests now also pass on Windows and Linux, including cross-client receipts and safe detachment. Bounded real Codex/Claude CLI authoring exercises now pass, with independent trace and persisted-state verification. [Agent-client evidence](evidence/m2-agent-authoring.json). A candidate Windows desktop Agent window now passes one Codex chat turn that creates exactly one guarded entity through the editor-owned endpoint, with missing-executable recovery and unchanged existing entities. Physical input, approval-card interaction, interrupt completion, Claude chat and broad game creation remain unqualified. [Workspace evidence](evidence/m2-agent-workspace.json). [Initial evidence](evidence/m2-native-mcp.json), [cross-platform endpoint evidence](evidence/m2-mcp-shared.json). Opt-in PNG image content now passes Linux codec/MCP tests and archived SDL capture conversion; fresh Windows GPU images now reach both agent CLIs, which pass a simple undisclosed-color identification exercise. This does not establish general visual reasoning. [Observation evidence](evidence/m2-agent-observation.json). [Image evidence](evidence/m2-mcp-images.json).

Optional [single-sample deferred rendering](DEFERRED_RENDERING.md) passes bounded analytic lighting, shadows, clustered-light fallback, sky, skinning, motion, reconstruction and UI checks on both laptop GPUs under strict Vulkan validation. The default forward path retains exact saved-reference image comparisons. Deferred adds 32 bytes of material storage per render pixel; expanded products remain allocated. These checks establish integration correctness, not a performance gain, GI or game-scale readiness. [Evidence](evidence/m2-deferred-rendering.json).

Optional [FSR 3.1.4 reconstruction](RECONSTRUCTION.md) is implemented for the Windows Vulkan renderer, disabled by default. Native AA and three fixed upscaling ratios pass bounded HDR, reset/resize, independent-view, capture API and UI-composition checks on both laptop GPUs under strict Vulkan validation. Existing rendering with reconstruction disabled retains exact reference comparisons. The separate SDK-disabled renderer builds and rejects unavailable FSR requests. Static edge improvement coexists with persistent near-edge color residue in moving scenes; temporal quality and performance are not qualified. Frame generation, DLSS and live-player timing qualification remain unfinished. [Evidence and limitations](evidence/m2-reconstruction.json).

Single-sample development rendering provides [depth, world-space shading normals and backward UV motion](SCENE_PRODUCTS.md), with explicit surface/correspondence validity and up to 64 raw pixel probes per capture. Per-view history follows accepted submissions, including camera/object movement and prior skinned positions; source replacement, cuts and changed renderable lifetimes invalidate correspondence. Strict Vulkan synchronization validation passes on both laptop GPUs: each passes 80 native captures, including interleaved views, plus four agent-service captures. Nine Linux contract groups, 221 valid schedule configurations and 328 rejection checks pass. This establishes bounded geometric correspondence, not occlusion rejection, temporal antialiasing, upscaling or game-scale performance. Multisample products and full floating-point image export remain unfinished. [Evidence](evidence/m2-scene-motion.json).

Reconstruction timing now separates live players/editor views from frozen capture and input replay. The clock commits dispatch timestamps only after accepted submission, clamps live intervals and restarts on history reset. Synthetic clock tests pass on Linux and Windows; live-player image quality and pacing remain unqualified. [Timing checks](evidence/m2-temporal-clock.json).

The earlier [depth/normal checkpoint](evidence/m2-scene-products.json) records 26 analytic display captures per GPU, exact single-sample color comparison and continuous resize/recreation checks. Its quantized BMP checks are separate from the new raw pixel probes.

The development renderer now executes a [typed pass schedule](RENDER_SCHEDULE.md) that validates actual resource identities, initialization, usage and frame-slot ownership before recording. Portable checks cover 204 valid variants and 250 rejection checks. On both laptop GPUs, the triangle and 16 scene captures match the preceding renderer exactly under Vulkan synchronization validation; ten renderer, eight UI/player/hosted-view and four legacy editor layout groups pass. This retains the existing rendering order and does not add GI or automatic pass parallelism. [Evidence](evidence/m2-render-schedule.json).

The development renderer now uses bounded one- or two-frame submission with per-frame completion, query and readback ownership. Strict Vulkan and synchronization validation pass on both laptop GPUs: 16 captures per GPU preserve exact reference parity, all 49 changing-frame results per mode retain submission attribution, and warmed draw phases add no device-wide idle waits. Ten renderer regression groups, eight UI/player/hosted-view groups and eight Linux headless contract suites pass. GPU scratch remains ordered, uploads can still stall, and presentation cleanup differs by device; this is correctness qualification, not a performance gain. [Contract](RENDER_DIAGNOSTICS.md#submission-and-presentation-lifetime), [evidence](evidence/m2-frame-retirement.json).

The development [scene HDR composition path](HDR_COMPOSITION.md) now passes a full Windows build, 174 scene captures and eight UI/player/native-viewport integration groups across both laptop GPUs. Floating-point scene color resolves MSAA before one exposure/output transform; UI remains independent of exposure. This is bounded correctness evidence, not performance qualification. HDR monitor output, GI and post-processing effects remain unfinished. [Evidence](evidence/m2-hdr-composition.json).

[Explicit save upgrades](SAVE_UPGRADES.md) connect stable-ID mapping, authored-content validation and snapshot transformation to guarded external `save.load`. The earlier scalar checkpoint qualified Linux CoreCLR continuation, component changes, rejection isolation and recovery; its [integration record](evidence/m2-save-upgrade-integration.json) retains that scope. Hierarchical and collection evolution has separate 0.0.79 qualification above. Automatic gameplay/editor upgrade selection remains unfinished; ordinary restores remain exact. The earlier cross-platform [primitive evidence](evidence/m2-save-upgrade-primitives.json) covers mapping/plan guards only.

The latest development [C# lifecycle checkpoint](GAMEPLAY_LIFECYCLE.md) passes five compiled integration groups under both CoreCLR and Native AOT on Linux and Windows. Agent-service tests cover guarded births, durable restoration and continued gameplay; the CoreCLR path also verifies compatible reload with spawned entities. This is root-prop support, not general entity/component lifecycle. [Qualification evidence](evidence/m2-managed-lifecycle.json).

[Custom components](CUSTOM_COMPONENTS.md) provide native-owned per-entity data with stable IDs and generated C# accessors. Spawned root props can carry template-defined components during Play; development [bounded collections](COMPONENT_COLLECTIONS.md) now pass Linux native and real CoreCLR gameplay/save tests. Windows CoreCLR and Native AOT capacity-four entity/int32 buffers are now qualified separately. Arbitrary component addition/removal on existing entities and general schema/save migrations remain unfinished. Services ABI 7 now has a bounded compatibility contract; upgrading from earlier epochs still requires matching rebuilt artifacts.

The development [standalone template catalog](RUNTIME.md#standalone-template-catalog-development) adds transactional root-prop recipes without requiring live prototype objects. Runtime catalog inspection uses a frozen authored revision, and template-only models/textures participate in content dependencies. A guarded world-service transaction can create and remove spawned root props at paused boundaries, with session-scoped retry receipts. Version 3 snapshots preserve those spawned objects and generated-ID history. Native hosts can schedule guarded structural edits within an atomic multi-tick batch. The [C# lifecycle API](GAMEPLAY_LIFECYCLE.md) reserves IDs, initializes registered components, starts kinematic movement and removes spawned props within Tick. It uses the services ABI 7 baseline (176 bytes); earlier service epochs require coordinated rebuilding. RPC tick scheduling, arbitrary authored-entity removal, desktop template authoring and a dedicated generated-prop hierarchy/Inspector remain unfinished. [Service and desktop lifecycle evidence](evidence/m2-runtime-lifecycle-service.json). Tick-guarded RPC mutations also require the observed structure revision after membership changes; editor component, gameplay and animation drafts retain it. Linux and Windows pass the focused guards and save regressions, and Windows passes five native desktop checks. This checkpoint does not qualify the full GUI or activate a new desktop package. [Structure-guard evidence](evidence/m2-runtime-structure-guards.json).

[Keyboard/mouse profiles](INPUT_PROFILES.md), [gamepad profiles](GAMEPADS.md), configurable bindings, device discovery and hosted editor assignment are implemented. Physical gamepad qualification remains outstanding. General settings remain unfinished. The experimental [save-slot service](RUNTIME.md#durable-save-slots) adds durable generations, explicit recovery and guarded runtime replacement to the native snapshot foundation. The [editor Save/Load window](EDITOR_SAVES.md) now uses that service. [Typed gameplay requests](GAMEPLAY_SAVES.md) use a post-batch native owner. General migrations and asynchronous save scheduling remain unfinished. Authored document recovery and internal runtime rollback are separate contracts.

Cinematic authoring, comprehensive post-processing and a general package manager are not implemented. Bounded [FBX import and exact-skeleton clip composition](FBX_IMPORT.md), reference-frame conversion and [explicit rotation retargeting](ANIMATION_RETARGETING.md) are supported; general Mixamo/exporter compatibility and automatic anatomical/contact retargeting remain unqualified. Supported asset formats and limits are documented in [Assets](ASSETS.md) and [Animation assets](ANIMATION_ASSETS.md).

The [native profiler](PROFILER.md) now provides bounded shared captures, an editor CPU timeline, subsystem summaries, native job-worker intervals, separate GPU duration samples and trace export. General worker-stack sampling, allocation/GC and process/VRAM tracking, GPU clock correlation and a sustained benchmark workflow remain unfinished. Existing [render diagnostics](RENDER_DIAGNOSTICS.md) also retain capture/player aggregates and draw counters. Scene transform handles are implemented; orientation/axis-view controls, camera frustums and light/component icons remain planned. The current Console is a log tab beside Project.

Planned editor workflow improvements include configurable hierarchy/folder styling, component isolation/search/copy-paste, focused object/asset tabs, persistent favorites, fullscreen panels, configurable smooth navigation, hover highlighting and overlap selection, a live preferences inspector, clickable breadcrumbs/back-forward history/context locking, and integrated Git/GitHub workflows. Existing striping, automatic icons, basic geometry picking and dock panels cover only the initial subset.

Runtime collision supports boxes, capsule controllers and explicit [static triangle meshes](MESH_COLLISION.md), preserving openings present in source geometry. Moving/deforming mesh colliders, distinct movement versus weapon-query shapes, finite-radius projectile sweeps and texture-cutout collision masks remain unfinished. Contacts use triangle front faces; rays hit both sides. Numeric/resource bounds and synthetic fixture results do not establish exact arithmetic or game-scale collision performance.

Animation supports bounded two-pose crossfades, opt-in native inertial transitions and ordered masked override/additive layers, including opt-in compiled C# controls. Legacy fade interruption freezes the current pose. Inertial mode adds decaying corrections that preserve estimated distinct-tick output motion; it does not guarantee smooth clip discontinuities or foot contacts. State machines and transition-history scrubbing, IK, root-motion extraction and automatic anatomical/contact retargeting remain unfinished. Internal batch rollback is not retained simulation history or a time-travel debugger.

## Implemented

- **Persistent gameplay metadata (development):** opt-in C# global-state fields carry stable IDs and literal defaults through CoreCLR, Native AOT, inspection and exact snapshots. Native artifacts declare a metadata capability that export and bundle inspection enforce. Both platforms pass malformed-schema, managed parity, compiled default/initialization and exact-save continuation checks; preserved old binaries still run. This supplies identities for explicit upgrades, not automatic save migration. [Contract](GAMEPLAY_PERSISTENCE.md), [evidence](evidence/m2-gameplay-persistence-metadata.json).

- **Save source validation (development):** native hosts can validate complete source snapshots without loading their old gameplay executable. Exact restore shares that validation and still requires the trusted backend/image/type/schema match. Windows and Linux pass native data checks and compiled CoreCLR/Native AOT restore regressions, including initialization guards, 16 rejection cases and 40 restore iterations per backend. This prepares explicit save evolution; cross-version migration remains unfinished. [Contract](RUNTIME.md#portable-runtime-snapshot-foundation), [evidence](evidence/m2-save-source-validation.json).

- **Gameplay compatibility (development):** service epoch 7 preserves its 176-byte baseline when a host appends opaque fields. Native artifact v2 declares required call layout, service prefix and features instead of requiring an exact producing-engine version; known 0.0.39 v1 artifacts remain supported. Windows and Linux tests execute preserved old CoreCLR and Native AOT binaries, including an old game with the new bridge/SDK. Managed prefix-validation tests pass on Windows and Linux; 37 Windows package/protocol checks and compiled UI control regressions pass. Saved schema/content/image checks remain exact, and future tail-dependent services require explicit negotiation. [Contract](NATIVE_GAMEPLAY.md), [evidence](evidence/m2-gameplay-compatibility.json).

- **Native UI input (development):** the player and Game viewport route pointer, keyboard and assigned-gamepad events through the displayed native layout into compiled controls. Interactive menus may omit a CharacterController; hosted input supports a camera-only binding. Stale visible UI consumes its own hits and held releases without executing callbacks or taking over clicks outside nonmodal controls. Focus, resize, reassignment and runtime replacement cancel gestures. Focused Linux document/presenter tests and Windows compiled desktop checks pass; physical devices and full Avalonia interaction remain unqualified. [Input contract](GAME_UI.md#player-and-game-view-input), [evidence](evidence/m2-ui-input.json).

- **Collection Room sample (development):** a small C# game combines actual capsule movement, camera raycasts, template-spawned collectibles, removal, native HUD controls and durable checkpoints. Windows CoreCLR and Native AOT checks collect all three items, restore the player and remaining objects, and continue in a fresh process. Both laptop GPUs produce initial/completed Vulkan readbacks. An exported bundle completes the scripted room from an unrelated working directory with zero reported Vulkan errors and unchanged bundle inventory. This is primitive sample geometry, not a performance benchmark; physical input, Linux sample execution and cross-version save migration remain unqualified. [Source and reproduction instructions](../examples/collection-game), [evidence](evidence/m2-collection-room.json).

- **Compiled UI controls and native presentation (development):** guarded button activation invokes C# `Control` at an unchanged physics tick, stages UI/gameplay changes and save intentions atomically and retains retry receipts across Load replacement. Successful controls advance gameplay and control revisions, protecting older Inspector/save drafts. This checkpoint introduced the services ABI 7 baseline; older service epochs require matching rebuilt artifacts. Snapshot v5 preserves the control sequence and reads older v4 UI saves with sequence zero. Native default layout feeds named-camera rendering; desktop owners apply Pause/Resume intents once. This checkpoint preceded live input routing; general layout authoring and deployment qualification remain unfinished. [Contract](GAME_UI.md), [bounded qualification](evidence/m2-ui-control-presentation.json).

- **Authoritative UI state (development):** native panel/label/button definitions now support world transactions, history, frozen runtime inspection and atomic text/visibility/enabled/modal edits at an unchanged simulation tick. Snapshot v4 preserves this state alongside spawned props and custom components; UI-free saves retain versions 1–3. Linux passes ten selected runtime suites and five authoring-only suites. Windows passes three native suites and 27 protocol test cases; the linked C# Save model verifies same-tick UI freshness and retained retry guards. At this checkpoint logical state and RmlUi presentation were separate, without automatic binding, action execution or C# UI callbacks. Live input routing was not part of that checkpoint. [Contract](GAME_UI.md#authoritative-controls), [evidence](evidence/m2-native-ui-state.json).

- **Native game UI presentation foundation:** optional pinned RmlUi/FreeType layout produces owned geometry and glyph atlases for the common Vulkan composition pass. Registered labels/buttons support inspection, literal text updates, visibility/enabled state, focus and returned semantic actions. Native checks pass on Windows/Linux; both laptop GPUs pass numeric captures at 1×/4× MSAA and the text HUD capture. The separate logical UI service supports authored definitions, same-tick guarded state edits and portable saves. That checkpoint did not include automatic presentation binding, C# callbacks, paused action execution or live player/desktop input routing. The first backend rejects unsupported effects and requires sRGB composition. [Contract and bounds](GAME_UI.md), [evidence](evidence/m2-native-game-ui.json).

- **0.0.39 editor audio:** opt-in Game-camera sound output uses committed snapshots, bounded worker DSP, owner-thread SDL submission, guarded Audio controls and asynchronous shutdown. All 51 runtime and 42 authoring-only Linux suites pass; Windows passes native audio/lifetime, playback/input and packaged C# checks. Audio controls pass 67 actions on each laptop GPU, with 232 general editor actions plus four restart checks. First device initialization remains synchronous: one Windows poll took 666 ms on the shared test machine. Physical listening and end-to-end latency remain unqualified. [Usage](EDITOR_AUDIO.md), [evidence](evidence/m2-editor-audio.json).

- **0.0.38 editor gamepads:** hosted SDL input now shares native profiles and fixed-tick evaluation with the player, without dispatching the editor’s Windows message loop. Stable evaluator ownership and staged device acquisition preserve existing controls on failed changes. Game Input settings expose explicit defaults/profile choice, assignment and neutral-gate status; Start releases capture only. All 49 runtime and 40 authoring-only Linux suites pass. Windows passes 18 SDL groups, seven direct bridge groups/66 RPCs, and existing keyboard/mouse and playback regressions. The final package passes 55 focused actions on each laptop GPU, 232 general actions plus four restart checks, and exact-package C# ABI/integration checks with 100 reloads. Virtual devices, hidden HWND capture-timeout tests and synthetic GUI controls do not qualify physical controller behavior. [Usage](GAMEPADS.md#desktop-editor), [evidence](evidence/m2-editor-gamepads.json) and [attached settings render](evidence/m2-editor-gamepads.png).

- **0.0.37 custom C# components:** native-owned scalar data now has stable type/field IDs, generated C# accessors, guarded authoring and paused live Inspector editing. Compatible reload retains data; staged writes and sparse journals join rollback, and snapshots preserve fixed membership. Native AOT artifacts bind verified component metadata. All 49 runtime and 40 authoring-only Linux suites pass. CoreCLR and Native AOT pass 1,170 RPCs across both OSes, including 100 compatible reloads per CoreCLR run and actual gameplay save/load. The final editor passes 133 focused actions on each laptop GPU, 232 general actions plus four restart actions, and 129 Save/Load actions. A relocated 40-file Windows game bundle runs compiled component updates and saves with its source project unavailable. That checkpoint covered scalar types, fixed membership and exact-schema saves. Subsequent lifecycle work adds template-based root props; arbitrary component membership changes and general migrations remain unfinished. [Usage](CUSTOM_COMPONENTS.md) and [evidence](evidence/m2-custom-components.json).

- **Per-tick editor playback:** automatic Play now uses guarded native one-tick commits without per-tick JSON requests or public retry receipts. A later failed tick preserves earlier successful ticks and saves; explicit multi-tick agent requests remain atomic. Input commits without allocation after success, and a bounded exact frame trace preserves mouse backlog and one-shot edges for replay. All 46 Linux runtime and 38 authoring-only suites pass, including typed/RPC state parity and real physics rollback. Windows passes native units, real C# save/load/failure checks and playback/input tests on both laptop GPUs. The final package passes 232 editor actions, four restart actions and 43 Profiler actions. That checkpoint preceded the later hosted editor gamepad and audio integrations. [Playback contract](DESKTOP_EDITOR.md) and [evidence](evidence/m2-editor-tick-boundary.json).

- **Shared native profiler:** bounded opt-in captures record native CPU phases, failed/rolled-back work and session/tick context. The editor timeline, source filters, summaries and trace export use the same guarded service as agents. GPU durations remain separate observations; counters distinguish tracked bytes from counts. All 45 runtime and 37 authoring-only Linux suites pass. Real CoreCLR/Native AOT save/load and rollback traces pass on both OSes. Both laptop GPUs preserve exact profiled/unprofiled pixels and replay state. The final editor passes 43 profiler actions and the full 232-action regression plus four restart actions; its attached-content image is readable. Short paired native timing trials retain overhead distributions and equal logical state without a game-performance claim. [Contract](PROFILER.md), [evidence](evidence/m2-native-profiler.json) and [tool image](evidence/m2-profiler-window.png).

- **Typed gameplay saves and safe player replacement:** C# callbacks queue save/load requests; the native owner services them after a committed simulation batch. Failed batches roll back requests without writes. Loads create fresh sessions/epochs, preserve retry outcomes and reset player input, timing and audio without retaining the old runtime. Standalone games accept an external `--save-root`. That checkpoint introduced services ABI 4; current builds use ABI 7 and require matching rebuilt hosts and game artifacts. Real CoreCLR and Native AOT requests pass six groups/85 RPCs per OS, with a separate CoreCLR-disabled Linux run. All 42 runtime and 34 authoring-only Linux suites pass; 100 compatible C# reloads still pass. The final editor package passes 232 actions plus four restart actions and 129 Save/Load actions. Both laptop GPUs pass native replacement fixtures; a relocated compiled game saves through the final runtime. Storage remains synchronous and snapshots require exact supported content/schema. [Contract](GAMEPLAY_SAVES.md) and [evidence](evidence/m2-gameplay-save-requests.json) distinguish final-package checks from earlier player binaries and synthetic input from physical qualification.

- **Editor Save/Load window:** folder configuration and named checkpoints use the existing native service. Observed guards remain fixed until explicit inspection, failed operations retain their request IDs, and recovery choices are bound to the inspected slot. Loading restores real typed C# state without changing the authored scene; unapplied Inspector/gameplay drafts block replacement. Save drafts and pending operations prevent accidental close. The packaged editor passes 129 Save/Load actions across three fresh processes on each Windows GPU, including state continuation, corruption recovery and close guards. The unchanged full editor regression also passes 232 actions plus four restart actions. Six attached-window content renders were inspected; these are not OS screenshots or physical-input qualification. [Workflow](EDITOR_SAVES.md) and [evidence](evidence/m2-editor-saves.json).

- **Durable save-slot service:** explicit external storage roots, generation guards and 32 persisted write receipts support retryable saves and fresh-process loads. The service binds the frozen authored definition and complete referenced asset inventory, stages a candidate runtime, then activates it under a fresh session ID without changing current authoring. Two retained payloads, explicit prior-generation recovery and preservation of a failed generation cover bounded corruption handling. The editor service requires paused save mutations and clears old input/session state on replacement. Seven service groups pass on each OS, including fresh-process retry and recovery; CoreCLR and Native AOT each pass four integration groups per OS. Windows desktop service qualification passes four groups/47 RPCs. Storage tests cover nine exception and nine process-exit boundaries per run on both OSes; all 40 Linux runtime and 33 authoring-only regression suites pass. General migrations, asynchronous scheduling and power-loss qualification remain unfinished. [Contract](RUNTIME.md#durable-save-slots) and [evidence](evidence/m2-save-slots.json).

- **Native runtime snapshot foundation:** an experimental C++ API exports bounded versioned logical state and stages a separate restored runtime. It preserves body velocities/sleep, controller view state with reconstructed support, in-progress kinematic motion, interrupted animation fades, logical sounds and exact-schema typed C# values. Trusted content/module bindings reject incompatible data; restore skips gameplay `Initialize`. Three new native suites pass on Linux and Windows. The 4,012-byte fixture restores in fresh processes and across both OS directions with exact canonical bytes. CoreCLR and Native AOT each pass 16 rejection cases and 40 restore/release cycles on each OS. The full Linux regressions pass 36 runtime and 30 authoring-only suites, with the subsequently added checked-in version-1 fixture test also passing. This foundation milestone did not include storage or service activation; the save-slot service extends it. General migrations and asynchronous capture remain unfinished. [Contract](RUNTIME.md#portable-runtime-snapshot-foundation) and [evidence](evidence/m2-runtime-snapshot.json).

- **0.0.36 editor C# iteration:** session-local launch profiles load a prebuilt `Game<TState>` before the first Play tick. The C# Gameplay window exposes typed live fields and paused compatible assembly reloads through the same guarded native operations as agents. Configuration generations, runtime/tick/gameplay revisions and bounded retry receipts prevent stale replacement; invalid drafts remain available. Failed startup stops the fresh runtime without authored writes, and failed reload preserves the existing module. Windows native qualification passes eight groups/193 RPCs; the final self-contained editor passes 192 focused actions, including 100 compatible reloads with zero retired contexts left alive, and the full 232-action editor regression. All 33 Linux runtime and 28 authoring-only suites pass. Three inspected Avalonia visual-content images show readable controls; Windows was locked, so they are explicitly not desktop screenshots. Compilation remains external and profiles are not persisted. The 0.0.37 custom component checkpoint extends this initial gameplay SDK. [Workflow](EDITOR_GAMEPLAY.md) and [evidence](evidence/m2-editor-gameplay.json).

- **0.0.35 native compiled C# gameplay:** a build-time generator binds the existing `Game<TState>` source into a Native AOT shared library. The engine verifies its inventory, target and generated schema, then uses the existing state/service/rollback contracts. Project/game manifest v2 packages the native artifact; v1 remains unchanged. Ten real-module groups and 12 exact CoreCLR/native comparison checkpoints pass on each OS. A separate Linux build with CoreCLR disabled passes the native suite. A relocated Windows bundle reproduces a 480-tick replay's gameplay state and exact pixels with all 38 files unchanged. CoreCLR regression passes 100 reloads per OS. All 33 Linux runtime and 28 authoring-only CTest suites pass. The final packaged editor passes 232 actions with an inspected screenshot. One earlier GUI run lost input capture while other Windows tests were active; an unchanged isolated rerun passed, and the cause remains an inference. Native library replacement requires a process restart. Clean-machine deployment, Linux graphics and consoles remain unqualified. [Contract](NATIVE_GAMEPLAY.md) and [evidence](evidence/m2-native-gameplay.json).

- **0.0.34 unlocked visual follow-up:** the AnimationRig editor fixture passed 101 semantic actions on NVIDIA, with an inspected full-window screenshot at 125% scale. Controls were readable in the captured layout. This closes the previously unavailable capture for that fixture; physical input and general DPI/accessibility coverage remain unqualified. [Visual evidence](evidence/m2-managed-animation-visual.json) and [screenshot](evidence/m2-managed-animation-visual.png).

- **0.0.34 C# animation API:** typed `GetAnimation` and `SetAnimation` expose the native playback/transition authority to handwritten gameplay. Commands stage until the callback returns; caller/gameplay conflicts and duplicate targets reject the whole batch. Native animation state survives compatible code reload, while failed ticks restore it together with gameplay fields, sounds and physics. That checkpoint introduced ABI v3; current builds require services ABI 7 with the save/component/lifecycle extensions and a matching rebuilt managed bridge. Twelve integration groups and 13 independent ABI checks passed on each of Linux and Windows, including the checked-in sample, interrupted-fade reload and rollback of animation/physics/sound/gameplay state. The existing gameplay suite passed 100 reloads per OS; both Windows GPUs passed a 480-tick C# door replay with exact reference-image agreement. All 32 Linux runtime CTest suites and three selected authoring-only compatibility checks passed. The final packaged editor passed 101 scripted actions on NVIDIA; its full-window screenshot remained unavailable under the locked Windows session. [C# contract](MANAGED_GAMEPLAY.md#control-animation-from-c) and [recorded evidence](evidence/m2-managed-animation.json).

- **0.0.33 animation crossfades:** optional `blend_ticks` blends advancing outgoing and destination clips over 0–3600 fixed ticks, including transitions to and from the authored rest pose. Missing channels use the authored baseline; rotation interpolation follows the shortest quaternion arc. Interruptions retain one immutable local-pose buffer rather than a recursive blend tree. Active transition clocks/weights are inspectable, completion removes the transition, and invalid source/target poses roll back the batch with physics and gameplay state. The typed AnimationRig Inspector exposes named authored clips and guarded live controls that advance a paused runtime by one tick. Native tests and seven protocol groups passed on Linux and Windows; 48 captures across two Windows GPUs produced 24 exact-pixel comparisons against independently authored reference poses. The final packaged desktop passed 101 semantic actions per GPU. A locked Windows desktop prevented a fresh full-window screenshot, so those semantic tests do not establish visual UI qualification. No crowd-throughput or full character-system claim is made. [Animation contract](RUNTIME_ANIMATION.md) and [recorded evidence](evidence/m2-animation-blending.json).

- **0.0.32 static mesh collision:** explicit `MeshCollider` references an imported, unweighted model primitive independently of its rendered mesh. Indexed triangle acceleration preserves geometric gaps, and raycasts report original triangle ordinals. Static hierarchies and positive nonuniform scale are supported; incompatible bodies, animation-owned transforms, shear and invalid geometry reject. The Inspector attaches/removes collision in one guarded undoable edit and exposes typed primitive/material fields while retaining invalid or conflicted drafts. Windows native mesh tests, nine public-protocol groups and runtime/animation regressions (7/11 groups) passed; both laptop GPUs passed 76 semantic GUI actions with doorway ray queries and Scene/Game captures. These tests use synthetic fixtures. Collision-only bundle export and all 11 project/export regression groups also passed with a fresh runtime installation. [Collision contract](MESH_COLLISION.md) and [recorded evidence](evidence/m2-mesh-collision.json).

- **0.0.31 independent Scene and Game panels:** two native HWND/renderers share one authoring session and fixed-tick simulation. Cameras, capture targets, renderer errors and detach/reattach lifetimes remain pane-specific; drawing never advances the clock. Modified Tall is the default workspace, with a vertically split Scene/Game variant. Version 2 layout preferences retain all six panels and migrate existing version 1 arrangements. Native tests pass three headless groups and seven groups for each GPU at 1×/4× MSAA; isolated sky/player regression passes 88 captures. Layout storage passes 28 cases on each OS. The final desktop passes 97 scripted actions and a three-action fresh-process restart on each GPU. [Desktop guide](DESKTOP_EDITOR.md) and [independent-viewport evidence](evidence/m2-desktop-dual-view.json).

- [Editor Game input](DESKTOP_EDITOR.md): keyboard/mouse bindings control runtime characters from Game view through the same evaluator as the player. Native atomic event batches have session-scoped retry receipts, focus/lifecycle clearing, frozen profiles and inspectable tick delivery. The 0.0.29 checkpoint passed 227 GUI actions on each laptop GPU plus native replay/state checks. Physical raw mouse acquisition, cursor restoration and input feel remain unqualified by these synthetic tests. [Game-input evidence](evidence/m2-desktop-game-input.json).

- [C#/Avalonia desktop editor](DESKTOP_EDITOR.md) over a native C ABI and the shared world service: Vulkan GUI composition, directly hosted native Vulkan Scene and Game panels, floating docks, typed Transform/Camera/mesh/material/light/environment inspectors, folder browser, hierarchy stripes and component icons. This is the new human-editor prototype, not a production-complete editor. Scene fly/orbit/pan/frame controls, CPU geometry picking, guarded transform gizmos and saved dock layouts are implemented. Native real-time Play/Pause/Resume/Step and independent Scene/Game cameras share agent-accessible commands. Game view connects keyboard/mouse gameplay through native bindings, atomic retryable event batches and explicit focus; assigned gamepads and opt-in [audio output](EDITOR_AUDIO.md) share the owner lifecycle. Physical input and multi-display qualification remain distinct from HWND message tests. Headless engine builds retain no GUI dependency. [Desktop foundation evidence](evidence/m2-desktop-editor.json) and [navigation/layout evidence](evidence/m2-desktop-navigation.json), [gizmo evidence](evidence/m2-desktop-gizmos.json) and [playback evidence](evidence/m2-desktop-playback.json).

- [Shared local authoring sessions](SHARED_SESSIONS.md): `serve`/`connect`, Windows same-user named pipes and Linux Unix sockets, multiple clients over one writer/revision/history, and live native-editor attachment. Inspector drafts retain their original revision across external edits; stale Apply is rejected. Fresh viewport captures report authored/runtime revision and tick separately. This is local authoring transport, not multiplayer or a finished MCP integration.

- Shared native `WorldSession` API, immutable external-camera snapshots and bounded session-local undo/redo, with revision guards, durable retry receipts and identity-preserving restoration. The retained desktop editor and earlier [ImGui frontend](EDITOR.md) use these same operations. Package management, production accessibility and large-project responsiveness remain unfinished.

- Native keyboard/mouse and gamepad profiles with alternate bindings, sensitivity/inversion, radial stick deadzones/response, trigger hysteresis, atomic profile edits, persistent retry receipts and isolated event evaluation. SDL gamepad discovery, single-player device assignment and attachment/focus/disconnect handling feed the same evaluator as headless traces; semantic replay remains independent of bindings. General action maps, in-game rebinding UI, haptics and general settings menus remain unfinished. [Input profile contract](INPUT_PROFILES.md), [gamepad contract](GAMEPADS.md).

- glTF and bounded FBX skin/curve packages, paginated key/joint/pose inspection and CPU reference deformation with isolated Vulkan pose capture. A [compute skinning pass](GPU_SKINNING.md) feeds the shared material and shadow renderer. Editable rig/node bindings connect compiled curve sampling to fixed-tick runtime playback, independent instance clocks, atomic commands, bounded crossfades, pose/physics rollback and immutable live palettes. Native masked layers add independent clocks, transitions and persistent weights, with opt-in compiled C# layer controls. Retargeting, IK and root motion remain unfinished. [Runtime animation contract](RUNTIME_ANIMATION.md), [animation asset contract](ANIMATION_ASSETS.md), [FBX profile](FBX_IMPORT.md).

- Native mono WAV import and content-addressed audio packages, editable acoustic materials/emitters, and optional Steam Audio direct-path/HRTF capture from authored or live poses. Linux/Windows tests measure actual PCM, dynamic-door obstruction, delayed arrival, directional cues and mixing; the C# use action changes the observed acoustic path. Native logical voices, rollback-safe C# play/stop, persistent direct/HRTF streams, temporal replay recording and optional SDL3 player output and a separate bounded [editor DSP worker](EDITOR_AUDIO.md) are now implemented; reflections, production scheduling and the full environmental system remain unfinished. [Event/output contract](AUDIO_EVENTS.md). [Native audio contract and evidence](AUDIO.md).

- Optional C# gameplay integrated with the native runtime: one `Game<TState>`, typed native-owned fields, compact agent inspection/editing, same-tick use/raycast/kinematic interaction, compatible-field reload and joint gameplay/physics batch rollback. Linux and Windows pass 100 reloads plus real failure and retained-context checks; both laptop GPUs pass the scripted door replay. Full component/event APIs and production deployment qualification remain unfinished. [C# API and evidence](MANAGED_GAMEPLAY.md).

- Native collider raycasts with stable entity hits and ignore filters, timed kinematic targets that persist across input batches, live progress inspection and replay. Door collision, child poses, dynamic-body pushing and real failure rollback are checked; The optional C# module now uses this surface for scripted use-button behavior. [Physics interactions and evidence](PHYSICS_INTERACTIONS.md).

- Conservative object frustum culling for the camera and each shadow view, draw counters and optional CPU/64-bit GPU timestamps on capture/play. Exact pixel comparisons retain offscreen casters and moving runtime shadows on both laptop GPUs. Timings describe these small fixtures, not qualified game performance. [Visibility and render diagnostics](RENDER_DIAGNOSTICS.md).

- Optional cascaded directional, six-face point and spot shadow maps, bounded resolution/storage, receiver-plane filtering and moving runtime casters. GPU occlusion and continuous shadowed replay are checked against independent observations. Caching, occlusion culling, ray-traced/virtual shadows and performance qualification remain outstanding. [Shadow contract and evidence](SHADOWS.md).

- Default procedural sky in new projects: editable linear colors, horizon falloff and directional Sun disk, shared by captures, player and desktop. Inspector settings use the native schema and guarded transactions; existing worlds retain their background. This is an artistic background, without atmospheric scattering, IBL, clouds or day/night simulation. [Sky contract](LIGHTING.md#procedural-sky) and [0.0.30 evidence](evidence/m2-procedural-sky.json): 44 captures and 59 GUI actions per laptop GPU.

- Authored directional/point/spot lights, ambient fill and exposure, resolved headless inspection, and frozen runtime settings with moving hierarchical light poses. The development renderer supports 1,024 enabled lights with conservative clustered assignment and complete all-light fallback on overflow. Seventeen clustered/reference capture pairs match exactly on each laptop GPU; HDR, lighting, profiler, player and native viewport regressions pass. Eight affected Linux headless contract suites also pass. GI and representative game-scale performance remain unfinished. [Clustered-light evidence](evidence/m2-clustered-lighting.json). [Lighting contract and evidence](LIGHTING.md).

- Static glTF/GLB import and content-addressed cooked models, editable hierarchy instances, StaticMesh/PbrMaterial components, indexed geometry and a direct-light GGX path with PNG/JPEG base-color, metallic/roughness, emissive and occlusion textures, cooked mip chains and glTF samplers. Normal maps use authored or MikkTSpace-generated tangents. Independent image imports, PbrTextures overrides and effective material inspection are available; broader glTF features remain excluded explicitly. [Asset contract](ASSETS.md), [material authoring and evidence](MATERIAL_AUTHORING.md), [texture evidence](evidence/m2-textures.json) and [captured material example](evidence/m2-textured-grid.png).

- Continuous native player: fixed-step human input, deterministic visual replay, a persistent Vulkan context, resize/minimize handling, retry receipts and final-state capture. Actual Windows replay/lifecycle checks pass; the automated desktop could not grant foreground input, so physical controls remain unqualified. [Player contract](PLAYER.md).

- Optional EnTT/Jolt fixed-step runtime: frozen authored revisions, stable identities, static/dynamic/kinematic box collision, static triangle mesh collision, capsule locomotion/jumping/look, guarded input batches, live inspection/capture and internal failure rollback. [Runtime contract](RUNTIME.md).

- Initial M2 observation path: Vulkan depth-tested box rendering from an immutable authored snapshot, camera/hierarchy evaluation, preview or authored lighting, 1×/4× MSAA and revision-tagged BMP capture. Actual image checks pass on NVIDIA and AMD. [Scene capture](SCENE_CAPTURE.md).

- Persistent authored-world service: stable entity IDs, hierarchy queries, Transform/Camera/MeshRenderer inspection/editing, previewable atomic transactions, revision guards, persisted retry receipts and prior-snapshot recovery. [Protocol and limitations](WORLD_SERVICE.md).

- C++20 core library and native CLI, with bounded CMake/Ninja build presets.
- Machine-readable command discovery, request schemas, version information and errors.
- Native host inspection, plus optional Vulkan loader/device enumeration using pinned headers.
- A separate headless build with no downloaded dependencies or graphics requirement.
- Black-box CLI contract tests and local installation with license notices.
- Optional SDL3/NVRHI Vulkan window, bounded presentation, explicit GPU selection and final-frame BMP readback.
- DXC compilation of embedded HLSL vertex/pixel shaders to SPIR-V; no runtime shader-file dependency.
- Pinned workspace-local Linux-host LLVM-MinGW toolchain producing a native Windows executable.
- Isolated native-module lab: host-owned state, a versioned C ABI, staged replacement/migration, canonical checkpointing and machine-readable interactive commands. This is an extension/iteration experiment; the production `hot_reload` capability remains false.
- Native Windows launcher hosting CoreCLR, with collectible C# game assemblies, native entity buffers, staged updates/migration, checkpoint parity and unload/allocation diagnostics. This remains a separate development experiment, not the production C# gameplay SDK.
- Linux x64 CoreCLR/Native AOT shipping experiment using the same C# game, with independently verified full-state equality, cross-runtime checkpoint resume/migration, error handling and runtime/native-allocation diagnostics.

See the [build instructions](BUILD.md). The 0.0.33 Linux configurations pass all 32 optional-runtime and 27 authoring-only CTest suites. [Animation evidence](evidence/m2-animation-blending.json) records this checkpoint. The earlier 0.0.32 Linux qualification has passing results for 26 authoring-only and 31 optional-runtime CTest suites. Initial runs exposed one stale discovery-schema assertion in each configuration; it was updated from revision 23 to 24 and the affected suite rerun successfully. [Collision evidence](evidence/m2-mesh-collision.json) records that checkpoint. Earlier Windows qualification covers the native shared-session test, ten history cases and seven applicable CLI cases; unbuilt-editor/renderer tests are skipped in that configuration. Earlier bootstrap evidence also covers the Linux Vulkan-inspection variant. Three explicit Windows renderer failure tests cover a missing driver, an unavailable selected GPU and an unwritable capture path. These are bounded checks, not the planned game workload or agent-task qualification suite.

## Findings

The inspected host exposes 16 logical CPUs and about 7.4 GiB of RAM to WSL. Its Vulkan loader enumerates llvmpipe (a CPU software device), not the laptop's discrete GPU. Enumeration success therefore does not qualify hardware rendering. The strict hardware check exits with the documented unavailable result.

The cross-compiled Windows x64 executable runs on Windows through WSL interoperability, using Windows Vulkan drivers. Both the RTX 4070 Laptop GPU and AMD Radeon integrated GPU presented 12 frames and produced a verified 960×540 BMP. The automated image check observes approximately 24.7% triangle coverage and all three vertex colors. The NVIDIA image was also visually inspected. No NVRHI errors were reported. This is one laptop with two GPU vendors, not broad hardware qualification.

The bootstrap used a serialized FIFO loop and NVRHI validation. Later development adds bounded submission/retirement and qualification with the Khronos validation layer, including synchronization validation; see the current [render diagnostics](RENDER_DIAGNOSTICS.md). Resize and minimize/restore are supported. Device-loss recovery, representative game performance and broad hardware qualification remain unfinished. The bootstrap triangle and box captures do not establish those properties. A Windows-hosted MSVC engine build also remains unverified.

The [Windows rendering evidence](evidence/m0-windows-render.json) records binary/source hashes, GPU reports, capture checks and test output; [the captured frame](evidence/m0-windows-triangle.png) is available for inspection. The [earlier WSL evidence](evidence/m0-wsl-bootstrap.json) remains a historical record of the 0.0.1 bootstrap.

The [native-module lab](NATIVE_MODULE_LAB.md) preserves 1,000 entities across behavior changes and 100 reloads on Linux and native Windows. It verifies rejected ABI/migration replacements, failed compilation while old code keeps running, rollback of a partially written update, canonical checkpoint round trips, corruption rejection and a 32→40-byte entity layout migration. The [baseline evidence](evidence/m0-native-module.json) records 40 actual source-edit timings per platform and resident-memory samples. These are small C++ fixture results, not C# results or full-game iteration qualification; jobs, callbacks and GPU/audio resource retirement remain untested.

The [managed gameplay lab](MANAGED_GAMEPLAY_LAB.md) tests C# development inside a native C++ Windows host using SDK 10.0.204 and CoreCLR 10.0.11. Its 40 real source edits reached the first verified logical update at p95 **1.876 seconds**, including build, reload and protocol/interop overhead; diagnostic GC was measured separately. All 100 repeated reloads collected their retired contexts. An intentionally retained game object was detected, then successfully released. Native buffers reached zero bytes on shutdown. Checkpoint bytes match the C++ Windows/Linux fixture. [Recorded C# evidence](evidence/m0-managed-gameplay.json)

These results describe one small 1,000-entity development fixture, not a game performance target or a complete C# SDK. The measured hot loop allocated zero managed bytes, but staging/snapshots/JSON transport allocate outside that interval. The separate [Linux shipping experiment](MANAGED_SHIPPING_LAB.md) now passes full-state CoreCLR/Native AOT equality, save/resume and schema migration, malformed-input rejection and twenty process restarts per runtime. The native executable reports no JIT support. [Shipping evidence](evidence/m0-managed-shipping.json)

The integrated 0.0.35 native gameplay route qualifies bounded Linux/Windows fixtures and a relocated Windows Vulkan replay. Complex jobs, general event lifetimes, clean-machine production deployment and console C# support remain unqualified. Native AOT game libraries remain process-lived; see the [native gameplay contract](NATIVE_GAMEPLAY.md).

## Qualification boundaries

| Area | Current support and limits |
| --- | --- |
| Build/resource inventory | Linux and cross-compiled Windows executables tested. Workspace tools pinned. Representative whole-engine build/reload/resource measurements and Windows-hosted engine compilation are not qualified. |
| Vulkan rendering | Presentation and readback passed on NVIDIA/AMD Windows GPUs. Full API validation, device-loss recovery and representative game workloads are not qualified. |
| Gameplay-language iteration | C++ and C# CoreCLR Windows/Linux reload fixtures tested; Linux CoreCLR/Native AOT lab parity passes. C# runtime integration also passes on both OSes. Bounded Native AOT modules are tested on Windows and Linux; production workloads, clean-machine distribution and console C# deployment remain unqualified. |
| Shader compilation | Pinned DXC compiles embedded HLSL to SPIR-V. Representative shader workloads and incremental compilation performance are not qualified. |
| Upscaling/frame generation | Optional experimental FSR 3.1.4 Native AA and fixed-ratio reconstruction; quality/performance qualification remains open. No DLSS or frame generation. |
| Acoustics | Native direct-path/HRTF and dynamic-door fixtures pass, including C# use. Optional player output exists; reflections/pathing and production resource/listening qualification are incomplete. |

The initial world suite contains nine cases, with one Windows-interop skip: abrupt Windows process termination remains unqualified by that WSL harness. See [initial world evidence](evidence/m1-world-service.json) and [expanded scene evidence](evidence/m2-scene-capture.json). The authored document is a bounded JSON model. The separate EnTT/Jolt runtime provides fixed ticks and live render snapshots; custom scalar components and exact-schema persistent simulation saves are supported, while general migrations and broader content/shader reload remain unfinished. [Native project creation/export](PROJECTS.md) is implemented; bounded Native AOT gameplay deployment is supported, while general incremental packaging remains unfinished. Integrated C# supports compatible-field reload. Game UI and production audio scheduling remain incomplete.
