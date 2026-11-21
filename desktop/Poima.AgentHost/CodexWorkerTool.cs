// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace Poima.AgentHost;

public enum CodexWorkerScope { Scene, GameplaySource }

public sealed record CodexWorkerTask(string Task, CodexWorkerScope Scope = CodexWorkerScope.Scene);
public sealed record CodexWorkerFile(string Path, string Sha256);
public sealed record CodexWorkerScene(string DraftId, string WorldSha256)
{
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public CodexWorkerFile[]? Files { get; init; }
}
public sealed record CodexWorkerFollowUp(CodexWorkerScene Draft, string Task);
public sealed record CodexWorkerRequest(string RequestId, long? ExpectedRevision, string[] Tasks, int WaitMilliseconds,
    CodexWorkerScene[]? Drafts = null, CodexWorkerFollowUp? FollowUp = null, string? InspectDraftId = null)
{
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public CodexWorkerTask[]? Assignments { get; init; }
}

public static class CodexWorkerTool
{
    public const string Name = "poima_workers";
    public const int MaximumWorkers = 10;
    public const int MaximumActiveWorkers = 5;
    public const int MaximumTaskLength = 4000;
    public const int MaximumFilesPerDraft = 64;

    public static JsonObject Definition() => new()
    {
        ["type"] = "function",
        ["name"] = Name,
        ["description"] = "Assign and verify independent scene work in this Poima project. " +
            "Use assign with a fresh 32-hex request_id, the world.inspect revision and an array of focused tasks. " +
            "Workers start from one saved checkpoint and write to separate drafts. Work above the active worker limit stays queued. " +
            "Use inspect with the same request_id " +
            "to collect statuses and reports. Use inspect_draft with a draft_id to read its current revision and world_sha256, " +
            "including when a worker failed or stopped. Use revise with a fresh request_id, draft_id, its current world_sha256, and a focused task " +
            "to follow up with the same worker. Inspect the follow-up request for the new report and exact scene hash. " +
            "Use verify with a new request_id and the draft_id and world_sha256 from each selected finished worker. " +
            "Inspect the verification request for build and playtest results. inspect can wait briefly for a change. The creator approves passing builds.",
        ["inputSchema"] = JsonNode.Parse("""
            {"type":"object","additionalProperties":false,"required":["action","request_id"],"properties":{
                "action":{"type":"string","enum":["assign","inspect","inspect_draft","revise","verify"]},
                "request_id":{"type":"string","pattern":"^[0-9a-f]{32}$"},
                "expected_revision":{"type":"integer","minimum":0,"maximum":9007199254740991},
                "tasks":{"type":"array","minItems":1,"maxItems":10,"items":{"type":"string","minLength":1,"maxLength":4000}},
                "draft_id":{"type":"string","pattern":"^[0-9a-f]{32}$"},
                "world_sha256":{"type":"string","pattern":"^[0-9a-f]{64}$"},
                "task":{"type":"string","minLength":1,"maxLength":4000},
                "drafts":{"type":"array","minItems":1,"maxItems":10,"items":{
                    "type":"object","additionalProperties":false,"required":["draft_id","world_sha256"],"properties":{
                        "draft_id":{"type":"string","pattern":"^[0-9a-f]{32}$"},
                        "world_sha256":{"type":"string","pattern":"^[0-9a-f]{64}$"}
                    }
                }},
                "wait_ms":{"type":"integer","minimum":0,"maximum":10000}
            }}
            """)
    };

