// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using System.Threading.Channels;

namespace Poima.AgentHost;

public interface IAgentSession : IAsyncDisposable
{
    string? ConversationId { get; }
    ChannelReader<JsonObject> Events { get; }
    Task ReplyAsync(JsonNode id, JsonNode? response, CancellationToken cancellation = default);
    Task RejectAsync(JsonNode id, string reason, CancellationToken cancellation = default);
}
