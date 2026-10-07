# Scene products and presentation history

This document describes the scene-product contract. Check capability discovery before using it; the bounded qualification results are recorded below.

`scene_debug_view` selects `color` (the default), `depth`, or `shading_normal`. It is a rendering option on `world.capture`, `runtime.capture`, `asset.animation.capture`, and `runtime.play`. Existing revision, session, tick, camera and playback guards still apply. Discover the current build's protocol schema before constructing requests; this addition belongs to schema version 42.

## Selecting a view

Depth and shading-normal views require `samples: 1`. A non-color view with more than one sample is rejected. Set the sample count explicitly: the capture protocol otherwise defaults to four samples.

For example, the parameters of an existing valid `world.capture` request can include:

```json
{
  "revision": 7,
  "camera": "33333333333333333333333333333333",
  "path": "depth.bmp",
  "samples": 1,
  "scene_debug_view": "depth"
}
```

Use the actual revision and camera ID from the project. Change the view to `shading_normal` for normals or `color` for ordinary rendering. This option changes the scene display; runtime UI and other supported overlays can still be drawn over it.

## Products and display conventions

At one sample, scene rendering produces depth and shading normals even when the selected view is `color`. Ordinary multisampled color rendering retains its existing path; this milestone does not define multisample depth/normal products or a surface-aware resolve.

| Product | Internal representation | Debug display |
| --- | --- | --- |
| Depth | `D32` device depth in `[0,1]`, with near 0 and far/clear 1 | Positive view-space distance divided by the camera's far distance, displayed as grayscale |
| Shading normal | `RGBA16_FLOAT`; RGB is the final world-space shading normal, alpha is surface validity | RGB is `normal * 0.5 + 0.5` for a valid surface |

The depth visualization is not raw device depth or distance in metres. Its brightness depends on the camera's far distance, so a large far distance can make nearby geometry look very dark. The normal visualization represents orientation in world space, including the material's normal mapping and applicable face handling. It is distinct from the geometric triangle normal used for operations such as shadow bias.

Valid rendered surfaces write normal alpha 1. The normal attachment clears to zero; sky and background have no surface product and display as black in both debug views. A black background does not represent a valid zero-length normal or a measured zero distance.

Debug views bypass scene exposure and Reinhard tone mapping. The formulas above specify the desired **display RGB values**: they are not scene-linear light intensities to be brightened by an additional sRGB conversion. Output handling accounts for whether the destination attachment performs sRGB encoding. Ordinary color rendering continues to use its usual lighting and output transform.

## Diagnostics

Capture and player results expose the product description under `render_diagnostics.scene_products`:

- `available`: whether this render provides the scene products.
- `view`: the selected display view.
- `normal_buffer_bytes`: the normal attachment's logical texel payload (eight bytes per pixel), excluding driver allocation overhead.
- `normal_format`, `normal_space`, and `normal_alpha`: normal encoding and validity conventions.
- `depth_convention` and `depth_visualization`: the distinction between stored device depth and its display mapping.

Use `available` rather than inferring availability from the descriptive format fields. The qualified depth/normal milestone does not return floating-point pixel arrays; the motion/probe extension below adds bounded numeric samples.

## Motion, history and numeric probes

**Qualified in the bounded cohort recorded below.** Schema version 43 adds `motion` and `motion_validity` views and `scene_product_probes`. The earlier depth/normal qualification record does not qualify these additions. Capability discovery remains the authority for the running build.

Motion is `previous_uv - current_uv`, measured relative to the active scene viewport with a top-left origin, positive X right and positive Y down. It is displacement between accepted renders, not velocity per second or displacement per simulation tick. Current rendering has no projection jitter. Camera, object and skinned deformation correspondence use retained previous render state where available.

The `motion` display maps valid displacement to `R/G = clamp(0.5 + motion_in_pixels / 64, 0, 1)` and B = 0. A valid stationary surface therefore appears approximately `(0.5, 0.5, 0)`. Invalid correspondence is black. The `motion_validity` display is white for valid correspondence and black otherwise. Both views require one sample, bypass scene exposure/tone mapping and retain supported UI overlays. Display colors are visualizations, not the numeric motion values.

**Geometric correspondence validity does not establish temporal sample acceptance.** A valid vector identifies a previous projected location for the same surface; that location may be off-screen, outside the previous depth clip range, or previously occluded. The current validity test requires finite correspondence and positive previous clip W; it does not require previous UV in `[0,1]`. This extension does not perform previous-depth disocclusion rejection, accumulate history color or decide whether an upscaler should trust a history sample. Sky/background has no surface correspondence. Zero motion must be distinguished from invalid motion using the separate validity value.

History belongs to each renderer view and advances after accepted graphics submission. A CPU frame-slot index, swapchain image or simulation tick is not its identity. Repeated renders at one tick can have different camera positions; separate Scene and Game views maintain separate history. Source replacement, explicit cuts and incompatible view configuration invalidate continuity. Object incarnation changes prevent correspondence with replaced geometry even when an entity identifier is unchanged. An empty source identity or zero object incarnation opts out of correspondence. Missing immediately preceding skin output invalidates that object. View configuration includes camera, lens and viewport; resize resets history. An accepted submission can advance history even if later presentation reports an out-of-date swapchain; it does not prove monitor display. These presentation identities are not serialized game state.

