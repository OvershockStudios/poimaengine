// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Globalization;
using System.Text.Json.Nodes;
using Poima.AgentHost;

internal static class CodexDraftContract
{
    public const string ModeVariable = "POIMA_TEST_CODEX_DRAFT";
    private const string NativeVariable = "POIMA_TEST_CODEX_DRAFT_NATIVE";
    private const string TraceVariable = "POIMA_TEST_CODEX_DRAFT_TRACE";
    private const string ManifestName = "project.poimaproject";
    private const string MarkerName = ".poima-draft.json";
    private const string Model = "draft-fixture";
    private const string LanternTask = "Draft a lantern.";
    private const string BenchTask = "Draft a quay bench.";
    private const string ReportTask = "Show Markdown report.";
    private const string WaitForRelease = "Wait for the release signal.";
    private const string HoldTask = "Hold draft turn.";
    private const string Holding = "Draft turn is waiting.";
    private const string MissingScope = "Missing execution scope";
    private const string LocalExecutor = "Unexpected local executor";
    private const string GameplayExecutor = "Unexpected gameplay executor";
    private const string WritableScope = "Unexpected write access";
    private const string SourceRead = "project.source.read";
    private const string SourceWrite = "project.source.write";
    private const string MissingSourceTool = "Missing source tool";
    private const string MissingSourceRead = "Missing source read";
    private const string SourceWriteOnDispatcher = "Source write on dispatcher";
    private const string ExpandedSourceTool = "Expanded source tool";
    private const string BuildOnDispatcher = "Build on dispatcher";
    private const string ChecksOnDispatcher = "Checks on dispatcher";
    private const string ExtraChecksTool = "Extra checks tool";
    private const string SceneWithSourceRead = "Scene with source read";
    private static readonly string[] SceneTools = ["poima_call", "poima_discover", "poima_reference"];
    private static readonly string[] GameplayTools = [.. SceneTools, "poima_source"];

    public static bool IsDraftPeer(string[] arguments) =>
        Override(arguments, "mcp_servers.poima.args") is JsonArray args && args.Any(value => value?.GetValue<string>() == "--project");

