# Scene depth and shading-normal views

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

Use `available` rather than inferring availability from the descriptive format fields. These diagnostics describe the rendered products; they do not return the underlying floating-point pixel arrays.

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

These harnesses do not automatically install or enable Vulkan validation layers. The recorded strict-validation runs configured Khronos validation and synchronization validation separately; reproduce that environment when checking API and synchronization diagnostics.

## Scope and qualification

The [qualification record](evidence/m2-scene-products.json) binds source, binary and raw-report hashes. On AMD Radeon(TM) Graphics and NVIDIA GeForce RTX 4070 Laptop GPU, 26 analytic images per device cover depth, normal mapping, backfaces, skinned geometry, exposure independence and runtime playback. Continuous single-sample color, depth and normal fixtures each capture 16 images per device and verify exact slot/lifecycle comparisons through resize, recreation and UI composition. Multisampled color also retains 16 exact reference comparisons per device.

The completed cohort includes 220 valid portable schedule variants and 269 rejection checks, eight Linux contracts, ten renderer regression groups, eight native integration groups and four legacy editor layout groups. GPU qualification uses strict Khronos synchronization validation. These checks make no performance, physical-input, installed-desktop or Linux-rendering claim.

BMP checks measure the displayed products. They are quantized display images, not lossless exports of the native depth or normal attachments.

Native floating-point product export, motion vectors, temporal history and temporal reconstruction are not implemented by this milestone. The products do not establish DLSS/FSR support or a complete deferred material buffer. Multisample surface selection, transparency coverage and temporal consumers require separate contracts and qualification.
