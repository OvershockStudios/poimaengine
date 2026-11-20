// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Channels;
using System.Globalization;

namespace Poima.AgentHost;

public sealed record CodexBrowserLogin(string Id, Uri AuthorizationPage);

/// <summary>
/// An official Codex app-server connection. The owner consumes Events continuously,
/// renders streamed items and explicitly answers server approval requests.
/// Returned turn IDs acknowledge submission, not completion or engine rollback.
/// </summary>
public sealed class CodexSession : IAgentSession
{
    private readonly AgentRpcProcess rpc;
    private readonly string directory;
    private readonly bool reviewOnly;
    private readonly SemaphoreSlim opening = new(1, 1);
    public string? ThreadId { get; private set; }
    string? IAgentSession.ConversationId => ThreadId;
    public ChannelReader<JsonObject> Events => rpc.Events;
    public string DiagnosticTail => rpc.DiagnosticTail;

    private CodexSession(AgentRpcProcess rpc, string directory, bool reviewOnly)
    {
        this.rpc = rpc;
        this.directory = directory;
        this.reviewOnly = reviewOnly;
    }

    /// <summary>Attach Poima through its existing endpoint; never open a second world writer.</summary>
    public static Task<CodexSession> ConnectAsync(string codexExecutable, string poimaExecutable,
        string endpoint, string workingDirectory, CancellationToken cancellation = default, bool allowPoimaEdits = false,
        bool allowPoimaSourceEdits = false, bool allowPoimaBuilds = false, bool allowPoimaChecks = false,
        bool receiveQuestions = false) => ConnectCoreAsync(codexExecutable, poimaExecutable, endpoint, workingDirectory,
            null, cancellation, allowPoimaEdits, allowPoimaSourceEdits, allowPoimaBuilds, allowPoimaChecks, receiveQuestions);

    internal static Task<CodexSession> ConnectReviewAsync(string codexExecutable, string poimaExecutable,
        string endpoint, string workingDirectory, long revision, CancellationToken cancellation)
    {
        if (revision is < 0 or > 9007199254740991L) throw new ArgumentOutOfRangeException(nameof(revision));
        return ConnectCoreAsync(codexExecutable, poimaExecutable, endpoint, workingDirectory, revision, cancellation,
            false, false, false, false, false);
    }