    public static async Task VerifyAsync(CancellationToken cancellation)
    {
        var client = Environment.ProcessPath!;
        var scratch = Directory.CreateTempSubdirectory("poima-codex-draft-").FullName;
        var mode = Environment.GetEnvironmentVariable(ModeVariable);
        var native = Environment.GetEnvironmentVariable(NativeVariable);
        var trace = Environment.GetEnvironmentVariable(TraceVariable);
        Environment.SetEnvironmentVariable(ModeVariable, "1");
        Environment.SetEnvironmentVariable(NativeVariable, null);
        Environment.SetEnvironmentVariable(TraceVariable, scratch);
        try
        {
            var first = WriteDraft(scratch, "Lantern draft");
            var second = WriteDraft(scratch, "Bench draft");
            var logic = WriteDraft(scratch, "Gameplay draft");
            var wrong = WriteDraft(scratch, "Wrong marker", wrongProjectId: true);
            var noMarker = WriteDraft(scratch, "Missing marker");
            var wrongCatalogs = new (string Manifest, bool Source)[]
            {
                (WriteDraft(scratch, "Wrong catalog"), false),
                (WriteDraft(scratch, SceneWithSourceRead), false),
                (WriteDraft(scratch, MissingSourceTool), true),
                (WriteDraft(scratch, MissingSourceRead), true),
                (WriteDraft(scratch, SourceWriteOnDispatcher), true),
                (WriteDraft(scratch, ExpandedSourceTool), true),
                (WriteDraft(scratch, BuildOnDispatcher), true),
                (WriteDraft(scratch, ChecksOnDispatcher), true),
                (WriteDraft(scratch, ExtraChecksTool), true)
            };
            var unsafeDrafts = new (string Manifest, bool Source)[]
            {
                (WriteDraft(scratch, MissingScope), false),
                (WriteDraft(scratch, LocalExecutor), false),
                (WriteDraft(scratch, GameplayExecutor), true),
                (WriteDraft(scratch, WritableScope), false)
            };
            File.Delete(Path.Combine(Path.GetDirectoryName(noMarker)!, MarkerName));
            Task<CodexReview> Open(string manifest, bool source = false) => source
                ? CodexReview.OpenDraftAsync(client, client, manifest, Model, cancellation, allowGameplaySourceEdits: true)
                : CodexReview.OpenDraftAsync(client, client, manifest, Model, cancellation);

            foreach (var invalid in new[] { wrong, noMarker })
            {
                var rejected = false;
                try { await using var unexpected = await Open(invalid); }
                catch (Exception error) when (error is ArgumentException or IOException or InvalidOperationException or InvalidDataException)
                { rejected = true; }
                Check(rejected, "A project with an invalid or missing worker marker launched a draft session.");
            }
            Check(!Directory.EnumerateFiles(scratch, "draft-*.jsonl").Any(),
                "An invalid draft started a provider process before validation.");

            foreach (var (manifest, source) in unsafeDrafts)
            {
                var rejected = false;
                try { await using var unexpected = await Open(manifest, source); }
                catch (InvalidOperationException error) when (error.Message.Contains("could not confirm an isolated scene worker", StringComparison.Ordinal))
                { rejected = true; }
                Check(rejected, "A draft worker opened with an unconfirmed local execution scope.");
            }

            foreach (var (manifest, source) in wrongCatalogs)
            {
                var rejected = false;
                try { await using var unexpected = await Open(manifest, source); }
                catch (InvalidOperationException error) when (error.Message.Contains("scoped draft tools", StringComparison.Ordinal))
                { rejected = true; }
                Check(rejected, "Draft connection accepted a missing or excessive permission in " + Path.GetDirectoryName(manifest));
            }

            await using (var lantern = await Open(first))
            await using (var bench = await Open(second))
            {
                using var stop = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
                var started = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
                var waiting = lantern.AskAsync(HoldTask, draft => started.TrySetResult(draft), stop.Token);
                Check(await started.Task.WaitAsync(cancellation) == Holding, "Draft turn did not stream its pending state.");
                var updates = new List<string>();
                var finding = await bench.AskAsync(BenchTask, updates.Add, cancellation);
                Check(finding == "Draft quay bench created at revision 3." && updates.SequenceEqual([finding]),
                    "A second draft did not complete independently or its stream included unrelated text.");
                Check(!waiting.IsCompleted, "One draft completed another draft's turn.");
                stop.Cancel();
                try { await waiting; throw new InvalidDataException("A cancelled draft accepted a result."); }
                catch (OperationCanceledException) when (!cancellation.IsCancellationRequested) { }
                await lantern.DisposeAsync();
                Check(await bench.AskAsync("Check the draft.", _ => { }, cancellation) == "Draft is ready to review.",
                    "Retiring one draft stopped the other draft's conversation.");
            }
            await using (var gameplay = await Open(logic, source: true))
                Check(await gameplay.AskAsync("Check the draft.", _ => { }, cancellation) == "Draft is ready to review.",
                    "An explicitly permitted gameplay worker could not start a turn.");

            var expected = wrongCatalogs.Concat(unsafeDrafts)
                .ToDictionary(target => target.Manifest, target => target.Source, StringComparer.Ordinal);
            expected.Add(first, false);
            expected.Add(second, false);
            expected.Add(logic, true);
            var peers = Directory.GetFiles(scratch, "draft-*.jsonl")
                .Select(file => File.ReadLines(file).Select(line => JsonNode.Parse(line)!.AsObject()).ToArray()).ToArray();
            Check(peers.Length == expected.Count &&
                peers.Select(peer => peer[0]["thread"]!.GetValue<string>()).Distinct().Count() == peers.Length,
                "Drafts did not own independent provider threads.");
            foreach (var peer in peers)
            {
                var launch = peer[0];
                var manifest = launch["manifest"]!.GetValue<string>();
                Check(expected.Remove(manifest, out var source) && launch["gameplay_source"]?.GetValue<bool>() == source &&
                    launch["command"]?.GetValue<string>() == client,
                    "The draft launched with a different project, executable, or source permission.");
                if (unsafeDrafts.Any(target => target.Manifest == manifest) || wrongCatalogs.Any(target => target.Manifest == manifest))
                    Check(peer.Select(row => row["kind"]!.GetValue<string>())
                        .SequenceEqual(["launch", "native/catalog", "thread/start", "exit"]),
                        "A rejected draft connection was used after the scope check.");
                Check(peer[^1]["kind"]?.GetValue<string>() == "exit", "Draft peer did not close on EOF.");
                try
                {
                    using var child = Process.GetProcessById(launch["pid"]!.GetValue<int>());
                    Check(child.HasExited, "Draft provider still runs after retirement.");
                }
                catch (ArgumentException) { } // The OS has already removed the exited process.
            }
        }
        finally
        {
            Environment.SetEnvironmentVariable(ModeVariable, mode);
            Environment.SetEnvironmentVariable(NativeVariable, native);
            Environment.SetEnvironmentVariable(TraceVariable, trace);
            Directory.Delete(scratch, recursive: true);
        }
        Console.WriteLine("Codex draft targets, opt-in gameplay source, no-executor scope, restricted approvals, streaming and retirement passed.");
    }

