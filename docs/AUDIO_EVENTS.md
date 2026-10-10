# Sound events, temporal recording and player output

Poima 0.0.16 adds native logical voices, C# play/stop calls, persistent direct/HRTF filters, temporal WAV recording and optional SDL3 player output. [Acoustic assets and materials](AUDIO.md) remain the authoring surface. This is the first integrated event/output path, not the complete environmental audio system or a production performance qualification.

## Logical voice ownership

An enabled `AudioEmitter` is an available source, not an automatically playing source. `runtime.step` accepts `sounds`, as do `runtime.play` replay segments and `runtime.audio.replay` segments:

```json
"sounds": [
  {"op":"play", "emitter":"00000000000000000000000000000065", "gain":0.5},
  {"op":"stop", "voice":1}
]
```

Commands run in array order on the segment/batch's first tick, before C# gameplay. Gain defaults to 1 and must be finite in [0,4]; it multiplies the authored emitter gain. Each play starts a separate voice, including repeated plays on one emitter. Stops are idempotent while the record exists; unknown or expired handles fail. Playing and immediately stopping at the same tick emits nothing. The authored `loop` flag controls repetition.

C# uses `long voice = context.PlaySound(emitter, gain)` and `context.StopSound(voice)`
inside `Tick`; these calls are unavailable in `Initialize` and UI `Control`.
Store handles in native-owned game state when they must survive reload. At most
64 caller commands per batch and 64 C# commands per tick are accepted; at most
64 voices may emit at once. Handles are monotonic within a runtime session and
never reused after an ordinary stop. A failed batch restores voice state
**and handle allocation**, alongside C# fields and physics. No device or DSP
observes the failed intermediate state. Sound callbacks belong to the current
epoch-7 baseline services prefix; use the matching
[gameplay compatibility profile](ALPHA_GAMEPLAY_PROFILE.md), SDK and bridge.

`runtime.step` reports `sound_events: {first_voice, next_voice}`, the half-open handle range created by that successful batch, including C# calls. Retry receipts return the same result without creating more voices. These are logical handles; playback is a separate consumer of committed simulation.

`runtime.audio.voices {session_id,tick,after?,limit?}` lists retained records in handle order. The default limit is 64, maximum 256; `after` is an exclusive handle cursor. The response includes `next_voice`, `emitting`, `retained`, `has_more`, source/asset, gains, loop mode, start tick, stop/end sample and current emitting clip frame. Stop/end sample positions are decimal strings to retain integer precision even at very large runtime ticks; an unbounded loop has a null end. `clip_frame` describes emission time, not the delayed sample at the listener. The oldest finished records are evicted when the 256-record history fills. DSP may still be playing a delayed arrival or filter tail after emission ends.

### Removing emitters and saving

Removing a runtime instance retires every logical voice belonging to its removed
emitters, including finished records. Surviving voices keep their handles and
cursors; `next_voice` keeps the allocator's high-water mark. Retained IDs can have
gaps, omit the latest allocated handle, or be empty with `next_voice > 1`.
Saving and restoring those states preserves the allocator without resurrecting
removed emitters. IDs must still be positive, strictly increasing and below the
allocator; existing chronology, trusted clip bindings and capacity checks apply.
The `poima.sound-state` format remains version 1. Older engine builds that enforce
contiguous retained IDs cannot load these valid retirement states.

Do not attach a pickup cue to the prop that gameplay immediately despawns: its
logical voice is retired with the prop. Use a surviving dedicated emitter when
the cue should continue. Retired or evicted handles are unknown to `StopSound`;
do not retain them as an indefinite stop capability.

To verify this lifecycle, import a clip, author a hierarchical emitter recipe,
spawn instances, play them and remove one at an observed tick/structure revision.
Inspect `runtime.audio.voices`, then use guarded `save.write`. Reopen the world and
cooked assets in a fresh owner and load the exact slot; compare retained voices,
sample cursors and `next_voice`. Spawn/play again and check allocation continues
above every retired handle. The [executable task](../tests/sound_retirement_contract.py)
also checks whole-history retirement, failed-batch rollback and relocated
source-independent saves. [Recorded qualification](evidence/m2-sound-retirement.json).

These are logical simulation/save guarantees. Device queues and acoustic filters
are reconstructed after restoration; they do not promise seamless waveforms or
prove audibility. See the player output section below for those boundaries.

