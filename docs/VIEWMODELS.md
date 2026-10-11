# First-person viewmodels

A viewmodel is a drawable rendered with the selected camera's transform and a
separate lens. Use it for first-person arms and weapons whose apparent size
should remain independent of the player's world FOV. Its own depth buffer
preserves occlusion between those drawables while preventing world depth from
cutting them out of the image.

This is a presentation system. Weapon collision, ballistic origins, penetration,
damage queries and physical clearance still use world gameplay and physics.

## Configure a camera and drawables

Add an optional complete lens to an ordinary Camera component:

```json
{
  "vertical_fov": 90,
  "near": 0.1,
  "far": 1000,
  "viewmodel": { "vertical_fov": 65, "near": 0.01, "far": 10 }
}
```

Place the arm/weapon drawable beneath that camera, directly or through an
ordinary imported rig hierarchy. Add a CameraDraw component to **each drawable**:

```json
{
  "camera": "00000000000000000000000000000001",
  "mode": "viewmodel"
}
```

Membership is explicit and is not inherited from a parent, AnimationRig or
import wrapper. CameraDraw belongs to an entity with exactly one MeshRenderer,
StaticMesh or SkinnedMesh. Bind imported primitive entities rather than their
non-drawable wrapper. Each viewmodel drawable must be a strict descendant of
its referenced Camera, and that Camera must have a viewmodel lens.

For a third-person body that should cast world shadows without obscuring the
player's view, use CameraDraw with `mode:"hidden"`. A hidden body needs neither
a viewmodel lens nor a camera-parent relationship. Its ordinary cast/receive
shadow settings remain effective; global `visible:false` hides it completely.

Submit related component and hierarchy edits in one revision-guarded
`world.transact`. The final graph is validated atomically, allowing an agent to
create the camera, rig and bindings together. Removing a referenced camera,
required lens or drawable rejects unless that transaction also repairs or
removes its bindings. Discover the strict schemas with `world.describe` using
`view:"component",name:"CameraDraw"` or `name:"Camera"`.

## Cameras and editor inspection

| Drawable | Bound Game camera | Other Game camera | External Scene camera |
|---|---|---|---|
| Ordinary | World geometry | World geometry | World geometry |
| Viewmodel | Foreground geometry | Omitted | Ordinary inspection geometry |
| Hidden body | Camera-hidden; world shadows retained | World geometry | World geometry |

Viewmodels never cast world shadows, including during external inspection.
Their authored `receive_shadows` setting samples the existing world shadow
atlas where it covers them. A wider foreground lens does not expand world
cascades. This provides neither foreground-specific cascades nor viewmodel
self-shadowing; its independent depth provides self-occlusion.

Player FOV preferences affect the world lens only. Lens presence and bindings
are frozen when the runtime starts. [Gameplay camera controls](CAMERA_LENSES.md)
can change complete world/foreground lenses on fixed ticks; presentation
interpolation remains separate work. Existing animation playback, layers, IK and C# controls operate
on the same rigs. There is one runtime and one committed pose.

Hierarchical template bindings reference local cameras and remap on birth.
Complete removal and rebirth, rollback and saves retain their configuration.
Material/texture adoption preserves the lens and bindings. General cross-instance
weapon attachment to an external camera is not admitted by this local template
contract.

## Rendering and observations

World rendering and optional reconstruction finish first. An unjittered
foreground pass renders to output-resolution linear HDR and its own depth.
A distinct HDR composite then feeds the ordinary exposure/tone-map pass; game
UI follows it. World depth, normals, motion, AO and reconstruction inputs remain
world-only. Their diagnostic views omit foreground composition.

Scene receipts append `camera_draw` when a foreground lens or selected hidden
body exists. It contains the exact lens and sorted drawable IDs/incarnations
for the snapshot used by observed, submitted and presented work. Configured
empty foreground differs from an absent lens. Selected membership is not a
claim that every drawable survived camera culling or depth tests.

Rendering diagnostics separately report `viewmodel.rendered`, output extent,
sample count, actual draw counts, logical payload bytes and its own light-assignment
grid. `viewmodel.samples` is the selected count when `rendered:true`; when
`rendered:false` it records intent without asserting an allocated framebuffer.
Foreground uses a separate cluster grid for its lens, sharing immutable world
lights and the existing overflow fallback. Frame receipts describe accepted
queue submission/presentation, not physical display scan-out or input latency.

## Foreground anti-aliasing

Set `viewmodel_samples` to `1`, `2`, `4` or `8` on `world.capture`,
`runtime.capture`, `runtime.play` or `runtime.player.start`. Native player/editor launches accept
`--viewmodel-samples`. The default is `1`; the setting is independent of world
`samples`, lighting path and FSR reconstruction. It does not change the authored
camera, save schema or gameplay service ABI, and is not a saved preference.

For multiple samples, foreground color and self-depth use matching multisample
attachments. An explicit shader averages every HDR color/coverage sample into a
single-sample image. Cleared samples are zero; opaque covered samples have alpha
one. This preserves premultiplied coverage for composition:
`foreground.rgb + world.rgb * (1 - foreground.a)`. Exposure/tone mapping still
runs once after composition. At one sample, color and resolved color share one
allocation and the resolve pass is omitted.

The renderer checks the selected device's actual image-format/sample support
before allocating foreground attachments. Unsupported counts or exceeded
budgets reject; quality is never silently reduced. MSAA improves geometric
silhouette coverage, not temporal stability or texture/specular aliasing.
Hardware multisample behavior is specified by the [Vulkan rasterization
specification](https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-multisampling).

## Bounds and current limits

- Lens FOV: 5–150 degrees; `0.001 <= near < far <= 10,000,000`.
- At most 32 configured drawables of each mode per camera, invisible included.
- Opaque foreground with independent self-depth and optional MSAA. Foreground
  temporal AA, transparency and reconstruction are not provided.
- Logical foreground resident-plus-staging payload is capped at 384 MiB,
  including retained generations, attachments, buffers and readbacks. Existing
  aggregate renderer/reconstruction budgets also apply. Resizing can reject a
  staging pair that exceeds the aggregate budget even when its foreground
  portion fits. Allocation failure never falls back to world FOV or depth.
- At one sample, attachments use 20 logical bytes per output pixel; at `N > 1`,
  they use `12*N + 16` bytes. At 1080p that is about 39.6 MiB at one sample,
  126.6 MiB at four and 221.5 MiB at eight, before buffers. Four-sample 4K
  exceeds the foreground budget. Logical payload is not measured Vulkan
  allocator heap usage or a performance estimate.

Qualification results belong in the implementation status and evidence record;
this contract does not establish a complete FPS weapon framework or Alpha readiness.
