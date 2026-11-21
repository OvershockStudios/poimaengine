// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Globalization;
using System.Text.Json.Nodes;
using Poima.AgentHost;

internal static class CodexReviewContract
{
    public const string ModeVariable = "POIMA_TEST_CODEX_REVIEW";
    private const string TraceVariable = "POIMA_TEST_CODEX_REVIEW_TRACE";
    private const string Model = "review-fixture";
    private const string CrateTask = "Review the crate position.";
    private const string LightTask = "Review the light intensity.";
    private const string FollowupTask = "Explain the crate finding.";
    private const string SlowTask = "Wait for cancellation.";
    private const string CrateDraft = "Reviewing the crate.";
    private const string CrateFinding = "The crate needs more clearance.";
    private const string LightFinding = "The light is too bright.";
    private const string FollowupFinding = "The crate blocks the doorway.";
    private const string SlowDraft = "Waiting for cancellation.";
    private const string ReleaseCrate = "release-crate";

    public static async Task VerifyAsync(CancellationToken cancellation)
    {
        const long revision = 9007199254740991;
        const string endpoint = "review-contract";
        var client = Environment.ProcessPath!;
        var trace = Directory.CreateTempSubdirectory("poima-codex-review-").FullName;
        var savedMode = Environment.GetEnvironmentVariable(ModeVariable);
        var savedTrace = Environment.GetEnvironmentVariable(TraceVariable);
        Environment.SetEnvironmentVariable(ModeVariable, "1");
        Environment.SetEnvironmentVariable(TraceVariable, trace);
        try
        {
            Task<CodexReview> Open(string connection = endpoint, long pinned = revision) =>
                CodexReview.OpenAsync(client, client, connection, trace, pinned, Model, cancellation);
            foreach (var invalid in new[] { -1L, revision + 1 })
            {
                try { await using var review = await Open(pinned: invalid); throw new InvalidDataException("Accepted an invalid review revision."); }
                catch (ArgumentOutOfRangeException) { }
            }
            try { await using var review = await Open("review-invalid-tools"); throw new InvalidDataException("Accepted an unscoped review catalog."); }
            catch (InvalidOperationException error) when (error.Message.Contains("scoped review tools", StringComparison.Ordinal)) { }

            await using (var crate = await Open())
            await using (var light = await Open())
            {
                var started = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
                var crateTurn = crate.AskAsync(CrateTask, draft => started.TrySetResult(draft), cancellation);
                Check(await started.Task.WaitAsync(cancellation) == CrateDraft, "Crate review accepted a draft from another turn or thread.");
                try
                {
                    var drafts = new List<string>();
                    Check(await light.AskAsync(LightTask, drafts.Add, cancellation) == LightFinding &&
                        drafts.SequenceEqual([LightFinding]), "Overlapping light review received unrelated output.");
                    Check(!crateTurn.IsCompleted, "The crate turn finished before the independent light turn.");
                }
                finally { File.WriteAllText(Path.Combine(trace, ReleaseCrate), ""); }
                Check(await crateTurn == CrateFinding, "Crate review did not return its completed finding.");
                Check(await crate.AskAsync(FollowupTask + " 水", _ => { }, cancellation) == FollowupFinding,
                    "The follow-up did not return the crate finding.");

                using var stopping = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
                var slowStarted = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
                var pending = crate.AskAsync(SlowTask, draft => slowStarted.TrySetResult(draft), stopping.Token);
                Check(await slowStarted.Task.WaitAsync(cancellation) == SlowDraft, "The review never reached its pending turn.");
                stopping.Cancel();
                try { await pending; throw new InvalidDataException("Cancellation accepted a finding."); }
                catch (OperationCanceledException) when (!cancellation.IsCancellationRequested) { }
                await crate.DisposeAsync();
                Check(await light.AskAsync(LightTask, _ => { }, cancellation) == LightFinding,
                    "Retiring the crate review affected the independent light review.");
            }

            var peers = Directory.GetFiles(trace, "codex-*.jsonl")
                .Select(file => File.ReadLines(file).Select(line => JsonNode.Parse(line)!.AsObject()).ToArray()).ToArray();
            Check(peers.Length == 3, "Expected two independent app-server processes and one rejected connection.");
            Check(peers.Select(peer => peer[0]["thread"]!.GetValue<string>()).Distinct().Count() == 3,
                "Specialist threads were reused across connections.");
            foreach (var peer in peers)
            {
                var launch = peer.Single(row => Is(row, "launch"));
                var engineEndpoint = launch["endpoint"]!.GetValue<string>();
                Check(launch["command"]?.GetValue<string>() == client && launch["revision"]?.GetValue<long>() == revision &&
                    JsonNode.DeepEquals(launch["args"], new JsonArray("mcp", "--endpoint", engineEndpoint,
                        "--review-revision", revision.ToString(CultureInfo.InvariantCulture))), "The launch did not pin the expected endpoint, revision and executable.");
                var thread = peer.Single(row => Is(row, "thread/start"));
                Check(thread["approvalPolicy"]?.GetValue<string>() == "never" && thread["sandbox"]?.GetValue<string>() == "read-only" &&
                    thread["ephemeral"]?.GetValue<bool>() == true && thread["model"]?.GetValue<string>() == Model,
                    "The review thread had incorrect permissions, persistence, or model.");
                Check(peer[^1]["kind"]?.GetValue<string>() == "exit", "The review peer did not close its connection.");
                try
                {
                    using var child = Process.GetProcessById(launch["pid"]!.GetValue<int>());
                    Check(child.HasExited, "A retired review peer is still running.");
                }
                catch (ArgumentException) { } // The OS has already removed the exited process.
            }
            var cratePeer = peers.Single(peer => peer.Any(row => row["text"]?.GetValue<string>() == CrateTask));
            var turns = cratePeer.Where(row => Is(row, "turn/start")).ToArray();
            Check(turns.Select(row => row["text"]!.GetValue<string>()).SequenceEqual([CrateTask, FollowupTask + " 水", SlowTask]) &&
                turns.Select(row => row["turn"]!.GetValue<string>()).Distinct().Count() == 3 &&
                cratePeer.Count(row => Is(row, "turn/completed")) == 2,
                "Follow-up and cancellation were not correlated to distinct turns in the original review thread.");

            Task<CodexSession> NoShell(string name) =>
                CodexSession.ConnectAsync(client, client, name, trace, cancellation, disableShell: true);
            string leadThread;
            await using (var lead = await NoShell("no-shell-lead"))
            {
                leadThread = await lead.OpenAsync(cancellation: cancellation, model: Model, offerWorkers: true);
                await lead.SendAsync("A project turn without a shell.", cancellation);
            }
            await using (var restored = await NoShell("no-shell-lead"))
            {
                Check(await restored.OpenAsync(leadThread, cancellation, Model) == leadThread,
                    "Reconnecting did not confirm the shell was removed from the original thread.");
                await restored.SendAsync("A resumed turn without a shell.", cancellation);
            }
            await using (var refused = await NoShell("no-shell-unconfirmed"))
            {
                try
                {
                    await refused.OpenAsync(cancellation: cancellation, model: Model);
                    throw new InvalidDataException("The lead opened with an unconfirmed executor scope.");
                }
                catch (InvalidOperationException error) when (error.Message.Contains("shell access is disabled", StringComparison.Ordinal))
                { Check(refused.ThreadId is null, "The lead accepted turns following a scope failure."); }
            }
        }
        finally
        {
            Environment.SetEnvironmentVariable(ModeVariable, savedMode);
            Environment.SetEnvironmentVariable(TraceVariable, savedTrace);
            Directory.Delete(trace, recursive: true);
        }
        Console.WriteLine("Codex review permissions, independent turns, cancellation, and confirmed no-shell lead scope on start and resume passed.");
    }

