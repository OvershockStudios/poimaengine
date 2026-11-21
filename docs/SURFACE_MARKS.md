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
rays use world metres by default. Add a `source` entity ID to interpret
the ray in that entity's final rigid local frame. The first hit must be on
the named receiver and return a surface normal. Placement uses the final
pose on the first tick of the step batch, including motion submitted in
the same request.

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

For a directional scuff, add `"travel_direction": [0, 0, 1]` and give the
first half extent the desired length. Use the same world or source-local
frame as the ray. The engine normalizes the direction and projects it onto
the hit plane to set the first ellipse axis. Each component must be finite
and within ±1,000,000, and the vector length must be at least `1e-8`.
A projected unit direction of length `1e-6` or less uses the same stable
receiver-local axis as a request with no travel direction. The saved frame
follows the receiver's rotation and survives removal of the source.

Submit up to 16 placements per tick across caller and gameplay commands.
The engine places caller marks first, then gameplay marks in callback order.
A scene retains up to 256 marks with at most four per receiver. Capacity
and ray failures roll back the step batch. A retry of the same successful
request uses the existing receipt.

## Author a mask profile

Import a PNG or JPEG with `asset.image.import` and `color_space: "linear"`.
The mask uses the red channel as coverage, from zero for a clear area to
one for full strength. Use an image from 1 to 256 pixels on each side.
Create a profile in a guarded `world.transact` request with a nonzero
32-digit lowercase hexadecimal ID and the cooked image's SHA-256.

```json
{
  "op": "surface_mark.profile.set",
  "id": "00000000000000000000000000000092",
  "mask": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
}
```

Use that ID in the placement command's `profile` field. The ellipse
still controls falloff and the mask multiplies its coverage. The mask is
bilinearly sampled at mip zero. The first axis points toward the right of
the image and the positive second axis points toward the top. The full
image fits inside the enclosing rectangle of the ellipse.

A world can define 32 profiles and use 16 distinct mask images. Profiles
that point to the same cooked image share it in the standalone export.
`world.inspect` lists profiles and `world.asset.references` reports their
image links. Submit `mask: null` to remove a profile. Changes take effect
when a new runtime freezes the edited world. Exports containing profiles
require a Vulkan package advertising `features.surface_mark_masks`.

## Place marks from gameplay

Implement `ISurfaceMarkGame` on the C# game module. During `Tick`, use
`context.PlaceSurfaceMark(receiver, origin, direction, distance, style)`
to submit a world ray. Pass a `source` entity to use its final local frame.
The engine resolves the frame after physics and attachments. A source born
from a template during the same tick can supply the ray. Marks remain with
the receiver if the source later moves or is removed.

```csharp
var style = new SurfaceMarkStyle(new Vector2(.12f, .22f), .02f,
    .8f, .3f, new Vector3(.12f, .07f, .04f), .6f, .85f);
context.PlaceSurfaceMark(receiver, new Vector3d(0, .1, 0),
    new Vector3d(0, -1, 0), .5, style, source);
```

The optional `surface_mark_v1` service uses a 136-byte command and a
544-byte epoch-7 prefix. `world.describe` reports this under
`gameplay_surface_marks`. The gameplay callback allows up to 16 wire
attempts per tick. Placement joins the same batch as motion, particles,
and gameplay state, including save continuation.

Implement `IDirectionalSurfaceMarkGame` to call
`context.PlaceDirectionalSurfaceMark(receiver, origin, direction, distance, style, travelDirection, source)`.
Discover `surface_mark_direction_v2` and its 160-byte command under
`gameplay_surface_marks.directional`. It uses a 552-byte epoch-7 prefix.
A game can declare both interfaces to place both kinds of marks in the
same tick. Both commands share the bounds and save format.

Implement `ITexturedSurfaceMarkGame` to call
`context.PlaceTexturedSurfaceMark(profile, receiver, origin, direction, distance, style, travelDirection, source)`.
Discover `surface_mark_texture_v3` and its 176-byte command under
`gameplay_surface_marks.textured`. It uses a 560-byte epoch-7 prefix.
The game supplies the profile and direction of travel. The mark still
uses the same placement budget and rollback as the other commands.

## Inspect and restore

Read `runtime.surface_marks.inspect` using the current runtime session ID,
tick, and structure revision. The optional `entity` filter selects one
receiver. The response includes the saved receiver-local frame and material
parameters for each mark in placement order. Removing a receiver retires its
marks and leaves their IDs used.

Saves carry marks in a versioned snapshot extension. A textured record
stores the profile ID and exact cooked mask digest. Restoring uses the
catalog in the saved authored world, including after a profile has been
reassigned in the current project. Keep that original cooked file available
for existing saves. A new export includes the current catalog's images.

```sh
python3 tests/surface_marks_capture.py build/linux-game/poima \
  --headless --output build/surface-marks
python3 tests/surface_marks_capture.py "$runtime/bin/poima.exe" \
  --windows-interop --runtime-root "$runtime" --output build/surface-marks-vulkan \
  --gpu 0 --lighting-path forward
```

The [directional gameplay fixture](../tests/gameplay_directional_surface_mark_contract.py)
uses the compiled game in `tests/managed_directional_surface_mark_gameplay`.
Publish it with `scripts/publish_native_gameplay.py` and pass the resulting
`native-gameplay.json` as `--native-descriptor`. Its standalone route checks
the ellipse along and across the saved axis before and after a receiver turn.

The [textured gameplay fixture](../tests/textured_surface_marks_contract.py)
uses the compiled game in `tests/managed_textured_surface_mark_gameplay`.
It checks filtered coverage in relocated builds and checks that replacing
a profile in the project preserves the original mask in an existing save.