Desktop camera navigation preserves the explicit cut generation. `desktop.camera` accepts optional `cut: true` for a Scene discontinuity; framing and camera selection changes also advance the affected view's generation. `desktop.inspect.views` exposes `view_cut_generation`. Game camera selection changes affect Game independently. A rejected camera request must not advance the generation. Capture receipts also retain identity/cut guards so runtime replacement cannot silently complete an older queued capture against a different source.

For bounded numeric inspection, append probe coordinates to an otherwise valid capture request:

```json
{
  "samples": 1,
  "scene_debug_view": "motion",
  "scene_product_probes": [{"x": 160, "y": 120}]
}
```

This is a parameter fragment: existing camera, revision/session/tick and output-path requirements still apply. A nonempty probe list requires a capture path and `samples: 1`; it accepts at most 64 integer pixel coordinates inside the requested capture extent. Coordinates address the attachment from its top-left corner, rather than normalized Scene coordinates. `runtime.play` samples probes at its requested final capture. Ordinary color captures can also request probes without switching their displayed view.

Results are under `render_diagnostics.scene_products.probes`. Each entry contains `x`, `y`, device `depth`, world-space `shading_normal`, UV `motion`, `surface_valid`, and `motion_valid`. These are captured attachment values, with `RGBA16_FLOAT` normal precision and `RG32_FLOAT` motion precision, rather than values recovered from the BMP visualization. They provide sparse samples, not a full floating-point image export. Overlaid UI does not become scene depth or motion data.

Additional diagnostics report `motion_available`, `motion_buffer_bytes`, `history_valid`, `history_sequence`, `history_reset_reason`, `motion_convention`, and `motion_visualization`. History diagnostics describe the correspondence used by the completed submission, and `history_sequence` counts accepted submissions. View-level `history_valid` does not imply that every pixel has valid object correspondence; inspect per-pixel `motion_valid` where needed. These fields describe geometric correspondence, not a completed temporal reconstruction or vendor upscaler integration.

## Reproduce checks

Run the analytic display checks against the engine executable:

```sh
python tests/scene_products_capture.py PATH_TO_POIMA \
  --gpu 0 --output build/scene-products-check
```

Optionally add `--reference-binary PATH_TO_PREVIOUS_POIMA` to check that single-sample color matches a preserved earlier engine build. The recorded qualification uses checkpoint `c225e5e` as that reference.

Continuous lifetime checks use the **frame-execution test executable**, not the main engine:

```sh
python tests/frame_execution_capture.py PATH_TO_FRAME_EXECUTION_TEST \
  --gpu 0 --scene-view depth --output build/scene-products-depth-lifecycle
```

Repeat with `--scene-view shading_normal` and `--scene-view color1`, using separate output directories. Repeat on another GPU index where available. Add `--windows-interop` when running Linux Python against Windows executables through WSL.

The motion fixture uses its own native executable and an output filename prefix:

```sh
PATH_TO_SCENE_MOTION_TEST build/motion-check 0
python tests/scene_product_probes.py PATH_TO_POIMA \
  --gpu 0 --output build/motion-rpc-check
```

The native fixture exercises one- and two-slot submissions and interleaved view histories. The RPC harness checks raw depth, normals and stationary motion through actual capture requests, including ordered duplicate coordinates and the 64-probe limit. Each RPC capture creates a fresh renderer context and renders two frames: its second-frame stationary correspondence is valid. It does not establish cross-request history.

These harnesses do not automatically install or enable Vulkan validation layers. The recorded strict-validation runs configured Khronos validation and synchronization validation separately; reproduce that environment when checking API and synchronization diagnostics.

## Scope and qualification

The [qualification record](evidence/m2-scene-products.json) binds source, binary and raw-report hashes. On AMD Radeon(TM) Graphics and NVIDIA GeForce RTX 4070 Laptop GPU, 26 analytic images per device cover depth, normal mapping, backfaces, skinned geometry, exposure independence and runtime playback. Continuous single-sample color, depth and normal fixtures each capture 16 images per device and verify exact slot/lifecycle comparisons through resize, recreation and UI composition. Multisampled color also retains 16 exact reference comparisons per device.

The completed cohort includes 220 valid portable schedule variants and 269 rejection checks, eight Linux contracts, ten renderer regression groups, eight native integration groups and four legacy editor layout groups. GPU qualification uses strict Khronos synchronization validation. These checks make no performance, physical-input, installed-desktop or Linux-rendering claim.

BMP checks measure the displayed products. They are quantized display images, not lossless exports of the native depth or normal attachments.

That earlier record excludes the later motion/history/probe extension. Its [separate qualification record](evidence/m2-scene-motion.json) records checks on both named GPUs: 80 native captures and four RPC captures per device. Native cases cover initial invalidity, rigid and camera movement, subpixel displacement, queued previous poses, cuts, source and object replacement, hidden skips, resize, skinned movement, opt-out identities and independent interleaved views. Nine Linux contracts and the Windows portable schedule checks passed; the desktop C ABI checks use hidden owned windows without rendering. Ten renderer regression groups, eight native integration groups and four legacy editor layout groups passed. The final native integration run uses the UI fixture rebuilt against the current core. Existing depth/normal and single-sample color fixtures pass again, and multisampled color retains 16 exact reference comparisons per device. The Windows schedule covers 221 valid variants and 328 rejection checks, including 204 repeated unacquired-image guards. GPU runs use externally configured Khronos synchronization validation; these results make no performance, physical-input, installed-desktop or Linux-rendering claim. Full floating-point image export and temporal reconstruction remain outside its scope. The products do not establish DLSS/FSR support or a complete deferred material buffer. Multisample surface selection, transparency coverage and temporal consumers require separate contracts and qualification.