    // This peer also drives the Windows creator UI fixture. It does not launch Poima MCP.
    public static async Task RunPeerAsync(string[] arguments)
    {
        const string commandPrefix = "mcp_servers.poima.command=";
        const string argsPrefix = "mcp_servers.poima.args=";
        var command = JsonNode.Parse(arguments.Single(arg => arg.StartsWith(commandPrefix, StringComparison.Ordinal))[commandPrefix.Length..]);
        var engineArgs = JsonNode.Parse(arguments.Single(arg => arg.StartsWith(argsPrefix, StringComparison.Ordinal))[argsPrefix.Length..])!.AsArray();
        Check(engineArgs[0]?.GetValue<string>() == "mcp" && engineArgs[1]?.GetValue<string>() == "--endpoint", "Invalid Poima launch arguments.");
        var endpoint = engineArgs[2]!.GetValue<string>();
        long? revision = engineArgs.Count == 5 && engineArgs[3]?.GetValue<string>() == "--review-revision"
            ? long.Parse(engineArgs[4]!.GetValue<string>(), CultureInfo.InvariantCulture) : null;
        Check(revision is null ? engineArgs.Count == 3 : arguments.Length == 5, "Unexpected launch flags on the Poima connection.");
        var pid = Environment.ProcessId;
        var thread = Guid.NewGuid().ToString("N");
        var noShell = arguments.Contains("features.shell_tool=false", StringComparer.Ordinal);
        if (noShell)
            Check(arguments.Contains("sandbox_mode=\"read-only\"", StringComparer.Ordinal) &&
                arguments.Contains("features.shell_snapshot=false", StringComparer.Ordinal) &&
                arguments.Contains("features.memories=false", StringComparer.Ordinal), "Shell-less lead launch retained local tool setup.");
        var hasExecutor = !noShell;
        var traceDirectory = Environment.GetEnvironmentVariable(TraceVariable);
        var traceFile = traceDirectory is null ? null : Path.Combine(Directory.CreateDirectory(traceDirectory).FullName, $"codex-{pid}.jsonl");
        JsonArray? dynamicTools = null;
        await Trace("launch", new JsonObject { ["endpoint"] = endpoint, ["revision"] = revision, ["command"] = command, ["args"] = engineArgs });

        while (await Console.In.ReadLineAsync() is { } line)
        {
            var call = JsonNode.Parse(line)!.AsObject();
            var method = call["method"]!.GetValue<string>();
            var parameters = call["params"]!.AsObject();
            Task Reply(JsonObject result) => Send(new JsonObject { ["id"] = call["id"]?.DeepClone(), ["result"] = result });
            switch (method)
            {
                case "initialize":
                    Check(revision is null || parameters["capabilities"] is null, "A review enabled interactive requests.");
                    await Reply(new JsonObject());
                    break;
                case "initialized": break;
                case "account/read":
                    await Reply(new JsonObject { ["requiresOpenaiAuth"] = true, ["account"] = new JsonObject { ["type"] = "chatgpt" } });
                    break;
                case "model/list":
                    await Reply(new JsonObject { ["data"] = new JsonArray(new JsonObject { ["model"] = Model, ["isDefault"] = true }) });
                    break;
                case "thread/start":
                    Check(parameters["cwd"]?.GetValue<string>() == Directory.GetCurrentDirectory(), "The thread opened outside the project directory.");
                    Check(revision is not null || parameters["approvalPolicy"]?.GetValue<string>() == "on-request" &&
                        parameters["sandbox"]?.GetValue<string>() == (noShell ? "read-only" : "workspace-write") &&
                        parameters["ephemeral"]?.GetValue<bool>() == false &&
                        (!noShell || parameters["environments"] is JsonArray { Count: 0 }),
                        "The lead thread lost its normal settings.");
                    hasExecutor |= endpoint == "no-shell-unconfirmed";
                    dynamicTools = parameters["dynamicTools"] as JsonArray;
                    await Trace(method, parameters);
                    await Reply(noShell ? NoShellResponse() : new JsonObject { ["thread"] = new JsonObject { ["id"] = thread } });
                    break;
                case "thread/resume" when noShell:
                    Check(parameters["sandbox"]?.GetValue<string>() == "read-only" &&
                        parameters["approvalPolicy"]?.GetValue<string>() == "on-request", "The resumed lead lost its scope.");
                    thread = parameters["threadId"]!.GetValue<string>();
                    hasExecutor = true; // The provider can attach its default on resume.
                    await Reply(NoShellResponse());
                    break;
                case "thread/settings/update" when noShell:
                    Check(parameters["threadId"]?.GetValue<string>() == thread &&
                        parameters["environments"] is JsonArray { Count: 0 }, "The lead did not detach the resumed executor.");
                    hasExecutor = false;
                    await Reply(new JsonObject());
                    break;
                case "thread/read" when noShell:
                    await Reply(NoShellResponse());
                    break;
                case "mcpServerStatus/list":
                    Check(parameters["serverName"]?.GetValue<string>() == "poima" && parameters["threadId"]?.GetValue<string>() == thread,
                        "MCP discovery did not target the review thread and server.");
                    var tools = new JsonObject { ["poima_discover"] = new JsonObject(), ["poima_reference"] = new JsonObject(), ["poima_inspect"] = new JsonObject() };
                    if (endpoint == "review-invalid-tools") tools["poima_call"] = new JsonObject();
                    await Reply(new JsonObject { ["data"] = new JsonArray(new JsonObject { ["name"] = "poima", ["tools"] = tools }) });
                    break;
                case "turn/start":
                    Check(parameters["threadId"]?.GetValue<string>() == thread, "A turn entered a different thread.");
                    Check(!noShell || !hasExecutor, "A shell-less thread submitted a turn with a local executor.");
                    var turn = Guid.NewGuid().ToString("N");
                    var text = parameters["input"]![0]!["text"]!.GetValue<string>();
                    await Trace(method, new JsonObject { ["turn"] = turn, ["text"] = text });
                    await Reply(new JsonObject { ["turn"] = new JsonObject { ["id"] = turn } });
                    await Send(TurnEvent("turn/started", thread, turn));
                    if (revision is not null)
                    {
                        await CheckInteractiveRequestsAsync(thread, turn);
                        await Send(Event("item/agentMessage/delta", new JsonObject { ["threadId"] = "other-thread", ["turnId"] = turn, ["delta"] = "Unrelated thread." }));
                        await Send(Event("item/agentMessage/delta", new JsonObject { ["threadId"] = thread, ["turnId"] = "previous-turn", ["delta"] = "Unrelated turn." }));
                        await Send(TurnEvent("turn/completed", "other-thread", turn));
                        await Send(TurnEvent("turn/completed", thread, "previous-turn"));
                    }
                    Task Draft(string delta) => Send(Event("item/agentMessage/delta", new JsonObject { ["threadId"] = thread, ["turnId"] = turn, ["delta"] = delta }));
                    if (revision is not null && text.Contains(SlowTask, StringComparison.Ordinal))
                    {
                        await Draft(SlowDraft);
                        break;
                    }
                    if (revision is not null && text.Contains(CrateTask, StringComparison.Ordinal) && traceDirectory is not null)
                    {
                        await Draft(CrateDraft);
                        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(25));
                        while (!File.Exists(Path.Combine(traceDirectory, ReleaseCrate))) await Task.Delay(25, deadline.Token);
                    }
                    var finding = revision is null
                        ? await CodexLeadWorkerContract.ExerciseAsync(text.Trim(), dynamicTools, thread, turn, Trace) ?? "Lead received the findings."
                        : Find(text);
                    await Draft(finding);
                    await Send(Event("item/completed", new JsonObject { ["threadId"] = thread, ["turnId"] = turn,
                        ["item"] = new JsonObject { ["type"] = "agentMessage", ["text"] = finding } }));
                    await Send(TurnEvent("turn/completed", thread, turn));
                    await Trace("turn/completed", new JsonObject { ["turn"] = turn });
                    break;
                default: throw new InvalidDataException("Unexpected review fixture method " + method);
            }
        }
        await Trace("exit", new JsonObject());

