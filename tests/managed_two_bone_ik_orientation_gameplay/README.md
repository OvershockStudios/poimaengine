# Compiled end-orientation consumer

This game exercises the public `ITwoBoneIkOrientationGame` SDK surface through
real Tick callbacks. It is a test consumer, not a character controller or a
solver reference. The independent native/protocol tests check poses and complete
saved operands. The old point-only consumer remains separate.

Build `Poima.ManagedTwoBoneIkOrientationGame.csproj` against the matching SDK.
The type is `Poima.Tests.ManagedTwoBoneIkOrientationGame`; the stable identity is
`poima.test.managed-two-bone-ik-orientation`. The same source supports CoreCLR and
NativeAOT publication. It requires named `baseline_v7`, `animation_ik_v1` and
`animation_ik_end_orientation_v1`, with at least 352 service bytes at epoch 7.

`Initialize` selects rig entity 400, slot 1, and disables the explicit failure
trigger with `FailAt=-1`. Every Tick reads committed oriented IK state first.
`Reads`, `Writes`, the point diagnostics, the canonical goal and actual quaternion
fields report observed callbacks. They do not establish pose correctness.
Successful commands reset `Mode` to zero; `FailAt` throws after staging at the
selected callback tick so the host can check complete rollback.

| Mode | Action |
| --- | --- |
| 0 | Read committed state only. |
| 1 | Point weight 1 and full rig-space Z90 orientation. |
| 2 | Point weight 0 and full noncommuting quaternion `(.5,.5,.5,.5)`. |
| 3 | Point weight .5 and independent orientation weight .25. |
| 4 | Complete V1 point replacement, clearing orientation. |
| 5 | Complete V2 replacement with a null orientation. |
| 6 | Valid quaternion with zero orientation weight, canonicalizing to absent. |
| 7–8 | V2/V1 writes to the same slot, in both orders; reject atomically. |
| 9–10 | Write 64 or 65 separately configured rigs, IDs 400 onward. |
| 11–12 | Read 1,024 or 1,025 times, sharing V1/V2 read accounting. |
| 13 | Reject an invalid zero quaternion through the SDK. |
| 14 | Request save slot `ik-orientation-intent` and stage orientation. |
| 15 | Inspect the save ticket result. |
| 16 | Spawn the template selected by `TemplateHigh/Low` and set its pending IK. |
| 17 | Spawn, then conflict V2/V1 on the same pending slot; reject atomically. |

The pending birth is recorded as `Spawned`; template IDs remain scalar bits,
not incorrectly persisted entity references. The protocol test owns fixture
setup, callback/caller conflicts, expected outcomes, full checkpoint comparisons
and independent quaternion geometry. A successful module build alone qualifies
none of those workflows.
