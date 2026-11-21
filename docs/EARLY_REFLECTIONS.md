# Early acoustic reflections

Set `AudioSnapshot::early_reflections` before calling `observe_audio` or feeding
a committed snapshot to an `AudioStream`. The default selects the existing
direct propagation. Both paths use Steam Audio's default binaural effect
with the live listener's orientation. The reflection has its own distance,
arrival sample, three-band response and HRTF history.

## Geometry and selection

- Enabled acoustic proxy boxes contribute finite plane faces. The solver
  reflects the point source across each exterior face, intersects the ray
  from the listener to the image, and checks the bounce against the face's
  two triangles.
- Every enabled acoustic triangle can block a reflection leg. A 2 mm
  endpoint margin excludes the contact with the reflector. The visibility
  check treats blockers as opaque for reflected paths. Direct transmission
  retains the Steam Audio simulation.
- Among paths up to 100 m, the solver checks visibility for up to eight
  candidates ranked by distance-adjusted mean band gain, then by distance,
  entity ID and face. It selects the strongest visible candidate among
  those checked. Cooked meshes participate as blockers.
- Each band has pressure gain `sqrt((1 - absorption) * (1 - scattering))`.
  The bounce uses inverse-distance gain with a 1 m minimum distance and
  integer arrival samples at 343 m/s. This models a single coherent image
  source with an angle-independent material response.
- The calculation checks up to 16,384 enabled acoustic triangles per
  snapshot. Larger scenes keep direct sound and set
  `AudioReport::reflection_budget_exceeded`. The direct and reflection
  estimates are synchronous.

`AudioPath::reflection` names the reflecting entity, face, world-space
bounce point, listener-local direction, total distance, propagation delay,
distance gain and band gains. Faces are numbered in the proxy's local
`-Z, +Z, -Y, +Y, -X, +X` order. `observe_audio` returns the path even for a
zero-frame inspection. In a PCM capture the reflected samples begin at
their later arrival. The mix remains linear and unclipped.

Pass `early_reflections: true` to `world.audio.inspect`, `world.audio.capture`,
`runtime.audio.inspect`, or `runtime.audio.capture` to select this path. Each
source reports `early_reflection` as the selected bounce or `null`. The reply
also reports whether the geometry budget prevented reflection selection.
Use the same opt-in in `runtime.audio.replay` to record events with reflections.

The reference image-source method is described by Allen and Berkley.
[Image method for efficiently simulating small-room acoustics](https://jontallen.ece.illinois.edu/uploads/537.F18/Papers/Public/Allen-PDFs/AllenBerkley79.pdf)

## Playback and recovery

A persistent stream selects the reflection identity and delay when a voice
attaches. Updates use the latest direction and material gains if the same
face still wins selection. If a blocker appears or a different face wins,
the voice fades its old reflection over one 512-frame block. The selected
face can resume when it returns. Voices started after an acoustic change
select from the new snapshot. Voice timing, stopped emission, emitter
retirement, stream resets and tail truncation retain the existing stream
rules. Each voice holds at most two DSP paths within the existing 256-voice
limit. Save data represents logical voices. A recreated stream estimates
its paths and starts with fresh DSP history.

## Verification

`tests/audio_reflections_native.cpp` checks the analytic delayed path,
face boundaries, material attenuation, occlusion, ear dominance,
reproducibility, and preservation of a one-shot reflection after its direct
tail completes. A continuing loop verifies fading and resuming its bounce
when a reflector is removed and restored. Further gates include dynamic multi-reflector transitions,
frequency-dependent reflected transmission, late reverb, production
listening and scene-scale performance.
