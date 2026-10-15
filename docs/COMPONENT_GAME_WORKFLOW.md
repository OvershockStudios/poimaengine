# Compiled component workflow

<!-- SPDX-License-Identifier: Apache-2.0 -->

This Windows workflow publishes the original
[ComponentGame](../tests/components_gameplay/README.md), creates a project with
nonempty native `Health` and `Interaction` components, adopts the trusted Native
AOT artifact, exports it, and checks source and readonly gameplay through the
same public APIs. [Recorded results](evidence/m2-compiled-component-workflow.json)
identify the actual artifact, runtime, source cohort and retained attempts.

## Prerequisites

Use an x64 Windows host, Python 3.11 or newer without optimization, the matching
Poima checkout and .NET 10 SDK, and the supported C++ linker/Windows SDK for
[Native AOT publication](NATIVE_GAMEPLAY.md#publish). Start from an x64 developer
shell with its toolchain environment configured. Publication executes trusted
project build tasks. Compiler descendants must finish naturally; a successful
compiler exit alone does not satisfy the ownership gate.

The recorded publication selected a seven-file, build-owned copy of installed
linker tools with `IlcUseEnvironmentalTools=true` and its directory prefixed to
`PATH`, while preserving configured `LIB`. It omitted `vctip.exe`, which can leave
a telemetry child, limited build workers to two and disabled shared build
servers. The commands below use your configured developer-toolchain environment;
they do not recreate that exact private tool inventory or its complete
environment admission. They are a public workflow recipe, not the exact
qualified toolchain recipe or a claim of identical producer inputs elsewhere.

Have a Windows `poima.exe` with simulation and native gameplay enabled and its
matching **installed** runtime tree. Follow [build instructions](BUILD.md) and
[project export](PROJECTS.md#install-a-runtime-and-export). The runtime tree must
contain `runtime.json`, player binaries and required notices; a raw build folder
is not an installed runtime. Use the actual runtime's compiled version, target
and advertised gameplay requirements. The recorded checkpoint uses native
0.0.142 with a newly published artifact; it does not imply a native 0.0.144 build.

Run from the checkout root. Select new work directories outside the installed
runtime and published artifact. The preparation and verifier retain hashes and
reject changes to their original inputs. The verifier opens an owned copy of
the source project and keeps saves outside the exported bundle.

## Supervise each command

Save this standard-library wrapper as `build/run-component-owned.py` after
creating `build/`. It imports the repository's existing Windows Job supervisor.
The first worker argument is a Python script, not a Python interpreter flag.

```python
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--job", required=True, type=Path)
parser.add_argument("--timeout", required=True, type=int)
parser.add_argument("script", type=Path)
parser.add_argument("arguments", nargs=argparse.REMAINDER)
args = parser.parse_args()
root = Path.cwd().resolve()
sys.path.insert(0, str(root / "tests"))
from switchback_ownership import gated

script = args.script.resolve(strict=True)
job = args.job.resolve()
if job.exists() or not 30 <= args.timeout <= 1800:
    parser.error("Use a new Job directory and a 30..1800 second deadline")
job.parent.mkdir(parents=True, exist_ok=True)
record = gated([sys.executable, str(script), *args.arguments], job, args.timeout)
print(json.dumps({"passed": record["passed"], "owner": str(job / "owner.json")}))
raise SystemExit(0 if record["passed"] else 1)
```

The supervisor assigns the worker before releasing it. It retains stdout,
stderr, ownership and termination accounting in the selected Job directory.
Passing requires natural zero exits, an empty owned Job and no forced cleanup.
A timeout or nonzero exit remains a failed attempt; inspect its retained report
before selecting another fresh output. This supervises the owned process tree,
not an OS sandbox or a global process census.

## Publish, prepare and verify

In PowerShell, set these paths to your configured tools and matching runtime:

```powershell
$Python = (Get-Command python).Source
$Dotnet = (Get-Command dotnet).Source
$Binary = (Resolve-Path "build/windows-runtime/poima.exe").Path
$Runtime = (Resolve-Path "build/runtime-windows").Path
$Work = Join-Path (Get-Location) "build/component-workflow-1"
$RuntimeInfo = Get-Content -Raw (Join-Path $Runtime "runtime.json") | ConvertFrom-Json
$Version = $RuntimeInfo.engine_version
New-Item -ItemType Directory -Path $Work -ErrorAction Stop | Out-Null
```

The example build paths are selectors, not a requirement to use those folder
names. `$Work` must be new. Use a different suffix for another complete attempt.
All Python workers below run through the same wrapper.

Publish the fixture once; the publisher builds its matching SDK and generators.
Keep Native AOT intermediates separate from the deployable artifact:

```powershell
& $Python build/run-component-owned.py --job "$Work/publish-job" --timeout 930 `
  scripts/publish_native_gameplay.py `
  --project tests/components_gameplay/Poima.ComponentGame.csproj `
  --type Poima.Tests.ComponentGame --rid win-x64 --dotnet $Dotnet `
  --engine-version $Version --output "$Work/artifact" --work "$Work/publish-work"
if ($LASTEXITCODE -ne 0) { throw "Publication failed; inspect publish-job/owner.json" }
```

Do not rewrite descriptor versions, schemas or inventory hashes to repair an
admission failure. The publisher inherits the configured toolchain environment
and disables shared compiler/MSBuild servers; the supervisor independently
checks closure. Toolchain setup is distinct from this fixture's engine checks.

Create, author, adopt and export with the matching engine:

```powershell
& $Python build/run-component-owned.py --job "$Work/prepare-job" --timeout 180 `
  scripts/prepare_component_workflow.py --binary $Binary `
  --artifact "$Work/artifact" --runtime $Runtime --output "$Work/fixture"
if ($LASTEXITCODE -ne 0) { throw "Preparation failed; inspect fixture/report.json" }
```

Preparation creates `fixture/source-project/project.json` and
`fixture/game/game.json`. It imports the generated schemas, assigns `Health` to
three entities and `Interaction` to one of them, and declares initial `Mode=0`.
It verifies the whole typed exported world against the source with only the
specified receipts/retired-history sanitation and checks the bundle inventory.
Original artifact, installed runtime and source files remain unchanged.

Run the retained consumer against that source and exported game:

```powershell
& $Python build/run-component-owned.py --job "$Work/verify-job" --timeout 240 `
  tests/workflow_components_game_service.py --binary $Binary `
  --source "$Work/fixture/source-project" --game "$Work/fixture/game" `
  --output "$Work/verification" --timeout 180
if ($LASTEXITCODE -ne 0) { throw "Verification failed; inspect verification/report.json" }
```

`--project` aliases `--source`; either can name the project directory or
`project.json`. `--game` accepts the exported directory or `game.json`. Readonly
hosts and clients use the **declared bundled engine executable**, whose exact
inventory path, size and hash are checked. The standalone source phase uses
`--binary`. Readonly owners automatically start their declared module at tick
zero. Their active module is checked against the bundled descriptor and DLL,
separately from source-phase selection.

## What the checks mean

The verifier retains original native requests, responses and NDJSON streams. It
applies the [prospective runtime/save result checker](WORKFLOW_RESPONSE_CONTRACT.md)
with prior native schema context. Integer, float, Boolean, decimal-text `long`,
entity IDs, schemas, memberships and every selected field remain distinct.

Three pre-ticket source/bundle pairs compare complete selected entity, component,
gameplay and UI observations. Each read checks its actual session, tick and
revisions. The verified native-library path and top-level session boundary are
handled explicitly; other payload keys, including migration metadata, remain
in the typed comparison. Query and joined-observation continuation must preserve
sorted membership, exact selected values and applicable guards.

The unchanged compiled fixture supplies these behaviors:

| Mode | Checked behavior |
| --- | --- |
| 0 | Query/read/liveness with no component writes. |
| 1 | Tick decrements selected health, increments lossless score and keeps queued writes invisible to reads inside the callback. |
| 2 | Duplicate staged write rejects with native `-32040`. |
| 4 | A two-tick batch starts at an observed even tick; the second callback throws and the entire selected batch state rolls back. |
| 8 | Tick requests the `components-game` save and records its native ticket. |
| 9 | Tick requests load, which replaces the runtime and restores the selected saved state. |

Modes 2 and 4 run in both source and readonly owners. Each rejection must identify
the armed failure and preserve the full selected checkpoint, actual save routing
and queue, and external save-file inventory. No successful receipt is inferred
for a rejected command. Historical successful load/step retries must not replay
mutation.

Inside the saving callback, `SaveState=1` means its ticket was queued. The owner
can subsequently report completion state `3` while that saved gameplay value
still equals `1`; the next compiled Tick observes `3`. Same-owner load retains
the terminal ledger. A fresh readonly owner restores the exact saved values but
has no old owner ledger, so its next Tick observes expired state `0`. The cold
continuation must match the same-owner continuation in every selected field
except that explicitly expected `SaveState` result. Saved ticket globals remain
present and exact; random epochs are not erased to manufacture cross-owner
post-save equality.

`verification/report.json` records selected checkpoint pairs, rollback checks
and owner exits. Per-owner `transcript.json` and retained streams provide full
wire evidence. Join these results with `verify-job/owner.json` to establish
natural closure; an acknowledgement alone does not prove process exit.

This fixture covers scalar `Health`/`Interaction` components. Readonly bounded
arrays, complete internal save-snapshot equivalence, rendering, physical input,
audio listening and representative performance remain separate checks. Source,
compiler and generic tools may remain available: this is neither source-absence
nor clean-machine qualification, and it does not release a complete stable
runtime API or a broader native ABI guarantee.

## Save failure and recovery

Use the same prepared scalar fixture and a separate fresh output to exercise
failed storage operations through compiled callbacks:

```powershell
& $Python build/run-component-owned.py --job "$Work/failure-job" --timeout 300 `
  tests/workflow_save_failures.py --binary $Binary `
  --source "$Work/fixture/source-project" --game "$Work/fixture/game" `
  --output "$Work/failure-verification" --timeout 240
if ($LASTEXITCODE -ne 0) { throw "Failure checks failed; inspect failure-verification/report.json" }
```

The fixture must start with `Mode=0`; the verifier creates its own source copy
and fresh external storage. It checks a compiled load before the slot exists,
one successful compiled save, and a fresh save requesting the stale generation
zero. It then performs 32 legitimate guarded external writes, reaching generation
33 with receipts 2 through 33, and repeats the compiled stale request. No stored
receipt or payload is rewritten to manufacture an outcome.

Both stale requests must finish as failed state `4` with error `-32009`, rather
than remaining unresolved. The missing load reports failed state `4` with error
`-32070`. Each callback's Tick and intended Health/Score/global changes remain
committed; the failed storage operation must leave the slot and files unchanged.
The next callback observes the failure without requesting again. These checks
verify the specified committed changes, not equality of all fields across a
legitimate gameplay Tick.

Repeated result queries, malformed/unknown ticket queries and historical step
retries preserve complete selected observations, routing, queues and save files.
The owner retains its earlier terminal results after durable receipts roll over;
its 64-result ledger is a different history from the store's 32 write receipts.
A later external save in a new slot restores exact selected state in the same
owner and a fresh exported-game process. The compiled callback advances and
reads the restored Health/Score; its full selected continuation agrees with
the retained same-owner continuation. Saved ticket globals remain exact, while
the cold owner's query correctly expires a prior owner's ticket.

The [0.0.146 qualification](evidence/m2-save-publication-rejection.json) uses
new native 0.0.146 source and bundled executables with the retained ComponentGame
artifact from the earlier fixture. That artifact's descriptor declares engine
version 0.0.142; it was not relabelled or recompiled for this check. Background
World tests use a separate newly compiled 0.0.146 SaveProbe. Unknown publication,
corrupt-slot recovery, save upgrades, full internal snapshots, source absence,
clean installation, rendering and physical input are outside this consumer.

## Bounded collection profile

The [CollectionWireGame fixture](../tests/collection_wire_gameplay/README.md)
uses generated capacity-two buffers for `int32`, decimal-text `int64`,
`float32`, `float64` and entity handles, plus one scalar revision field.
The native component wire payload is 256 bytes. Schema version 2 and
`component_collections_v1` are required; global managed collections and capacity
migration are separate features.

Using the same matching tools, runtime and supervising wrapper above, select a
fresh collection work directory and publish the collection module:

```powershell
$CollectionWork = Join-Path (Get-Location) "build/collection-workflow-1"
New-Item -ItemType Directory -Path $CollectionWork -ErrorAction Stop | Out-Null
& $Python build/run-component-owned.py --job "$CollectionWork/publish-job" --timeout 930 `
  scripts/publish_native_gameplay.py `
  --project tests/collection_wire_gameplay/Poima.CollectionWireGame.csproj `
  --type Poima.Tests.CollectionWireGame --rid win-x64 --dotnet $Dotnet `
  --engine-version $Version --output "$CollectionWork/artifact" --work "$CollectionWork/publish-work"
if ($LASTEXITCODE -ne 0) { throw "Collection publication failed" }
& $Python build/run-component-owned.py --job "$CollectionWork/prepare-job" --timeout 210 `
  scripts/prepare_component_workflow.py --binary $Binary `
  --artifact "$CollectionWork/artifact" --runtime $Runtime `
  --output "$CollectionWork/fixture" --fixture collections
if ($LASTEXITCODE -ne 0) { throw "Collection preparation failed" }
& $Python build/run-component-owned.py --job "$CollectionWork/verify-job" --timeout 240 `
  tests/workflow_collection_game_service.py --binary $Binary `
  --source "$CollectionWork/fixture/source-project" --game "$CollectionWork/fixture/game" `
  --output "$CollectionWork/verification" --timeout 180
if ($LASTEXITCODE -ne 0) { throw "Collection verification failed" }
```

`--fixture scalar` remains the preparation default. Selecting `collections`
requires its original collection module identity and all five generated
capacity-two arrays. Preparation assigns empty arrays and the generated scalar
defaults to the three starter entities, then adopts initial `Mode=0`.
The dedicated consumer checks actual compiled publication, typed array reads,
complete component edits, exact save restoration and rejected writes in source
and readonly owners. The readonly service uses its declared bundled executable.
[Qualification and limits](evidence/m2-collection-workflow.json).
