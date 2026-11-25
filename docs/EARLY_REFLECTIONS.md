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

## Enable reflections in a game

Read `manifest_sha256` with `poima project inspect`. Set both audio and the
reflection option in one guarded update.

```sh
poima project configure MyRoom/project.poimaproject \
  --expected-manifest-sha256 SHA256 \
  --patch '{"audio":true,"early_reflections":true}'
```

Rebuild the project to use the option in the exported launcher. Export
requires an installed runtime with `features.player_early_reflections`.
Both `runtime.play` and `player.start` accept `early_reflections: true`
with `audio: true` for a specific player. The player report includes the
effective option and total `audio.reflection_handoffs`, including earlier
audio timelines within that player. Existing projects default to direct sound.

Set `early_reflections` to false to keep direct audio only. To disable all
player audio, set both fields to false in the same update. The headless
recording methods still select reflections on each request.

## Playback and recovery

A persistent stream starts with the reflection selected when a voice
attaches. At subsequent mix blocks, the same face updates its direction and
material gains while keeping its delay. When a different face wins, the
stream fades the old contribution out and the new one in over one 512-frame
block. A newly visible face can also fade in from silence. The replacement's
delay is derived from its own path, and the emitter keeps its original clip
position. The new branch starts with fresh filter history.

If all faces are blocked, the last contribution fades out. Its filters can
resume if that face returns. An active voice holds one direct and at most two
reflected paths, with the second present only during a handoff block. With
`early_reflections: true`, `runtime.audio.replay` reports the number of paths
acquired or replaced after attachment as `stream.reflection_handoffs`. DSP
history remains local to a stream. Save restoration recreates it from the
logical voice timeline and current geometry.

## Verification

`tests/audio_reflections_native.cpp` checks the analytic delayed path,
face boundaries, material attenuation, occlusion, ear dominance,
reproducibility, and preservation of a one-shot reflection after its direct
tail completes. A continuing loop verifies fading and resuming its bounce
when a reflector is removed and restored. A separate repeating-signal case
gains a reflection and switches to a farther surface on the opposite side.
The settled signal is compared to phase-aligned fixed-path controls.
Frequency-dependent reflected transmission, late reverb, production listening
and scene-scale performance remain open.

`tests/player_early_reflections.py` uses the main Windows player and SDL's
disk output to check both variants after moving the export and its source.
Run it with the matching installed runtime and an existing output directory.
From WSL, use Windows-accessible paths and add `--windows-interop`.

```sh
python tests/player_early_reflections.py RUNTIME/bin/poima.exe \
  --runtime RUNTIME --output EXISTING_DIRECTORY --gpu 0
```

The fixture compares every submitted PCM frame after accounting for SDL's
silence before playback. Physical speaker and headphone playback require a
separate check. [SDL disk output](https://wiki.libsdl.org/SDL3/SDL_HINT_AUDIO_DISK_OUTPUT_FILE)
