// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using Poima.AgentHost;

internal static class AcpContract
{
    private const string Png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jJxkAAAAASUVORK5CYII=";

    public static async Task VerifyAsync(CancellationToken token)
    {
        var executable = Environment.ProcessPath!;
        var image = new AgentImage(Convert.FromBase64String(Png));
        await using (var textOnly = await AcpSession.ConnectAsync(executable, ["acp", "text"], executable,
            "acp-contract", Directory.GetCurrentDirectory(), token))
        {
            await textOnly.OpenAsync(token);
            Check(!textOnly.SupportsImages, "Omitted ACP image capability");
            try
            {
                await textOnly.PromptAsync("Read Poima", [image], token);
                throw new InvalidDataException("Text-only agent accepted an attachment.");
            }
            catch (InvalidOperationException error) when (error.Message == "This agent connection accepts text only.") { }
        }
        await using var agent = await AcpSession.ConnectAsync(executable, executable, "acp-contract",
            Directory.GetCurrentDirectory(), token);
        await agent.OpenAsync(token);
        Check(agent.ConversationId == "acp-test" && agent.SupportsImages, "Session identity and image capability");
        var prompt = agent.PromptAsync("Read Poima", [image], token);
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
        Console.WriteLine("ACP v1 session, shared endpoint, image capability and payload, exact approval and ordered prompt completion passed.");
    }

    public static async Task RunPeerAsync(bool supportsImages = true)
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
        var capabilities = new JsonObject { ["protocolVersion"] = 1 };
        if (supportsImages) capabilities["agentCapabilities"] = new JsonObject
        {
            ["promptCapabilities"] = new JsonObject { ["image"] = true }
        };
        await Reply(initialize, capabilities);
        var session = await Read("session/new");
        var server = session["params"]!["mcpServers"]![0]!;
        Check(server["name"]?.GetValue<string>() == "poima" &&
            JsonNode.DeepEquals(server["args"], new JsonArray("mcp", "--endpoint", "acp-contract")), "Shared MCP endpoint");
        await Reply(session, new() { ["sessionId"] = "acp-test" });
        if (!supportsImages)
        {
            Check(await Console.In.ReadLineAsync() is null, "Text-only session received an unsupported image request");
            return;
        }
        var prompt = await Read("session/prompt");
        var content = prompt["params"]?["prompt"] as JsonArray;
        Check(content?.Count == 2 && content[0]?["text"]?.GetValue<string>() == "Read Poima" &&
            JsonNode.DeepEquals(content[1], new JsonObject { ["type"] = "image", ["mimeType"] = "image/png", ["data"] = Png }),
            "Prompt text and exact encoded image");
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
