# Native publish and export jobs

Host-configured development profiles let agents publish C# Native AOT gameplay
and export a native game bundle through the world service. The host selects its
tools, project root, output root, runtime distribution and complete child
environment before accepting work. RPC clients select a named profile; they
cannot add tools, change environment values or request a shell command.

These are trusted-project build operations. MSBuild targets and selected tools
can execute code, access files and contact the network. Root checks constrain
the service's generated paths; they do not sandbox a trusted build. Authorize
build-job submission separately from world editing. Other world operations can
still execute explicitly selected trusted gameplay; this distinction is not a
general code-execution sandbox.

## Configure a host

Create an owner-controlled JSON file outside public project history. All paths
below must be absolute existing paths on the **same operating system as the
host**. Use `win-x64` on native Windows or `linux-x64` on native Linux.

```json
{
  "format": "poima.development-profiles",
  "version": 1,
  "profiles": [{
    "name": "native",
    "project_root": "/absolute/projects",
    "output_root": "/absolute/build-products",
    "python": "/absolute/tools/python3",
    "publisher": "/absolute/poima/scripts/publish_native_gameplay.py",
    "dotnet": "/absolute/tools/dotnet",
    "exporter": "/absolute/runtime/bin/poima",
    "runtime_root": "/absolute/runtime",
    "target_rid": "linux-x64",
    "environment": [
      ["PATH", "/absolute/native-toolchain/bin:/usr/bin:/bin"],
      ["HOME", "/absolute/build-profile"],
      ["DOTNET_CLI_HOME", "/absolute/build-profile"],
      ["NUGET_PACKAGES", "/absolute/package-cache"],
      ["DOTNET_CLI_TELEMETRY_OPTOUT", "1"]
    ]
  }]
}
```

