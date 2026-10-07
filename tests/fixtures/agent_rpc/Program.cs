// SPDX-License-Identifier: Apache-2.0
using System.Reflection;
using System.Text.Json.Nodes;
using Poima.AgentHost;

if (args.Length > 0 && args[0] == "--codex") { await CodexIntegration.RunAsync(args); return; }

if (args.Length > 0 && args[0] == "peer")
{
    while (await Console.In.ReadLineAsync() is { } line)
    {
        var call = JsonNode.Parse(line)!;
        var id = call["id"]?.DeepClone();
        var method = call["method"]?.GetValue<string>();
        JsonObject Response(JsonNode? value) => new() { ["id"] = id, ["result"] = value };
        switch (method)
        {
            case "echo": await Console.Out.WriteLineAsync(Response(call["params"]?.DeepClone()).ToJsonString()); break;
            case "error": await Console.Out.WriteLineAsync(new JsonObject { ["id"] = id, ["error"] = new JsonObject { ["code"] = -32000, ["message"] = "Denied" } }.ToJsonString()); break;
            case "approval":
                await Console.Out.WriteLineAsync("{\"id\":\"approval-1\",\"method\":\"approval\",\"params\":{}}");
                await Console.Out.FlushAsync();
                var answer = JsonNode.Parse((await Console.In.ReadLineAsync())!);
                await Console.Out.WriteLineAsync(Response(answer).ToJsonString()); break;
            case "slow":
                await Console.Out.WriteLineAsync("{\"method\":\"started\",\"params\":{}}");
                await Console.Out.FlushAsync();
                await Task.Delay(100);
                await Console.Out.WriteLineAsync(Response(JsonValue.Create("late")).ToJsonString()); break;
            case "exit": return;
            case "partial": await Console.Out.WriteAsync("{\"id\":"); await Console.Out.FlushAsync(); return;
            case "malformed": await Console.Out.WriteLineAsync("{\"id\":1,\"result\":null,\"error\":null}"); break;
            case "oversize": await Console.Out.WriteLineAsync(new string('x', AgentRpcProcess.MaximumFrameCharacters + 1)); break;
            case "diagnostics":
                await Console.Error.WriteAsync(new string('d', 20000));
                await Console.Error.FlushAsync();
                await Console.Out.WriteLineAsync(Response(JsonValue.Create(true)).ToJsonString()); break;
            default: throw new InvalidOperationException("Unexpected test method.");
        }
        await Console.Out.FlushAsync();
    }
    return;
}

using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(30));
var token = deadline.Token;
var assembly = Assembly.GetExecutingAssembly().Location;
AgentRpcProcess Start() => new(Environment.ProcessPath!,
    Path.GetFileNameWithoutExtension(Environment.ProcessPath) == "dotnet" ? [assembly, "peer"] : ["peer"],
    Directory.GetCurrentDirectory());
void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
await using (var peer = Start())
{
    var calls = Enumerable.Range(0, 32).Select(async i =>
    {
        var answer = await peer.RequestAsync("echo", new JsonObject { ["value"] = i, ["text"] = "Unicode: 水; shell: $(echo nope)" }, token);
        Check(answer?["value"]?.GetValue<int>() == i, "Concurrent request correlation");
        Check(answer?["text"]?.GetValue<string>() == "Unicode: 水; shell: $(echo nope)", "Unicode/literal preservation");
    });
    await Task.WhenAll(calls);
    try { await peer.RequestAsync("error", cancellation: token); throw new Exception("Error accepted"); }
    catch (AgentRpcException e) { Check(e.Message == "Denied", "RPC error preserved"); }
    var approval = peer.RequestAsync("approval", cancellation: token);
    var request = await peer.Events.ReadAsync(token);
    await peer.ReplyAsync(request["id"]!, new JsonObject { ["decision"] = "decline" }, token);
    var reply = await approval;
    Check(reply?["id"]?.GetValue<string>() == "approval-1" && reply["result"]?["decision"]?.GetValue<string>() == "decline", "Exact server request ID and decision");
    using var canceled = new CancellationTokenSource();
    var slow = peer.RequestAsync("slow", cancellation: canceled.Token);
    Check((await peer.Events.ReadAsync(token))["method"]?.GetValue<string>() == "started", "Peer received request");
    canceled.Cancel();
    try { await slow; throw new Exception("Cancellation ignored"); }
    catch (AgentExchangeException e) { Check(e.OutcomeUnknown, "Sent cancellation must remain uncertain"); }
    Check((await peer.RequestAsync("echo", new JsonObject { ["value"] = 99 }, token))?["value"]?.GetValue<int>() == 99, "Late response isolation");
    try { await peer.RequestAsync("echo", cancellation: canceled.Token); throw new Exception("Pre-canceled call accepted"); }
    catch (OperationCanceledException) { }
    await peer.RequestAsync("diagnostics", cancellation: token);
    Check(peer.DiagnosticTail.Length <= 8192, "Bounded diagnostic tail");
}
foreach (var failure in new[] { "exit", "partial", "malformed", "oversize" })
{
    await using var peer = Start();
    try { await peer.RequestAsync(failure, cancellation: token); throw new Exception($"Accepted {failure}"); }
    catch (AgentExchangeException e) { Check(e.OutcomeUnknown, $"Lost response remains uncertain: {failure}"); }
    try { await peer.RequestAsync("echo", cancellation: token); throw new Exception("Closed connection accepted call"); }
    catch (AgentExchangeException e) { Check(!e.OutcomeUnknown, "Closed connection does not resend"); }
}
Console.WriteLine("Agent RPC contracts passed: concurrency, Unicode, errors, approval IDs, cancellation, late replies, diagnostics, EOF, malformed and oversized frames.");
