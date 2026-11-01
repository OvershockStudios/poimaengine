# Physical Character Yard

A reproducible full-body PhysicalRig specimen using the unchanged **KayKit
Adventurers 1.0 Knight**, by **Kay Lousberg**, licensed **CC0-1.0**. The example
contains a metric physics profile and original code; supply the artwork and its
original license separately.

The profile maps fifteen named bodies and fourteen joints to original model
nodes. Its total authored mass is 72 kg. The complete original library remains
intact: 57 nodes, 41 skin joints, 76 animation takes, 7,024 source vertices and
6,952 triangles. Capsules follow measured source segment directions; joint
constraint-X axes are calibrated separately from capsule-Y axes. Pelvis/chest
proxies use explicit source landmarks. Dimensions, masses and joint limits are
reviewable tuning values rather than claims of anatomical accuracy.

## Original inputs

Obtain the files from the [pinned official KayKit repository](https://github.com/KayKit-Game-Assets/KayKit-Character-Pack-Adventures-1.0/tree/672074b73ba276876a19e8816ecdc5241817ab47):

| Input | Repository path | Bytes | SHA-256 |
| --- | --- | --- | --- |
| Knight | `addons/kaykit_character_pack_adventures/Characters/gltf/Knight.glb` | 3,659,532 | `60428e3abc09ba83e595d256e3af8c5c976b46cdae599f0802fc82b4a3445168` |
| Original notice | `LICENSE.txt` | 891 | `ae322141814056dda0deea7540d74c41d87aee1da319977cd1bd84ee5a923629` |

`source-manifest.json` retains attribution and these identities. The source must
remain unchanged: no scale-channel stripping, geometry repair, converted
replacement, mesh generation or altered animation library is used.

## Calibration and source checks

These utilities run from a Poima source checkout and reuse the independent
original GLB/accessor/FK/LBS reader in `tests/character_library_intake.py`.
From the repository root:

```sh
python3 -B examples/physical-character-yard/knight_profile.py \
  --source /path/to/Knight.glb --license /path/to/LICENSE.txt \
  --output build/knight-calibration
python3 -B tests/physical_character_calibration.py
python3 -B tests/physical_character_source.py \
  --source /path/to/Knight.glb --license /path/to/LICENSE.txt
```

Use a fresh output directory. Calibration reproduces every byte of the selected
`profiles/knight.json`, reports original source influences and coverage, records
all 91 nonadjacent proxy pairs across five selected poses, and emits the complete
76-take summary. It does not create artwork or invoke the engine. The sixteen
analytic primitive-distance controls qualify the geometric audit math only.

The opt-in binding is `pose_frame: "skeletal_fixed_scale"`. Physical bodies use
logical quaternion chains and actual affine bone origins; authored COM offsets
stay in metres. Source local scales and ordinary TRS/FK remain intact. **Idle
(take 36)** and **Running_A (take 48)** are initialized in separate worlds. Their
scale channels are constant within each take but differ on 28 nodes. The test
does not switch or crossfade them, average a scale, or claim that every take
supports this binding. A one-ULP sampled scale change is a failure.

## Native acceptance

Use an engine build exposing `PhysicalRig.pose_frame = "skeletal_fixed_scale"`.
An external process-tree supervisor must own and bound the following test:

```sh
python3 -B tests/physical_character_native.py /path/to/poima \
  --source /path/to/Knight.glb --license /path/to/LICENSE.txt \
  --output build/physical-character-native
```

The test imports the original asset and independently compares every triangle
corner, skin influence, inverse bind and original curve array. Each take has
its own world: Idle uses stairs, Running_A uses a slope. A translated/rotated
wrapper exercises actual frame composition. The original base animation plays
for six ticks before passive takeover. All 57 published node poses and strict
local scales are checked against original source FK. Complete saved final-pose
history independently verifies each mapped COM and linear/angular finite
velocity; aggregate post-solver linear momentum accounts for fixed gravity and
drag without pretending individual velocities are unchanged by constraints.

It then applies an offset shoulder hit, observes a fall, performs same-owner
warm restore and fresh-process cold restore, and compares each immediate and
future **complete archive byte sequence** without field exclusions. Settling
and a later waking hit have fixed bounds. The final settling chunk is observed.
Literal requests, responses, complete saves, imported packages, failures and
CLI process closure are retained. No compiler, renderer or download is launched
by this test. The external supervisor separately proves descendant ownership.

## Limits and quality review

The profile is provisional. Original-source inspection records head/upper-arm
overlaps around 25.9 mm at rest and 8–10.4 mm during selected Idle poses. Selected
Running_A poses also have chest/forearm and pelvis/shin overlap. Every pair is
reported; internal contact is not hidden by extra exclusions. Zero overlap in
every authored animation is not a universal acceptance condition. Meaningful
core proxies, actual contact behavior and visible quality must inform tuning;
vertex coverage alone does not establish a good collision proxy.

`neutral_rotation: identity` refers to the original rest/T-pose joint frame.
Nonzero rest stiffness can bias a fallen body toward that pose. Relaxed neutrals
need explicit authoring and hard-limit validation; hinge-axis projection or an
out-of-limit candidate must not become an undocumented clamp.

The [Linux qualification](../../docs/evidence/m2-skeletal-physical-rigs.json) and
[Windows qualification](../../docs/evidence/m2-skeletal-physical-rigs-windows.json)
each pass both original-take workflows, with four naturally closed engine
processes, eighteen archives and eight complete warm/cold archive comparisons.
Source geometry/math and headless execution do not establish visual quality. Rendered
licensed-character contact/motion, hands/feet/accessories, active balance,
stand-up recovery, controller handoff, animation blending, Native AOT and
representative performance require separate evidence. Current controller,
root-motion and IK ownership restrictions remain in force. The analytic ribbon
continues to provide a separate clean skin/projection oracle.
