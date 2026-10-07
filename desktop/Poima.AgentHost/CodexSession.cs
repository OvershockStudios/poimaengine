// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Channels;

namespace Poima.AgentHost;

/// <summary>
/// An official Codex app-server connection. The owner consumes Events continuously,
/// renders streamed items and explicitly answers server approval requests.
/// Returned turn IDs acknowledge submission, not completion or engine rollback.
/// </summary>
public sealed class CodexSession : IAsyncDisposable
{
    private readonly AgentRpcProcess rpc;
    private readonly string directory;
    private readonly SemaphoreSlim opening = new(1, 1);
    public string? ThreadId { get; private set; }
    public ChannelReader<JsonObject> Events => rpc.Events;
    public string DiagnosticTail => rpc.DiagnosticTail;

    private CodexSession(AgentRpcProcess rpc, string directory)
    {
        this.rpc = rpc;
        this.directory = directory;
    }

    /// <summary>Attach Poima through its existing endpoint; never open a second world writer.</summary>
    public static async Task<CodexSession> ConnectAsync(string codexExecutable, string poimaExecutable,
        string endpoint, string workingDirectory, CancellationToken cancellation = default, bool allowPoimaEdits = false)
    {
        if (endpoint.Length is < 1 or > 64 || endpoint.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '-' && c != '_'))
            throw new ArgumentException("Invalid Poima endpoint.", nameof(endpoint));
        if (!Path.IsPathFullyQualified(poimaExecutable) || !File.Exists(poimaExecutable))
            throw new ArgumentException("An existing absolute Poima executable path is required.", nameof(poimaExecutable));
        var directory = Path.GetFullPath(workingDirectory);
        // CLI overrides apply only to this process. No provider account files or
        // global configuration are written. Provider authentication stays in Codex.
        var arguments = new List<string>
        {
            "app-server", "-c", "mcp_servers.poima.command=" + JsonSerializer.Serialize(poimaExecutable),
            "-c", "mcp_servers.poima.args=" + JsonSerializer.Serialize(new[] { "mcp", "--endpoint", endpoint })
        };
        if (allowPoimaEdits)
            arguments.AddRange(["-c", "mcp_servers.poima.tools.poima_call.approval_mode=\"approve\""]);
        var rpc = new AgentRpcProcess(codexExecutable, arguments, directory);
        var session = new CodexSession(rpc, directory);
        try
        {
            await rpc.RequestAsync("initialize", new JsonObject
            {
                ["clientInfo"] = new JsonObject { ["name"] = "poima", ["version"] = "0.0.40", ["title"] = "Poima" }
            }, cancellation);
            await rpc.NotifyAsync("initialized", cancellation: cancellation);
            return session;
        }
        catch { await session.DisposeAsync(); throw; }
    }

    /// <summary>Start a persistent thread, or resume its provider-owned history without replaying prompts.</summary>
    public async Task<string> OpenAsync(string? threadId = null, CancellationToken cancellation = default, string? model = null)
    {
        if (threadId is not null) ArgumentException.ThrowIfNullOrWhiteSpace(threadId);
        if (model is not null) ArgumentException.ThrowIfNullOrWhiteSpace(model);
        await opening.WaitAsync(cancellation);
        try
        {
            if (ThreadId is not null) throw new InvalidOperationException("This connection already has an open thread.");
            var parameters = new JsonObject
            {
                ["cwd"] = directory, ["approvalPolicy"] = "on-request", ["sandbox"] = "workspace-write"
            };
            if (model is not null) parameters["model"] = model;
            if (threadId is null) parameters["ephemeral"] = false;
            else { parameters["threadId"] = threadId; parameters["excludeTurns"] = true; }
            var result = await rpc.RequestAsync(threadId is null ? "thread/start" : "thread/resume", parameters, cancellation);
            var returned = result?["thread"]?["id"]?.GetValue<string>();
            if (string.IsNullOrWhiteSpace(returned) || (threadId is not null && returned != threadId))
                throw new AgentExchangeException("Provider returned an invalid thread identity.", true);
            ThreadId = returned;
            return returned;
        }
        finally { opening.Release(); }
    }

    public async Task<string> SendAsync(string text, CancellationToken cancellation = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(text);
        var result = await rpc.RequestAsync("turn/start", new JsonObject
        {
            ["threadId"] = RequireThread(),
            ["input"] = new JsonArray(new JsonObject { ["type"] = "text", ["text"] = text })
        }, cancellation);
        var id = result?["turn"]?["id"]?.GetValue<string>();
        if (string.IsNullOrWhiteSpace(id)) throw new AgentExchangeException("Provider returned no turn identity.", true);
        return id;
    }

    public Task<JsonNode?> InterruptAsync(string turnId, CancellationToken cancellation = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(turnId);
        return rpc.RequestAsync("turn/interrupt", new JsonObject { ["threadId"] = RequireThread(), ["turnId"] = turnId }, cancellation);
    }

    public Task<JsonNode?> ListTurnsAsync(string? cursor = null, CancellationToken cancellation = default) =>
        rpc.RequestAsync("thread/turns/list", new JsonObject { ["threadId"] = RequireThread(), ["cursor"] = cursor, ["limit"] = 25 }, cancellation);

    public Task<JsonNode?> ListMcpServersAsync(CancellationToken cancellation = default) =>
        rpc.RequestAsync("mcpServerStatus/list", new JsonObject { ["threadId"] = RequireThread(), ["serverName"] = "poima" }, cancellation);

    public Task<JsonNode?> ListModelsAsync(CancellationToken cancellation = default) =>
        rpc.RequestAsync("model/list", new JsonObject { ["limit"] = 100 }, cancellation);

    public Task<JsonNode?> ListItemsAsync(string turnId, string? cursor = null, CancellationToken cancellation = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(turnId);
        return rpc.RequestAsync("thread/items/list", new JsonObject { ["threadId"] = RequireThread(), ["turnId"] = turnId,
            ["cursor"] = cursor, ["limit"] = 25 }, cancellation);
    }

    // Provider request types have distinct response shapes. Keep the payload
    // intact rather than converting permission grants or elicitation into a
    // command-approval enum. The frontend owns the explicit user decision.
    public Task ReplyAsync(JsonNode requestId, JsonNode? response, CancellationToken cancellation = default) =>
        rpc.ReplyAsync(requestId, response, cancellation);
    public Task RejectAsync(JsonNode requestId, string reason, CancellationToken cancellation = default) =>
        rpc.RejectAsync(requestId, reason, cancellation);

    private string RequireThread() => ThreadId ?? throw new InvalidOperationException("Open a thread first.");
    public ValueTask DisposeAsync() => rpc.DisposeAsync();
}
