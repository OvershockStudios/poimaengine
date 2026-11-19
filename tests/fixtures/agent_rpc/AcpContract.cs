// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using Poima.AgentHost;

internal static class AcpContract
{
    public static async Task VerifyAsync(CancellationToken token)
    {
        var executable = Environment.ProcessPath!;
        await using var agent = await AcpSession.ConnectAsync(executable, executable, "acp-contract",
            Directory.GetCurrentDirectory(), token);
        await agent.OpenAsync(token);
        Check(agent.ConversationId == "acp-test", "Session identity");
        var prompt = agent.PromptAsync("Read Poima", token);
        var first = await agent.Events.ReadAsync(token);
        var permission = await agent.Events.ReadAsync(token);
        Check(first["params"]?["update"]?["content"]?["text"]?.GetValue<string>() == "POIMA_", "Initial text delta");
        agent.AcknowledgeEvent();
        await agent.ReplyAsync(permission["id"]!, new JsonObject
        {
            ["outcome"] = new JsonObject { ["outcome"] = "selected", ["optionId"] = "permission-once" }
        }, token);
        agent.AcknowledgeEvent();
        var last = await agent.Events.ReadAsync(token);
        Check(last["params"]?["update"]?["content"]?["text"]?.GetValue<string>() == "READY", "Final text delta");
        // This following notification proves the transport read the prompt result.
        var postResponse = await agent.Events.ReadAsync(token);
        Check(postResponse["method"]?.GetValue<string>() == "test/response_sent", "Fixture received prompt result");
        Check(!prompt.IsCompleted, "Final text has not been handled yet");
        agent.AcknowledgeEvent();
        agent.AcknowledgeEvent();
        Check(await prompt == "end_turn", "Prompt completion after text delivery");
        Console.WriteLine("ACP v1 session, shared endpoint, exact approval and ordered prompt completion passed.");
    }

    public static async Task RunPeerAsync()
    {
        async Task<JsonObject> Read(string method)
        {
            var request = JsonNode.Parse((await Console.In.ReadLineAsync())!)!.AsObject();
            Check(request["jsonrpc"]?.GetValue<string>() == "2.0" &&
                request["method"]?.GetValue<string>() == method, "ACP request envelope");
            return request;
        }
        async Task Send(JsonObject message)
        {
            message["jsonrpc"] = "2.0";
            await Console.Out.WriteLineAsync(message.ToJsonString());
            await Console.Out.FlushAsync();
        }
        Task Reply(JsonObject request, JsonObject result) =>
            Send(new() { ["id"] = request["id"]!.DeepClone(), ["result"] = result });
        Task Delta(string text) => Send(new()
        {
            ["method"] = "session/update",
            ["params"] = new JsonObject
            {
                ["sessionId"] = "acp-test",
                ["update"] = new JsonObject
                {
                    ["sessionUpdate"] = "agent_message_chunk",
                    ["content"] = new JsonObject { ["type"] = "text", ["text"] = text }
                }
            }
        });

        var initialize = await Read("initialize");
        Check(initialize["params"]?["protocolVersion"]?.GetValue<int>() == 1, "ACP handshake");
        await Reply(initialize, new() { ["protocolVersion"] = 1 });
        var session = await Read("session/new");
        var server = session["params"]!["mcpServers"]![0]!;
        Check(server["name"]?.GetValue<string>() == "poima" &&
            JsonNode.DeepEquals(server["args"], new JsonArray("mcp", "--endpoint", "acp-contract")), "Shared MCP endpoint");
        await Reply(session, new() { ["sessionId"] = "acp-test" });
        var prompt = await Read("session/prompt");
        Check(prompt["params"]?["prompt"]?[0]?["text"]?.GetValue<string>() == "Read Poima", "Prompt text");
        await Delta("POIMA_");
        await Send(new() { ["id"] = "review-poima-tool", ["method"] = "session/request_permission" });
        var permission = JsonNode.Parse((await Console.In.ReadLineAsync())!)!;
        Check(permission["jsonrpc"]?.GetValue<string>() == "2.0" &&
            permission["id"]?.GetValue<string>() == "review-poima-tool" &&
            permission["result"]?["outcome"]?["optionId"]?.GetValue<string>() == "permission-once", "ACP approval result");
        await Delta("READY");
        await Reply(prompt, new() { ["stopReason"] = "end_turn" });
        await Send(new() { ["method"] = "test/response_sent" });
        await Console.In.ReadLineAsync();
    }

    private static void Check(bool condition, string description)
    {
        if (!condition) throw new InvalidOperationException(description);
    }
}