    private static string WriteDraft(string scratch, string name, bool wrongProjectId = false)
    {
        var root = Directory.CreateDirectory(Path.Combine(scratch, name)).FullName;
        var projectId = Guid.NewGuid().ToString("N");
        var manifest = Path.Combine(root, ManifestName);
        File.WriteAllText(manifest, new JsonObject
        {
            ["format"] = "poima.project", ["version"] = 1, ["project_id"] = projectId, ["name"] = name,
            ["entry"] = new JsonObject
            {
                ["world"] = "world.json",
                ["camera"] = "00000000000000000000000000000003",
                ["controller"] = "00000000000000000000000000000002"
            }
        }.ToJsonString());
        File.WriteAllText(Path.Combine(root, MarkerName), new JsonObject
        {
            ["format"] = "poima.project-draft", ["version"] = 1, ["project"] = ManifestName,
            ["project_id"] = wrongProjectId ? Guid.NewGuid().ToString("N") : projectId,
            ["base_project_id"] = Guid.NewGuid().ToString("N"), ["base_revision"] = 2, ["base_sha256"] = new string('1', 64)
        }.ToJsonString());
        return manifest;
    }

    // The scripted provider drives an actual native MCP owner when the native mode is set.
    public static async Task RunPeerAsync(string[] arguments)
    {
        var command = Override(arguments, "mcp_servers.poima.command")!.GetValue<string>();
        var engineArgs = Override(arguments, "mcp_servers.poima.args")!.AsArray().Select(value => value!.GetValue<string>()).ToArray();
        var source = engineArgs is ["mcp", "--project", _, "--draft", "--gameplay-source"];
        Check(source || engineArgs is ["mcp", "--project", _, "--draft"],
            "The draft must target a standalone Poima project.");
        var expectedTools = source ? GameplayTools : SceneTools;
        var manifest = engineArgs[2];
        var cwd = Directory.GetCurrentDirectory();
        var pathComparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        Check(Path.IsPathFullyQualified(manifest) && string.Equals(Path.GetDirectoryName(manifest), cwd, pathComparison),
            "Draft provider started outside its workspace.");
        var enabledTools = Override(arguments, "mcp_servers.poima.enabled_tools")!.AsArray().Select(value => value!.GetValue<string>());
        Check(enabledTools.Order(StringComparer.Ordinal).SequenceEqual(expectedTools) &&
            Override(arguments, "mcp_servers.poima.tools.poima_call.approval_mode")?.GetValue<string>() == "approve" &&
            Override(arguments, "mcp_servers.poima.tools.poima_source.approval_mode")?.GetValue<string>() == (source ? "approve" : null) &&
            arguments.Count(arg => arg.StartsWith("mcp_servers.poima.tools.", StringComparison.Ordinal)) == (source ? 2 : 1),
            "Draft session approval or enabled tools differ from the scoped authoring catalog.");
        Check(Override(arguments, "sandbox_mode")?.GetValue<string>() == "read-only" &&
            Override(arguments, "features.shell_tool")?.GetValue<bool>() == false &&
            Override(arguments, "features.shell_snapshot")?.GetValue<bool>() == false &&
            Override(arguments, "features.memories")?.GetValue<bool>() == false,
            "Draft launch did not disable shell tooling and provider memory.");
        var fixture = Path.GetFileName(cwd);
        var thread = Guid.NewGuid().ToString("N");
        var pid = Environment.ProcessId;
        var traceDirectory = Environment.GetEnvironmentVariable(TraceVariable);
        var traceFile = traceDirectory is null ? null : Path.Combine(Directory.CreateDirectory(traceDirectory).FullName, $"draft-{pid}.jsonl");
        await Trace("launch", new JsonObject
        {
            ["command"] = command, ["args"] = new JsonArray(engineArgs.Select(argument => JsonValue.Create(argument)).ToArray()),
            ["manifest"] = manifest, ["cwd"] = cwd, ["gameplay_source"] = source
        });

        await using (var native = Environment.GetEnvironmentVariable(NativeVariable) == "1"
            ? new AgentRpcProcess(command, engineArgs, cwd, requireJsonRpcVersion: true) : null)
        {
            using var startup = new CancellationTokenSource(TimeSpan.FromSeconds(30));
            var tools = native is null ? FixtureTools(fixture, source) : await ReadToolsAsync(native, startup.Token);
            if (native is not null)
                Check(tools.Select(tool => tool.Key).Order(StringComparer.Ordinal).SequenceEqual(expectedTools),
                    "Native draft advertised tools outside its scoped catalog.");
            await Trace("native/catalog", new JsonObject { ["native"] = native is not null, ["tools"] = tools.DeepClone() });
            while (await Console.In.ReadLineAsync() is { } line)
            {
                var request = JsonNode.Parse(line)!.AsObject();
                var method = request["method"]!.GetValue<string>();
                var parameters = request["params"]!.AsObject();
                Task Reply(JsonObject result) => CodexReviewContract.Send(new JsonObject { ["id"] = request["id"]?.DeepClone(), ["result"] = result });
                switch (method)
                {
                    case "initialize":
                        Check(parameters["capabilities"]?["experimentalApi"]?.GetValue<bool>() == true,
                            "Draft worker needs the provider environment-selection protocol.");
                        await Reply(new JsonObject());
                        break;
                    case "initialized": break;
                    case "thread/start":
                    {
                        Check(string.Equals(parameters["cwd"]?.GetValue<string>(), cwd, pathComparison) &&
                            parameters["approvalPolicy"]?.GetValue<string>() == "never" &&
                            parameters["sandbox"]?.GetValue<string>() == "read-only" &&
                            parameters["ephemeral"]?.GetValue<bool>() == true &&
                            parameters["environments"] is JsonArray { Count: 0 },
                            "Draft thread has incorrect workspace, execution, approval or persistence settings.");
                        await Trace(method, parameters);
                        var forcedFailure = traceDirectory is null ? null :
                            Path.Combine(traceDirectory, "reject-scope-" + fixture);
                        var rejectScope = File.Exists(forcedFailure);
                        if (rejectScope) File.Delete(forcedFailure!);
                        var identity = new JsonObject { ["id"] = thread };
                        if (fixture != MissingScope)
                            identity["environments"] = fixture is LocalExecutor or GameplayExecutor || rejectScope
                                ? new JsonArray(new JsonObject { ["environmentId"] = "local", ["cwd"] = cwd })
                                : new JsonArray();
                        await Reply(new JsonObject
                        {
                            ["thread"] = identity,
                            ["cwd"] = cwd,
                            ["approvalPolicy"] = "never",
                            ["sandbox"] = new JsonObject
                            {
                                ["type"] = fixture == WritableScope ? "workspaceWrite" : "readOnly",
                                ["networkAccess"] = false
                            }
                        });
                        break;
                    }
                    case "mcpServerStatus/list":
                        Check(parameters["threadId"]?.GetValue<string>() == thread && parameters["serverName"]?.GetValue<string>() == "poima",
                            "Draft worker did not verify tools for its own thread.");
                        await Reply(new JsonObject { ["data"] = new JsonArray(new JsonObject { ["name"] = "poima", ["tools"] = tools.DeepClone() }) });
                        break;
                    case "turn/start":
                    {
                        Check(parameters["threadId"]?.GetValue<string>() == thread, "A draft turn went to another thread.");
                        var turn = Guid.NewGuid().ToString("N");
                        var text = parameters["input"]![0]!["text"]!.GetValue<string>();
                        const string assignment = "\n\nCurrent assignment:\n";
                        var separator = text.LastIndexOf(assignment, StringComparison.Ordinal);
                        var task = separator < 0 ? text : text[(separator + assignment.Length)..];
                        await Trace(method, new JsonObject { ["turn"] = turn, ["text"] = text });
                        await Reply(new JsonObject { ["turn"] = new JsonObject { ["id"] = turn } });
                        await CodexReviewContract.CheckInteractiveRequestsAsync(thread, turn);
                        Task Delta(string delta, string id) => CodexReviewContract.Send(CodexReviewContract.Event("item/agentMessage/delta",
                            new JsonObject { ["threadId"] = id, ["turnId"] = turn, ["delta"] = delta }));
                        await Delta("Unrelated text.", "other-thread");
                        if (task.Contains(HoldTask, StringComparison.Ordinal))
                        {
                            await Delta(Holding, thread);
                            break;
                        }
                        using var operation = new CancellationTokenSource(TimeSpan.FromSeconds(30));
                        var finding = await PerformAssignmentAsync(native, task, operation.Token);
                        if (traceDirectory is not null && task.Contains(WaitForRelease, StringComparison.Ordinal))
                        {
                            await Delta("Draft edit verified and waiting for release.", thread);
                            while (!File.Exists(Path.Combine(traceDirectory, "release-draft")))
                                await Task.Delay(25, operation.Token);
                        }
                        if (task.Contains(ReportTask, StringComparison.Ordinal))
                        {
                            await Delta(finding[..12], thread);
                            await Delta(finding[12..], thread);
                        }
                        else await Delta(finding, thread);
                        await CodexReviewContract.Send(CodexReviewContract.Event("item/completed", new JsonObject
                        {
                            ["threadId"] = thread, ["turnId"] = turn, ["item"] = new JsonObject { ["type"] = "agentMessage", ["text"] = finding }
                        }));
                        await CodexReviewContract.Send(CodexReviewContract.TurnEvent("turn/completed", thread, turn));
                        await Trace("turn/completed", new JsonObject { ["turn"] = turn, ["answer"] = finding });
                        break;
                    }
                    default: throw new InvalidDataException("Unexpected draft fixture method " + method);
                }
            }
        }
        await Trace("exit", new JsonObject());

        Task Trace(string kind, JsonObject details)
        {
            if (traceFile is null) return Task.CompletedTask;
            details["kind"] = kind;
            details["thread"] = thread;
            details["pid"] = pid;
            return File.AppendAllTextAsync(traceFile, details.ToJsonString() + "\n");
        }

        async Task<JsonNode> World(AgentRpcProcess native, string method, JsonObject parameters, CancellationToken token)
        {
            var request = new JsonObject
            {
                ["name"] = "poima_call",
                ["arguments"] = new JsonObject { ["method"] = method, ["params"] = parameters }
            };
            var reply = await native.RequestAsync("tools/call", request, token);
            await Trace("native/tool", new JsonObject { ["request"] = request, ["reply"] = reply?.DeepClone() });
            if (reply?["isError"]?.GetValue<bool>() == true || reply?["structuredContent"]?["result"] is not JsonNode result)
                throw new InvalidDataException($"Native draft call {method} did not succeed.");
            return result;
        }

        async Task<string> PerformAssignmentAsync(AgentRpcProcess? native, string task, CancellationToken token)
        {
            (int Number, string Name) assignment;
            if (task.Contains(LanternTask, StringComparison.Ordinal)) assignment = (0xda01, "Draft lantern");
            else if (task.Contains(BenchTask, StringComparison.Ordinal)) assignment = (0xda02, "Draft quay bench");
            else if (task.Contains(CodexLeadWorkerContract.RepairTask, StringComparison.Ordinal)) assignment = (0xda03, "Draft lantern brace");
            else if (task.Contains(CodexLeadWorkerContract.RecoverTask, StringComparison.Ordinal)) assignment = (0xda04, "Worker recovery marker");
            else if (task.Contains(ReportTask, StringComparison.Ordinal)) return CodexLeadWorkerContract.MarkdownReport;
            else return "Draft is ready to review.";
            if (native is null) return assignment.Name + " created at revision 3.";

            var entityId = assignment.Number.ToString("x32", CultureInfo.InvariantCulture);
            var initial = await World(native, "world.inspect", new JsonObject(), token);
            var baseRevision = initial["revision"]!.GetValue<long>();
            var op = new JsonObject { ["op"] = "entity.create", ["id"] = entityId, ["name"] = assignment.Name };
            var result = await World(native, "world.transact", new JsonObject
            {
                ["base_revision"] = baseRevision, ["request_id"] = Guid.NewGuid().ToString("N"), ["ops"] = new JsonArray(op)
            }, token);
            var revision = result["revision"]!.GetValue<long>();
            Check(result["committed"]?.GetValue<bool>() == true && revision == baseRevision + 1, "Draft operation did not advance the native revision.");
            var entity = await World(native, "entity.get", new JsonObject { ["id"] = entityId, ["revision"] = revision }, token);
            Check(entity["value"]?["name"]?.GetValue<string>() == assignment.Name, "Draft entity read did not confirm the edit.");
            return $"{assignment.Name} created at revision {revision}. Entity {entityId}.";
        }
    }

