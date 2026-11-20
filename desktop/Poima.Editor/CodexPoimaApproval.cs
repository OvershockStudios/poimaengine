// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Poima.Editor;

internal sealed record CodexPoimaApproval(string Title, string Details)
{
    private const string LegacyPrompt = "Allow the poima MCP server to run tool \"";
    private const int MaximumReviewCharacters = 1024 * 1024;
    private static readonly JsonSerializerOptions ReadableJson = new() { WriteIndented = true };

    public static CodexPoimaApproval? Read(JsonObject? request, string? thread, string? turn)
    {
        if (thread is null || turn is null || Text(request, "threadId") != thread || Text(request, "turnId") != turn ||
            Text(request, "serverName") != "poima" || Text(request, "mode") != "form" ||
            request?["requestedSchema"] is not JsonObject schema || Text(schema, "type") != "object" ||
            schema["properties"] is not JsonObject { Count: 0 } ||
            (schema.ContainsKey("required") && schema["required"] is not JsonArray { Count: 0 }) ||
            schema.Any(field => field.Key is not ("type" or "properties" or "required" or "additionalProperties")) ||
            (schema.ContainsKey("additionalProperties") &&
                (schema["additionalProperties"] is not JsonValue extra || !extra.TryGetValue<bool>(out _))) ||
            request["_meta"] is not JsonObject meta || Text(meta, "codex_approval_kind") != "mcp_tool_call" ||
            meta["tool_params"] is not JsonObject arguments) return null;

        var tool = Text(meta, "tool_name");
        // Older app-servers omit the name from metadata and use this exact
        // provider-generated prompt. Unknown forms never become approvals.
        if (tool is null && !meta.ContainsKey("tool_name") && Text(request, "message") is { Length: <= 160 } message &&
            message.Length >= LegacyPrompt.Length + 2 && message.StartsWith(LegacyPrompt, StringComparison.Ordinal) &&
            message.EndsWith("\"?", StringComparison.Ordinal))
            tool = message[LegacyPrompt.Length..^2];
        var title = tool switch
        {
            "poima_discover" => "Codex wants to discover Poima capabilities",
            "poima_call" => "Codex wants to use a project operation",
            "poima_source" => "Codex wants to change gameplay source",
            "poima_checks" => "Codex wants to change gameplay checks",
            "poima_build" => "Codex wants to build, verify, or adopt gameplay",
            _ => null
        };
        if (title is null) return null;
        var detail = arguments.ToJsonString(ReadableJson);
        return detail.Length <= MaximumReviewCharacters ? new(title, tool + "\n" + detail) : null;
    }

    private static string? Text(JsonObject? value, string key) =>
        value?[key] is JsonValue scalar && scalar.TryGetValue<string>(out var text) ? text : null;
}
