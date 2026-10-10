# Development jobs

The native `poima::development::Jobs` worker runs explicitly requested build
processes without blocking world-owner calls on compilation. It has no renderer,
SDL, editor, .NET hosting or package-manager dependency. One worker executes a
bounded queue sequentially; it uses Windows process APIs or Linux `posix_spawn`.

Submit an absolute executable, absolute working directory, UTF-8 argument vector
and timeout. Arguments are passed directly to the executable, with no shell or
PATH lookup. The process inherits the host environment by default; a native
caller can provide a complete replacement. The worker does not edit
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

## Per-job environments

The native `Request.environment` member is optional. Omission retains inherited
environment behavior; a present list replaces the entire child environment.
An empty list requests an empty environment. It does not mean inherit or merge.

```cpp
poima::development::Request request{tool,working_directory,arguments,
    std::chrono::minutes(10)};
request.environment=std::vector<std::pair<std::string,std::string>>{
    {"PATH",selected_tool_paths}, {"NUGET_PACKAGES",selected_package_cache},
    {"DOTNET_CLI_HOME",job_cli_home}, {"DOTNET_CLI_TELEMETRY_OPTOUT","1"}};
auto id=jobs.submit(std::move(request));
```

The caller supplies every needed operating-system, linker, locale and cache
variable. Windows tools may need `SystemRoot`, `PATH`, `INCLUDE`, `LIB` and
`LIBPATH` from the selected native toolchain, plus machine configuration paths
such as `ProgramFiles(x86)` and `ProgramData`. MSBuild's Windows native path
also requires its `OS=Windows_NT` marker. Do not forward unrelated account
variables. .NET/NuGet also require usable profile/configuration directories;
the qualifier assigns job-owned `DOTNET_CLI_HOME`, `USERPROFILE`, `APPDATA`,
`LOCALAPPDATA` or `HOME`, plus private HTTP/plugin caches. This confines
environment inheritance; it does not sandbox trusted
MSBuild projects or prevent a process from reading files or credentials itself.

Validation accepts at most 256 pairs. Names contain 1–256 UTF-8 bytes without
`=` or NUL; values contain at most 32,768 UTF-8 bytes without NUL, and may be
empty. Combined names, values, one `=` and one NUL per pair fit within 131,072
bytes. Invalid requests consume no queue capacity. Duplicate names reject:
case-sensitive on Linux, Unicode ordinal case-insensitive on Windows. Windows
sorts names and passes a double-NUL-terminated UTF-16 block to `CreateProcessW`;
Linux supplies owned strings to `posix_spawn`. Both preserve the parent process
environment and existing process cleanup behavior.

`development.compile` retains its original RPC shape and inherited environment.
RPC clients cannot supply arbitrary environment values through this change.
[Host-configured native publishing/export profiles](DEVELOPMENT_PROFILES.md)
supply separate owner policy and build authorization; the native environment
member alone does not grant agent execution authority.

The [matching-host qualification tool](../tests/development_toolchain.py) uses
the compiled worker for real SDK builds, Native AOT publishing and native
Tick/Control execution with explicit replacement environments. It reuses the
DLL/project-reference and missing-copy regression, probes the exact child
environment, omits deliberately invalid owner startup hooks and checks unchanged
owner variables. Its requests and raw logs remain local build products. Run it
with an already configured matching-host SDK/linker and built
`poima-development-toolchain-test` and `poima-gameplay-compatibility-test`:

```sh
python tests/development_toolchain.py \
  --worker /absolute/path/poima-development-toolchain-test \
  --dotnet /absolute/path/dotnet \
  --compatibility-test /absolute/path/poima-gameplay-compatibility-test \
  --engine-version 0.0.90 --output /absolute/path/new-qualification
```

The native worker remains responsible for deadlines and child cleanup. Publisher
builds disable persistent compiler/MSBuild servers; publication does not modify
the selected gameplay or export a project automatically. Successful process
completion alone is not artifact validation. The qualification separately checks
artifact inventories and real native callbacks.
[Recorded environment qualification](evidence/m2-development-environment.json).

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
recorded separately.

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

## Worker success verification

A native caller can attach `Request.verify_success`, a worker-only callback
invoked after a successful child and the existing owned-child cleanup boundary.
It returns at most 65,536 valid UTF-8 bytes. Exceptions, oversized results or
invalid encoding convert zero-exit success into `failed`; other child outcomes
skip verification. `Status.result` appears atomically with success. The service
additionally requires a bounded nonempty JSON object. Cancellation suppresses
the result, including cancellation during verification. The callback itself
performs bounded work but is not interruptible; destruction joins it.

`validate_request` shares the submit-time validation rules without queueing or
launching a process, so owner profiles can reject invalid environments at startup.