    public static JsonObject Definition(bool allowGameplaySourceEdits)
    {
        var definition = Definition();
        if (!allowGameplaySourceEdits) return definition;

        definition["description"] = Text(definition["description"]) +
            " For C# edits, assign a task object with task and scope set to gameplay_source. " +
            "When verifying selected C# edits, include files with their inspected path and sha256.";
        var properties = definition["inputSchema"]!["properties"]!;
        var tasks = properties["tasks"]!;
        tasks["items"] = new JsonObject
        {
            ["oneOf"] = new JsonArray(tasks["items"]!.DeepClone(), JsonNode.Parse("""
                {"type":"object","additionalProperties":false,"required":["task"],"properties":{
                    "task":{"type":"string","minLength":1,"maxLength":4000},
                    "scope":{"type":"string","enum":["scene","gameplay_source"]}
                }}
                """))
        };
        properties["drafts"]!["items"]!["properties"]!["files"] = JsonNode.Parse("""
            {"type":"array","maxItems":64,"items":{
                "type":"object","additionalProperties":false,"required":["path","sha256"],"properties":{
                    "path":{"type":"string","maxLength":1024,"pattern":"^gameplay-src/.+[.]cs$"},
                    "sha256":{"type":"string","pattern":"^[0-9a-f]{64}$"}
                }
            }}
            """);
        return definition;
    }

    public static CodexWorkerRequest Read(JsonNode? arguments) => Read(arguments, allowGameplaySourceEdits: false);

    public static CodexWorkerRequest Read(JsonNode? arguments, bool allowGameplaySourceEdits)
    {
        if (arguments is not JsonObject input || Hex(input["request_id"], 32) is not { } id)
            throw new ArgumentException("Choose a 32-character lowercase hexadecimal request_id.");
        var action = Text(input["action"]);
        if (action == "inspect")
        {
            var wait = 0;
            if (input.Any(field => field.Key is not ("action" or "request_id" or "wait_ms")) ||
                input.ContainsKey("wait_ms") && (input["wait_ms"] is not JsonValue value ||
                    !value.TryGetValue(out wait) || wait is < 0 or > 10000))
                throw new ArgumentException("inspect accepts request_id and wait_ms from 0 through 10000.");
            return new(id, null, [], wait);
        }
        if (action == "inspect_draft")
        {
            if (input.Count != 3 || Hex(input["draft_id"], 32) is not { } draftId)
                throw new ArgumentException("inspect_draft requires request_id and draft_id.");
            return new(id, null, [], 0, InspectDraftId: draftId);
        }
        if (action == "revise")
        {
            if (input.Count != 5 || Hex(input["draft_id"], 32) is not { } draftId ||
                Hex(input["world_sha256"], 64) is not { } worldHash)
                throw new ArgumentException("revise requires request_id, draft_id, world_sha256 and task.");
            return new(id, null, [], 0, FollowUp: new(new(draftId, worldHash), ReadTask(input["task"])));
        }
        if (action == "verify")
        {
            if (input.Count != 3 || input["drafts"] is not JsonArray drafts || drafts.Count is < 1 or > MaximumWorkers)
                throw new ArgumentException("verify requires request_id and one to ten finished drafts.");
            var selection = new List<CodexWorkerScene>();
            var ids = new HashSet<string>(StringComparer.Ordinal);
            foreach (var item in drafts)
            {
                if (item is not JsonObject draft || draft.Any(field =>
                        field.Key is not ("draft_id" or "world_sha256") && !(allowGameplaySourceEdits && field.Key == "files")) ||
                    Hex(draft["draft_id"], 32) is not { } draftId ||
                    Hex(draft["world_sha256"], 64) is not { } hash || !ids.Add(draftId))
                    throw new ArgumentException("Each selected draft needs a unique draft_id and a lowercase world_sha256.");
                selection.Add(new(draftId, hash)
                {
                    Files = draft.ContainsKey("files") ? ReadFiles(draft["files"]) : null
                });
            }
            return new(id, null, [], 0, selection.OrderBy(draft => draft.DraftId, StringComparer.Ordinal).ToArray());
        }
        if (action != "assign" || input.Count != 4 ||
            input["expected_revision"] is not JsonValue revision || !revision.TryGetValue<long>(out var expected) ||
            expected is < 0 or > 9007199254740991L || input["tasks"] is not JsonArray tasks ||
            tasks.Count is < 1 or > MaximumWorkers)
            throw new ArgumentException(allowGameplaySourceEdits
                ? "assign requires request_id, expected_revision and one to ten tasks."
                : "assign requires request_id, expected_revision and one to ten scene tasks.");
        if (!allowGameplaySourceEdits) return new(id, expected, tasks.Select(ReadTask).ToArray(), 0);

        var assignments = tasks.Select(ReadAssignment).ToArray();
        return new(id, expected, assignments.Select(task => task.Task).ToArray(), 0)
        {
            Assignments = assignments.Any(task => task.Scope == CodexWorkerScope.GameplaySource) ? assignments : null
        };
    }

