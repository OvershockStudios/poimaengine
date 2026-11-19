# Inspect physical joint limits in a source pose

<!-- SPDX-License-Identifier: Apache-2.0 -->

Use `asset.animation.physical_limits` to identify which joints of an authored physical rig can follow a particular imported pose. For example, a character can stand with its wrist already past the authored hinge limit. The report identifies that wrist, shows its target in the calibrated joint frame and measures the angular change needed by the engine's fitting policy. It works headlessly before starting physics.

## Run a check

Open the project with `poima world path/to/world.json`. Select an entity with both `AnimationRig` and `PhysicalRig`. Use its ID and the current authored world revision in the request. These examples assume revision 2 and an existing rig with the illustrated ID.

```jsonl
{"jsonrpc":"2.0","id":1,"method":"asset.animation.physical_limits","params":{"entity":"00000000000000000000000000000064","revision":2,"clip":null,"time":0}}
{"jsonrpc":"2.0","id":2,"method":"asset.animation.physical_limits","params":{"entity":"00000000000000000000000000000064","revision":2,"clip":0,"time":0.75,"loop":false}}
```

Choose `clip: null` and `time: 0` for the model's rest pose. An integer selects a zero-based clip index in the rig's cooked model. Time is in seconds from 0 through 1,000,000,000. The sampler clamps at clip end by default. Set `loop: true` to wrap positive-duration clips. Rest requires `loop` false or omitted. The result echoes the model hash, entity ID, revision, selection and effective sample time. Discover the request schema through `world.describe` with `{"view":"method","name":"asset.animation.physical_limits"}`. For MCP, use the same discovery arguments with `poima_discover` and send the native method and parameters through `poima_call`.

## Read the result

The `poima.physical-pose-diagnostics.v1` result contains one row per joint, in authored order, including joints without active drives. There can be up to 63 rows. The `counts` object always gives independent totals for all three joint outcomes.

| Joint status | Interpretation |
| --- | --- |
| `within_limits` | The native policy admits the requested orientation within its numerical tolerance. |
| `outside_limits` | The native policy projects hinge swing, clamps twist, or clamps swing to the authored ellipse. The corresponding flags identify which corrections applied. |
| `indeterminate` | The native policy could not produce a reliable fit. Read `reason` for the cause. For instance, a 180-degree swing can make the swing/twist decomposition singular. The row preserves the requested rotation and returns `fit: null`. Other joints are still evaluated. |

The overall `status` reports `outside_limits` if any joint needs adjustment, then `indeterminate` if any measurement is unresolved, otherwise `within_limits`. A single-part rig returns `no_joints` with zero counts.

Each row carries its authored limits and the identities of both connected parts and model nodes. For either side, multiply `bone_rotation_model`, `body_rotation_in_bone` and `joint_rotation_in_body` in that order to obtain the model-space joint orientation. `requested_rotation` is the child joint's orientation relative to the parent joint. Rotations use active right-handed XYZW quaternions from the logical node rotation chain, including intervening nodes. The chain excludes translation and scale. Angular limits and `fit.correction_radians` use radians. Constraint X is the twist/hinge axis. Swing-twist limits use the authored Y/Z half-cone ellipse.

A successful fit returns its joint-space quaternion, correction angle and flags. For example, a 1.2-radian rotation around hinge X with an upper limit of 0.5 yields `twist_clamped: true` and a correction of 0.7 radians. The correction measures the angular distance to the engine's fitted rotation. `chart_denominator` is the dimensionless quantity `hypot(requested.x, requested.w)`. It approaches zero at the swing/twist singularity.

## Scope and recovery

The operation samples model rest or a single source clip over cooked rest locals.
Use [committed joint angle checks](JOINT_POSE_CHECKS.md) to inspect live
animation blends and procedural pose changes against explicit per-node limits.
Collision, anchor separation, scale-policy admission and recovery support have
their own checks. A diagnostic fit describes the correction a fitted-target
policy would make. Applying it to gameplay requires separate authoring and
explicit runtime control.

Read-only inspection leaves the scene, source clip and calibrated frames intact. A stale revision returns `-32009`. Read the current world and repeat against the intended revision. A missing entity returns `-32004`. An unsuitable entity, malformed time or invalid clip selection returns `-32602`. Cooked asset read or sampling failures return `-32050`.

## Reproduce the contract fixture

Build the `poima` target, then run the focused headless test.

```sh
ctest --test-dir <build-directory> -R '^physical_pose_diagnostics_contract$' --output-on-failure
```

To retain a specific output location, run `python tests/physical_pose_diagnostics_contract.py --binary <poima-executable> --output <new-directory>`. Add `--windows-interop` when invoking a Windows executable from WSL and use a Windows-accessible output directory. The runner records the original requests and replies, imported fixture, authored world and process closure. The fixture includes a passive hinge, a singular swing and an independent admitted joint in the same clip pose, with known calibration offsets and a model that stores a child before its parent.

## Reproduce the imported-character check

Prepare the [Physical Character Yard](../examples/physical-character-yard/README.md)
with its original Knight GLB and 14-joint profile. From the repository root, run:

```sh
python3 tests/physical_pose_diagnostics_knight.py --binary /path/to/poima \
  --project build/physical-character-project \
  --output build/knight-pose-diagnostics
```

The check reads the original GLB and evaluates all fourteen calibrated joint
orientations for six poses. It checks the fitted angles and limits independently
and verifies that the inspection leaves the transferred world and cooked
packages byte-identical. The [recorded Linux result](evidence/m2-physical-pose-diagnostics.json)
shows all joints admitted at rest. The largest correction in the selected clip
poses is about 0.288 rad at `wrist_r` at the beginning of `Running_A`.
