// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;

namespace Poima.AgentHost;

public sealed record CodexWorkerRequest(string RequestId, long? ExpectedRevision, string[] Tasks, int WaitMilliseconds);

public static class CodexWorkerTool
{
    public const string Name = "poima_workers";
    public const int MaximumWorkers = 10;
    public const int MaximumTaskLength = 4000;

    public static JsonObject Definition() => new()
    {
        ["type"] = "function",
        ["name"] = Name,
        ["description"] = "Assign independent scene work to Codex workers in this Poima project. " +
            "Use assign with a fresh 32-hex request_id, the world.inspect revision and an array of focused tasks. " +
            "Workers start from one saved checkpoint and write to separate drafts. Use inspect with the same request_id " +
            "to collect statuses and reports. inspect can wait briefly for a change. The creator reviews and checks the combination.",
        ["inputSchema"] = JsonNode.Parse("""
            {"type":"object","additionalProperties":false,"required":["action","request_id"],"properties":{
                "action":{"type":"string","enum":["assign","inspect"]},
                "request_id":{"type":"string","pattern":"^[0-9a-f]{32}$"},
                "expected_revision":{"type":"integer","minimum":0,"maximum":9007199254740991},
                "tasks":{"type":"array","minItems":1,"maxItems":10,"items":{"type":"string","minLength":1,"maxLength":4000}},
                "wait_ms":{"type":"integer","minimum":0,"maximum":10000}
            }}
            """)
    };

    public static CodexWorkerRequest Read(JsonNode? arguments)
    {
        if (arguments is not JsonObject input || Text(input["request_id"]) is not { Length: 32 } id ||
            id.Any(c => c is not (>= '0' and <= '9' or >= 'a' and <= 'f')))
            throw new ArgumentException("Choose a 32-character lowercase hexadecimal request_id.");
        if (Text(input["action"]) == "inspect")
        {
            var wait = 0;
            if (input.Any(field => field.Key is not ("action" or "request_id" or "wait_ms")) ||
                input.ContainsKey("wait_ms") && (input["wait_ms"] is not JsonValue value ||
                    !value.TryGetValue(out wait) || wait is < 0 or > 10000))
                throw new ArgumentException("inspect accepts request_id and wait_ms from 0 through 10000.");
            return new(id, null, [], wait);
        }
        if (Text(input["action"]) != "assign" || input.Count != 4 ||
            input["expected_revision"] is not JsonValue revision || !revision.TryGetValue<long>(out var expected) ||
            expected is < 0 or > 9007199254740991L || input["tasks"] is not JsonArray tasks ||
            tasks.Count is < 1 or > MaximumWorkers)
            throw new ArgumentException("assign requires request_id, expected_revision and one to ten scene tasks.");
        var text = new List<string>();
        foreach (var task in tasks)
        {
            var description = Text(task)?.Trim();
            if (string.IsNullOrEmpty(description) || description.Length > MaximumTaskLength)
                throw new ArgumentException("Each worker task needs between 1 and 4000 characters.");
            text.Add(description);
        }
        return new(id, expected, [.. text], 0);
    }

    public static JsonObject Response(bool success, JsonObject result) => new()
    {
        ["success"] = success,
        ["contentItems"] = new JsonArray(new JsonObject { ["type"] = "inputText", ["text"] = result.ToJsonString() })
    };

    public static string? Text(JsonNode? node) => node is JsonValue value && value.TryGetValue<string>(out var text) ? text : null;
}
