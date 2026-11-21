// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text.Json.Nodes;
using Poima.AgentHost;

internal static class CodexLeadWorkerContract
{
    public const string DisabledPrompt = "Confirm disabled worker permission at revision ";
    public const string DelegatePrompt = "Delegate scene work at revision ";
    public const string ReplayPrompt = "Replay worker assignment ";
    public const string VerifyPrompt = "Verify drafts for assignment ";
    public const string Finished = "Lead collected both worker reports.";
    public const string MarkdownReport = """
        ## Review notes

        - **Lighting** uses `draft`.
        - *Placement* stays separate.

        ```cs
        var ready = true;
        ```

        | Item | State |
        | --- | --- |
        | Lantern | Verified |

        > Keep the original project.

        <b>Literal HTML</b>
        """;

    public static async Task<string?> ExerciseAsync(string prompt, JsonArray? tools, string thread, string turn,
        Func<string, JsonObject, Task> trace)
    {
        var disabled = prompt.StartsWith(DisabledPrompt, StringComparison.Ordinal);
        var replay = prompt.StartsWith(ReplayPrompt, StringComparison.Ordinal);
        var verify = prompt.StartsWith(VerifyPrompt, StringComparison.Ordinal);
        if (!disabled && !replay && !verify && !prompt.StartsWith(DelegatePrompt, StringComparison.Ordinal)) return null;
        var prefix = disabled ? DisabledPrompt : DelegatePrompt;
        var firstLine = prompt.Split('\n', 2)[0].TrimEnd();
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(100));
        if (verify)
        {
            await VerifyAsync(firstLine.Substring(VerifyPrompt.Length, 32));
            return "Lead checked both combinations.";
        }
        var revision = long.Parse(firstLine.AsSpan(replay ? ReplayPrompt.Length + 32 + " at revision ".Length : prefix.Length).TrimEnd('.'),
            CultureInfo.InvariantCulture);
        var id = replay ? firstLine.Substring(ReplayPrompt.Length, 32) : Guid.NewGuid().ToString("N");
        var request = new JsonObject
        {
            ["action"] = "assign", ["request_id"] = id, ["expected_revision"] = revision,
            ["tasks"] = new JsonArray("Draft a lantern. Wait for the release signal.", "Draft a quay bench.")
        };

        if (disabled)
        {
            Check(tools is null, "The lead registered worker tools before they were authorized.");
            await Call(request, success: false);
            return "Worker permission is off.";
        }
        Check(tools is { Count: 1 } && tools[0]?["name"]?.GetValue<string>() == CodexWorkerTool.Name &&
            tools[0]?["type"]?.GetValue<string>() == "function", "The enabled lead did not register the worker tool.");
        if (replay)
        {
            var original = await Call(new JsonObject { ["action"] = "inspect", ["request_id"] = id }, success: true);
            var replayed = await Call(request, success: true);
            Check(original["state"]?.GetValue<string>() == "completed" && replayed["replayed"]?.GetValue<bool>() == true &&
                JsonNode.DeepEquals(original["workers"], replayed["workers"]) && replayed["workers"] is JsonArray { Count: 2 },
                "The reopened creator did not return the original assignment result.");
            return "Reopened assignment receipt verified.";
        }
        await Call(request, success: false, threadId: "different-thread");
        await Call(request, success: false, turnId: "previous-turn");
        var invalid = request.DeepClone().AsObject();
        invalid["tasks"] = new JsonArray(" ");
        await Call(invalid, success: false);

        var stale = request.DeepClone().AsObject();
        var staleId = Guid.NewGuid().ToString("N");
        stale["request_id"] = staleId;
        stale["expected_revision"] = revision + 1;
        await Call(stale, success: true);
        var rejected = await InspectUntil(staleId, row => row["state"]?.GetValue<string>() == "failed");
        Check(rejected["workers"]?.AsArray().Count == 0, "A stale assignment launched a worker.");

        var accepted = await Call(request, success: true);
        Check(accepted["request_id"]?.GetValue<string>() == id && accepted["replayed"]?.GetValue<bool>() == false,
            "The assignment did not receive its own identity.");
        var repeated = await Call(request, success: true);
        Check(repeated["replayed"]?.GetValue<bool>() == true, "An exact assignment retry did not reuse the batch.");
        var changed = request.DeepClone().AsObject();
        changed["tasks"] = new JsonArray("A different task.");
        await Call(changed, success: false);
        var parallel = await InspectUntil(id, row => row["workers"] is JsonArray { Count: 2 } workers &&
            workers[0]?["state"]?.GetValue<string>() == "running" && workers[1]?["state"]?.GetValue<string>() == "completed");
        Check(parallel["state"]?.GetValue<string>() == "running", "The lead confused a partial result with completion.");
        var traces = Environment.GetEnvironmentVariable("POIMA_TEST_CODEX_DRAFT_TRACE") ??
            throw new InvalidDataException("The lead fixture requires draft traces.");
        await File.WriteAllTextAsync(Path.Combine(traces, "release-draft"), "release", deadline.Token);
        var completed = await InspectUntil(id, row => row["state"]?.GetValue<string>() is "completed" or "failed");
        var results = completed["workers"]!.AsArray();
        Check(completed["state"]?.GetValue<string>() == "completed" && results.Count == 2 &&
            results[0]?["report"]?.GetValue<string>()?.Contains("Draft lantern created", StringComparison.Ordinal) == true &&
            results[1]?["report"]?.GetValue<string>()?.Contains("Draft quay bench created", StringComparison.Ordinal) == true,
            "The lead did not read both native worker reports.");
        return Finished + "\n\n" + MarkdownReport;

