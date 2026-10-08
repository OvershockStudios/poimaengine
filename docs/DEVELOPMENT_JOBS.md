# Development jobs

The native `poima::development::Jobs` worker runs explicitly requested build
processes without blocking world-owner calls on compilation. It has no renderer,
SDL, editor, .NET hosting or package-manager dependency. One worker executes a
bounded queue sequentially; it uses Windows process APIs or Linux `posix_spawn`.

Submit an absolute executable, absolute working directory, UTF-8 argument vector
and timeout. Arguments are passed directly to the executable, with no shell or
PATH lookup. The process inherits the host environment; the worker does not edit
global environment, tools, accounts or configuration. This is an execution
primitive, not a sandbox: its caller must authorize filesystem effects and tools.

```cpp
poima::development::Jobs jobs;
auto id=jobs.submit({dotnet_executable,project_directory,
    {"build",project_file,"--no-restore","--disable-build-servers",
     "--configuration","Debug","--output",output_directory},
    std::chrono::minutes(10)});
auto status=jobs.poll(id); // no wait for child completion
```

`submit`, `poll`, `list`, `cancel` and `forget` do not wait for a subprocess.
They use a short mutex for bounded in-memory state. `forget` accepts terminal
jobs only; reaching capacity rejects new work rather than silently evicting
history. Destruction requests cancellation and joins worker cleanup, so dispose
the worker during host shutdown rather than in a frame callback.

Status reports `queued`, `running`, `succeeded`, `failed`, `cancelled`,
`timed_out` or `launch_failed`, with an optional exit code, elapsed time and an
explicit error message. Cancellation may race completion: a cancellation
accepted before terminal status is published yields `cancelled`; it does not
undo output files. Nonzero exits and timeouts are distinct from launch failures.

Defaults retain at most 32 jobs and the latest 32 KiB from each output stream.
Byte counters and truncation flags preserve overflow information while both
pipes continue draining. Diagnostic tails are raw bytes and may start inside a
UTF-8 sequence; JSON/UI adapters must replace invalid UTF-8 or encode the bytes.
Maximum limits are 128 jobs and 1 MiB per stream. Requests cap arguments at 128,
argument bytes at 64 KiB, paths at 32,768 native units and timeout at 24 hours.
IDs remain in the exact JSON-integer range. Windows additionally enforces its
32,766 UTF-16-unit command-line bound before launch.

Windows starts children suspended, restricts inherited handles, assigns a
kill-on-close Job Object, then resumes them. Completion and cancellation terminate
remaining owned children and wait up to five seconds for zero active processes;
failure is reported explicitly. Linux redirects standard streams, closes all
other child descriptors, creates a separate process group and sends `SIGKILL`
to that group before reaping the direct child. The leader stays unreaped until
the group signal, preventing PID-reuse cleanup races. Linux uses the libc
`addchdir_np` and `addclosefrom_np` spawn extensions. Reaping grandchildren is
the operating system's responsibility. Programs that escape their process group
or Windows job are outside this cleanup contract. These are build jobs, not a
host for persistent services or build-server processes.

The standalone `tests/development_jobs.cpp` contract uses a real tiny child,
not a simulated process. It covers argument fidelity (including Unicode,
quotes, backslashes and shell metacharacters), working directory, separate
diagnostics, overflow draining/counts/tails, nonzero and missing executables,
queued/running cancellation, timeout, capacity and terminal forgetting. Child
markers check cleanup after cancellation, successful leader exit and worker
destruction. Linux additionally checks unrelated descriptor exclusion and
redirection when the host's standard descriptors are closed.

This foundation does not claim project watching, automatic reload, dependency
restore, import scheduling or a complete development service. The authoring
service supplies policy, retry receipts and generated compilation arguments.

## Authoring compile adapter (development)

Source integration exposes `development.compile`, `development.jobs`,
`development.inspect`, `development.diagnostics`, `development.cancel` and `development.forget` through
the world service. Packaged read-only worlds hide and reject these operations.
Compile accepts an explicit absolute SDK executable, `.csproj`, output directory,
fresh request ID, Debug/Release configuration and optional timeout. It generates
`dotnet build` arguments with separate artifact/managed output and disabled build
servers; it does not construct a shell command or reload running gameplay. Project
builds execute trusted MSBuild logic and inherit the host environment.

Exact compile retries return the original job ID. Reusing an ID with different
parameters is rejected; forgetting a terminal job does not permit recompilation
under that ID. Receipts are session-local: 128 are retained and 4096 accepted IDs
are remembered per session. Expired receipt IDs are rejected, not silently
re-executed. Query jobs before repeating an uncertain request after host restart.

Catalog results omit logs; individual inspection includes capped UTF-8-safe
diagnostics, byte counters and truncation flags. Cancellation preserves any
already-written output files. Compilation does not change authored world revisions.

The standalone worker passes real-child contracts on Linux and native Windows.
The service passes Linux and native Windows self-child checks for generated arguments, path fidelity,
retry guards, capacity-failure rollback, expired receipts and malformed diagnostic
bytes. A real .NET 10 SDK build through this adapter produces the escape-room
fixture assembly. Full world-host and packaged integration qualification is
recorded separately; no new installed desktop package is implied.

## Structured compiler feedback (development)

Agents can request recognized compiler errors and warnings without retrieving
the raw diagnostic tails:

```json
{"jsonrpc":"2.0","id":1,"method":"development.diagnostics","params":{"job_id":1,"limit":16}}
```

The response retains the job's state, exit code and stream byte/truncation
counters. `diagnostics` contains deduplicated records with `severity`, `code`,
`origin`, `message`, optional `project` and optional `location`. Locations are
one-based; absent columns/end positions remain null. An origin can be a source
path or a tool name, and is preserved without filesystem resolution. Raw
stdout/stderr are omitted; `development.inspect` still retrieves them.

`recognized_lines` counts matching physical lines before deduplication, including
MSBuild's repeated summary. Results follow retained stdout then stderr, without
claiming cross-stream time order. `limit` defaults to 32 and accepts 1–128;
`more:true` means additional distinct recognized records were omitted. Re-query
with a larger limit for more records; this is a live job observation, not a
revision-bound pagination cursor.

The parser recognizes a conservative subset of
[Microsoft's diagnostic format](https://learn.microsoft.com/en-us/visualstudio/msbuild/msbuild-diagnostic-format-for-tasks?view=visualstudio):
error/warning categories with codes, optional origins and common source-coordinate
forms. It caps each input stream at 64 KiB and each line at 16 KiB. `incomplete`
marks skipped truncated prefixes, unfinished running-job lines, oversized lines
or malformed diagnostic-looking input. Other formats may remain unrecognized;
an empty list never proves that compilation succeeded or that no errors occurred.
Use the job's terminal state and exit status, and inspect raw logs when needed.

The standalone parser and adapter contracts pass on Linux and native Windows.
A real Linux SDK failure reports its code and exact source coordinates; repairing
that fixture produces an assembly while retaining its warning. Rebuilt native
world hosts also pass real SDK success/failure checks on both platforms, with
unchanged authored revisions, retry guards and compact source-located errors.
Native read-only/shared-scope tests hide and reject all development methods.
This API does not load the resulting module or change gameplay automatically.
[World-host evidence](evidence/m2-development-host.json),
[earlier standalone evidence](evidence/m2-development-diagnostics.json).
