# Import a multi-take character library

Import standard character files with their original animation takes, inspect
their source data, and reopen their cooked content without the admitted source
path. These operations use the shared headless service; no editor, graphics
device, managed compiler or conversion application is required.

This guide covers one original glTF library and the supported bounded profile.
It does not establish automatic anatomical retargeting, contact solving,
production character rendering or general FBX exporter compatibility.
The [qualification record](evidence/m2-character-library-intake.json) separates
the original failed checker from successful follow-up observations.

## Original source

The reference is **KayKit Adventurers 1.0 Knight**, by Kay Lousberg, from the
[official repository at its pinned revision](https://github.com/KayKit-Game-Assets/KayKit-Character-Pack-Adventures-1.0/tree/672074b73ba276876a19e8816ecdc5241817ab47).
Obtain the unchanged [Knight.glb](https://raw.githubusercontent.com/KayKit-Game-Assets/KayKit-Character-Pack-Adventures-1.0/672074b73ba276876a19e8816ecdc5241817ab47/addons/kaykit_character_pack_adventures/Characters/gltf/Knight.glb)
and its [original CC0 license notice](https://raw.githubusercontent.com/KayKit-Game-Assets/KayKit-Character-Pack-Adventures-1.0/672074b73ba276876a19e8816ecdc5241817ab47/LICENSE.txt).
Save them as `build/character-source/Knight.glb` and
`build/character-source/LICENSE.txt`. The verifier does not download content.

| Original | Bytes | SHA-256 |
| --- | --- | --- |
| Knight.glb | 3,659,532 | `60428e3abc09ba83e595d256e3af8c5c976b46cdae599f0802fc82b4a3445168` |
| LICENSE.txt | 891 | `ae322141814056dda0deea7540d74c41d87aee1da319977cd1bd84ee5a923629` |

The source contains 57 nodes, one 41-joint skin, six skinned and nine rigid mesh
nodes, and 76 takes with 8,712 transform channels and 90,748 time keys. Its opaque
material uses one embedded atlas. The artwork is supplied separately from the
Apache-2.0 verifier; retain the source license and creator information when
sharing content. Engine [provenance records](ASSET_PROVENANCE.md) can retain
credits alongside cooked assets.

## Inspect and import

Use a 0.0.174 or later native engine with the documented asset profile. Start a
new world service and send these requests, substituting your source path:

```json
{"jsonrpc":"2.0","id":1,"method":"asset.source.inspect","params":{"source":"../character-source/Knight.glb"}}
{"jsonrpc":"2.0","id":2,"method":"asset.source.inspect","params":{"source":"../character-source/Knight.glb","section":"animations","offset":0,"limit":64}}
{"jsonrpc":"2.0","id":3,"method":"asset.import","params":{"source":"../character-source/Knight.glb"}}
```

Paths resolve relative to the world document. Source inspection does not publish
a package. Page through all original takes and retain `model_sha256` for guarded
source inspection or clip composition. Import returns the cooked asset identity
and diagnostics without changing the authored world revision.

With **0.0.176 or later / schema revision 105**, add
`"expected_model_sha256":"<model_sha256 from request 1>"` to the import
parameters to bind admission to the inspected original. Replace the placeholder
with the returned 64-character fingerprint. The raw source/license hashes in
the table above are separate file checks. See [guarded import and recovery](ANIMATION_ASSETS.md#bind-imports-to-inspected-original-models)
for donor guards, structured mismatch errors and validation order.

Inspect cooked nodes, skin joints and takes through the
[asset and animation APIs](ANIMATION_ASSETS.md). `asset.inspect` supplies section
counts in fields such as `nodes` and `animations`; source inspection and animation
pagers use `total`. Follow each response's `next_offset` rather than assuming all
pagers have identical envelopes. [Source inspection](ANIMATION_RETARGETING.md#inspect-the-original-sources)
also exposes original channel metadata and reference poses.

## Run the independent checks

Use native Windows Python 3.11+ and a matching native executable. From the
repository root, run this Python snippet in a script or Python session:

```python
from pathlib import Path
import sys

sys.path.insert(0, "tests")
from switchback_ownership import gated

result = gated([
    sys.executable, "tests/character_library_intake.py",
    "--binary", "build/windows-runtime/bin/poima.exe",
    "--source", "build/character-source/Knight.glb",
    "--license", "build/character-source/LICENSE.txt",
    "--output", "build/character-library-check",
], Path("build/character-library-owner"), 150)
assert result["passed"], result
```

Both output directories must be new. The supervisor owns the Windows process
tree before releasing the verifier and retains its termination records. The
verifier has a 120-second, 512-request, 128-MiB output budget. It operates on an
owned source copy and preserves the caller's original source and license.

The checks compare every original triangle corner, joint palette, inverse bind,
take, channel and curve array with the cooked package. Six numerical observations
cover rest, two takes at exact and intermediate key times, and a cold observation.
Each checks all 57 node transforms and sixteen vertices across rigid and skinned
meshes against original-accessor calculations. Matrix, position and normal error
limits are respectively `2e-5`, `5e-5` and `1e-4`.

Stale source fingerprints, a truncated owned source and a corrupted cooked
package must reject without changing the complete authored payloads, history or
package inventory. After repair and clean shutdown, the verifier relocates the
world and cooked store, makes their old paths and the admitted source path
unavailable, then checks a fresh owner and persisted retry receipt. It retains
the source copy elsewhere; this is not a claim that every source copy is absent
from the machine. Fresh-owner comparisons forecast the reset of session-local
undo/redo history while checking the complete observation and original authored
bytes. An active writer lock is inspected as operational metadata;
its contents are read only after the owning process closes.

Optionally add `--legacy-binary` with a retained 0.0.173 executable. Two additional
owners compare the complete inspection/import results and every cooked package
byte for the unchanged original animated-ribbon fixture. This checks one retained
accepted asset, not universal package compatibility.

## Use the library in compiled gameplay

[Locomotion Yard](../examples/locomotion-yard/README.md#original-knight-library)
has an explicit `kaykit-knight-original-glb-v1` source profile. It retains the
complete library and selects its original Idle and Running_A takes, with the
capsule owning movement. The default Kenney FBX profile remains separate.

The Windows headless consumer checks controller-driven playback, a checkpoint
during an active inertial fade, complete warm/cold restoration and route
completion after relocation. Settled runtime joints are compared with the
original curve arrays. Contact solving, IK and rendered appearance need their
own qualification; intake alone does not establish them.

## Limits and recovery

The [animation profile](ANIMATION_ASSETS.md#supported-profile-and-mathematics)
allows 16,384 channels across all takes. The separate limits of 256 takes, one
million keys, 4,096 joint references, 8 MiB cooked metadata and 64 MiB per package
remain unchanged. These are stage budgets, not total process-memory guarantees.
Donor selection follows complete source validation; selecting one take does not
bypass whole-source limits.

The [collapsed-UV policy](ASSETS.md#initial-supported-profile) supplies a
deterministic orthogonal frame only for invalid generated tangents on
nondegenerate, exactly UV-collapsed triangles without a normal map. Diagnostics
report repaired corners per primitive. Valid frames and source geometry remain
intact. Repair the source UVs before adding a normal map; the fallback does not
recover UV direction or establish normal-map fidelity.

Import/package failures use `-32050`. Preserve the original error, inspect the
supported profile and repair the source deliberately. Do not remove takes,
replace materials or change the verifier's expected source hash and then report
the modified file as supported original content.

The cooked v4 layout is unchanged. Readers with the old 8,192-channel limit
still reject a new package above that profile. The source PNG's bytes/header and
cooked mip identities/dimensions are checked; this verifier does not independently
validate every decoded texel or reproduce mip filtering. Rendered appearance,
contacts and broader source families need separate evidence. The compiled-game
workflow above records runtime locomotion separately from these intake checks.

## Reuse the headless workflow

[Locomotion Yard's public headless command](../examples/locomotion-yard/README.md#author-playtest-and-export-without-a-window)
composes this original library with a compatible compiled courier game. Choose
`author`, `verify` or `export`; retain raw SDK traces and complete checkpoints
without opening a player. The default Kenney profile remains separate.
[Recorded public-command results](evidence/m2-headless-character-workflow.json)
distinguish complete immediate restoration from selected later continuation
observations.
