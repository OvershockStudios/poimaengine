# Gameplay, content and save boundaries

Poima's first alpha targets small Windows 3D single-player games built through
agents, with a human editor available. This profile defines the boundaries to
use when composing and qualifying that workflow. Supported profiles describe
qualified workflows; they do not freeze every gameplay API or file format.
The [alpha gates](ALPHA_ROADMAP.md) and [implementation status](IMPLEMENTATION_STATUS.md)
separate completed evidence from remaining release work.

## Distinguish the contracts

| Boundary | What a consumer may depend on | What it must not infer |
| --- | --- | --- |
| Authored editing | [Authoring-core v1](AUTHORING_API_COMPATIBILITY.md) releases selected world/entity reads, atomic edits, Transform data, guards and receipts. Its request, response and native behavioral gates are separate. | Every mutation exposed by `world.describe` is stable, or authored edits patch the running simulation. |
| Native gameplay | Call ABI 1/80 and services epoch 7 negotiate named features and readable prefixes. A consumer declares its exact requirements and the host checks availability. | A larger service allocation grants undeclared operations, or arbitrary SDK/bridge versions can be mixed. |
| Content | Documented intake profiles produce hashed cooked assets with explicit bindings and provenance. Export verifies the declared dependency closure and runtime/artifact requirements. | Every exporter, Unity scene, FBX rig or material works, or rebuilding an importer preserves existing cooked bytes. |
| Checkpoints | Ordinary restores bind the saved backend, trusted compiled image/type and complete schema. Supported updates need an explicit source-to-target upgrade plan. | Compatible development reload authorizes loading old saves, or a version number replaces image/schema identities. |

Only the selected authoring contract currently carries the released stability
promise. Gameplay and content compatibility must be evaluated at their declared
boundary and against retained consumers. Broader API stabilization remains an
alpha gate. Never relabel an experimental operation as stable because a sample
uses it successfully.

## Compile and select gameplay

Use one authoritative `Game<TState>` module, native-owned typed components,
explicit fixed-step input, and staged mutations. The matched SDK, generator and
CoreCLR bridge belong to one build cohort. Native AOT publication supplies a
real compiled library, descriptor, complete file inventory and redistribution
notices; deployment does not interpret C# source.

The baseline requires `baseline_v7`, a 176-byte readable services prefix and
call ABI 1/80. Named service extensions have these minimum prefixes:

