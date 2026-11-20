// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;

namespace Poima.AgentHost;

internal static class CodexDraftTarget
{
    private const int MaximumMetadataBytes = 65536;
    private const long MaximumRevision = 9007199254740991L;

    public static string Validate(string manifestPath)
    {
        if (!Path.IsPathFullyQualified(manifestPath))
            throw new ArgumentException("An absolute draft manifest path is required.", nameof(manifestPath));
        var path = Path.GetFullPath(manifestPath);
        var root = Path.GetDirectoryName(path) ?? throw new ArgumentException("A draft needs its own project directory.", nameof(manifestPath));
        for (DirectoryInfo? directory = new(root); directory is not null; directory = directory.Parent)
            if (!directory.Exists || directory.Attributes.HasFlag(FileAttributes.ReparsePoint))
                throw new InvalidDataException("The draft directory and its parents must be ordinary directories.");
        if (Path.Exists(Path.Combine(root, ".poima-incomplete")))
            throw new InvalidDataException("The draft was not fully published.");

        using var marker = ReadMetadata(Path.Combine(root, ".poima-draft.json"));
        using var manifest = ReadMetadata(path);
        var draft = marker.RootElement;
        var project = manifest.RootElement;
        var draftId = StringValue(draft, "project_id");
        var baseId = StringValue(draft, "base_project_id");
        if (StringValue(draft, "format") != "poima.project-draft" || IntegerValue(draft, "version") != 1 ||
            StringValue(draft, "project") != Path.GetFileName(path) || !LowerHex(draftId, 32) ||
            !LowerHex(baseId, 32) || draftId == baseId || !LowerHex(StringValue(draft, "base_sha256"), 64) ||
            IntegerValue(draft, "base_revision") is not (>= 0 and <= MaximumRevision) ||
            StringValue(project, "format") != "poima.project" || IntegerValue(project, "version") is not (1 or 2) ||
            StringValue(project, "project_id") != draftId)
            throw new InvalidDataException("The project manifest does not match its draft metadata.");
        return path;
    }

    private static JsonDocument ReadMetadata(string path)
    {
        var info = new FileInfo(path);
        if (!info.Exists || info.Attributes.HasFlag(FileAttributes.ReparsePoint))
            throw new InvalidDataException("Expected a regular draft metadata file.");
        if (info.Length is < 1 or > MaximumMetadataBytes)
            throw new InvalidDataException("Draft metadata exceeds its file size limit.");
        using var stream = info.Open(FileMode.Open, FileAccess.Read, FileShare.Read);
        var length = stream.Length;
        if (length is < 1 or > MaximumMetadataBytes)
            throw new InvalidDataException("Draft metadata exceeds its file size limit.");
        var content = new byte[(int)length];
        stream.ReadExactly(content);
        if (stream.ReadByte() != -1) throw new InvalidDataException("Draft metadata changed during the read.");
        try
        {
            return JsonDocument.Parse(content, new JsonDocumentOptions { MaxDepth = 16, AllowDuplicateProperties = false });
        }
        catch (JsonException error)
        {
            throw new InvalidDataException("Draft metadata is not valid JSON.", error);
        }
    }

    private static string? StringValue(JsonElement owner, string name) =>
        owner.ValueKind == JsonValueKind.Object && owner.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    private static long? IntegerValue(JsonElement owner, string name) =>
        owner.ValueKind == JsonValueKind.Object && owner.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out var number) ? number : null;

    private static bool LowerHex(string? text, int length) =>
        text is not null && text.Length == length && text.All(c => c is (>= '0' and <= '9') or (>= 'a' and <= 'f'));
}