    private static JsonObject FixtureTools(string fixture, bool source)
    {
        var dispatcher = new JsonArray("world.inspect", "world.transact", "entity.get");
        if ((source && fixture != MissingSourceRead) || fixture == SceneWithSourceRead) dispatcher.Add(SourceRead);
        if (fixture == SourceWriteOnDispatcher) dispatcher.Add(SourceWrite);
        if (fixture == BuildOnDispatcher) dispatcher.Add("development.publish");
        if (fixture == ChecksOnDispatcher) dispatcher.Add("project.verification.write");
        var result = new JsonObject
        {
            ["poima_call"] = MethodTool(dispatcher),
            ["poima_discover"] = new JsonObject(),
            ["poima_reference"] = new JsonObject()
        };
        if (source && fixture != MissingSourceTool)
        {
            var writes = new JsonArray(SourceWrite);
            if (fixture == ExpandedSourceTool) writes.Add("development.publish");
            result["poima_source"] = MethodTool(writes);
        }
        if (fixture == "Wrong catalog") result["poima_build"] = new JsonObject();
        if (fixture == ExtraChecksTool) result["poima_checks"] = new JsonObject();
        return result;

        static JsonObject MethodTool(JsonArray methods) => new()
        {
            ["inputSchema"] = new JsonObject
            {
                ["properties"] = new JsonObject
                {
                    ["method"] = new JsonObject { ["enum"] = methods }
                }
            }
        };
    }

    private static async Task<JsonObject> ReadToolsAsync(AgentRpcProcess native, CancellationToken cancellation)
    {
        var result = new JsonObject();
        var initialization = await native.RequestAsync("initialize", new JsonObject
        {
            ["protocolVersion"] = "2025-11-25", ["capabilities"] = new JsonObject(),
            ["clientInfo"] = new JsonObject { ["name"] = "poima-draft-fixture", ["version"] = "1" }
        }, cancellation);
        Check(initialization?["protocolVersion"]?.GetValue<string>() == "2025-11-25", "Native draft did not initialize.");
        await native.NotifyAsync("notifications/initialized", cancellation: cancellation);
        var catalog = await native.RequestAsync("tools/list", cancellation: cancellation);
        foreach (var tool in catalog!["tools"]!.AsArray())
        {
            var name = tool!["name"]!.GetValue<string>();
            result[name] = tool.DeepClone();
        }
        return result;
    }

    private static JsonNode? Override(string[] arguments, string key)
    {
        var prefix = key + "=";
        var value = arguments.SingleOrDefault(argument => argument.StartsWith(prefix, StringComparison.Ordinal));
        return value is null ? null : JsonNode.Parse(value[prefix.Length..]);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidDataException(message);
    }
}
