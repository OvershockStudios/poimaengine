# Retain progress when changing Relay Yard

A game update can change code and field layout without changing the scene.
Relay Yard uses an explicit save-upgrade plan for that boundary. This workflow
consumes a real source checkpoint and separately compiled source and target
Native AOT artifacts. It builds no code and generates no replacement source save.

## Retain the source first

Keep the complete successful source `tests/relay_yard_contract.py` output, its
original Native AOT artifact, and the source version's creation evidence. The
contract output contains:

```text
world.json
world.json.assets/...
saves/slot-relay-yard/current.json
saves/slot-relay-yard/previous.json
saves/slot-relay-yard/p-....bin
evidence.json
```

For the qualified 0.0.85 scenario, the selected checkpoint is generation 2 at
tick 567: one power cell collected, two still present, courier stationary, and
native checkpoint menu open. Keep both durable generations and their manifests.
The `.bin` file is a checksummed JSON save document, selected by the current
manifest; do not choose a file merely by its name or modification time.

Copy the retained directory when testing. Keep the original world, complete
cooked closure, save directory and compiled artifact unchanged. An old source
artifact must exact-restore that checkpoint in its own process before accepting
an upgrade result. Rebuilding the source today would demonstrate a new baseline,
not compatibility with a retained release.

## Build and select a target