## Headless temporal recording

Unlike `runtime.audio.capture`, which observes one frozen pose and restarts enabled clips, `runtime.audio.replay` advances the simulation and records explicit live voices:

```json
{
  "session_id":"00000000000000000000000000000384",
  "request_id":"000000000000000000000000000003e8",
  "expected_tick":0,
  "listener":"00000000000000000000000000000064",
  "path":"build/temporal.wav",
  "sequence":[
    {"ticks":30,"sounds":[{"op":"play","emitter":"00000000000000000000000000000065"}]},
    {"ticks":60,"motions":[{"entity":"00000000000000000000000000000002","position":[3,1.5,-3],"rotation":[0,0,0,1],"duration_ticks":60}]},
    {"ticks":30,"sounds":[{"op":"stop","voice":1}]}
  ]
}
```

Requires a runtime and the optional Steam Audio build. Segments accept `ticks`, `inputs`, `motions` and `sounds`; no character controller is mandatory. The same native input semantics apply: movement is held, look/jump/use occur on the first tick. There are 1–256 segments, each 1–600 ticks, totaling at most 3,600 ticks (60 seconds). Every successful tick contributes exactly 800 frames to the final stereo 48 kHz float WAV, including silent intervals. Output is not normalized or limited; peak and over-range samples expose excessive gain.

The operation validates structural input and destination before starting, then commits one tick at a time. A gameplay/DSP/output failure returns `success:false`, the actual final `tick`, detail and stream diagnostics. Earlier successful ticks remain committed; a failing simulation tick rolls back. Failed recordings have no successful capture metadata; file-write failure can leave an incomplete artifact. Retry the same request ID to retrieve its receipt without running more ticks; use a new request ID and the actual current tick to continue. Receipts share the session's bounded 32-entry cache with other runtime mutations. Do not interpret a failure as a whole-sequence rollback.

Successful output includes the WAV SHA-256 and format/frame metadata. `stream` reports generated frames, processed 512-frame blocks, voices started, acoustic path updates, peak, over-range samples and synchronous DSP/path-update time. Each new recording attaches to active voices at their current emission offset; it does not restart clips. A new stream has fresh filter history, so attaching mid-playback has a brief filter warm-up. Delayed arrivals from retained recent voices can still be heard. Recordings do not preserve DSP state across separate requests.

## Native player device output

Set `audio:true` on `runtime.play` for interactive or replay output. It defaults to false. The camera is the listener; events may come from replay `sounds` or C# gameplay. `audio_device_playback` in `capabilities` indicates that the adapter was built, not that a device is present. `sound_events` and `audio_stream_capture` report the separate logical and recording capabilities.

The first PC adapter uses SDL3's already-integrated platform layer. On Windows the build enables WASAPI while leaving PC rendering Vulkan-only. SDL receives copied 48 kHz stereo float PCM; its device thread never calls Poima gameplay, geometry construction or Steam Audio DSP. The current mixer/propagation work is synchronous on the player thread, so expensive acoustic scenes can still stall frames and starve output. Worker scheduling and production latency/resource gates remain outstanding. Source streaming is not implemented by this adapter.

Output prebuffers 2,048 frames, uses bounded backpressure above 4,800 queued input frames, and errors if the queue does not drain within 500 ms. The largest ordinary submission is 1,024 frames. Interactive focus/capture loss pauses the device with simulation; resume continues the stream. Replay is paced by graphics and audio backpressure. Finishing trims the final DSP block to committed time, flushes and drains the stream with a two-second bound. The capture duration can truncate remaining tails; stopping the viewport does not advance the game to finish them.

The player response's `audio` object reports driver, enabled state, submitted frames, maximum queued input frames, observed empty queues, backpressure time, stream-drained state, voice starts, peak, over-range samples and DSP time. An observed empty input queue is **not** a measured hardware underrun; queue length is **not** end-to-end output latency. Draining SDL's stream is not evidence that the final sample reached the speakers. Physical-device loopback, subjective listening, focus/device-loss stress and latency qualification remain separate gates.

## Editor device output

The desktop editor has a separate [bounded DSP worker and Audio window](EDITOR_AUDIO.md), with guarded enable/mute/volume controls, epoch invalidation and asynchronous shutdown. It follows automatic committed Play ticks and keeps SDL submissions on the owner thread. The synchronous standalone player adapter described above is unchanged.

