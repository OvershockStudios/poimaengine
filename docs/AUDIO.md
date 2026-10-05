# Native acoustic authoring and capture

Poima 0.0.15 adds native WAV import, editable `AudioEmitter` and `AcousticMaterial` components, and Steam Audio 4.8.1 observations of authored or live worlds. Agents can inspect direct paths and save actual stereo HRTF-processed output. A moving physics door changes its acoustic obstruction even when the source and listener remain stationary. The existing C# door game is included in that regression test.

This first integration renders **frozen audio snapshots offline**. It does not yet play sound through an output device, maintain event/voice cursors during gameplay, or provide C# sound events. Reflections, reverberation, diffraction, streaming, buses, captions and the complete environmental audio system remain unfinished. These capabilities are reported explicitly rather than implied by the presence of Steam Audio.

## Build

The ordinary headless build supports audio component authoring and clip import without an SDK, device or network download. For propagation and HRTF capture, enable the optional native backend:

```sh
python3 scripts/bootstrap_tools.py --only audio
cmake --preset runtime-headless -DPOIMA_ENABLE_AUDIO=ON
cmake --build --preset runtime-headless
ctest --preset runtime-headless
```

Use `windows-runtime` instead for the native Windows build. The SDK is pinned to 4.8.1 with the release's SHA-256 and extracted under `.cache/sdk/steam-audio-4.8.1`. `POIMA_STEAM_AUDIO_ROOT` selects that SDK directory. CMake verifies its header version. The Windows build copies `phonon.dll` beside `poima.exe`; the Linux build links its `libphonon.so`. Installation includes the selected library and Steam Audio/third-party notices. The Linux installation uses a relative runtime library path. No graphics device or .NET installation is needed for native audio capture; C# testing additionally needs the [managed integration](MANAGED_GAMEPLAY.md).

Capabilities distinguish `audio_authoring`, `wav_import`, frozen `audio_capture`, logical `sound_events`, `audio_stream_capture` and optional `audio_device_playback`. Poima 0.0.16 adds [sound events, temporal recording and player output](AUDIO_EVENTS.md); the snapshot methods on this page retain their original behavior. The optional backend uses Steam Audio's CPU scene implementation with an SSE2 ceiling. Console implementations remain unqualified.

## Import and author

`asset.audio.import {"source":"path/to/clip.wav"}` imports mono, 48,000 Hz little-endian WAV containing PCM16 or IEEE float32. Source paths resolve against the world document's directory. Other rates/channels/compression currently return an explicit error; automatic resampling, stereo beds and compressed streaming will be separate import modes. Sources are bounded to 32 MiB, with clips of 1–2,880,000 frames (60 seconds). Samples must be finite and in [-1,1].

Import writes a canonical `poima.audio.v1` package beside the world's existing asset store, identified by its SHA-256. Reimporting equivalent samples gives the same identity. `asset.audio.inspect {"asset":"<64 lowercase hex digits>"}` returns format, package bytes, frames, duration, channel count and sample rate. Loading verifies the package hash; the original WAV can be removed after import. The version-one binary contains the eight-byte `PAUDIO1\0` signature, a little-endian uint32 frame count and mono IEEE float32 samples at 48 kHz. Signed zero is canonicalized on import.

Both new components use existing `component.set`, `component.remove`, preview transactions, hierarchy queries and `entity.get`. `world.describe` schema revision **14** supplies their full parameter schemas.

```json
{
  "AudioEmitter": {
    "asset": "<imported clip SHA-256>",
    "gain": 1.0,
    "loop": true,
    "enabled": true
  },
  "AcousticMaterial": {
    "absorption": [0.1, 0.2, 0.3],
    "scattering": 0.5,
    "transmission": [0.5, 0.15, 0.03],
    "enabled": true
  }
}
```

An emitter is an omnidirectional point at its entity's world position. Object scale does not change its loudness or radius. Gain is [0,4], and `loop`/`enabled` are Booleans. At most 64 emitters may be enabled. Each observation starts every enabled clip at sample zero; `loop` repeats it through the capture duration. Disabled emitters are omitted. Shared clips load once into a bounded cache, with at most 64 MiB of audio packages per snapshot/runtime definition.

Acoustic coefficients are [0,1]. Absorption and transmission contain three low/mid/high bands; scattering is scalar. This direct-path implementation uses transmission. Absorption and scattering are authored and reported for future reflection simulation, but currently do not produce reflections.

Only geometry with an enabled `AcousticMaterial` participates. If a `BoxCollider` is present, its scaled half extents define the acoustic box. Otherwise a `StaticMesh` supplies its actual triangles, or a `MeshRenderer` supplies its unit box. Visual visibility does not disable acoustic geometry. Authored audio observations load only required acoustic meshes and audio clips; unrelated visual assets and texture overrides are not loaded. Removing/disabling the material opens its acoustic path. Source placement should avoid embedding the source within its own acoustic blocker; emitter-specific geometry exclusion is not yet implemented.

Geometry and source positions must fit the initial ±10 km coordinate envelope, and source-listener distance is limited to 10 km. The listener may be any entity with a rigid, unscaled world transform, typically the player's camera. The observation budget is 131,072 acoustic triangles and 393,216 vertices. These are initial implementation guards, not a qualified large-world envelope.

## Observe and capture

| Method | Required parameters | Optional parameters |
| --- | --- | --- |
| `world.audio.inspect` | `revision`, `listener` | None |
| `world.audio.capture` | `revision`, `listener`, `path` | `frames` (default 48,000) |
| `runtime.audio.inspect` | `session_id`, `tick`, `listener` | None |
| `runtime.audio.capture` | `session_id`, `tick`, `listener`, `path` | `frames` (default 48,000) |