Publish the current sample using the [ordinary build instructions](README.md#build-the-gameplay-module).
Use separate artifact and work directories. The target retains the source's
module identity, type, built-in scene, UI, assets, templates and locomotion
component schemas. Its global state now has stable field IDs, a changed layout,
and `CheckpointVersion` with persistence default 2.

Every legacy source field needs a complete old-name-to-ID association. All 49
old fields are preserved with the same kinds; only the new version field is
defaulted. No geometry change, component membership change, numeric conversion
or arbitrary migration callback is authorized by this edge. Frozen source and
target content can have the same digest even though their code and global state
schemas differ. Use actual host observations for those identities.

A Native AOT image is pinned to its process. Start a separate trusted process
with the target artifact; do not overwrite a library already loaded into the
source process. The plan selects one exact source/target pair through world,
content, backend, module, type, image and complete schema digests.

## Verify the edge and continue

Use native Python and paths for the engine's operating system. The verifier
accepts retained source input, the two exact artifacts and a new output folder:

```powershell
python tests/relay_yard_upgrade.py `
  --binary build/windows-runtime/poima.exe `
  --baseline path/to/retained-relay-yard-contract `
  --source-artifact path/to/retained-source/native-gameplay.json `
  --target-artifact build/relay-yard-artifact/native-gameplay.json `
  --output build/relay-yard-upgrade-check
```

The same command runs with a Linux headless executable, Linux Native AOT
artifacts and Linux paths. Source and target must use the same backend and OS.
Without `--capture`, this verifier is semantic; no renderer or input device is required. On Windows, add `--capture --gpu 1` (or the discovered supported GPU index) to compare source/target native pixels at the same saved tick through the authored overview camera. Both captures must submit positive imported weighted geometry and leave authoritative state unchanged. This comparison is not an independent source-pose oracle or physical-device qualification.

The verifier obtains target metadata and content through a separate native
probe, derives a complete version-1 plan from the retained source and observed
target, and hashes the plan's exact bytes. It first checks ordinary source
restoration and denied changed-image restoration. Invalid plan selections must
leave the current runtime and original storage unchanged.

Immediately after the authorized load, inspect every preserved field, the
new default and the native snapshot before any target callback. Then exercise
the remaining pickups through the actual controller and camera ray, courier
navigation and animation, and final terminal Use. A restored value alone does
not establish that the changed game remains playable.

Successful upgrade does not write a checkpoint. Save the verified target state
in a new slot, then exact-load it in a fresh target process and continue a real
tick. Keep the old slot and source artifact so recovery remains possible. Exact
retry of an accepted upgrade must return its receipt without activating again,
even if the plan file is temporarily absent; altered reuse must reject.

## Focused recovery and exported-game consumer

[`tests/workflow_save_upgrade.py`](../../tests/workflow_save_upgrade.py) checks
one retained Windows Native AOT 0.0.85-to-0.0.86 edge, including recovery of the
previous payload and continuation through an actual exported target executable.
It complements the complete-course verifier above. The
[0.0.147 qualification](../../docs/evidence/m2-retained-save-recovery-upgrade.json)
passes with native 0.0.146, the original retained images and a newly prepared
target export: 694 calls through four naturally closed owners. The record also
preserves the failed export setup and five failed verifier attempts.

Supply these inputs unchanged:

- The complete passing 0.0.85 contract directory described above, including its
  original `evidence.json`, world, cooked assets and both durable generations.
- The exact source Native AOT artifact and the retained 0.0.86 target artifact.
  This target preserves all 49 globals and adds only `CheckpointVersion=2`.
  Recompiling a replacement source does not establish retained compatibility.
- A matching Windows engine with simulation/native gameplay and an actual
  [exported project](../../docs/PROJECTS.md#install-a-runtime-and-export) using
  that exact target artifact. Export the retained world and complete cooked
  closure with the player camera selected; the fixture's player is entity 100
  and its child camera is 101. Use an installed runtime matching the host build,
  not a raw build directory or a bundle from the old source game.
- Windows Python 3.11+ and a supervising Windows Job. No compiler, original FBX,
  GPU or provider is used by this verifier. Retained artifacts are caller inputs;
  they are not supplied by a fresh source checkout.

From the repository root, use the [standard-library supervising wrapper](../../docs/COMPONENT_GAME_WORKFLOW.md#supervise-each-command).
Both output directories must be new, outside every supplied input:

```powershell
python build/run-component-owned.py `
  --job build/relay-recovery-job-1 --timeout 360 `
  tests/workflow_save_upgrade.py `
  --binary path/to/matching/poima.exe `
  --baseline retained/relay-yard-85/contract `
  --source-artifact retained/relay-yard-85/artifact/native-gameplay.json `
  --target-artifact retained/relay-yard-86/artifact/native-gameplay.json `
  --game build/relay-target-export/game.json `
  --output build/relay-recovery-check-1 --timeout 300
if ($LASTEXITCODE -ne 0) { throw "Retained recovery/upgrade verification failed" }
```

The consumer first verifies ordinary restoration of the genuine generation-2,
tick-567 checkpoint. On its own storage copy it legitimately writes generation 3,
then deliberately damages only that newest payload. Recovery must select verified
generation 2 without rewriting the damaged slot. Recovery consent remains
independent of upgrade authorization: `allow_recovery:true` and an explicit,
hash-bound target plan are both required for the combined load.

Checks cover denied loads preserving selected state and storage, all 49 original
values and the single new default before callbacks, exact historical retries
with the plan absent, actual controller/camera-ray collection of one remaining
cell, and a new healthy target save. A fresh process uses the declared bundled
executable to restore that save and continue the same compiled callbacks.
Original inputs and the copied corrupt slot must remain unchanged.

The report retains original requests/replies, native snapshots and failures;
natural closure also requires the supervising Job receipt. Legacy version-5 to
version-7 reconstruction permits only the explicitly checked camera records and
singleton spawned-node maps. Other native payload changes must match the approved
gameplay image/schema/default map before target callbacks. The fresh target save
restores fully and exactly before callbacks. After continuation, the game's
`ObservedEpochHigh`, `ObservedEpochLow` and `LastSaveGeneration` must match that
owner's actual native save status; every other selected and native payload field
remains type-exact. Original values and independently verified payload checksums
are retained. Denial checks compare complete selected
entity/component/gameplay/UI observations, queue/routing and storage, rather than
claiming complete internal snapshot equality after every rejection. This slice
does not repeat full courier/terminal completion or qualify manifest recovery,
acknowledged recovery writes, arbitrary upgrades, rendering, performance or a
clean-machine installation.

## Boundaries

The host selects upgrades through [guarded `save.load`](../../docs/SAVE_UPGRADES.md#loading).
The compiled Welcome/Menu load actions remain exact, including after writing a
new target checkpoint. Persistence defaults do not run initialization code.
Native restoration bypasses `Initialize`; loading trusted code can still run
constructors and other process-level initialization outside engine rollback.

This example covers a global-state change in a retained small game, not arbitrary
world rebasing or automatic update deployment. Separate fixtures cover supported
component arrays and hierarchical instance mapping. Rendered appearance,
physical devices, audio, performance and clean-machine installation need their
own qualification. See [implementation status](../../docs/IMPLEMENTATION_STATUS.md)
for the actual recorded runs and retained failed attempts.
