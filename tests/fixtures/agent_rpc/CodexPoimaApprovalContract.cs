// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using Poima.Editor;

static class CodexPoimaApprovalContract
{
    public static void Verify()
    {
        const string request = """
            {"mode":"form","serverName":"poima","threadId":"thread-1","turnId":"turn-1",
             "message":"Allow the poima MCP server to run tool \"poima_call\"?",
             "requestedSchema":{"type":"object","properties":{}},
             "_meta":{"codex_approval_kind":"mcp_tool_call","persist":["session","always"],
                      "tool_params":{"method":"world.inspect","params":{"label":"泉"}}}}
            """;
        JsonObject ReadFixture() => JsonNode.Parse(request)!.AsObject();
        CodexPoimaApproval? Review(JsonObject value) => CodexPoimaApproval.Read(value, "thread-1", "turn-1");
        var legacy = Review(ReadFixture()) ?? throw new InvalidDataException("The real legacy form was rejected.");
        var lines = legacy.Details.Split('\n', 2);
        if (lines[0] != "poima_call" || !JsonNode.DeepEquals(JsonNode.Parse(lines[1]), ReadFixture()["_meta"]!["tool_params"]))
            throw new InvalidDataException("Approval changed the visible tool arguments.");
        var modern = ReadFixture();
        modern["message"] = "Details supplied separately";
        modern["_meta"]!["tool_name"] = "poima_source";
        if (Review(modern)?.Details.StartsWith("poima_source\n", StringComparison.Ordinal) != true)
            throw new InvalidDataException("Named approval metadata was rejected.");

        var rejected = new Action<JsonObject>[]
        {
            value => value["serverName"] = "other-server",
            value => value["threadId"] = "other-thread",
            value => value["turnId"] = "previous-turn",
            value => value["turnId"] = null,
            value => value["mode"] = "url",
            value => value["requestedSchema"]!["properties"]!["question"] = new JsonObject { ["type"] = "string" },
            value => value["requestedSchema"]!["required"] = new JsonArray("question"),
            value => value["_meta"]!["codex_approval_kind"] = "browser_auth",
            value => value["_meta"]!["tool_name"] = "another_tool",
            value => value["_meta"]!["tool_name"] = 42,
            value => value["_meta"]!.AsObject().Remove("tool_params"),
            value => value["message"] = "Allow a tool?",
            value => value["message"] = "Allow the poima MCP server to run tool \"?",
            value => value["_meta"]!["tool_params"]!["too_large"] = new string('a', 1024 * 1024)
        };
        foreach (var alter in rejected)
        {
            var invalid = ReadFixture();
            alter(invalid);
            if (Review(invalid) is not null) throw new InvalidDataException("An unrelated or unreadable approval was offered.");
        }
        Console.WriteLine("Codex Poima approval form contract passed.");
    }
}