| Feature | Prefix bytes | Authoring/runtime guide |
| --- | ---: | --- |
| `animation_inertial_v1` | 192 | [Animation](RUNTIME_ANIMATION.md) |
| `animation_layers_v1`, also inertial | 208 | [Masked C# layers](MANAGED_GAMEPLAY.md#control-masked-layers-from-c) |
| `character_input_v1` | 216 | [Character input](CHARACTER_INPUT.md) |
| `navigation_query_v1` | 224 | [Navigation](NAVIGATION.md) |
| `hierarchical_instances_v1` | 232 | [Runtime instances](RUNTIME_INSTANCES.md) |
| `player_preferences_v1` | 256 | [Compiled preferences](COMPILED_PLAYER_SETTINGS.md) |

`gameplay_persistence_v1` and `component_collections_v1` authorize their metadata
formats without adding a services tail. Combined consumers declare every feature
used and the largest required prefix. Query actual availability; a navigation
feature can be absent in a build even when its services allocation is large
enough. The artifact descriptor records requirements independently of global
state layout. Missing features reject selection rather than silently falling back.

The [native artifact contract](NATIVE_GAMEPLAY.md#artifact-contents) and
[managed gameplay guide](MANAGED_GAMEPLAY.md) define supported callbacks and
selection commands. Native images remain pinned for their process lifetime.
Use a fresh process for a changed image. Trusted constructors, module/library
initializers and their external effects are outside simulation rollback.

Global state has sequential unmanaged layout, 1–128 supported scalar fields and
at most 65,536 bytes. Per-entity component schemas support the documented scalar
and bounded-array layouts, with their own field/payload budgets. Keep mutable
authoritative data in this state/storage; do not hide it in game-class fields.
`long` values use lossless decimal strings over JSON. Entity handles have explicit
liveness; references must remain valid in the final staged state. See
[components](CUSTOM_COMPONENTS.md) and [collections](COMPONENT_COLLECTIONS.md).

Reads inside one callback observe committed data. Aggregate local changes before
staging one write per entity/type. Native commit and rollback boundaries govern
physics, animation, UI, structural state, game state and save requests. A later
failure in an explicit multi-tick batch rolls that batch back; already committed
automatic one-tick batches remain committed.

## Import and retain content

The alpha path includes authored primitives, PNG/JPEG images, WAV audio, bounded
glTF/GLB and [FBX intake](FBX_IMPORT.md), explicit rigs/clips and reviewed material
bindings. [Imported characters](IMPORTED_CHARACTER.md) and
[retargeting](ANIMATION_RETARGETING.md) define their supported rig, frame and
reference-pose constraints. Unsupported constructs require an actionable report,
not a silent promise of visual parity. Broader exporter coverage, contact/IK,
character creation and arbitrary material conversion remain separate work.

Preserve the original licensed input and its recorded hashes. Cooked content
and provenance are immutable assets; a game bundle supplies its verified
closure and credits. Runtime may reopen that closure without the original
artist files. Keep unused authored history and development source out of a
shipping export according to [project packaging](PROJECTS.md). Content identities
come from native observations, not world-file hashes guessed by a client.

The [Relay Yard audio profile](../examples/relay-yard/README.md#optional-licensed-audio)
preserves the original licensed OGG files and conversion recipe while importing
the native mono 48 kHz WAVs. Export supplies their hashed cooked audio and credits.
Permanent cue emitters survive collectible removal; saved scalar cadence state
belongs to the character component. Register the full generated manifest even
for a newly authored silent scene. Adding these schemas, instances and emitters
creates a different content cohort: it does not authorize loading retained
two-schema scene saves or rebasing their geometry.

The full Windows target includes rendering; Linux currently qualifies native
headless authoring/simulation. Linux graphics, browser, advanced 2D, multiplayer
and consoles follow the [release roadmap](ROADMAP.md). An exported game checked
on the development computer is not evidence of a clean-machine installation.

## Keep saves across game updates

Use [persistent global field IDs](GAMEPLAY_PERSISTENCE.md) from the beginning.
Keep an ID for the same semantic datum when changing its name or layout position.
Persistence defaults initialize newly added mapped fields; ordinary `Initialize`
remains the new-game path. A schema revision is a label, not permission to migrate.

Ordinary compiled-menu and external loads remain exact. For supported changes,
the trusted host selects one hash-bound [upgrade plan](SAVE_UPGRADES.md) and an
explicit matching target artifact. Legacy global schemas require a complete
old-name-to-ID map. Preserved kinds and units stay unchanged; retirements and new
defaults are explicit. Component arrays need exact capacity permissions and
reject overflow rather than truncating. Native topology, assets, built-in
components and UI stay exact under this policy.

Read and verify the original source save and complete runtime snapshot before
mapping. Failed preparation must retain the committed runtime, authored content
and original slot. Successful replacement does not publish a new checkpoint;
verify continuation, write a new slot/generation deliberately, and exact-reopen
it in a fresh target process. Retain the old artifact, scene/cooked closure and
save so source recovery remains possible.

[Relay Yard's retained-game playbook](../examples/relay-yard/UPGRADING.md) composes
this workflow with real pickups, imported locomotion, navigation and menus.
[Hierarchical save evolution](SAVE_UPGRADES.md#task-upgrade-a-retained-hierarchical-game)
adds bounded component arrays and instance identity mapping. These do not imply
arbitrary world rebasing, conversion callbacks, upgrade chains or automatic
packaged-game update policy.

## Qualification before a compatibility claim

Retain source artifacts and checkpoints from an actual earlier run. Verify
ordinary source restoration before testing a changed target; never regenerate
the old source with the target. Record rejected requests and unchanged-state
checks as well as successful outcomes. Inspect mapped values and native state
before any callback, then prove meaningful compiled gameplay continuation.

Authoring request acceptance, response compatibility, ABI availability,
executable integrity, save identity and semantic continuation are distinct
checks. All are needed where applicable. A passing mapper, manifest checker or
pixel count alone does not establish a usable game or a stable API. Physical
input, audio output, representative performance, editor workflow and clean
installation remain their own alpha gates.