The environment example is illustrative: provide the selected SDK/linker's
actual dependencies and usable profile/cache directories. Native Windows also
needs its machine configuration paths, `OS=Windows_NT`, `SystemRoot` and the
selected C++ toolchain's `PATH`, `INCLUDE`, `LIB` and `LIBPATH`. See the
[environment contract](DEVELOPMENT_JOBS.md#per-job-environments). Poima does not
discover, install or activate a toolchain automatically.

The output root must be separate from all configured project, runtime and tool
directories. Paths supplied by requests are portable relative paths: traversal,
absolute paths, symlinks, reparse points, reserved device names and ambiguous
components reject. Outputs must name new directories. Accepted output paths
remain reserved for the session, including cancelled and failed jobs; choose a
fresh output name for a new attempt.

Start one of these hosts:

```text
poima world world.json --development-profiles profiles.json
poima serve world.json --endpoint authoring --development-profiles profiles.json
poima mcp --world world.json --development-profiles profiles.json
```

An MCP client connecting to an existing endpoint uses that host's profiles.
It cannot configure tools through `mcp --endpoint`. The native owner API
`WorldSession::configure_development_profiles` applies the same loader and policy.
Configuration succeeds once, before the first accepted development job.
Packaged read-only hosts hide and reject development operations.

Profiles allow at most 16 entries with unique names of 1–64 ASCII letters,
digits, underscores or hyphens. The loader rejects unknown fields, duplicate
JSON keys, excessive nesting and files above 1 MiB. Environment validation uses
the worker's own rules before configuration succeeds.

## Request a build

Discover `development.profiles` to obtain compact profile names, target RIDs,
enabled operations and policy fingerprints. Environment values and full
configuration are omitted.

```json
{"jsonrpc":"2.0","id":1,"method":"development.publish","params":{"request_id":"11111111111111111111111111111111","profile":"native","project":"MyGame/MyGame.csproj","type":"MyGame.Main","output":"native-build-1"}}
```

Publication generates a fixed Python publisher argument vector with the
profile's .NET SDK and matching-host RID. It executes the selected original
project and produces an inventoried native gameplay artifact. Publisher and
generator resources currently come from an explicitly selected source checkout;
an installed, source-independent authoring-tool distribution is not qualified.

```json
{"jsonrpc":"2.0","id":2,"method":"development.export","params":{"request_id":"22222222222222222222222222222222","profile":"native","project":"MyGame/project.json","output":"game-build-1"}}
```

Export invokes the selected native engine's existing `project build` operation
with its verified runtime distribution. To include published gameplay, explicitly
place the inventoried artifact in the project's declared content and update the
project's gameplay descriptor. Neither operation automatically modifies a project,
loads gameplay or changes authored world revisions.

Both operations return a session-local job ID. Poll `development.inspect`, read
compact compiler feedback with `development.diagnostics`, or use
`development.cancel` and `development.forget`. Timeout accepts 100–900,000 ms
and defaults to 900,000 ms; it governs the child process. Verification runs after
child cleanup and performs bounded native inventory/tool reads on the worker.
It is not interrupted by cancellation or the child deadline; shutdown joins it.

Exact retries return the original job, even after output creation or forgetting
its status. Request identity includes operation kind and selected profile
fingerprint. Changed retries reject; expired receipt IDs cannot silently rerun.
The existing 128-receipt/4,096-ID session budget is shared with compilation.

## Results and limits

A zero process exit is insufficient. Native verification checks the full
artifact or bundle inventory, hashes, format and expected target. Gameplay also
checks the requested compiled type. A changed directly selected tool or runtime
descriptor rejects verification. Results include descriptor/manifest and inventory
hashes, file/byte counts and target/identity information. They do not execute game
code to test gameplay behavior.

`result` is null while queued/running and for failures or cancellations.
Verification failure reports `state:failed` even if `exit_code:0`; inspect `error`.
Cancellation can race successful file publication and does not undo files.
Successful results identify bytes verified at completion, rather than leasing
or protecting them against later edits. Revalidate before activation or delivery.

Fingerprints cover configured policy and the directly selected Python, publisher,
.NET executable, exporter and runtime descriptor. They are not complete SDK,
package or transitive publisher-resource fingerprints. Publication builds live
trusted sources; it does not yet capture a coherent immutable source closure.
The exporter retains its existing source-change checks. These development
operations are outside the stable authored-world v1 contract.

## MCP authorization

Use the separate `poima_build` tool with `method:development.compile`,
`method:development.publish` or `method:development.export` and native `params`.
`poima_call` rejects all three before invoking the backend. Compilation retains
its explicit absolute SDK/project/output parameters and inherited environment;
the two new operations require profiles. Discovery, job inspection, diagnostics and
cancellation remain available through `poima_call`. Approving world editing does
not approve `poima_build`; configure the provider's build-tool consent separately.
Poima does not alter provider policy or credentials.

## Qualification

Run the opt-in matching-host check with an already configured SDK/linker,
gameplay SDK DLL, installed runtime and built compatibility harness:

```text
python tests/development_publish_protocol.py --binary POIMA_EXECUTABLE --dotnet DOTNET_EXECUTABLE --compatibility-test COMPATIBILITY_EXECUTABLE --sdk-dll POIMA_GAMEPLAY_DLL --runtime INSTALLED_RUNTIME --output NEW_PRIVATE_DIRECTORY
```

It retains raw configuration, tool logs and MCP messages privately. It checks
genuine publication, compiled Tick/Control callbacks, native game export,
inventories, exact retries, compiler failure, queued cancellation and responsive
unchanged owner-world queries. This is engine-protocol qualification, not an
autonomous external-agent or rendering benchmark. `--publish-only` excludes
bundle export when using a Linux headless runtime without graphics; report that
platform scope separately from a Windows full publish/export run.

The [0.0.91 qualification record](evidence/m2-development-profiles.json) reports
Windows publication/export and Linux publication separately, with retained failed
attempts and native/shared-host regression scope.