## DSP and platform boundaries

Logical events and cooked audio assets do not depend on Steam Audio types. Each presentation stream owns a persistent native context/HRTF and each voice retains direct and binaural filter state. At most 256 delayed/active/tail DSP voices may coexist; exceeding that presentation budget fails explicitly. Voice prioritization and virtualization remain unfinished.

Complete 512-frame blocks are generated only through committed time, leaving at most 511 frames pending. Finalization renders/trims a partial block. Source onset/stop uses sample time, so a clip shorter than a simulation tick is retained and rendered. Distance delay is latched when a stream first attaches to a voice; moving endpoints update attenuation and direction, but not travel time or Doppler. The latest committed snapshot supplies each block's path parameters; this is not sample-accurate moving geometry.

Unchanged source/listener/geometry snapshots reuse acoustic path reports. Changed snapshots currently rebuild the reference acoustic scene synchronously. Persistent instanced scene updates, coherent worker results, reflection/reverb/pathing, smooth path transitions, device changes, speaker mixes, buses/ducking, compressed streaming, virtual voices, captions, environmental transitions and proximity voice are still required.

Valve's public supported-platform list does not include Xbox or PlayStation. The current Steam Audio backend is a PC integration. Its source availability permits investigating a port, but dependency/platform qualification requires licensed SDK access; a replacement acoustics backend may be needed. Poima's game/authoring API should remain stable across that change. No console audio implementation or compatibility is claimed. [Valve platform documentation](https://valvesoftware.github.io/steam-audio/doc/capi/getting-started.html), [Steam Audio source](https://github.com/ValveSoftware/steam-audio).

## Verification

`tests/audio_contract.py` extends the frozen acoustic suite with logical budgets/history, receipt replay, invalid commands, short one-shot onset/silence, loop phase, gain, midgame attachment, live moving geometry, partial-progress errors, C# creation/stop rollback and reload. Build the fixture with `.cache/toolchains/dotnet-10.0.401/dotnet build tests/fixtures/audio_game -c Release`, then supply `--sound-game tests/fixtures/audio_game/bin/Release/net10.0/Poima.AudioProbe.dll` plus the hostfxr/bridge paths to run it. The test injects a failure after sound and door movement and verifies joint rollback.

`tests/audio_player.py` opens the real native player/device, plays a moving source, stops it, retries the completed request and compares mixer statistics with an independent headless replay. It rejects dummy/disk drivers. `--fail-device` selects an intentionally nonexistent SDL driver and checks failure receipts/tick preservation followed by successful headless recording; run that variant through native Windows Python when testing Windows, so its child inherits the intended environment directly. This verifies device submission and stream drain, not a recording from physical speakers. Build/test evidence and limitations are recorded with the implementation checkpoint.

SDL references: [device stream](https://wiki.libsdl.org/SDL3/SDL_OpenAudioDeviceStream), [copied PCM submission](https://wiki.libsdl.org/SDL3/SDL_PutAudioStreamData), [queued input bytes](https://wiki.libsdl.org/SDL3/SDL_GetAudioStreamQueued).

Run the complete [headless door timeline](../examples/audio-events-room.jsonl) from the repository root with a fresh world and unused WAV destination:

```sh
./build/runtime-headless/poima world build/audio-events.world.json < examples/audio-events-room.jsonl
```

It creates a looping diagnostic source, opens the acoustic door, stops the voice, and writes `build/audio-events.wav` (2.5 seconds). The final voice query shows the stopped logical state. The sound is an original test tone, not a demonstration of sound design.

[Checkpoint evidence](evidence/m2-audio-events.json) records 17 passing audio cases on Linux and 17 on native Windows, 13 headless/16 runtime CTest checks, and the actual Windows WASAPI run. That device run submitted 96,000 frames, peaked at 5,824 queued input frames, and observed no empty input queue at submission checks. These observations do not prove zero hardware underruns. The deliberately unavailable device case preserves tick zero and a retry receipt, then records headless audio successfully.

Listen to the [recorded diagnostic timeline](evidence/m2-audio-events.wav): the tone becomes less muffled as the door opens, then stops. No subjective realism rating is inferred from its automated checks.
