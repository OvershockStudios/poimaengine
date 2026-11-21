# Persistent surface marks

Use a surface mark for a stain, scuff, or scorch that stays
on a rigid object. The engine stores the position and projection axes in
the receiver's local space. A moving receiver carries its marks with it.

## Place a mark

Discover `surface_marks` and the `runtime.step` request schema with
`world.describe`. The installed runtime advertises `features.surface_marks`
when it includes simulation. Vulkan builds shade marks in the forward and
deferred world passes.

Append a `surface_marks` array to a guarded `runtime.step` request. Mark
rays use world metres. The first hit must be on the named receiver and
return a surface normal. Placement uses the final pose on the first tick
of the step batch, including motion submitted in the same request.

```json
{
  "receiver": "00000000000000000000000000000064",
  "ray": {"origin": [0, 2, 1], "direction": [0, -1, 0], "distance": 4},
  "half_extent_m": [0.12, 0.22],
  "half_depth_m": 0.02,
  "minimum_normal_dot": 0.8,
  "edge_fraction": 0.3,
  "linear_rgb": [0.12, 0.07, 0.04],
  "opacity": 0.6,
  "roughness": 0.85
}
```

Receivers need a visible, rigid, unscaled world drawable with opaque PBR.
A built-in unit box needs a box collider with half extents of 0.5 m. A
cooked mesh needs its exact render primitive bound as a triangle collider.
Camera and animation pose effects are outside this receiver contract.

The projection is an ellipse. The two half extents set its axes, and the
half depth bounds it along the surface normal. Set the normal dot threshold
to limit projection across a sharp corner. The edge fraction controls the
fraction of the radius used for a smooth falloff. Set it to zero for a hard
edge. Overlapping marks blend base color and roughness in placement order,
oldest first, before the surface is lit.

Submit up to 16 placements per step. A scene retains up to 256 marks with
at most four per receiver. Capacity and ray failures roll back the step
batch. A retry of the same successful request uses the existing receipt.

## Inspect and restore

Read `runtime.surface_marks.inspect` using the current runtime session ID,
tick, and structure revision. The optional `entity` filter selects one
receiver. The response includes the saved receiver-local frame and material
parameters for each mark in placement order. Removing a receiver retires its
marks and leaves their IDs used.

Saves carry marks in a versioned snapshot extension. Before any placement,
the save uses its earlier shape. The simulation state survives a fresh
process and supplies the renderer with the same ordered material changes.

```sh
python3 tests/surface_marks_capture.py build/linux-game/poima \
  --headless --output build/surface-marks
python3 tests/surface_marks_capture.py "$runtime/bin/poima.exe" \
  --windows-interop --runtime-root "$runtime" --output build/surface-marks-vulkan \
  --gpu 0 --lighting-path forward
```
