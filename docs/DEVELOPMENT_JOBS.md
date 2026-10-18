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

Forgetting a terminal job releases its verifier captures outside the state
mutex. A capture destructor may call `poll`, `list` or `cancel` on the same
still-live worker. Submission also constructs and moves captures before locking
and preserves ownership until after unlocking on admission failures. Capture
cleanup is caller code and can take time; this is not a constant-time cleanup
guarantee. Reentry while destroying the `Jobs` object is outside this contract.

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

For agent clients, the [build-job recipe](AGENT_BUILD_JOBS.md) combines compact
status polling, a finite local budget and deliberate cancellation followed by
terminal observation. It distinguishes a polling timeout from a native job
timeout and preserves uncertain transport outcomes.

Windows starts children suspended, restricts inherited handles, assigns a
kill-on-close Job Object, then resumes them. After the leader exits, the worker
allows up to one second for its children to finish, bounded by the original
request deadline. It continues draining output and checking cancellation and
compiler-server health. An empty Job needs no termination request. Remaining
children, cancellation and timeout use the forced-cleanup path, which waits up
to five seconds for zero active processes; failure is reported explicitly.
Linux redirects standard streams, closes all
other child descriptors, creates a separate process group and sends `SIGKILL`
to that group before reaping the direct child. The leader stays unreaped until
the group signal, preventing PID-reuse cleanup races. Linux uses the libc
`addchdir_np` and `addclosefrom_np` spawn extensions. Reaping grandchildren is
the operating system's responsibility. Programs that escape their process group
or Windows job are outside this cleanup contract. These are bounded build jobs. The explicit Windows compiler option below is
an internal worker-owned exception: it retains one private compiler process
between builds, with separate lifetime and cleanup fences. Generic requests do
not become persistent services.

The standalone `tests/development_jobs.cpp` contract uses a real tiny child,
not a simulated process. It covers argument fidelity (including Unicode,
quotes, backslashes and shell metacharacters), working directory, separate
diagnostics, overflow draining/counts/tails, nonzero and missing executables,
queued/running cancellation, timeout, capacity and terminal forgetting. Child
markers check cleanup after cancellation, successful leader exit and worker
destruction. Linux additionally checks unrelated descriptor exclusion and
redirection when the host's standard descriptors are closed.

The focused `development_capture_reentry` CTest runs two real children. The
second child's gated verifier proves the worker has released its local owner
of the first job. Forgetting the first job then destroys its last verifier
capture, which queries both jobs, lists retained work and cancels the second
job. The second job must publish cancellation with a zero child exit and no
successful verifier result. The owning test-process deadline bounds a deadlock;
a timeout is a failure. [Linux/Windows checks and retained negative reproduction](evidence/m2-development-capture-cleanup.json). Run just this regression with:

```sh
ctest --test-dir /path/to/build -R '^development_capture_reentry$' --output-on-failure
```

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

`development.compile` inherits the host environment; RPC clients cannot supply
arbitrary environment values. The compiler option below captures the effective
named Windows environment for its server and build child.
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
`dotnet build` arguments with separate artifact/managed output. Build servers
remain disabled unless the caller explicitly selects the compiler option below;
it does not construct a shell command or reload running gameplay. Project
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

## Dependency restore during C# iteration

`development.compile` accepts an optional boolean `restore`, defaulting to
`true`. Set it to `false` to pass `--no-restore` to the selected .NET build.
With `compiler_server` omitted, compiler servers remain disabled. Job ownership,
diagnostics, cancellation and timeouts retain their existing contracts. Omission and `true` share a retry
receipt. Changing the flag while reusing a request ID rejects the request.

First complete an ordinary build using the same project, configuration and
output directory. The output's `artifacts` directory holds its restore files.
Restore again after changing the project, imported build files, dependencies,
SDK selection or NuGet configuration. Poima does not determine whether those
inputs still match. A missing assets file produces a compiler failure, with no
automatic restore fallback. Treat only the new job's terminal success as build
success; an older DLL can remain after a failed build.