        async Task VerifyAsync(string assignment)
        {
            var ready = await Call(new JsonObject { ["action"] = "inspect", ["request_id"] = assignment }, success: true);
            Check(ready["state"]?.GetValue<string>() == "completed", "The assignment had no finished drafts.");
            var drafts = ready["workers"]!.AsArray().Select(row => new JsonObject
            {
                ["draft_id"] = row!["draft_id"]!.DeepClone(), ["world_sha256"] = row["world_sha256"]!.DeepClone()
            }).ToArray();
            var id = Guid.NewGuid().ToString("N");
            JsonObject Selection(string requestId, params JsonObject[] selected) => new()
            {
                ["action"] = "verify", ["request_id"] = requestId,
                ["drafts"] = new JsonArray(selected.Select(draft => draft.DeepClone()).ToArray())
            };
            await Call(Selection(assignment, drafts), success: false);
            await Call(Selection(id, drafts[0], drafts[0]), success: false);
            var stale = drafts[0].DeepClone().AsObject();
            stale["world_sha256"] = new string('0', 64);
            await Call(Selection(id, stale), success: false);

            var accepted = await Call(Selection(id, drafts), success: true);
            Check(accepted["request_id"]?.GetValue<string>() == id && accepted["replayed"]?.GetValue<bool>() == false,
                "The proposed combination has no receipt.");
            var repeated = await Call(Selection(id, drafts.Reverse().ToArray()), success: true);
            Check(repeated["replayed"]?.GetValue<bool>() == true, "Retrying the selection in a different order queued a new check.");
            await Call(Selection(id, drafts[0]), success: false);
            var passed = await InspectUntil(id, row => row["state"]?.GetValue<string>() is "passed" or "failed");
            Check(passed["state"]?.GetValue<string>() == "passed" && passed["groups_passed"]?.GetValue<int>() == 2 &&
                passed["groups_total"]?.GetValue<int>() == 2 && passed["report_sha256"]?.GetValue<string>()?.Length == 64 &&
                passed["awaiting_review"]?.GetValue<bool>() == true && passed["playable"]?.GetValue<bool>() == false,
                "A passing result did not become a reviewable proposal.");

            var subset = Guid.NewGuid().ToString("N");
            await Call(Selection(subset, drafts[0]), success: true);
            var failed = await InspectUntil(subset, row => row["state"]?.GetValue<string>() is "passed" or "failed");
            Check(failed["state"]?.GetValue<string>() == "failed" && failed["groups_passed"]?.GetValue<int>() == 1 &&
                failed["groups_total"]?.GetValue<int>() == 2 && failed["awaiting_review"]?.GetValue<bool>() == false &&
                failed["failures"]?[0]?["group"]?.GetValue<string>() == "Draft quay bench" &&
                failed["failures"]?[0]?["issue"]?.GetValue<string>() == "runtime.entity: Runtime entity does not exist.",
                "A failing selection did not identify its failed gameplay check.");
            var pending = await Call(new JsonObject { ["action"] = "inspect", ["request_id"] = id }, success: true);
            Check(JsonNode.DeepEquals(passed, pending), "A failing result altered the earlier verified proposal.");
        }

        async Task<JsonObject> Call(JsonObject arguments, bool success, string? threadId = null, string? turnId = null)
        {
            var callId = Guid.NewGuid().ToString("N");
            var parameters = new JsonObject
            {
                ["threadId"] = threadId ?? thread, ["turnId"] = turnId ?? turn, ["callId"] = callId,
                ["tool"] = CodexWorkerTool.Name, ["arguments"] = arguments.DeepClone()
            };
            await CodexReviewContract.Send(new JsonObject { ["id"] = callId, ["method"] = "item/tool/call", ["params"] = parameters });
            var line = await Console.In.ReadLineAsync().WaitAsync(deadline.Token);
            var reply = JsonNode.Parse(line ?? throw new EndOfStreamException())!;
            await trace("worker/tool", new JsonObject { ["request"] = parameters.DeepClone(), ["reply"] = reply.DeepClone() });
            Check(reply["id"]?.GetValue<string>() == callId && reply["result"]?["success"]?.GetValue<bool>() == success &&
                reply["result"]?["contentItems"]?[0]?["type"]?.GetValue<string>() == "inputText",
                "The lead got an incorrect worker tool reply.");
            return JsonNode.Parse(reply["result"]!["contentItems"]![0]!["text"]!.GetValue<string>())!.AsObject();
        }

        async Task<JsonObject> InspectUntil(string assignment, Func<JsonObject, bool> done)
        {
            while (!deadline.IsCancellationRequested)
            {
                var result = await Call(new JsonObject { ["action"] = "inspect", ["request_id"] = assignment, ["wait_ms"] = 1000 },
                    success: true);
                if (done(result)) return result;
                if (result["state"]?.GetValue<string>() == "failed")
                    throw new InvalidDataException("Assignment failed before the expected state.");
            }
            throw new TimeoutException("The lead did not observe the expected assignment state.");
        }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidDataException(message);
    }
}
