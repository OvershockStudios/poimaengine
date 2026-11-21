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
    public const string RepairPrompt = "Repair failed checks for assignment ";
    public const string ReplayRepairPrompt = "Replay worker follow-up ";
    public const string RecoverScopePrompt = "Recover blocked scene worker after ";
    public const string QueuePrompt = "Queue scene work at revision ";
    public const string RepairTask = "Add the lantern brace missing from the playtest.";
    public const string RecoverTask = "Add a worker recovery marker.";
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
        var repair = prompt.StartsWith(RepairPrompt, StringComparison.Ordinal);
        var replayRepair = prompt.StartsWith(ReplayRepairPrompt, StringComparison.Ordinal);
        var recoverScope = prompt.StartsWith(RecoverScopePrompt, StringComparison.Ordinal);
        var queue = prompt.StartsWith(QueuePrompt, StringComparison.Ordinal);
        if (!disabled && !replay && !verify && !repair && !replayRepair && !recoverScope && !queue &&
            !prompt.StartsWith(DelegatePrompt, StringComparison.Ordinal)) return null;
        var prefix = disabled ? DisabledPrompt : DelegatePrompt;
        var firstLine = prompt.Split('\n', 2)[0].TrimEnd();
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(100));
        if (queue)
        {
            var queuedRevision = long.Parse(firstLine.AsSpan(QueuePrompt.Length).TrimEnd('.'), CultureInfo.InvariantCulture);
            var queueId = Guid.NewGuid().ToString("N");
            var assignment = new JsonObject
            {
                ["action"] = "assign", ["request_id"] = queueId, ["expected_revision"] = queuedRevision,
                ["tasks"] = new JsonArray("Hold draft turn.", "Hold draft turn.", "Hold draft turn.", "Hold draft turn.",
                    "Hold draft turn.", "Draft a quay bench.", "Cancel this queued task.")
            };
            await Call(assignment, success: true);
            var waiting = await InspectUntil(queueId, row => row["workers"] is JsonArray { Count: 7 } workers &&
                workers.Select(worker => worker?["state"]?.GetValue<string>())
                    .SequenceEqual(["running", "running", "running", "running", "running", "queued", "queued"]));
            Check(waiting["active_worker_limit"]?.GetValue<int>() == 5, "The assignment did not describe the active limit.");
            var replayed = await Call(assignment, success: true);
            Check(replayed["replayed"]?.GetValue<bool>() == true && JsonNode.DeepEquals(waiting["workers"], replayed["workers"]),
                "Replaying a queued assignment changed its worker receipts.");
            return "Five workers active and two queued.";
        }
        if (recoverScope)
        {
            await RecoverScopeAsync(firstLine.Substring(RecoverScopePrompt.Length, 32));
            return "Lead recovered the scene worker after its scope was rejected.";
        }
        if (verify)
        {
            await VerifyAsync(firstLine.Substring(VerifyPrompt.Length, 32));
            return "Lead checked both combinations.";
        }
        if (repair)
        {
            await RepairAsync(firstLine.Substring(RepairPrompt.Length, 32));
            return "Lead fixed the failed check and verified all three groups.";
        }
        if (replayRepair)
        {
            var repairId = firstLine.Substring(ReplayRepairPrompt.Length, 32);
            var prior = await InspectUntil(repairId, result => result["state"]?.GetValue<string>() == "completed");
            var repeatedRepair = await Call(FollowUp(repairId, prior["draft_id"]!, prior["expected_world_sha256"]!, RepairTask),
                success: true);
            Check(repeatedRepair["replayed"]?.GetValue<bool>() == true, "The repaired worker receipt did not replay.");
            repeatedRepair["replayed"] = false;
            Check(JsonNode.DeepEquals(prior, repeatedRepair), "Reopened follow-up returned a different worker result.");
            return "Reopened worker follow-up receipt verified.";
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

        async Task RecoverScopeAsync(string priorId)
        {
            var prior = await InspectUntil(priorId, row => row["state"]?.GetValue<string>() == "completed");
            var draftId = prior["draft_id"]!;
            var originalHash = prior["world_sha256"]!;
            var traces = Environment.GetEnvironmentVariable("POIMA_TEST_CODEX_DRAFT_TRACE") ??
                throw new InvalidDataException("The lead fixture requires draft traces.");
            var folder = Path.GetFileName(Path.GetDirectoryName(prior["manifest"]!.GetValue<string>()));
            await File.WriteAllTextAsync(Path.Combine(traces, "reject-scope-" + folder), "once", deadline.Token);

            var failedId = Guid.NewGuid().ToString("N");
            await Call(FollowUp(failedId, draftId, originalHash, RecoverTask), success: true);
            var failed = await InspectUntil(failedId, row => row["state"]?.GetValue<string>() is not ("running" or "queued"));
            Check(failed["state"]?.GetValue<string>() == "failed" &&
                failed["error"]?.GetValue<string>()?.Contains("could not confirm an isolated scene worker", StringComparison.Ordinal) == true &&
                failed["world_sha256"] is null,
                "A rejected worker connection was not recorded as an unsuccessful follow-up.");
            var current = await Call(new JsonObject
            {
                ["action"] = "inspect_draft", ["request_id"] = Guid.NewGuid().ToString("N"), ["draft_id"] = draftId.DeepClone()
            }, success: true);
            Check(JsonNode.DeepEquals(current["world_sha256"], originalHash) &&
                current["latest_state"]?.GetValue<string>() == "failed" && current["matches_report"]?.GetValue<bool>() == false,
                "The rejected worker connection changed the draft or made its previous report eligible.");
            var id = Guid.NewGuid().ToString("N");
            await Call(FollowUp(id, draftId, current["world_sha256"]!, RecoverTask), success: true);
            var recovered = await InspectUntil(id, row => row["state"]?.GetValue<string>() == "completed");
            Check(!JsonNode.DeepEquals(recovered["world_sha256"], originalHash) &&
                recovered["report"]?.GetValue<string>()?.Contains("Worker recovery marker created", StringComparison.Ordinal) == true,
                "A guarded follow-up did not recover the same worker draft.");
            Check(JsonNode.DeepEquals(prior, await InspectUntil(priorId, row => row["state"]?.GetValue<string>() == "completed")),
                "Recovering the worker changed the prior completed receipt.");
        }

        async Task RepairAsync(string assignment)
        {
            var original = await InspectUntil(assignment, row => row["state"]?.GetValue<string>() == "completed");
            var rows = original["workers"]!.AsArray();
            var drafts = rows.Select(Scene).ToArray();
            var failed = await VerifySelection(Guid.NewGuid().ToString("N"), drafts);
            Check(failed["state"]?.GetValue<string>() == "failed" && failed["groups_passed"]?.GetValue<int>() == 2 &&
                failed["groups_total"]?.GetValue<int>() == 3 && failed["failures"]?[0]?["group"]?.GetValue<string>() == "Draft lantern brace",
                "The first playtest did not identify the missing brace.");
            var draftId = rows[0]!["draft_id"]!;
            var originalHash = rows[0]!["world_sha256"]!;
            var staleId = Guid.NewGuid().ToString("N");
            await Call(FollowUp(staleId, draftId, JsonValue.Create(new string('0', 64))!, RepairTask), success: true);
            var stale = await InspectUntil(staleId, row => row["state"]?.GetValue<string>() is not ("running" or "queued"));
            Check(stale["state"]?.GetValue<string>() == "failed" &&
                stale["error"]?.GetValue<string>()?.Contains("The draft changed.", StringComparison.Ordinal) == true,
                "A stale draft was allowed to start a follow-up.");

            var inspection = new JsonObject
            {
                ["action"] = "inspect_draft", ["request_id"] = Guid.NewGuid().ToString("N"),
                ["draft_id"] = new string('f', 32)
            };
            await Call(inspection, success: false);
            inspection["draft_id"] = draftId.DeepClone();
            inspection["wait_ms"] = 500;
            await Call(inspection, success: false);
            inspection.Remove("wait_ms");
            var current = await Call(inspection, success: true);
            Check(JsonNode.DeepEquals(current["draft_id"], draftId) &&
                JsonNode.DeepEquals(current["world_sha256"], originalHash) &&
                current["revision"]?.GetValue<long>() == original["base_revision"]!.GetValue<long>() + 1 &&
                current["latest_state"]?.GetValue<string>() == "failed" &&
                current["matches_report"]?.GetValue<bool>() == false,
                "The failed worker receipt did not permit independent draft inspection.");

            var repairId = Guid.NewGuid().ToString("N");
            var request = FollowUp(repairId, draftId, current["world_sha256"]!, RepairTask);
            await Call(FollowUp(assignment, draftId, originalHash, RepairTask), success: false);
            await Call(request, success: true);
            var replayed = await Call(request, success: true);
            Check(replayed["replayed"]?.GetValue<bool>() == true, "An exact follow-up retry started a second task.");
            await Call(FollowUp(repairId, draftId, originalHash, "A different repair."), success: false);
            var revised = await InspectUntil(repairId, row => row["state"]?.GetValue<string>() == "completed");
            Check(JsonNode.DeepEquals(revised["draft_id"], draftId) &&
                !JsonNode.DeepEquals(revised["world_sha256"], originalHash) &&
                revised["report"]?.GetValue<string>()?.Contains("Draft lantern brace created", StringComparison.Ordinal) == true,
                "The repaired worker did not supply the current draft hash.");
            var repairedDraft = await Call(inspection, success: true);
            Check(JsonNode.DeepEquals(repairedDraft["world_sha256"], revised["world_sha256"]) &&
                repairedDraft["revision"]?.GetValue<long>() == current["revision"]!.GetValue<long>() + 1 &&
                repairedDraft["latest_state"]?.GetValue<string>() == "completed" &&
                repairedDraft["matches_report"]?.GetValue<bool>() == true,
                "Draft inspection did not see the newly verified worker report.");
            await Call(new JsonObject
            {
                ["action"] = "verify", ["request_id"] = Guid.NewGuid().ToString("N"),
                ["drafts"] = new JsonArray(drafts.Select(draft => draft.DeepClone()).ToArray())
            }, success: false);
            var passed = await VerifySelection(Guid.NewGuid().ToString("N"), [Scene(revised), drafts[1]]);
            Check(passed["state"]?.GetValue<string>() == "passed" && passed["groups_passed"]?.GetValue<int>() == 3 &&
                passed["awaiting_review"]?.GetValue<bool>() == true && passed["playable"]?.GetValue<bool>() == false,
                "Reverification did not leave a passing candidate for approval.");
            var unchanged = await InspectUntil(assignment, row => row["state"]?.GetValue<string>() == "completed");
            Check(JsonNode.DeepEquals(original, unchanged), "Revising a worker rewrote the original assignment.");

            async Task<JsonObject> VerifySelection(string id, JsonObject[] selection)
            {
                await Call(new JsonObject
                {
                    ["action"] = "verify", ["request_id"] = id,
                    ["drafts"] = new JsonArray(selection.Select(draft => draft.DeepClone()).ToArray())
                }, success: true);
                return await InspectUntil(id, row => row["state"]?.GetValue<string>() is "passed" or "failed");
            }
        }

        static JsonObject Scene(JsonNode? row) => new()
        {
            ["draft_id"] = row!["draft_id"]!.DeepClone(), ["world_sha256"] = row["world_sha256"]!.DeepClone()
        };

        static JsonObject FollowUp(string request, JsonNode draft, JsonNode expected, string task) => new()
        {
            ["action"] = "revise", ["request_id"] = request, ["draft_id"] = draft.DeepClone(),
            ["world_sha256"] = expected.DeepClone(), ["task"] = task
        };

        async Task VerifyAsync(string assignment)
        {
            var ready = await Call(new JsonObject { ["action"] = "inspect", ["request_id"] = assignment }, success: true);
            Check(ready["state"]?.GetValue<string>() == "completed", "The assignment had no finished drafts.");
            var drafts = ready["workers"]!.AsArray().Select(Scene).ToArray();
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