Inspection returns the exact listener pose, each emitter's sampled position, clip/gain/loop settings, listener-relative direction, direct visibility, distance attenuation, three-band air absorption/transmission and propagation delay. Visibility is 1 for an unobstructed direct ray and 0 for an obstructed ray. The report includes the actual acoustic geometry, authored revision, runtime session/tick when applicable, backend identity and preparation/simulation/DSP timings. Timings are diagnostic wall-clock costs of this synchronous fixture, not an audio-callback performance claim.

Capture additionally returns the WAV path, SHA-256, peak, RMS and number of samples outside [-1,1]. Output is unclipped stereo IEEE float32 at 48 kHz, bounded to 1–480,000 frames (10 seconds), with the requested exact frame count. There is no automatic normalization or limiter to conceal excessive mix gain. It combines inverse-distance attenuation with a one-meter minimum distance, default air absorption, single-ray occlusion, frequency-dependent transmission across at most eight surfaces and the default HRTF with bilinear interpolation. Per-source delay uses the rounded sample count for distance / 343 m/s; output before that arrival is exactly silent. The finite capture window may truncate a delayed signal or filter tail.

Paths must be new files in existing directories. Relative output paths resolve from the process working directory, matching image captures. Existing files, world-service reserved files and paths within the immutable asset store are rejected. These observations create artifacts but do not advance physics, change game fields or mutate authored data. Captures have no retry receipts; use a fresh path after an uncertain response, or inspect the existing artifact.

At runtime, clips and acoustic component settings are frozen with the starting authored revision. Current transforms come from the live native runtime, including moving doors and their descendants. Each audio observation builds fresh geometry from that exact state. Later authored edits affect authored observations and the next runtime, not the current runtime's settings. Fresh synchronous snapshots avoid stale pose/geometry publication at this stage; asynchronous propagation, incremental scene updates and emitter-generation retirement still require implementation.

Typical errors are `-32003` for a backend not built, `-32009` for stale revision/tick, `-32004` for a missing listener, `-32050` for asset import/inspection errors, `-32070` for acoustic preparation/render failures and `-32602` for invalid parameters or unsafe output destinations.

## Reproduce the sliding-door capture

The original [diagnostic probe](../examples/assets/acoustic-probe.wav) contains a quiet three-frequency tone, not borrowed game audio. [audio-room.jsonl](../examples/audio-room.jsonl) imports it, creates a source and listener, captures the closed door, moves the solid door for 120 physics ticks, then captures the open path:

```sh
./build/runtime-headless/poima world build/audio-room.world.json < examples/audio-room.jsonl
```

Run from the repository root with a fresh world and unused `build/audio-door-closed.wav` / `build/audio-door-open.wav` paths. Its import source is relative to the world file in `build/`. For Windows from PowerShell in `D:\poimaengine`:

```powershell
Get-Content examples/audio-room.jsonl | .\build\windows-runtime\poima.exe world build/audio-room.world.json
```

The request file is platform-independent. It does not open a graphics window or audio device. Listen to the generated files using an ordinary audio player; headphones are needed to assess the binaural output. No automated listening or subjective realism claim is made by the numeric tests.

## Verification and remaining work

`tests/audio_contract.py` runs against the actual engine. It verifies source-independent packages and corrupt-file rejection; invalid formats; atomic authoring/preview; stationary source/listener with changing door obstruction; independent frequency-domain filtering measurements; gain preservation; repeatability; multi-emitter mix linearity; disabled and imported hidden geometry; scaled source placement; live physics poses and frozen authoring; stale guards; distance delay; different elevation HRTFs; lateral cues and listener rotation; and capture path protection.

For the C# interaction case, provide `--hostfxr`, `--bridge` and `--game` paths as in the [managed guide](MANAGED_GAMEPLAY.md). The game actually receives `use`, moves the native door, and changes the observed path without either audio endpoint moving. This fixture verifies acoustic reaction to C# gameplay. Separate [event tests](AUDIO_EVENTS.md) now exercise C# play/stop commands.

[Recorded evidence](evidence/m2-native-audio.json) preserves checks and capture hashes for Linux and native Windows. The door clips and measured frequency attenuation are linked from that record. Elevation-filter differences prove different DSP output, not reliable human above/below localization. Windows/Linux output may differ at floating-point roundoff level; no cross-platform bit-exact audio guarantee is made.

Listen to the actual C#-triggered [closed-door capture](evidence/m2-audio-door-closed.wav) and [open-door capture](evidence/m2-audio-door-open.wav). These contain the test signal, not a sound-design demonstration. In the separate Windows stationary-door fixture, measured closed/open amplitude ratios were:

| Probe frequency | Closed/open amplitude |
| --- | --- |
| 200 Hz | 0.310 |
| 1,000 Hz | 0.118 |
| 6,000 Hz | 0.052 |

The high-frequency component is attenuated more strongly. Those measurements come from captured PCM after startup, independently of the backend's reported path coefficients.

The continuous event/mixer, C# event bindings and optional device adapter are described in [Audio events](AUDIO_EVENTS.md). Worker publication, incremental acoustic updates, reflections/reverb and around-corner propagation are not implemented. Source streaming, buses/ducking, device changes, priority/virtualization, environmental transitions, proximity voice, captions and console backends are also unsupported; production listening and resource qualification remain incomplete.
