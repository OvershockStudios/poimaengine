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
