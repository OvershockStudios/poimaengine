# Weapon and foreground clearance

An optional camera profile retracts its foreground geometry when a conservative
capsule envelope approaches a physics obstacle. The native runtime evaluates
clearance **after camera/weapon pose interpolation**, using current physics.
Moving the player, changing recoil or interpolating an older extended pose
therefore does not bypass the final constraint on that configured envelope.

This is a presentation constraint. Gameplay aim, hit rays, body transforms,
animation clocks and saved offsets remain committed. Weapon firing rules and
ballistic origins remain game decisions. The generic [capsule query](CAPSULE_QUERIES.md)
lets compiled gameplay make its own fixed-tick decisions too.

## Author a profile

Add `viewmodel_clearance` to a Camera with a foreground lens through the normal
revision-guarded `component.set` transaction:

```json
{
  "vertical_fov":70, "near":0.1, "far":80,
  "viewmodel":{"vertical_fov":75,"near":0.01,"far":2},
  "viewmodel_clearance":{
    "position":[0.18,-0.17,-0.4],
    "rotation":[0.7071067811865476,0,0,0.7071067811865476],
    "radius":0.075, "half_height":0.175,
    "max_retraction":0.65, "skin":0.005
  }
}
```

The capsule uses local Y. This rotation aligns it with the sample barrel's Z
axis. Position/rotation are relative to the selected camera **and** its presented
foreground effect. The example encloses the FPS sample's small box weapon; it
is not a universal weapon or arms profile. Fit an envelope containing all
selected drawables through their supported animation/attachment poses. The
engine does not automatically prove containment of arbitrary animated meshes.

All six fields are required. Local position is within ±10 m; radius is
0.001–10 m, half-height 0–10 m and maximum retraction 0.001–10 m. Rotation uses
the capsule query's unit-quaternion rules. Skin is 0.0001 m to the smaller of
radius and maximum retraction. Removing the profile restores ordinary foreground
presentation. Changing it requires a new authored runtime, not a pose command.

The profile is part of authored content, including template cameras and cooked
world documents. No new entity references need remapping. Native runtime
presentation applies it; an authored external Scene preview does not evaluate
physics clearance. Inspect the Camera's authored JSON/schema through the shared
service. Dedicated desktop profile controls are separate tooling work.

## Resolution

For every prepared named runtime camera snapshot:

1. Compose camera, presented foreground offset and capsule-local pose.
2. Place the same fixed-orientation capsule at maximum retraction along camera
   local +Z, then sweep toward the desired pose.
3. Stop before the limiting contact by the authored skin margin and verify the
   final capsule with a fresh overlap query.
4. Translate all selected foreground draw matrices by the same safe retraction.

Physical camera ancestors are automatically ignored, including the normal
avatar collider. Other world colliders remain eligible. This policy evaluates
both mesh sides; mesh shells and query resource limits retain their documented
meaning. There is no mutable release spring or hidden frame history. Inward
constraints apply immediately; procedural motion remains game-owned.

A blocked fully retracted anchor is not a valid starting volume. If the anchor
or final check is blocked, foreground draws are suppressed and the receipt
explicitly says unresolved. World geometry and the selected camera remain
unchanged. The fallback is not reported as successful clearance. Query exhaustion
raises an error rather than publishing a false clear frame.

The constraint protects the authored capsule at each prepared snapshot. It is
not an angular continuous sweep of an imported mesh, a motion solver for the
player, full-body weapon IK, a gameplay firing lock or a guarantee about visual
contact under every combination of independent world/foreground projection.

## Inspect the actual result

Observed, submitted and presented scene receipts add `viewmodel_clearance`:

- `profile` and `cast` retain the complete local recipe and actual world query.
- `anchor_hit`, `sweep_hit` and `final_hit` identify limiting contacts.
- `retraction`, `resolved` and `suppressed` distinguish outcomes.
- `status` is `clear`, `retracted`, `blocked_anchor` or `blocked_final`.
- `ignored_entities` and `suppressed_entities` identify the actual affected sets.

The accompanying `camera_pose` receipt contains actual camera and foreground
draw matrices after resolution, even without optional pose interpolation. A
suppressed foreground has an empty draw list while retaining its configured
lens. These are prepared/submitted scene facts; presentation sequence tags
retain their existing meanings and do not measure display scan-out.

[Player captures](PLAYER_POSE_PRESENTATION.md#observe-and-capture) freeze the
pose/lens metadata, owned geometry and clearance result together. Queries and
captures do not serialize the derived clamp into gameplay state. Complete saves
retain game-owned feedback and authoritative transforms. The immutable authored
profile contributes to the existing content identity.

`examples/fps-feedback/configure.py` installs the example profile in its guarded
authoring transaction. `tests/runtime_viewmodel_clearance_native.cpp` exercises
native extraction and complete-state preservation. The separate Windows
`tests/viewmodel_clearance_player.py` uses the actual compiled FPS game and
current physics with thin/edge obstructions and an explicitly blocked anchor.
Its records retain source inputs, original calls, captures and complete saves;
qualification scope and outcomes are recorded separately from these contracts.

See the [0.0.208 results and platform scopes](evidence/m2-viewmodel-clearance.json).
