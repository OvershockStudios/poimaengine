# Editor audio

The Audio window controls native sound output during Play. Open **Window → Audio**, select **Enable output**, set volume, and apply. The Game toolbar speaker button toggles mute; its context menu opens Audio settings. Preferences last for the current editor session. Output starts disabled.

Select a Game camera as the listener. Existing gameplay sound events drive voices; adding an enabled AudioEmitter does not automatically play its clip. Enabling output while stopped opens no device. Paused stepping is silent. Input capture, Scene navigation, pane visibility and docking do not select additional listeners or duplicate sound.

The current backend provides the same direct-path attenuation, occlusion, transmission, propagation delay and binaural processing as [native audio events](AUDIO_EVENTS.md). It does not add reflection/reverb, mixer buses, compressed streaming, voice virtualization or device selection. This is a Windows editor integration; the native audio observation and standalone player have their own platform contracts.

## Shared commands

`desktop.audio.inspect` returns configuration, generation, worker/output state, listener/session, epoch, queue counts, generated/submitted/dropped frames, discontinuities and errors. `desktop.inspect` includes the same `audio` object. Discover schemas through `desktop.describe`.

A complete configuration uses an optimistic generation guard and request ID:

```json
{"jsonrpc":"2.0","id":1,"method":"desktop.audio.configure","params":{"request_id":"00000000000000000000000000000001","expected_generation":0,"enabled":true,"muted":false,"volume":0.75}}
```

Volume must be finite and between zero and one. The latest 32 accepted configurations retain exact retry receipts in memory. An exact retry returns the original result with `replayed:true`; reusing an ID with different parameters conflicts. A stale generation cannot overwrite another human or agent's preferences. The Audio window preserves dirty drafts and pending requests until explicitly resolved.

`desktop.audio.retry` clears a presentation fault and permits a fresh baseline on the next eligible Play poll. It does not restart simulation or create sound events. No output device, an invalid listener, resource-budget overflow or DSP failure is an audio error; an already committed gameplay tick remains committed.

## Processing and lifecycle

The owner captures immutable audio state after each successful automatic tick. A single worker processes acoustics and PCM without accessing the world, registry, GUI or owner-thread profiler. Owner polling submits completed PCM to SDL. No poll waits for DSP completion or for a device queue to drain. Snapshot capture, queue synchronization and SDL operations still have costs; this is not a hard real-time guarantee.

Explicit audio observation, capture and replay commands retain their synchronous contracts. This worker schedules automatic Play output; it does not turn every audio command into a background job.

Pause, Stop, save/load replacement, Game-camera changes and capture suspension invalidate the audio epoch and clear application output queues. Old worker results cannot enter the resumed epoch. Resume attaches to current logical voice timing with fresh filter history, rather than preserving reverb/filter tails. Muting clears audible buffers while allowing DSP to continue. Already delivered operating-system or hardware samples cannot be recalled.

The presenter bounds queued work and output. Overload discards presentation and rebases from a current snapshot, reporting discontinuities and dropped work rather than slowing simulation or advancing skipped ticks through stale geometry. Queue lengths do not measure speaker latency; muted/discarded frames and worker timing do not establish audible quality or hardware underrun rates.

There are at most eight queued snapshots and one in flight. Queue/in-flight descriptor storage is capped at 16 MiB, with a 4 MiB per-frame cap; DSP may also retain its current snapshot. An epoch pins a bounded immutable asset closure: up to 64 MiB of clip storage and 256 MiB of mesh/texture storage. A retiring worker can retain the previous closure while the new one is pending. These figures exclude SDK allocations and the rest of the engine; they are not a total process-memory budget. PCM results and SDL input each cap at 4,800 stereo frames, with one worker result in flight. New asset identities require a fresh baseline; ordinary frozen-runtime ticks share existing assets.

The GUI closes asynchronously: `desktop.close.begin` stops presentation, then owner polling continues until `audio.closed`. The worker retains its resources until its current SDK call finishes. Native clients should follow that sequence before detaching and destroying the host. Direct `poima_desktop_destroy` remains a synchronous compatibility fallback; an uncancellable acoustic SDK call has no guaranteed retirement deadline. Closing rejects ordinary editing/playback requests while final inspection remains available.

Owner work appears under `editor.audio.snapshot`, `world.audio.snapshot` and `editor.audio.poll` in the [Profiler](PROFILER.md). Worker DSP totals appear in audio status; they are not cross-thread timeline events.

## Verification

The [0.0.39 evidence](evidence/m2-editor-audio.json) records native PCM/lifetime tests, 93 Linux suites, Windows bridge and C# checks, and Audio controls on both laptop GPUs. Actual output was muted or set to zero volume. The first WASAPI-open poll took 666 ms on the shared test machine and caused the normal simulation clock cap to drop wall time. Device initialization remains synchronous; moving DSP off-thread does not eliminate that startup stall.
