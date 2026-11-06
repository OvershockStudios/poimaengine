<!-- SPDX-License-Identifier: Apache-2.0 -->

# Compiled physical joint-drive fixture

`Poima.Tests.PhysicalDriveGame` consumes the engine's real physical summary,
part, activation and joint-drive callbacks. It requests `baseline_v7`,
`physical_rig_v1` and `physical_rig_drive_v1`, requiring the 448-byte services
prefix in compatibility epoch 7. It runs through CoreCLR or a matching NativeAOT
artifact. These sources do not establish either execution path as qualified.

Build with a matching .NET SDK and engine gameplay SDK:

```sh
dotnet build tests/managed_physical_drive_gameplay/Poima.ManagedPhysicalDriveGame.csproj \
  -c Release --artifacts-path build/physical-drive-managed
```

Set `PoimaGameplayAssembly` to select an existing matching SDK DLL instead of
building the checkout's SDK. The module identity is
`poima.test.physical-rig-drive`. Load it through the normal gameplay API with a
matched bridge, or publish it with the public NativeAOT publisher. An external
supervisor owns qualification processes; this fixture creates no processes,
renderer, audio device or test clock.

## Required graph

The default `Rig` is `00000000000000000000000000000010` (hexadecimal low word
`0x10`). Bind an `AnimationRig` and `PhysicalRig` to it, with complete original
node mappings. The corresponding small native fixture has pelvis, arm and hand
parts on node entities ending `11`, `12` and `13`, respectively. Joint
`shoulder` connects pelvis to arm; `elbow` connects arm to hand. Each can opt into
authored drive gains. A drive-less version of either known joint supports the
absent-read and rejected-write checks.

The reference profile has shoulder swing limits 1.2 radians and twist limits
[-0.8, 0.8]; elbow is an X hinge with limits [-0.8, 1.2]. Its drive gains are
stiffness 60, damping 6 and maximum torque 10. The fixture does not assume these
numbers in its assertions: it records actual frozen gains and native part state.
The supervising check must compare them with its independently authored graph.

## Controls and observations

Use ordinary guarded `runtime.gameplay.edit` operations before stepping:

| Field | Behavior |
| --- | --- |
| `Phase=0` | Read the committed summary, selected joint drive and selected body. |
| `Phase=1` | Stage activation, then a drive write in that order. |
| `Phase=2` | Stage a drive write; requires physical ownership already or an earlier caller activation. |
| `Phase=3` | Stage activation only. |
| `Joint` | 0 selects shoulder, 1 elbow, 2 an unknown joint for native rejection. |
| `Part` | 0 selects pelvis, 1 arm, 2 hand; default is arm. |
| `TargetAngle` | X-axis target in radians; default 0.3. Native authored limits remain authoritative. |
| `Strength` | Submitted drive strength, default 0.5. Zero disables this joint's drive. |
| `RejectAfterWrite` | Nonzero throws after callback staging and committed-read checks. |
| `FailAt` | Throws on the selected native callback tick; -1 disables this failure. |

Successful phases reset to zero. Reads before and after staging must remain
identical within that callback. The next committed tick reports the resulting
mode and target. `ObservedMode` is Animated=0, Passive=1 or Driven=2; setting one
drive's strength to zero leaves Driven when another joint still has positive
strength. Zeroing every drive returns Passive without animation recovery.

Every mutable value is a typed scalar field in `PhysicalDriveGameState` and is
included in gameplay serialization. Observations include the selected node and
native body identity, position, orientation, velocities, mass, activation,
drive target/strength and authored gains. They are committed observations from
the beginning of the callback, not a substitute for the complete runtime save.

Qualification should retain complete native saves around warm/cold continuation,
activation plus drive, retargeting, zero strength and rollback. Preserve both the
literal failure reply and whole-state equality when a staged write or a later
tick fails. A write counter establishes that the callback returned; only native
observations and complete state checks establish its committed effect. Native
joint motors remain off; no active balance, locomotion or recovery is implied.
