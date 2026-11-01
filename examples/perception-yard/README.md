# Perception Yard

This is a small compiled consumer of the optional [AI perception package](../../packages/ai/README.md).
A sentry checks an intruder with the package's adapter for the existing
`GameContext.Raycast` and keeps explicit last-seen memory. The world uses only
deterministic primitive boxes and needs no imported art or audio.

[Qualification for 0.0.217](../../docs/evidence/m2-npc-perception.json) covers
Linux headless and Windows CoreCLR: 176 requests, 55 typed fields, 42 observations,
nine complete checkpoints and five complete archive-byte pairs per platform.
The source builds with .NET 10.0.401/C# 14. These are native CPU gameplay checks;
interactive appearance, physical input and Native AOT for this sample remain
separate.

| ID | Entity | Role |
| --- | --- | --- |
| 10, 11 | Sentry post, visor | Self colliders. The eye (0, 1.6, 6) lies inside the post. |
| 20, 21 | Intruder torso, head | Kinematic compound target. Samples are evaluated torso first, then head. |
| 30 | Tall wall | Hides the intruder at the `hide` station. |
| 31 | Low wall | Hides the torso, but not the head, at the `low` station. |
| 1, 2, 3 | Floor, spectator controller, camera | The scene and the required project entry. |

The intruder moves between five stations at up to 0.5 m per Tick: open ground,
behind the tall wall, head over the low wall, a flank outside the 0.7 rad
horizontal limit, and a point beyond the 12 m range. Torso and head move
together. Controls queue a move for the next Tick. The other buttons schedule a
rollback fault, clear it, or forget the target and return the gaze to rest.

## Game policy

This policy belongs to the sample; the package does not require it. Sight uses
a yaw/pitch window of 0.7/0.45 rad, a range of 0.25–12 m, `FirstVisible`
sampling and a budget of 4 rays per sight/search call. The game records their
combined count in `TotalRays`; an aggregate per-Tick budget remains a caller
policy. Retention is 90 Ticks (1.5 s at 60 Hz). The
remembered position is the torso centre. While the sentry sees the intruder, its
gaze holds. While it only remembers the intruder, the gaze turns toward the last
known position. That spot is then probed with an empty target list, so only
`Clear` counts as in view and any collider in the way occludes. After expiry or a
reset, the gaze returns to rest.

`TargetMode`, `SelfMode`, `RayBudget`, `Sampling`, the angles, the range and the
retention are ordinary typed fields that can be changed with gameplay edits:

- `TargetMode = 1` declares only the torso. The head then fails closed as an occluder.
- `SelfMode = 1` stops ignoring the visor. `SelfMode = 2` ignores no self
  colliders, so every ray hits the post at distance 0.
- `RayBudget = 1` leaves the head unchecked. The result is then `Inconclusive`, not `Occluded`.

Out-of-domain settings, for example `MaxRange > 10000`, make the Tick throw
before any query. The native batch then rolls back, and further steps keep
failing until the field is corrected.

All 55 state fields are `int`, `long`, `double` or `EntityId`. This covers the
configuration, the committed intruder poses, the gaze, both samples' status, reason,
hit and distances, the aggregate sight result, the search probe and every scalar
of the memory record. The package's records are rebuilt each Tick.

## Prepare, build and verify

```sh
python examples/perception-yard/prepare.py --binary <poima> --output build/perception-prepare
dotnet build examples/perception-yard/Poima.PerceptionYardGame.csproj -c Release -o <sample-build> -p:PoimaGameplayAssembly=<Poima.Gameplay.dll>
python tests/ai_perception_gameplay_contract.py <poima> --hostfxr <hostfxr> --bridge <Poima.ManagedBridge.dll> --assembly <sample-build>/Poima.PerceptionYardGame.dll --output build/perception-contract
```

Preparation writes a new manifest. It then authors the world in one guarded
`world.transact` and records its requests and the native close result. It builds
nothing and never runs gameplay. The game build needs .NET 10/C# 14, a matched
gameplay SDK, a CoreCLR bridge and hostfxr, and a native build with managed
gameplay and game UI enabled. No new services ABI or marker is required. Use
`--windows-interop` when you run a Windows engine from WSL.

The fixture builds nothing and opens no window. It needs an external
process-tree owner, has a 180 s / 600-RPC bound and retains the literal RPC
text. Its expected values come from an independent model of the authored boxes
at the analytic station positions. The fixture refuses expectations that are
within 2 mm of a classification boundary. Memory expectations come from the
movement path: the remembered spot is the last visible point on that path. The
fixture covers wall and self occlusion, compound targets, budget versus
blocking, view and range rejection, search, expiry and reacquisition. It also
checks complete checkpoint bytes across an invalid-settings failure, a
second-Tick fault, a same-owner restore and a fresh-process continuation.

## Limits

Sight is tested at two sample points, not silhouette coverage. Queries see
committed poses, so a moving intruder is evaluated at its pose from the previous
Tick. Hit distances depend on native float collision. The fixture tolerances
are 0.1–0.2 mm, and they are not a guarantee of identical results across
platforms. Hearing, navigation, cover, squads, behavior graphs, damage, Native
AOT, interactive rendering and physical input are outside this sample.
