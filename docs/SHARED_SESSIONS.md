# Shared local sessions

Poima’s desktop editor, CLI users and external agents can work through one authoritative world session. The editor is a first-class authoring workspace, and headless hosting is also supported. Edits, revisions, undo and retry receipts belong to the host. Disconnecting a client leaves the host and other clients running. This is local authoring IPC, separate from future multiplayer networking.

## Open an editor and attach

From Windows, after [building the desktop editor](DESKTOP_EDITOR.md#build-and-launch):

```text
build\desktop-win-x64\Poima.Editor.exe projects\desktop-sandbox\world.json --endpoint poima-desktop
build\windows-runtime\poima.exe connect poima-desktop
```

The second command accepts UTF-8 JSON-RPC, one object per line, and flushes replies as they arrive. It can be kept open by an agent or supplied a file through stdin. The desktop launcher exposes `poima-desktop` by default. Choose distinct endpoint names for simultaneous editors. Parent project directories must exist.

```json
{"jsonrpc":"2.0","id":1,"method":"world.describe"}
{"jsonrpc":"2.0","id":2,"method":"desktop.describe"}
{"jsonrpc":"2.0","id":3,"method":"world.inspect"}
{"jsonrpc":"2.0","id":4,"method":"desktop.inspect"}
```

Use the same `world.transact`, entity queries and history operations described in [the world service](WORLD_SERVICE.md). Read the current revision before editing. A second client submitting an outdated base revision gets a conflict. Both clients see the same undo history; undo is session-wide, not per-client.

For a headless host:

```sh
./build/headless/poima serve build/example.world.json --endpoint example
./build/headless/poima connect example
```

`host.shutdown` closes a headless host. `session.close` is rejected in shared sessions with `-32080`; close stdin to detach a client. The editor owns its window lifetime and does not expose `host.shutdown`. Direct `poima world` remains a standalone owner whose EOF closes the session. A second standalone writer cannot open a world already owned by a host/editor.

Windows and Linux endpoints are separate OS facilities. From WSL, use the Windows `poima.exe connect` to reach a Windows editor, with Windows paths in operation parameters. Native Linux `poima connect` reaches a native Linux host.

## Desktop operations

The current [desktop editor](DESKTOP_EDITOR.md) exposes `desktop.*` methods over this transport:

| Method | Parameters and behavior |
| --- | --- |
| `desktop.describe`, `desktop.inspect` | Discover methods and inspect shared state, including independent Scene/Game attachment, errors and presentation metadata. |
| `desktop.camera` | Update the free Scene inspection camera. |
| `desktop.game.camera` | Select a Game Camera entity, or `null` to clear it. Scene remains independent. |
| `desktop.play.start` | `revision`, fresh `session_id`, optional `paused`; starts real-time playback unless `paused: true`. |
| `desktop.play.pause`, `.resume`, `.stop` | Control the shared simulation using its `session_id`. |
| `desktop.play.step` | Advance a paused runtime through the discovered runtime-step schema. |
| `desktop.capture` | Guarded `revision`, new `path`, optional `view: "scene"` or `"game"`; queue a native viewport BMP. Named viewports default to Scene. |
| `desktop.capture.status` | Inspect the asynchronous job by `capture_id`. |

One host poll drives the fixed-step simulation. Drawing either viewport never advances it. Scene and Game can be docked, floated or shown together; they share authoring and runtime state but have independent cameras and presentation lifetimes. A queued capture holds automatic ticking until completion or error. There is one pending capture slot across both panes, and only the target pane completes its job. Scene camera changes do not invalidate a Game capture. Desktop captures contain the native viewport, not the full Avalonia interface.

Direct `runtime.start` remains paused. During Play, Game uses the runtime’s frozen camera lens and live pose; stopping returns to the authored camera. Gameplay input is a separate explicit focus gate. Consult the desktop guide for input and capture details.

## Legacy ImGui editor operations

The older `poima editor` command exposes the following `editor.*` API. These methods and whole-window capture behavior belong to that frontend, not the Avalonia desktop:

| Method | Parameters and behavior |
| --- | --- |
| `editor.describe` | No parameters; discovers editor methods and policies. |
| `editor.inspect` | Current selection, authoring revision, Inspector draft/conflict, runtime state and last presented revision/tick. |
| `editor.select` | `id`; refuses changing selection while an Inspector draft is unfinished. |
| `editor.play`, `editor.stop` | Start a frozen runtime or stop it. Remote Play starts paused. |
| `editor.pause` | `paused` Boolean. |
| `editor.step` | `ticks` in 1..600; requires an active runtime. |
| `editor.capture` | `revision` and new `path`; capture the fresh current editor window through its existing renderer. |

`runtime.status` is also available without starting a runtime, including in builds without simulation. It reports availability, active identity, tick and the runtime's frozen authored revision. A runtime started through another client appears paused in the GUI. Core runtime stepping and the GUI share the same runtime instance. Authoring edits during runtime inspection affect the authored world; the frozen runtime retains its starting scene until restarted.

A capture waits for a fresh presented frame, up to two seconds when presentation is unavailable. Its result separates the guarded current authoring `revision` from `scene_revision`, `source` (`authored` or `runtime`), `tick`, `frame`, `width`, `height` and output `path`. Thus a frozen runtime cannot masquerade as the newest authored scene. Later queued client operations wait until that capture resolves. GUI edits that change the guarded revision before capture cause a conflict. Captures include the actual editor UI and inspection camera; they do not require an authored Camera entity.

A missing asset or other snapshot-construction failure keeps the authoring session open for repair. The viewport shows the last valid scene with an error, and `editor.inspect.presentation` distinguishes its revision from current authoring. Capturing the unavailable current scene fails instead of returning stale pixels. Undo or a correcting transaction can restore presentation. GPU/device failures still require recreating the editor.

```json
{"jsonrpc":"2.0","id":5,"method":"editor.capture","params":{"revision":0,"path":"build/editor-view.bmp"}}
```

Use an existing parent directory and a new output filename. Captures cannot overwrite existing files, the world, its sidecars, the script or reserved startup report/capture outputs. A failed disk write can leave its own incomplete new file. Captures are BMP, with one staging allocation capped at 128 MiB of RGBA texels; ordinary frames do not perform readback.

Both shared editors reject `world.capture`, `runtime.capture`, `asset.animation.capture` and `runtime.play` with `-32080`, since those bypass the editor-owned graphics lifetimes. Discovery points to `desktop.describe` for the current desktop or `editor.describe` for ImGui. Use that frontend’s capture and simulation controls instead.

## Human drafts and agent changes

An unfinished Inspector draft retains its original values and base revision across incoming edits, including deletion of its entity. A changed world revision marks the draft conflicted; Apply refuses a stale draft. **Reload** explicitly discards it and reads the latest committed state. Even unrelated remote edits require Reload in this initial conservative policy; field-level merging is not implemented.

Selection-changing local actions, delete and undo/redo refuse to discard dirty drafts silently. Applying one component preserves other unfinished Inspector fields. The Avalonia desktop keeps unfinished Inspector drafts locally and refuses closing until Apply or Reload; its native selection can differ from a retained dirty Inspector selection after an external request. The legacy ImGui API exposes draft/conflict state through `editor.inspect`, but provides no remote method to forcibly clear it; closing that frontend discards uncommitted drafts. Neither frontend persists drafts as project data.

## Transport and limits

Endpoint names are 1–64 ASCII letters, digits, underscores or hyphens. They are logical names, not paths. Windows uses local named pipes with a protected current-user DACL, owner validation and remote-client rejection. Linux uses AF_UNIX in an owned mode-0700 `/tmp/poima-UID` directory, mode-0600 sockets and same-UID peer checks. Endpoint names are not secrets. Any process running as the same user can use the authoring API; this is not a sandbox for untrusted plugins.

The framing is a four-byte little-endian payload length followed by strict UTF-8. Requests are 1 byte through 1 MiB; responses are zero through 32 MiB. Empty responses acknowledge notifications without emitting stdout. JSON-RPC batches remain unsupported. The host admits eight active clients, one in-flight request each; OS connection backlogs may contain additional waiting connections. Polling bounds each client's transport I/O to 256 KiB per call. World operations execute serially on the owner thread.

In 0.0.45 development, `reply()` also attempts an immediate nonblocking write,
with its own 256 KiB budget. Small responses can leave the server before the next
owner sleep or editor frame; partial or backpressured responses retain their
offsets for later polls. This write does not receive or dispatch new requests.
Successful acceptance is not confirmation that the client received the reply.
An observed disconnect returns false, possibly after partial delivery; the world
operation may already have committed. There is no automatic replay, extra
transport thread, busy wait or change to global timer resolution.

In 0.0.46 development, the Windows client opens its pipe for overlapped I/O
and waits on a completion event instead of sleeping and retrying during an
exchange. Reads and writes share one absolute deadline, including partial
frames. At that checkpoint the server retains its nonblocking polling contract;
endpoint startup retries, headless owner sleeps and editor dispatch cadence
are unchanged.
Linux clients continue to wait on socket readiness.

A failed Windows wait cancels and reaps that operation before releasing its
buffer, event or `OVERLAPPED` structure, then closes the connection. Reaping
kernel cancellation can exceed the requested deadline; the timeout does not
promise a hard bound on driver cleanup. See Microsoft's [overlapped I/O
guidance](https://learn.microsoft.com/en-us/windows/win32/ipc/synchronous-and-overlapped-input-and-output)
and [cancellation rules](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex).

In 0.0.47 development, the headless owner calls `wait(timeout_ms)` after
polling and dispatching requests. Windows servers retain overlapped connection,
read and write operations with private completion events; Linux servers wait on
eligible socket readiness. This replaces the headless owner's repeated 2 ms
sleep. A wait can arm transport I/O but never returns, discards or dispatches
requests; the next `poll()` consumes progress. Synchronous completions and
budget continuations remain eligible without another client write.

`wait()` accepts 0..600000 ms, including an immediate check at zero. It returns
true when eligible progress, a connection or an error may need polling, and
false on timeout or interruption. Pending application replies exclude
pipelined reads. Windows checks those clients' liveness in completion-wait
slices of at most 100 ms; the headless owner also uses a 100 ms idle maintenance
ceiling. Disconnect/error cleanup cancels and reaps pending Windows operations
before reusing buffers or peer slots and can exceed that deadline. All calls
remain serialized by the owner. GUI callers keep the normally nonblocking
`poll()`; editor dispatch cadence and endpoint startup retries are unchanged.

`connect --timeout-ms N` accepts 100..600000 milliseconds, default 30000, for connection establishment and each exchange. A timeout/disconnect closes the client connection and does not replay the operation. The operation may already have committed: inspect state, then explicitly retry with the same durable `request_id` where supported. Editor controls/captures do not have durable request receipts. Transport startup/exchange errors exit the CLI with code 4; stderr explains the failure, and stdout uses the CLI error envelope rather than a JSON-RPC result.

Normal shutdown attempts a bounded reply drain; a nonreading client or graphics failure can still observe a disconnect. Linux clean shutdown removes only the owned socket inode. A crash can leave a socket path: verify the old host is gone before removing that endpoint, or use a new name. Startup never removes a preexisting endpoint automatically.

The native interfaces are `poima/local_session.hpp`, `poima/shared_session.hpp` and `WorldSession::request(..., WorldRequestScope)`. Call `poll()` to receive requests and flush remaining `reply()` data; never discard requests from an extra poll intended only to flush replies. Callers serialize WorldSession access; this is not a stable binary plugin ABI.

### Measure authoring latency

The repository includes a provider-free probe using the Python client. Supply a
compatible native engine and a persisted authored world, and run Python on the
engine's operating system. The output directory must be new:

```powershell
python tools/benchmark_shared_session.py `
  --engine build/windows-runtime/poima.exe `
  --world docs/evidence/fixtures/agent-defense/world.json `
  --output build/shared-session-probe --repeats 4
```

Each pair measures standalone and freshly owned shared hosts against identical
world copies. The order alternates; four pairs balance which mode runs first.
Defaults are ten warmup and 100 timed `world.inspect` calls per mode per pair,
with core response validation and identity/revision/count checks. Startup,
discovery, warmup and cleanup are outside individual timings. Requests and
cleanup have explicit budgets; no runtime, GPU, provider or authored edits run.
Success requires zero exit codes from all directly owned native processes.

`public-summary.json` contains hashes, parameters, per-trial and pooled timing
statistics, correctness and cleanup results. `local-report.json` also contains
machine-local paths, individual samples and bounded diagnostics. Publish the
summary rather than the local report. The [0.0.45 evidence](evidence/m2-immediate-replies.json)
records Windows before/after trials. These are authoring-query measurements on
one machine, not game frame rates, agent success rates or GUI input latency.

### Measure idle host CPU time

A second provider-free probe measures one idle headless host with one connected
idle client. Use native Python on the engine's operating system and a new output
directory:

```powershell
python tools/benchmark_shared_idle.py `
  --engine build/windows-runtime/poima.exe `
  --world docs/evidence/fixtures/agent-defense/world.json `
  --output build/shared-idle-probe --seconds 5
```

Startup, inspection queries and shutdown are outside the accounting interval.
The probe checks world identity before and after, unchanged input hashes, and
clean exits for both owned processes. Windows sums kernel and user time with
[GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes);
Linux sums `utime` and `stime` from
[/proc/pid/stat](https://man7.org/linux/man-pages/man5/proc_pid_stat.5.html).
The recorded percentage is relative to one CPU core. Accounting has finite
granularity: a zero delta does not prove zero work or energy use. This measures
neither battery life nor GUI, rendering or gameplay performance. Publish
`public-summary.json`; local diagnostics stay in `local-report.json`.

## Qualification and current limits

The following evidence describes the original ImGui shared-session checkpoint. Current Avalonia and independent Scene/Game qualification is documented in the [desktop guide](DESKTOP_EDITOR.md).

[Recorded evidence](evidence/m2-shared-sessions.json) covers 23 headless and 27 runtime CTest suites, native Windows transport/session checks, seven Windows shared-host tests, and live shared-editor checks on both NVIDIA and AMD laptop GPUs. The [actual editor capture](evidence/m2-shared-sessions.png) shows an Inspector draft preserved across an external revision. `tests/shared_editor_capture.py` uses two real CLI connections and actual captures; its draft setup uses the GUI's semantic action dispatcher. It does not qualify physical mouse/keyboard input or autonomous agent policy.

Imports, transactions and runtime calls remain synchronous and can stall every client and the GUI. The legacy ImGui editor loop has a 60 Hz CPU-side cap (30 Hz when unfocused or minimized). The Avalonia desktop uses a 33 ms dispatcher timer: it polls the shared host once, then draws its two viewports synchronously. Native simulation uses fixed 60 Hz ticks with bounded catch-up; the dispatcher interval is not a rendering frame-rate guarantee. View rendering can still wait on frame-slot completion; see [render diagnostics](RENDER_DIAGNOSTICS.md). No large-project responsiveness or game-performance claim follows from this fixture. The desktop now exposes remote Scene/Game camera controls. The development [MCP adapter](MCP.md) targets this transport; bounded Windows/Linux endpoint and agent-client checks are recorded in the [client guide](AGENT_CLIENTS.md). Change subscriptions, separate-process GUI attachment to a headless host, durable drafts and collaborative permissions remain future work.
