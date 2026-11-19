// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using System.Threading.Channels;

namespace Poima.AgentHost;

/// <summary>A local Antigravity CLI session using ACP v1 and Poima's shared MCP endpoint.</summary>
public sealed class AcpSession : IAgentSession
{
    private readonly AgentRpcProcess rpc;
    private readonly string directory, poimaExecutable, endpoint;
    private int prompting;
    public string? ConversationId { get; private set; }
    public ChannelReader<JsonObject> Events => rpc.Events;
    public void AcknowledgeEvent() => rpc.AcknowledgeEvent();

    private AcpSession(AgentRpcProcess rpc, string directory, string poimaExecutable, string endpoint)
    {
        this.rpc = rpc;
        this.directory = directory;
        this.poimaExecutable = poimaExecutable;
        this.endpoint = endpoint;
    }

    public static async Task<AcpSession> ConnectAsync(string executable, string poimaExecutable, string endpoint,
        string workingDirectory, CancellationToken cancellation = default)
    {
        if (endpoint.Length is < 1 or > 64 || endpoint.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '-' && c != '_'))
            throw new ArgumentException("Invalid Poima endpoint.", nameof(endpoint));
        if (!Path.IsPathFullyQualified(poimaExecutable) || !File.Exists(poimaExecutable))
            throw new ArgumentException("An existing absolute Poima executable path is required.", nameof(poimaExecutable));
        var directory = Path.GetFullPath(workingDirectory);
        var rpc = new AgentRpcProcess(executable, ["acp"], directory, requireJsonRpcVersion: true);
        var session = new AcpSession(rpc, directory, poimaExecutable, endpoint);
        try
        {
            var reply = await rpc.RequestAsync("initialize", new JsonObject
            {
                ["protocolVersion"] = 1, ["clientCapabilities"] = new JsonObject()
            }, cancellation);
            if (reply?["protocolVersion"]?.GetValue<int>() != 1)
                throw new IOException("Antigravity must support ACP version 1. Check that agy acp is available.");
            return session;
        }
        catch { await session.DisposeAsync(); throw; }
    }

    public async Task OpenAsync(CancellationToken cancellation = default)
    {
        if (ConversationId is not null) throw new InvalidOperationException("The ACP session is already open.");
        try
        {
            var reply = await rpc.RequestAsync("session/new", new JsonObject
            {
                ["cwd"] = directory,
                ["mcpServers"] = new JsonArray(new JsonObject
                {
                    ["name"] = "poima", ["command"] = poimaExecutable,
                    ["args"] = new JsonArray("mcp", "--endpoint", endpoint), ["env"] = new JsonArray()
                })
            }, cancellation);
            var id = reply?["sessionId"]?.GetValue<string>();
            if (string.IsNullOrWhiteSpace(id)) throw new IOException("Antigravity returned no ACP session ID.");
            ConversationId = id;
        }
        catch (AgentRpcException error) when (error.Error["code"]?.GetValue<int>() == -32000 &&
            error.Message.Contains("auth", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("Sign in with agy in a terminal, then connect again.", error);
        }
    }

    public async Task<string> PromptAsync(string text, CancellationToken cancellation = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(text);
        var id = RequireSession();
        if (Interlocked.Exchange(ref prompting, 1) != 0)
            throw new InvalidOperationException("Wait for the current Antigravity turn to finish.");
        try
        {
            var reply = await rpc.RequestAsync("session/prompt", new JsonObject
            {
                ["sessionId"] = id, ["prompt"] = new JsonArray(new JsonObject { ["type"] = "text", ["text"] = text })
            }, cancellation);
            await rpc.WaitForEventDeliveryAsync(cancellation);
            return reply?["stopReason"]?.GetValue<string>() ??
                throw new AgentExchangeException("Antigravity returned no prompt completion reason.", true);
        }
        finally { Volatile.Write(ref prompting, 0); }
    }

    public Task CancelAsync(CancellationToken cancellation = default) => rpc.NotifyAsync("session/cancel",
        new JsonObject { ["sessionId"] = RequireSession() }, cancellation);
    public Task ReplyAsync(JsonNode id, JsonNode? response, CancellationToken cancellation = default) =>
        rpc.ReplyAsync(id, response, cancellation);
    public Task RejectAsync(JsonNode id, string reason, CancellationToken cancellation = default) =>
        rpc.RejectAsync(id, reason, cancellation);
    private string RequireSession() => ConversationId ?? throw new InvalidOperationException("Open an ACP session first.");
    public ValueTask DisposeAsync() => rpc.DisposeAsync();
}