    private static async Task<CodexSession> ConnectCoreAsync(string codexExecutable, string poimaExecutable,
        string endpoint, string workingDirectory, long? reviewRevision, CancellationToken cancellation,
        bool allowPoimaEdits, bool allowPoimaSourceEdits, bool allowPoimaBuilds, bool allowPoimaChecks, bool receiveQuestions)
    {
        if (endpoint.Length is < 1 or > 64 || endpoint.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '-' && c != '_'))
            throw new ArgumentException("Invalid Poima endpoint.", nameof(endpoint));
        if (!Path.IsPathFullyQualified(poimaExecutable) || !File.Exists(poimaExecutable))
            throw new ArgumentException("An existing absolute Poima executable path is required.", nameof(poimaExecutable));
        var directory = Path.GetFullPath(workingDirectory);
        var engineArguments = new List<string> { "mcp", "--endpoint", endpoint };
        if (reviewRevision is { } revision)
            engineArguments.AddRange(["--review-revision", revision.ToString(CultureInfo.InvariantCulture)]);
        // CLI overrides apply only to this process. No provider account files or
        // global configuration are written. Provider authentication stays in Codex.
        var arguments = new List<string>
        {
            "app-server", "-c", "mcp_servers.poima.command=" + JsonSerializer.Serialize(poimaExecutable),
            "-c", "mcp_servers.poima.args=" + JsonSerializer.Serialize(engineArguments)
        };
        if (receiveQuestions)
            arguments.AddRange(["-c", "features.default_mode_request_user_input=true"]);
        if (allowPoimaEdits)
            arguments.AddRange(["-c", "mcp_servers.poima.tools.poima_call.approval_mode=\"approve\""]);
        if (allowPoimaSourceEdits)
            arguments.AddRange(["-c", "mcp_servers.poima.tools.poima_source.approval_mode=\"approve\""]);
        if (allowPoimaBuilds)
            arguments.AddRange(["-c", "mcp_servers.poima.tools.poima_build.approval_mode=\"approve\""]);
        if (allowPoimaChecks)
            arguments.AddRange(["-c", "mcp_servers.poima.tools.poima_checks.approval_mode=\"approve\""]);
        var rpc = new AgentRpcProcess(codexExecutable, arguments, directory);
        var session = new CodexSession(rpc, directory, reviewRevision.HasValue);
        try
        {
            var initialization = new JsonObject
            {
                ["clientInfo"] = new JsonObject { ["name"] = "poima", ["version"] = "0.0.40", ["title"] = "Poima" }
            };
            if (receiveQuestions) initialization["capabilities"] = new JsonObject { ["experimentalApi"] = true };
            await rpc.RequestAsync("initialize", initialization, cancellation);
            await rpc.NotifyAsync("initialized", cancellation: cancellation);
            return session;
        }
        catch { await session.DisposeAsync(); throw; }
    }

    /// <summary>Start a persistent thread, or resume its provider-owned history without replaying prompts.</summary>
    public async Task<string> OpenAsync(string? threadId = null, CancellationToken cancellation = default, string? model = null)
    {
        if (threadId is not null) ArgumentException.ThrowIfNullOrWhiteSpace(threadId);
        if (reviewOnly && threadId is not null) throw new ArgumentException("Reviews open a new scoped conversation.", nameof(threadId));
        if (model is not null) ArgumentException.ThrowIfNullOrWhiteSpace(model);
        await opening.WaitAsync(cancellation);
        try
        {
            if (ThreadId is not null) throw new InvalidOperationException("This connection already has an open thread.");
            var parameters = new JsonObject
            {
                ["cwd"] = directory, ["approvalPolicy"] = reviewOnly ? "never" : "on-request",
                ["sandbox"] = reviewOnly ? "read-only" : "workspace-write"
            };
            if (model is not null) parameters["model"] = model;
            if (threadId is null) parameters["ephemeral"] = reviewOnly;
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

    public Task<string> SendAsync(string text, CancellationToken cancellation = default) => SendAsync(text, [], cancellation);

    public async Task<string> SendAsync(string text, IReadOnlyList<AgentImage> images, CancellationToken cancellation)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(text);
        AgentImage.CheckCount(images);
        var input = new JsonArray(new JsonObject { ["type"] = "text", ["text"] = text });
        foreach (var image in images) input.Add(image.CodexInput());
        var result = await rpc.RequestAsync("turn/start", new JsonObject
        {
            ["threadId"] = RequireThread(),
            ["input"] = input
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
        ListTurnsAsync(cursor, cancellation, 25);

    public Task<JsonNode?> ListTurnsAsync(string? cursor, CancellationToken cancellation, int limit) =>
        rpc.RequestAsync("thread/turns/list", new JsonObject { ["threadId"] = RequireThread(), ["cursor"] = cursor, ["limit"] = limit }, cancellation);

    public Task<JsonNode?> ReadThreadAsync(string threadId, CancellationToken cancellation = default) =>
        rpc.RequestAsync("thread/read", new JsonObject { ["threadId"] = threadId, ["includeTurns"] = false }, cancellation);

    public Task<JsonNode?> ListMcpServersAsync(CancellationToken cancellation = default) =>
        rpc.RequestAsync("mcpServerStatus/list", new JsonObject { ["threadId"] = RequireThread(), ["serverName"] = "poima" }, cancellation);

    public Task<JsonNode?> ListModelsAsync(CancellationToken cancellation = default) =>
        rpc.RequestAsync("model/list", new JsonObject { ["limit"] = 100 }, cancellation);

    public async Task<bool> RequiresSignInAsync(CancellationToken cancellation = default)
    {
        var result = await rpc.RequestAsync("account/read", new JsonObject { ["refreshToken"] = false }, cancellation);
        if (result?["requiresOpenaiAuth"] is not JsonValue required || !required.TryGetValue<bool>(out var requiresAuth))
            throw new IOException("Codex returned no account status.");
        return requiresAuth && result["account"] is null;
    }

    public async Task<CodexBrowserLogin> StartBrowserLoginAsync(CancellationToken cancellation = default)
    {
        var result = await rpc.RequestAsync("account/login/start", new JsonObject { ["type"] = "chatgpt" }, cancellation);
        var id = result?["loginId"]?.GetValue<string>();
        if (result?["type"]?.GetValue<string>() != "chatgpt" || string.IsNullOrWhiteSpace(id) || id.Length > 128 ||
            !Uri.TryCreate(result["authUrl"]?.GetValue<string>(), UriKind.Absolute, out var page) ||
            page.Scheme != Uri.UriSchemeHttps || !page.IsDefaultPort || page.UserInfo.Length != 0 ||
            page.IdnHost is not ("auth.openai.com" or "chatgpt.com"))
            throw new InvalidDataException("Codex returned an invalid sign-in page or login ID.");
        return new(id, page);
    }

    public Task<JsonNode?> CancelBrowserLoginAsync(string loginId, CancellationToken cancellation = default) =>
        rpc.RequestAsync("account/login/cancel", new JsonObject { ["loginId"] = loginId }, cancellation);

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