        JsonObject NoShellResponse() => new()
        {
            ["thread"] = new JsonObject
            {
                ["id"] = thread,
                ["environments"] = hasExecutor ? new JsonArray(new JsonObject { ["environmentId"] = "local" }) : new JsonArray()
            },
            ["cwd"] = Directory.GetCurrentDirectory(),
            ["approvalPolicy"] = "on-request",
            ["sandbox"] = new JsonObject { ["type"] = "readOnly", ["networkAccess"] = false }
        };

        Task Trace(string kind, JsonObject details)
        {
            if (traceFile is null) return Task.CompletedTask;
            details["kind"] = kind;
            details["pid"] = pid;
            details["thread"] = thread;
            return File.AppendAllTextAsync(traceFile, details.ToJsonString() + "\n");
        }
    }

    private static string Find(string text)
    {
        if (text.Contains(CrateTask, StringComparison.Ordinal)) return CrateFinding;
        if (text.Contains(LightTask, StringComparison.Ordinal)) return LightFinding;
        if (text.Contains(FollowupTask, StringComparison.Ordinal)) return FollowupFinding;
        throw new InvalidDataException("Unexpected review fixture task.");
    }

    internal static async Task CheckInteractiveRequestsAsync(string thread, string turn)
    {
        await Send(new JsonObject { ["id"] = "scoped-approval", ["method"] = "mcpServer/elicitation/request",
            ["params"] = new JsonObject { ["threadId"] = thread, ["turnId"] = turn } });
        var approval = JsonNode.Parse((await Console.In.ReadLineAsync())!);
        Check(approval?["id"]?.GetValue<string>() == "scoped-approval" && approval["result"]?["action"]?.GetValue<string>() == "cancel",
            "The specialist granted an interactive tool approval.");
        await Send(new JsonObject { ["id"] = 706, ["method"] = "item/tool/requestUserInput",
            ["params"] = new JsonObject { ["threadId"] = thread, ["turnId"] = turn } });
        var question = JsonNode.Parse((await Console.In.ReadLineAsync())!);
        Check(question?["id"]?.GetValue<int>() == 706 && question["error"]?["code"]?.GetValue<int>() == -32601,
            "The specialist accepted an interactive question.");
    }

    private static bool Is(JsonObject row, string kind) => row["kind"]?.GetValue<string>() == kind;

    internal static JsonObject Event(string method, JsonObject parameters) => new() { ["method"] = method, ["params"] = parameters };

    internal static JsonObject TurnEvent(string method, string thread, string turn) => Event(method,
        new JsonObject { ["threadId"] = thread, ["turn"] = new JsonObject { ["id"] = turn, ["status"] = "completed" } });

    internal static async Task Send(JsonObject message)
    {
        await Console.Out.WriteLineAsync(message.ToJsonString());
        await Console.Out.FlushAsync();
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidDataException(message);
    }
}