    public static JsonObject Response(bool success, JsonObject result) => new()
    {
        ["success"] = success,
        ["contentItems"] = new JsonArray(new JsonObject { ["type"] = "inputText", ["text"] = result.ToJsonString() })
    };

    public static string? Text(JsonNode? node) => node is JsonValue value && value.TryGetValue<string>(out var text) ? text : null;

    private static string ReadTask(JsonNode? node) =>
        Text(node)?.Trim() is { Length: > 0 and <= MaximumTaskLength } task
            ? task : throw new ArgumentException("Each worker task needs between 1 and 4000 characters.");

    private static CodexWorkerTask ReadAssignment(JsonNode? node)
    {
        if (node is not JsonObject input) return new(ReadTask(node));
        if (input.Any(field => field.Key is not ("task" or "scope")))
            throw new ArgumentException("A scoped worker assignment accepts task and scope.");
        var task = ReadTask(input["task"]);
        var scope = input.ContainsKey("scope") ? Text(input["scope"]) : "scene";
        return scope switch
        {
            "scene" => new(task),
            "gameplay_source" => new(task, CodexWorkerScope.GameplaySource),
            _ => throw new ArgumentException("Worker scope must be scene or gameplay_source.")
        };
    }

    private static CodexWorkerFile[]? ReadFiles(JsonNode? node)
    {
        if (node is not JsonArray files || files.Count > MaximumFilesPerDraft)
            throw new ArgumentException($"Select up to {MaximumFilesPerDraft} C# files per draft.");
        if (files.Count == 0) return null;
        var selected = new List<CodexWorkerFile>(files.Count);
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var item in files)
        {
            if (item is not JsonObject { Count: 2 } file || Text(file["path"]) is not { } path ||
                !IsGameplaySourcePath(path) || Hex(file["sha256"], 64) is not { } sha256 || !paths.Add(path))
                throw new ArgumentException("Each selected file needs a unique portable .cs path under gameplay-src and a lowercase sha256.");
            selected.Add(new(path, sha256));
        }
        return selected.OrderBy(file => file.Path, StringComparer.Ordinal).ToArray();
    }

    public static bool IsGameplaySourcePath(string path)
    {
        if (path.Length > 1024 || !path.StartsWith("gameplay-src/", StringComparison.Ordinal) ||
            !path.EndsWith(".cs", StringComparison.Ordinal))
            return false;
        var parts = path.Split('/');
        if (parts[^1].Length <= 3) return false;
        foreach (var part in parts)
        {
            if (part.Length is < 1 or > 128 || part[^1] is '.' or ' ' ||
                !part.All(c => char.IsAsciiLetterOrDigit(c) || c is '_' or '-' or '.' or ' '))
                return false;
            var lower = part.ToLowerInvariant();
            if (lower.StartsWith(".poima-", StringComparison.Ordinal) || lower is ".git" or ".vs")
                return false;
            var dot = lower.IndexOf('.');
            var stem = dot < 0 ? lower : lower[..dot];
            if (stem is "con" or "prn" or "aux" or "nul" ||
                (stem.Length == 4 && stem[..3] is ("com" or "lpt") && stem[3] is >= '1' and <= '9'))
                return false;
        }
        return true;
    }

    private static string? Hex(JsonNode? node, int length) => Text(node) is { } text && text.Length == length &&
        text.All(c => c is (>= '0' and <= '9') or (>= 'a' and <= 'f')) ? text : null;
}
