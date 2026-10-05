# Shared local sessions

Poima 0.0.23 lets multiple CLI clients use one authoritative world session, including a running native editor. Edits, revisions, undo and retry receipts belong to the host. Disconnecting a client leaves the host and other clients running. This is local authoring IPC, separate from future multiplayer networking.

## Open an editor and attach

From Windows, after building the optional editor:

```text
build\windows-runtime\poima.exe editor projects\sandbox\world.json --endpoint sandbox
build\windows-runtime\poima.exe connect sandbox
```

The second command accepts UTF-8 JSON-RPC, one object per line, and flushes replies as they arrive. It can be kept open by an agent or supplied a file through stdin. The default launcher exposes `sandbox`; a custom world passed to the launcher uses `editor`. Choose distinct endpoint names for simultaneous editors. Parent project directories must exist.

```json
{"jsonrpc":"2.0","id":1,"method":"world.describe"}
{"jsonrpc":"2.0","id":2,"method":"editor.describe"}
{"jsonrpc":"2.0","id":3,"method":"world.inspect"}
{"jsonrpc":"2.0","id":4,"method":"editor.inspect"}
```

Use the same `world.transact`, entity queries and history operations described in [the world service](WORLD_SERVICE.md). Read the current revision before editing. A second client submitting an outdated base revision gets a conflict. Both clients see the same undo history; undo is session-wide, not per-client.

For a headless host:

```sh
./build/headless/poima serve build/example.world.json --endpoint example
./build/headless/poima connect example
```

`host.shutdown` closes a headless host. `session.close` is rejected in shared sessions with `-32080`; close stdin to detach a client. The editor owns its window lifetime and does not expose `host.shutdown`. Direct `poima world` remains a standalone owner whose EOF closes the session. A second standalone writer cannot open a world already owned by a host/editor.

Windows and Linux endpoints are separate OS facilities. From WSL, use the Windows `poima.exe connect` to reach a Windows editor, with Windows paths in operation parameters. Native Linux `poima connect` reaches a native Linux host.

## Editor operations

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
{"jsonrpc":"2.0","id":5,"method":"editor.capture","params":{"revision":0,"path":"D:/poimaengine/build/editor-view.bmp"}}
```

Use an existing parent directory and a new output filename. Captures cannot overwrite existing files, the world, its sidecars, the script or reserved startup report/capture outputs. A failed disk write can leave its own incomplete new file. Captures are BMP, with one staging allocation capped at 128 MiB of RGBA texels; ordinary frames do not perform readback.

A shared editor rejects `world.capture`, `runtime.capture`, `asset.animation.capture` and `runtime.play` with `-32080`, since those create a separate graphics lifetime. Discovery removes these methods and points to `editor.describe`. Use `editor.capture` and the editor's simulation controls instead.

## Human drafts and agent changes

An unfinished Inspector draft retains its original values and base revision across incoming edits, including deletion of its entity. A changed world revision marks the draft conflicted; Apply refuses a stale draft. **Reload** explicitly discards it and reads the latest committed state. Even unrelated remote edits require Reload in this initial conservative policy; field-level merging is not implemented.

Selection-changing local actions, delete and undo/redo refuse to discard dirty drafts silently. Applying one component preserves other unfinished Inspector fields. The agent can inspect the draft/conflict state but has no remote method that forcibly clears a human draft. Closing the editor still discards uncommitted drafts; they are not persistent project data.

## Transport and limits

Endpoint names are 1–64 ASCII letters, digits, underscores or hyphens. They are logical names, not paths. Windows uses local named pipes with a protected current-user DACL, owner validation and remote-client rejection. Linux uses AF_UNIX in an owned mode-0700 `/tmp/poima-UID` directory, mode-0600 sockets and same-UID peer checks. Endpoint names are not secrets. Any process running as the same user can use the authoring API; this is not a sandbox for untrusted plugins.

The framing is a four-byte little-endian payload length followed by strict UTF-8. Requests are 1 byte through 1 MiB; responses are zero through 32 MiB. Empty responses acknowledge notifications without emitting stdout. JSON-RPC batches remain unsupported. The host admits eight active clients, one in-flight request each; OS connection backlogs may contain additional waiting connections. Polling bounds each client's transport I/O to 256 KiB per call. World operations execute serially on the owner thread.

`connect --timeout-ms N` accepts 100..600000 milliseconds, default 30000, for connection establishment and each exchange. A timeout/disconnect closes the client connection and does not replay the operation. The operation may already have committed: inspect state, then explicitly retry with the same durable `request_id` where supported. Editor controls/captures do not have durable request receipts. Transport startup/exchange errors exit the CLI with code 4; stderr explains the failure, and stdout uses the CLI error envelope rather than a JSON-RPC result.

Normal shutdown attempts a bounded reply drain; a nonreading client or graphics failure can still observe a disconnect. Linux clean shutdown removes only the owned socket inode. A crash can leave a socket path: verify the old host is gone before removing that endpoint, or use a new name. Startup never removes a preexisting endpoint automatically.

The native interfaces are `poima/local_session.hpp`, `poima/shared_session.hpp` and `WorldSession::request(..., WorldRequestScope)`. Call `poll()` to receive requests and flush queued `reply()` data. Callers serialize WorldSession access; this is not a stable binary plugin ABI.

## Qualification and current limits

[Recorded evidence](evidence/m2-shared-sessions.json) covers 23 headless and 27 runtime CTest suites, native Windows transport/session checks, seven Windows shared-host tests, and live shared-editor checks on both NVIDIA and AMD laptop GPUs. The [actual editor capture](evidence/m2-shared-sessions.png) shows an Inspector draft preserved across an external revision. `tests/shared_editor_capture.py` uses two real CLI connections and actual captures; its draft setup uses the GUI's semantic action dispatcher. It does not qualify physical mouse/keyboard input or autonomous agent policy.

Imports, transactions and runtime calls remain synchronous and can stall every client and the GUI. Interactive/editor-endpoint loops have a 60 Hz CPU-side cap (30 Hz when unfocused or minimized), but the renderer still waits for the GPU each frame. No large-project responsiveness or game-performance claim follows from this fixture. Remote camera control, change subscriptions, MCP packaging, separate-process GUI attachment to a headless host, durable drafts and collaborative permissions remain future work.
