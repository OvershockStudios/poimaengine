// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using System.Threading.Channels;

namespace Poima.AgentHost;

/// <summary>A local ACP v1 session with Poima's shared MCP endpoint.</summary>
public sealed class AcpSession : IAgentSession
{
    private readonly AgentRpcProcess rpc;
    private readonly string directory, poimaExecutable, endpoint;
    private int prompting;
    public string? ConversationId { get; private set; }
    public bool SupportsImages { get; private set; }
    public ChannelReader<JsonObject> Events => rpc.Events;
    public void AcknowledgeEvent() => rpc.AcknowledgeEvent();

    private AcpSession(AgentRpcProcess rpc, string directory, string poimaExecutable, string endpoint)
    {
        this.rpc = rpc;
        this.directory = directory;
        this.poimaExecutable = poimaExecutable;
        this.endpoint = endpoint;
    }

    public static Task<AcpSession> ConnectAsync(string executable, string poimaExecutable, string endpoint,
        string workingDirectory, CancellationToken cancellation = default) =>
        ConnectAsync(executable, ["acp"], poimaExecutable, endpoint, workingDirectory, cancellation);

    public static async Task<AcpSession> ConnectAsync(string executable, IReadOnlyList<string> arguments,
        string poimaExecutable, string endpoint, string workingDirectory, CancellationToken cancellation = default)
    {
        if (endpoint.Length is < 1 or > 64 || endpoint.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '-' && c != '_'))
            throw new ArgumentException("Invalid Poima endpoint.", nameof(endpoint));
        if (!Path.IsPathFullyQualified(poimaExecutable) || !File.Exists(poimaExecutable))
            throw new ArgumentException("An existing absolute Poima executable path is required.", nameof(poimaExecutable));
        var directory = Path.GetFullPath(workingDirectory);
        var rpc = new AgentRpcProcess(executable, arguments, directory, requireJsonRpcVersion: true);
        var session = new AcpSession(rpc, directory, poimaExecutable, endpoint);
        try
        {
            var reply = await rpc.RequestAsync("initialize", new JsonObject
            {
                ["protocolVersion"] = 1, ["clientCapabilities"] = new JsonObject()
            }, cancellation);
            if (reply?["protocolVersion"]?.GetValue<int>() != 1)
                throw new IOException("The agent must support ACP version 1.");
            session.SupportsImages = reply?["agentCapabilities"]?["promptCapabilities"]?["image"] is JsonValue image &&
                image.TryGetValue<bool>(out var supported) && supported;
            return session;
        }
        catch { await session.DisposeAsync(); throw; }
    }

    public async Task OpenAsync(CancellationToken cancellation = default)
    {
        if (ConversationId is not null) throw new InvalidOperationException("The ACP session is already open.");
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
        if (string.IsNullOrWhiteSpace(id)) throw new IOException("The agent returned no ACP session ID.");
        ConversationId = id;
    }

    public Task<string> PromptAsync(string text, CancellationToken cancellation = default) => PromptAsync(text, [], cancellation);

    public async Task<string> PromptAsync(string text, IReadOnlyList<AgentImage> images, CancellationToken cancellation)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(text);
        AgentImage.CheckCount(images);
        if (images.Count > 0 && !SupportsImages) throw new InvalidOperationException("This agent connection accepts text only.");
        var id = RequireSession();
        if (Interlocked.Exchange(ref prompting, 1) != 0)
            throw new InvalidOperationException("Wait for the current agent turn to finish.");
        try
        {
            var prompt = new JsonArray(new JsonObject { ["type"] = "text", ["text"] = text });
            foreach (var image in images) prompt.Add(image.AcpInput());
            var reply = await rpc.RequestAsync("session/prompt", new JsonObject
            {
                ["sessionId"] = id, ["prompt"] = prompt
            }, cancellation);
            await rpc.WaitForEventDeliveryAsync(cancellation);
            return reply?["stopReason"]?.GetValue<string>() ??
                throw new AgentExchangeException("The agent returned no prompt completion reason.", true);
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
