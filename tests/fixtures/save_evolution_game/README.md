# Compiled Collection Room save evolution

`tests/save_evolution_game.py` runs a real compiled target against a separately
preserved Collection Room source save. It accepts CoreCLR or Native AOT and
writes its requests, responses, hashes and terminal result to `evidence.json`.
It never generates a replacement source save from the target game.

The target renames `Collected` to `RecoveredCount`, rearranges the native state
layout, assigns explicit persistent IDs to the legacy fields, and adds
`UpgradeSteps` with default 731. The test verifies:

- Changed-image exact-load rejection and explicit plan authorization.
- An Initialize trap proven by a separate ordinary-load failure, armed before
  each restore process starts.
- Complete runtime snapshot equality around rejected upgrades; save ledger,
  component and gameplay inspection; unchanged original slot files.
- Correct global values before any callback, with every other saved runtime
  payload field preserved, including physics, UI and allocator state.
- Receipt retry with the plan file temporarily absent, conflicting reuse and
  stale guards, and collection of rejected detached managed modules.
- Actual compiled Control and Tick callbacks, remaining collectible gameplay,
  and completion HUD text.
- Recovery of an actual older source generation after corrupting a newer
  test-owned generation, explicit recovery consent and correct restore ledger.
- A new target save loaded exactly in a fresh process, followed by another Tick.

This is a global-field fixture, not arbitrary component migration, rendered
output, physical-input qualification or a performance benchmark.

## Preserve the source baseline separately

A baseline is an external directory supplied with `--baseline`. No historical
binaries or saved projects are checked in with this fixture. Keep it read-only
and retain its original source, compiled artifacts and creation evidence.

To create a **new** baseline, build the original
[`examples/collection-game`](../../../examples/collection-game/README.md) and run
its `run.py --verify` in a new project directory. Preserve that run's
`sample-evidence.json`, original source and compiled artifacts before building
this target. A newly created baseline demonstrates that particular source
revision; it must not be presented as an older retained release. The required
source checkpoint is generation 1 of `collection-room`, at tick 3 with one cell
collected. Do not substitute a target-generated save.

The baseline layout is:

```text
manifest.json
coreclr/world.json
coreclr/saves/slot-collection-room/...
# Or native/world.json and native/saves/... for the native_aot backend.
# Additional preserved source/artifact/evidence files may live anywhere below it.
```

`manifest.json` must contain `version: 1`, a `files` array of
`{"path":"relative/path","bytes":123,"sha256":"..."}` records, and
`identities.coreclr` or `identities.native`. Inventory all preserved files except
`manifest.json` itself. Each identity contains `world_id`, `content_sha256`,
`authored_revision`, `tick`, `backend`, `assembly_sha256`, `type`, `schema` and
`values`. Take these directly from the original saved runtime payload: the first
four are payload fields; the last five are in `payload.gameplay`. `backend` is
`coreclr` or `native_aot`; `type` is `Poima.Examples.CollectionGame`.

The source payload is the world-save JSON file selected by
`current.json` → `payload.current.file`; its runtime payload lives at
`snapshot.payload`. Verify that selected file against `payload.current.sha256`
and preserve the complete slot directory, including its manifests. Record file
sizes and SHA-256 from the retained bytes, never by recompiling the source.
The runner validates inventoried bytes before and after qualification and
copies the source world/save directory to a new output directory.

## Build the target and run

From the repository root, build the target using isolated artifacts:

```text
dotnet build tests/fixtures/save_evolution_game/Poima.CollectionGame.csproj -c Release --artifacts-path build/save-evolution-target/dotnet -o build/save-evolution-target/managed
```

Use a matching engine and managed bridge built with save-upgrade support:

```text
python tests/save_evolution_game.py --backend coreclr --baseline /path/to/preserved-baseline --binary /path/to/poima --hostfxr /path/to/libhostfxr.so --bridge /path/to/Poima.ManagedBridge.dll --assembly build/save-evolution-target/managed/Poima.CollectionGame.dll --output build/save-evolution-check
```

For Native AOT, publish the target through
`scripts/publish_native_gameplay.py` with this fixture's project and type
`Poima.Examples.CollectionGame`. Replace `--hostfxr`, `--bridge` and `--assembly`
with `--native-descriptor /path/to/native-gameplay.json`, and select
`--backend native_aot`. Run each backend in its own output directory/process.
Use `--windows-interop` when Linux Python launches Windows binaries through WSL;
paths are converted and the trap environment variable is forwarded.

The output directory must not exist and must be outside the baseline. Run
without Python optimization. All test-owned storage remains in that output
for inspection, including failed runs. The plan helper accepts explicit
`--baseline` and `--mapping` paths and derives the target edge from metadata
inspected from the actual compiled target, not from guessed schema values.