```python
job = client.call("development.compile", {
    "request_id": new_id(),
    "executable": dotnet_path,
    "project": project_path,
    "output": output_path,
    "configuration": "Release",
    "restore": False,
})
```

Use the [bounded observation recipe](AGENT_BUILD_JOBS.md) to inspect that job
before validating or loading its artifacts. The option follows [.NET build's
restore contract](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-build#implicit-restore).
It exposes build policy; it does not promise a particular iteration time.

The [real SDK check](../tests/development_compile_restore.py) takes explicit
`--binary`, `--dotnet` and a fresh `--output` directory. Run it with native Windows
Python inside an owned, gated process tree. It uses a package-free .NET 10
library and clears NuGet sources; the selected SDK's targeting packs must already
be installed. Its [recorded qualification](evidence/m2-compile-restore.json)
includes cold failure, a default restore, source-only compilation, syntax failure
with the prior DLL retained, and repair. Native command and retry contracts also
pass on Linux. These are correctness checks rather than an iteration benchmark.

## Optional owned Windows compiler

`development.compile` accepts an optional absolute `compiler_server` path naming
`VBCSCompiler.dll`. Omit it to retain the existing isolated build arguments. The
initial supported selection is the Windows x64 .NET SDK **10.0.401**: the worker
checks its exact `.version` contents, including source commit
`e34a38d2ae1fc26406a317517196e55c68ff83ab`, and requires the SDK's matching
`dotnet.exe` and `Roslyn/bincore/VBCSCompiler.dll`. Unsupported platforms reject
the option. Missing tools, unsupported SDK identities and startup failures fail
the job rather than silently removing the requested mode. Ordinary SDK-internal
compiler fallback remains possible and is reported as unproven use.

```python
job = client.call("development.compile", {
    "request_id": new_id(),
    "executable": dotnet_path,
    "compiler_server": vbcscompiler_dll_path,
    "project": project_path,
    "output": output_path,
    "configuration": "Release",
})
```

The native equivalent sets `Request.compiler_server`. The worker launches a
private pipe and owns the original compiler process in a separate Windows Job.
The pipe is not a request parameter or retry-receipt key; exact retries keep the
original job ID, and changing the selected DLL under that ID is a conflict.
MSBuild still evaluates the project and runs its normal generators and tasks;
this option does not replace it with a manual compilation.

One worker retains at most one compiler session. Reuse matches canonical host,
server and project paths, project file metadata and the effective environment.
Inherited Windows environments are captured as complete named entries for both
server and build; drive pseudoentries such as `=C:` are excluded and counted.
Tool metadata changes or a different session key require retirement. Idle
retirement is 60 seconds, the server has a 30-minute lifetime, and a new request
after 20 minutes starts a replacement. These are finite ownership limits, not
a guarantee that dependencies or restore outputs remain current. The `restore`
option keeps its independent contract.

An opt-in job's `compiler_server` observation reports `session_reused`, original
PID/birth, `compiler_server_used`, `proof_reason`, diagnostics and optional
`retirement`. `session_reused` means process survival, not proof of compilation.
`compiler_server_used:true` requires the original process's Received/Completed
log pair to match the actual MSBuild/Csc request label. `null` means actual use
is unproven; readiness alone only checks nonconnecting pipe availability and
held process identity. Neither proves the pipe server's PID. Use the ordinary
terminal build state and validate its new artifacts even when use is proven.

Opt-in client stdout/stderr tails retain at least 128 KiB each, while the owned
server's separate stream tails retain 32 KiB each. Byte counts and truncation
flags remain explicit. Inspection also exposes a bounded 64 KiB request log
segment; catalog responses omit raw logs. A private diagnostic file is bounded
to 1 MiB and a session is replaced before another request when it exceeds
512 KiB.

Failure, cancellation and timeout finish the owned compiler retirement path
before terminal publication; unverified cleanup is an error. Retirement records
distinguish verified cleanup, forced cleanup,
original exit and errors. Cancellation accepted while a success verifier is
running cannot interrupt that callback: the worker waits for it, retires the
compiler and suppresses success/result publication. Host destruction joins
cleanup. Already-written artifacts remain on disk. This option does not reload
gameplay, prove an iteration speedup or establish the three-second gameplay
iteration target.

### Compiler qualification and smoke check

The 0.0.105 native development library passes ten real Windows SDK jobs: a
cold build, two changed-source warm builds, syntax failure, repair, cancellation
and timeout inside an unresponsive source generator, recovery after each, and
missing-tool rejection. Six successful builds produce distinct DLLs. Syntax
failure retires the original compiler naturally with exit zero; cancellation
and timeout force termination of the exact owned group. Failed attempts preserve
the prior managed artifacts. Correlated raw server/Csc observations establish
actual use for each successful build.

A separate lifecycle check covers cancellation before launch, cancellation while
a success verifier is held, and healthy host destruction. Queued cancellation
launches no child; accepted late cancellation suppresses the successful result.
The destructor check proves the original process is dead and artifacts unchanged;
it does not expose a natural-exit receipt. Five Windows native contract groups
pass, including 1,183 discovery responses across six owner scopes. All outer
process owners finish empty without forced cleanup. A further check observes
idle process death after 60.07 seconds and verifies a fresh owned compiler and
changed DLL afterward. Its 55–75-second observation bound does not establish an
exact 60-second lifetime or attribute the idle exit to a particular mechanism.
An earlier run caught a build-admission race with normal SDK expiry; the worker
now replaces a fully reaped, unforced, exit-zero cache before starting the build,
provided its tools still match. Other readiness failures retain their original
diagnostics and failure behavior. The [evidence](evidence/m2-owned-compiler.json)
retains earlier failed retirement and idle-recovery attempts separately.

To reproduce a **single-build smoke**, explicitly build
`poima-development-service-test`, prepare a valid C# project and use a fresh
output directory. Pin SDK 10.0.401 in the project's `global.json`; its targeting
packs and dependencies must already be available. On Windows:

```powershell
& $serviceTest --sdk-server $dotnet $project $output $vbcsCompilerDll
```

The arguments name the native test executable, SDK host, absolute `.csproj`,
output directory and matching `Roslyn/bincore/VBCSCompiler.dll`. The test prints
the actual job status, including nullable owned-use evidence. It does not
reproduce the complete ten-job/lifecycle cohorts, whose pinned recipes and raw
receipts are retained locally. Longer lifetime limits, key-change saturation,
production AOT reuse and full gameplay iteration timing remain separate
qualification.

## Compact single-job inspection

In schema revision 79, `development.inspect` accepts optional boolean
`include_output`, defaulting to `true`. For progress polling on one accepted job:

```json
{"jsonrpc":"2.0","id":1,"method":"development.inspect","params":{"job_id":1,"include_output":false}}
```

Compact inspection keeps lifecycle, exit, elapsed, error, verified result,
byte/truncation counters and compiler identity/use/retirement metadata. It omits
client `stdout`/`stderr`, compiler `log_segment` and compiler diagnostic
`stdout`/`stderr`. The native `Jobs::poll_summary` constructs this projection
under the state mutex without copying those raw tails. Error, result and short
ownership strings still copy; this is not a lock-free or zero-allocation call.

Omission or `true` retains full inspection. Catalog, cancellation, forgetting,
retry identity and worker lifetime keep their existing behavior. Invalid flag
types reject before looking up the job; unknown and forgotten IDs remain errors.
Discover the current host's method schema before using the option with an older
build. Compact inspection is an observation of one job, not pagination or a new
execution permission.

After terminal status, retrieve full inspection and bounded structured diagnostics
when verifying a compilation or debugging a failure. Compact metadata alone does
not provide the raw correlated compiler-use or complete diagnostic evidence.
Reduced response content does not establish a measured latency improvement.

## Process and cleanup observations

Schema revision 100 adds optional boolean `include_process`, defaulting to
`false`, on `development.inspect`. It works with full or compact inspection:

```json
{"jsonrpc":"2.0","id":1,"method":"development.inspect","params":{"job_id":1,"include_output":false,"include_process":true}}
```

Omission or `false` preserves the existing response. Only `true` adds `process`;
catalog, diagnostics, cancellation and retry receipts keep their existing
shapes. Invalid flag types reject before job lookup. This is a development
inspection extension, outside the frozen authoring and game-session contracts.

`process` is null before the ordinary runner has published observations. It may
remain null while a job runs, or after queued cancellation or compiler-server
startup failure. A launch failure can instead retain a record with
`launched:false`. Observations copy from worker-owned state; inspection does not
probe a live process or wait for cleanup. A running verifier may already expose
completed process observations. The separate compiler-server lifetime retains
its existing metadata.

The version-1 record contains `platform`, `launched`, `leader_pid`,
`group_assigned`, `leader_exit_observed`, `leader_reaped`, `termination`,
`final_group_accounting` and `cleanup`. Older builds omit
`final_group_accounting`; current builds return null when it is unobserved or
inapplicable. Windows termination keeps
the unsigned `windows_exit_code`; POSIX keeps `posix_wait_status` and explicitly
distinguishes `posix_exit`, `posix_signal` and `posix_other`, with applicable
`exit_status`, `signal` and `core_dumped` values. The existing top-level
`exit_code` is unchanged: for example, POSIX exit 143 and SIGTERM both project
to 143 there, while their raw records differ. A Windows code alone does not
establish a crash, exception or its cause.

`cleanup.requests` retains at most four actual requests, with
`requests_overflow` reporting truncation. Each records its `action`, `reason`
(`completion`, `cancelled`, `timed_out` or `worker_failure`), observed API
`succeeded`/`error_code` and optional Windows Job accounting `before`/`after`.
Actions are `windows_job_terminate`, `windows_process_terminate`,
`windows_job_close` or `posix_group_kill`. Accounting retains raw
`total_processes`, `active_processes` and `total_terminated_processes`; those
counters do not attribute exits to a particular request. POSIX group counts
remain null. `cleanup.finished` means the existing cleanup sequence completed,
not that every descendant was observed or reaped. A naturally emptied Windows
Job has an empty `cleanup.requests` array. Its `final_group_accounting` retains
the raw counters checked at the completed retirement fence, including zero
active processes. POSIX does not synthesize Job counters.

These facts do not change job success: a successful leader can still require
termination of remaining children. An external supervisor's empty Job Object
or successful engine exit does not prove that every nested process exited
naturally. Retain explicit inner cleanup observations alongside the outer
owner's closure record. This extension is not a general crash reporter, stack
trace collector or process-tree census.

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
forms. It caps each input stream at 128 KiB and each line at 64 KiB. The finite
budget includes verbose compiler invocation and response-file lines; it does not
change the job output-tail or result-count limits. `incomplete`
marks skipped truncated prefixes, unfinished running-job lines, oversized lines
or malformed diagnostic-looking input. Other formats may remain unrecognized;
an empty list never proves that compilation succeeded or that no errors occurred.

The 0.0.106 parser checks cover verbose invocation/response lines and exact/over
limits on Windows and Linux. Fresh owned-compiler builds verify complete
diagnostics through a real syntax failure and repair, preserving the prior DLL.
These are parser/build checks, separate from full-game iteration timing.
[Qualification](evidence/m2-diagnostics-budget.json).
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

The [0.0.165 CPU checks](evidence/m2-source-topology-process-inspection.json)
record the selected native workflows, compatibility comparisons and retained
qualification failures separately.
