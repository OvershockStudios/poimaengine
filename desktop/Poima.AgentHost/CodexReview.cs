// SPDX-License-Identifier: Apache-2.0
using System.Text;
using System.Text.Json.Nodes;

namespace Poima.AgentHost;

/// <summary>One specialist conversation backed by a scoped Poima connection.
/// The caller serializes its turns and disposes it after cancelling a pending turn.</summary>
public sealed class CodexReview : IAsyncDisposable
{
    public const int MaximumFindingLength = 16000;
    private readonly CodexSession session;

    private CodexReview(CodexSession session) => this.session = session;

    public static Task<CodexReview> OpenAsync(string codexExecutable, string poimaExecutable,
        string endpoint, string directory, long revision, string? model, CancellationToken cancellation) =>
        OpenSpecialistAsync(CodexSession.ConnectReviewAsync(codexExecutable, poimaExecutable, endpoint, directory,
            revision, cancellation), "poima_inspect", model, cancellation);

    public static Task<CodexReview> OpenDraftAsync(string codexExecutable, string poimaExecutable,
        string draftManifest, string? model, CancellationToken cancellation) =>
        OpenSpecialistAsync(CodexSession.ConnectDraftAsync(codexExecutable, poimaExecutable, draftManifest, cancellation),
            "poima_call", model, cancellation);

    private static async Task<CodexReview> OpenSpecialistAsync(Task<CodexSession> connection, string tool,
        string? model, CancellationToken cancellation)
    {
        var session = await connection;
        try
        {
            await session.OpenAsync(cancellation: cancellation, model: model);
            var servers = await session.ListMcpServersAsync(cancellation);
            if (servers?["data"] is not JsonArray data || !data.Any(node =>
                node?["name"]?.GetValue<string>() == "poima" && node["toolsError"] is null &&
                node["tools"] is JsonObject tools && tools.Count == 3 && tools.ContainsKey(tool) &&
                tools.ContainsKey("poima_reference") && tools.ContainsKey("poima_discover") &&
                (tool != "poima_call" || HasDraftMethods(tools[tool]))))
                throw new InvalidOperationException(tool == "poima_call"
                    ? "The specialist could not connect to Poima's scoped draft tools."
                    : "The specialist could not connect to Poima's scoped review tools.");
            return new CodexReview(session);
        }
        catch { await session.DisposeAsync(); throw; }
    }

    private static bool HasDraftMethods(JsonNode? tool) =>
        tool?["inputSchema"]?["properties"]?["method"]?["enum"] is JsonArray methods &&
        methods.Any(method => method?.GetValue<string>() == "world.inspect") &&
        methods.Any(method => method?.GetValue<string>() == "world.transact");

    public async Task<string> AskAsync(string text, Action<string> showDraft, CancellationToken cancellation)
    {
        var turn = await session.SendAsync(text, cancellation);
        var draft = new StringBuilder();
        string? answer = null;
        await foreach (var message in session.Events.ReadAllAsync(cancellation))
        {
            var method = message["method"]?.GetValue<string>();
            if (message["id"] is JsonNode request)
            {
                if (method == "mcpServer/elicitation/request")
                    await session.ReplyAsync(request, new JsonObject { ["action"] = "cancel" }, cancellation);
                else
                    await session.RejectAsync(request, "A specialist session has no interactive approvals.", cancellation);
                continue;
            }
            var parameters = message["params"];
            if (parameters?["threadId"]?.GetValue<string>() != session.ThreadId) continue;
            if (method == "turn/completed" && parameters?["turn"]?["id"]?.GetValue<string>() == turn)
            {
                var status = parameters["turn"]?["status"]?.GetValue<string>();
                if (status != "completed") throw new InvalidOperationException("The specialist turn ended " + (status ?? "without a status") + ".");
                if (string.IsNullOrWhiteSpace(answer)) throw new InvalidOperationException("The specialist finished without findings.");
                return answer;
            }
            if (parameters?["turnId"]?.GetValue<string>() != turn) continue;
            if (method == "item/agentMessage/delta" && parameters["delta"]?.GetValue<string>() is { } delta)
            {
                draft.Append(delta.AsSpan(0, Math.Min(delta.Length, MaximumFindingLength - draft.Length)));
                showDraft(draft.ToString());
            }
            else if (method == "item/completed" && parameters["item"] is JsonObject item &&
                item["type"]?.GetValue<string>() == "agentMessage" && item["text"]?.GetValue<string>() is { } completed)
            {
                if (completed.Length > MaximumFindingLength)
                    throw new InvalidDataException("The specialist finding exceeds the display limit. Ask for a shorter review.");
                answer = completed;
            }
        }
        throw new IOException("The specialist connection ended before the turn finished.");
    }

    public ValueTask DisposeAsync() => session.DisposeAsync();
}
