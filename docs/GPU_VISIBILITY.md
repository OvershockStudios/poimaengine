# GPU camera visibility

The Vulkan renderer can generate indexed indirect draw arguments for rigid
world meshes. The GPU tests the camera frustum and writes one instance for
each surviving object. It writes zero instances for a culled object.

## Enable the path

Set `gpu_visibility: true` on `world.capture`, `runtime.capture`,
`asset.animation.capture`, `runtime.play`, or `player.start`. The schema
exposes this Boolean in revision 211. Its default is false. Vulkan builds
advertise `gpu_visibility` in capability discovery. Inspect
`render_diagnostics.gpu_visibility.supported` for the selected adapter.
Use the same option from native C++ through `RenderOptions::gpu_visibility`
or from a standalone Windows game.

```powershell
poima game run .\game.json --gpu-visibility true
```

The device must support `multiDrawIndirect` and `drawIndirectFirstInstance`.
Requests fall back to CPU visibility when the adapter lacks those features,
when `culling` or `draw_instancing` is false, or when the world exceeds the
16,384-drawable admission limit. A request with no eligible geometry also
keeps the direct path. Diagnose these cases using the result's `reason`.

## Draw order and ownership

Poima prepares indexed rigid world geometry, selected mesh LODs, and world
bounds on the CPU. The bounds include stock foliage displacement. The
compute shader applies the padded camera frustum and rounds conservatively
near clip planes. Skinned, streamed, nonindexed and camera-routed geometry
use their established paths. Shadows and separate first-person foreground
keep their own visibility decisions.

Each indirect command addresses the original object slot using Vulkan's
`firstInstance`. Compatible adjacent draws share a multi-draw call within
the device's draw limit. This keeps geometry, material and execution order
consistent with the direct path. Culled entries remain zero-instance
commands within the submitted range, so an entirely offscreen batch can
issue a command while recording zero visible objects in diagnostics.
[Vulkan indirect drawing reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndexedIndirect.html)

Each active frame slot carries a bounded candidate buffer, frustum,
indirect arguments and a reduced tally. The render schedule declares the
compute write and indirect read, and the CPU reads the tally when the
submission retires. The largest slot uses 856,176 logical payload bytes.
The capacity grows with the admitted count, so the 512-candidate fixture
uses 26,864 bytes per active slot.

## Inspect the result

`render_diagnostics.gpu_visibility` appears when the option is requested.
It reports support, active state, reason, candidates, visible candidates,
indirect batch calls and logical slot bytes. The retired
`last_draws.camera_draws`, `camera_culled`, and `camera_triangles` include
GPU visibility results. `last_draws.camera_batches` counts direct and
indirect draw calls together. Measure frame time independently of these
command counts.

The verification fixture compares 512 alternating in- and out-of-frustum
plant instances in four geometry runs. It also checks a direct control
object, a camera facing away from all the geometry, CPU fallback switches,
and a relocated runtime replay. Pass a configured Windows runtime, the
adapter index and a new output path while synchronization validation is
enabled. Add `--runtime-root` to exercise the source-free export.

```sh
python3 -B tests/gpu_visibility_capture.py "$runtime/bin/poima.exe" \
  --windows-interop --output build/gpu-visibility-check --gpu 0 --samples 4
```

[Qualification results](evidence/m2-gpu-visibility-412.json)
