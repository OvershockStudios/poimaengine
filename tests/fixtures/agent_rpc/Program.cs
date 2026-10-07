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
            case "stall":
                await Console.Out.WriteLineAsync(Response(JsonValue.Create(Environment.ProcessId)).ToJsonString());
                await Console.Out.FlushAsync();
                await Task.Delay(Timeout.Infinite); return;
            case "flood":
                for (var i = 0; i < 65; i++) await Console.Out.WriteLineAsync("{\"method\":\"event\",\"params\":{}}");
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
foreach (var failure in new[] { "exit", "partial", "malformed", "oversize", "flood" })
{
    await using var peer = Start();
    try { await peer.RequestAsync(failure, cancellation: token); throw new Exception($"Accepted {failure}"); }
    catch (AgentExchangeException e) { Check(e.OutcomeUnknown, $"Lost response remains uncertain: {failure}"); }
    try { await peer.RequestAsync("echo", cancellation: token); throw new Exception("Closed connection accepted call"); }
    catch (AgentExchangeException e) { Check(!e.OutcomeUnknown, "Closed connection does not resend"); }
}
foreach (var disposeDuringWrite in new[] { false, true })
{
    await using var peer = Start();
    var pid = (await peer.RequestAsync("stall", cancellation: token))!.GetValue<int>();
    using var writeCancellation = CancellationTokenSource.CreateLinkedTokenSource(token);
    var blocked = peer.RequestAsync("echo", new JsonObject { ["text"] = new string('x', 4 * 1024 * 1024) }, writeCancellation.Token);
    // The peer has stopped reading. Four MiB cannot fit in a pipe; let the
    // asynchronous write reach the blocked OS operation before interrupting it.
    await Task.Delay(100, token);
    if (disposeDuringWrite) await Task.WhenAll(peer.DisposeAsync().AsTask(), peer.DisposeAsync().AsTask());
    else writeCancellation.Cancel();
    try { await blocked; throw new Exception("Blocked write accepted"); }
    catch (AgentExchangeException e) { Check(e.OutcomeUnknown, "Partial write stays uncertain"); }
    try { await peer.RequestAsync("echo", cancellation: token); throw new Exception("Partial frame connection reused"); }
    catch (AgentExchangeException e) { Check(!e.OutcomeUnknown, "Partial frame closes connection"); }
    await peer.DisposeAsync();
    try
    {
        using var child = System.Diagnostics.Process.GetProcessById(pid);
        Check(child.HasExited, "Disposed child still running");
    }
    catch (ArgumentException) { } // The OS has already removed the exited child.
}
Console.WriteLine("Agent RPC contracts passed: concurrency, Unicode, errors, approval IDs, cancellation, late replies, diagnostics, EOF, malformed/oversized frames, event overflow, partial-write shutdown and concurrent disposal.");
