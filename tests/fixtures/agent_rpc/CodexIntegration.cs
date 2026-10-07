// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using Poima.AgentHost;

static class CodexIntegration
{
    // Explicit opt-in: uses the installed CLI's account and runs one short turn.
    public static async Task RunAsync(string[] args)
    {
        if (args.Length != 5) throw new ArgumentException("--codex CODEX POIMA ENDPOINT WORKING_DIRECTORY");
        using var deadline = new CancellationTokenSource(TimeSpan.FromMinutes(3));
        var token = deadline.Token;
        var completion = new TaskCompletionSource<JsonObject>(TaskCreationOptions.RunContinuationsAsynchronously);
        var methods = new HashSet<string>();
        string thread;
        string turn;
        Task Drain(CodexSession session, bool capture) => Task.Run(async () =>
        {
            try
            {
                await foreach (var message in session.Events.ReadAllAsync(token))
                {
                    var method = message["method"]!.GetValue<string>();
                    if (capture) methods.Add(method);
                    if (message["id"] is JsonNode id)
                        await session.RejectAsync(id, "This qualification does not approve tool use.", token);
                    if (capture && method == "turn/completed") completion.TrySetResult(message);
                }
            }
            catch (ObjectDisposedException) { }
            catch (EndOfStreamException) { }
            catch (Exception e) { if (capture) completion.TrySetException(e); else throw; }
        }, token);

        await using (var session = await CodexSession.ConnectAsync(args[1], args[2], args[3], args[4], token))
        {
            var draining = Drain(session, true);
            thread = await session.OpenAsync(cancellation: token);
            var servers = await session.ListMcpServersAsync(token);
            if (servers?["data"] is not JsonArray data || !data.Any(n => n?["name"]?.GetValue<string>() == "poima"
                && n["tools"] is JsonObject tools && tools.ContainsKey("poima_discover") && tools.ContainsKey("poima_call")
                && n["toolsError"] is null))
                throw new Exception("Poima MCP tools were not discovered through the thread.");
            turn = await session.SendAsync("Reply with exactly POIMA_SESSION_OK. Do not use tools, read files or modify anything.", token);
            var final = (await completion.Task.WaitAsync(token))["params"]!;
            if (final["threadId"]?.GetValue<string>() != thread || final["turn"]?["id"]?.GetValue<string>() != turn
                || final["turn"]?["status"]?.GetValue<string>() != "completed")
                throw new Exception("Turn did not complete with matching identities.");
            await session.DisposeAsync();
            await draining;
        }
        await using (var resumed = await CodexSession.ConnectAsync(args[1], args[2], args[3], args[4], token))
        {
            var draining = Drain(resumed, false);
            await resumed.OpenAsync(thread, token);
            var turns = await resumed.ListTurnsAsync(cancellation: token);
            if (turns?["data"] is not JsonArray data || !data.Any(n => n?["id"]?.GetValue<string>() == turn))
                throw new Exception("Persisted turn missing after reopening.");
            var items = await resumed.ListItemsAsync(turn, cancellation: token);
            if (items?["data"] is not JsonArray entries || !entries.Any(n => n?["turnId"]?.GetValue<string>() == turn
                && n["item"]?["type"]?.GetValue<string>() == "agentMessage"
                && n["item"]?["text"]?.GetValue<string>().Trim() == "POIMA_SESSION_OK"))
                throw new Exception("Persisted response missing after reopening.");
            await resumed.DisposeAsync();
            await draining;
        }
        Console.WriteLine($"Codex integration passed: Poima MCP listed, completed turn, process restart, persistent thread, paginated history; {methods.Count} notification kinds.");
    }
}
