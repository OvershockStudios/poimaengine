// SPDX-License-Identifier: Apache-2.0
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Poima.Editor;

public abstract record EditorLayoutNode(double Proportion);
public sealed record EditorTabLayout(double Proportion, IReadOnlyList<string> Panels, string Active) : EditorLayoutNode(Proportion);
public sealed record EditorSplitLayout(double Proportion, string Orientation, IReadOnlyList<EditorLayoutNode> Children) : EditorLayoutNode(Proportion);
// Dock's native host uses physical pixels for position and DIPs for size.
public sealed record EditorFloatBounds(double X, double Y, double Width, double Height);
public sealed record EditorFloatingLayout(EditorLayoutNode Content, EditorFloatBounds Bounds);
public sealed record EditorLayoutDocument(EditorLayoutNode? Main, IReadOnlyList<EditorFloatingLayout> Floating);

/// <summary>Bounded, explicitly typed preferences; never deserializes Dock objects or writes world storage.</summary>
public sealed class EditorLayoutStore
{
    public static readonly IReadOnlyList<string> PanelNames = Array.AsReadOnly(new[] { "Hierarchy", "Scene", "Inspector", "Project", "Console" });
    private const int Limit = 65536;
    public string FilePath { get; }

    public EditorLayoutStore(string projectPath, string? filePath = null)
    {
        var project = Path.GetFullPath(projectPath).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        if (OperatingSystem.IsWindows()) project = project.ToUpperInvariant();
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(local)) throw new IOException("User-local preferences directory is unavailable.");
        var key = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(project))).ToLowerInvariant();
        FilePath = filePath is null ? Path.Combine(local, "Poima", "Editor", "Layouts", key+".json") : Path.GetFullPath(filePath);
    }

    public bool TryLoad(out EditorLayoutDocument? document, out string? diagnostic)
    {
        document = null; diagnostic = null;
        try
        {
            if (!File.Exists(FilePath))
            {
                if (Directory.Exists(FilePath)) throw new IOException("Layout preference path is a directory.");
                return false;
            }
            document = Decode(ReadBytes()); return true;
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or JsonException or ArgumentException)
        { diagnostic = "Saved layout was not restored; its file was preserved: "+error.Message; return false; }
    }

    public bool TrySave(EditorLayoutDocument document, out string? diagnostic)
    {
        diagnostic = null; string? temporary = null;
        try
        {
            Validate(document);
            var bytes = Encoding.UTF8.GetBytes(Encode(document).ToJsonString(new JsonSerializerOptions { WriteIndented = true })+"\n");
            if (bytes.Length > Limit) throw new InvalidDataException("Layout preference exceeds 64 KiB.");
            byte[]? previous = null;
            if (File.Exists(FilePath))
            {
                previous = ReadBytes();
                // Never overwrite an unreadable, corrupt, or future-version file,
                // even after the application fell back to its default layout.
                _ = Decode(previous);
            }
            else if (Directory.Exists(FilePath)) throw new IOException("Layout preference path is a directory.");
            var directory = Path.GetDirectoryName(FilePath)!;
            Directory.CreateDirectory(directory);
            temporary = Path.Combine(directory, ".layout-"+Guid.NewGuid().ToString("N")+".tmp");
            using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            { stream.Write(bytes); stream.Flush(true); }
            if (previous is null)
                File.Move(temporary, FilePath, false);
            else
            {
                if (!ReadBytes().AsSpan().SequenceEqual(previous)) throw new IOException("Layout preferences changed while saving.");
                File.Replace(temporary, FilePath, null);
            }
            temporary = null; return true;
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or JsonException or ArgumentException)
        { diagnostic = "Layout preferences were not saved; existing preferences were preserved: "+error.Message; return false; }
        finally
        {
            if (temporary is not null)
                try { File.Delete(temporary); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }

    private byte[] ReadBytes()
    {
        if ((File.GetAttributes(FilePath) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("Layout preference file cannot be a link.");
        using var stream = new FileStream(FilePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (stream.Length > Limit) throw new InvalidDataException("Layout preference exceeds 64 KiB.");
        var bytes = new byte[checked((int)stream.Length)]; stream.ReadExactly(bytes); return bytes;
    }

    private static void Fields(JsonElement value, params string[] fields)
    {
        if (value.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Layout object expected.");
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var field in value.EnumerateObject())
            if (!fields.Contains(field.Name, StringComparer.Ordinal) || !seen.Add(field.Name))
                throw new InvalidDataException("Unknown or duplicate layout field: "+field.Name);
        if (seen.Count != fields.Length) throw new InvalidDataException("Required layout field is missing.");
    }
    private static string Text(JsonElement value) => value.ValueKind == JsonValueKind.String
        ? value.GetString()! : throw new InvalidDataException("Layout text expected.");
    private static double Number(JsonElement value) => value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out var number) && double.IsFinite(number)
        ? number : throw new InvalidDataException("Finite layout number expected.");

    private static EditorLayoutDocument Decode(byte[] bytes)
    {
        using var json = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 40 });
        var root = json.RootElement; Fields(root, "format", "version", "main", "floating");
        if (Text(root.GetProperty("format")) != "poima.editor-layout"
            || root.GetProperty("version").ValueKind != JsonValueKind.Number
            || !root.GetProperty("version").TryGetInt32(out var version) || version != 1)
            throw new InvalidDataException("Unsupported layout preference format/version.");
        var floating = root.GetProperty("floating");
        if (floating.ValueKind != JsonValueKind.Array || floating.GetArrayLength() > 5)
            throw new InvalidDataException("Layout permits at most five floating windows.");
        var windows = new List<EditorFloatingLayout>();
        foreach (var window in floating.EnumerateArray())
        {
            Fields(window, "content", "bounds"); var bounds = window.GetProperty("bounds");
            Fields(bounds, "x", "y", "width", "height");
            windows.Add(new(ReadNode(window.GetProperty("content"), 0), new(Number(bounds.GetProperty("x")),
                Number(bounds.GetProperty("y")), Number(bounds.GetProperty("width")), Number(bounds.GetProperty("height")))));
        }
        var main = root.GetProperty("main");
        var document = new EditorLayoutDocument(main.ValueKind == JsonValueKind.Null ? null : ReadNode(main, 0), windows);
        Validate(document); return document;
    }
    private static EditorLayoutNode ReadNode(JsonElement node, int depth)
    {
        if (depth > 16 || node.ValueKind != JsonValueKind.Object || !node.TryGetProperty("kind", out var kind))
            throw new InvalidDataException("Invalid or overly nested layout node.");
        if (Text(kind) == "tabs")
        {
            Fields(node, "kind", "proportion", "panels", "active"); var panels = node.GetProperty("panels");
            if (panels.ValueKind != JsonValueKind.Array || panels.GetArrayLength() is < 1 or > 5)
                throw new InvalidDataException("Tab group needs one to five panels.");
            return new EditorTabLayout(Number(node.GetProperty("proportion")), panels.EnumerateArray().Select(Text).ToArray(), Text(node.GetProperty("active")));
        }
        if (Text(kind) == "split")
        {
            Fields(node, "kind", "proportion", "orientation", "children"); var children = node.GetProperty("children");
            if (children.ValueKind != JsonValueKind.Array || children.GetArrayLength() is < 2 or > 5)
                throw new InvalidDataException("Split group needs two to five children.");
            return new EditorSplitLayout(Number(node.GetProperty("proportion")), Text(node.GetProperty("orientation")),
                children.EnumerateArray().Select(child => ReadNode(child, depth+1)).ToArray());
        }
        throw new InvalidDataException("Unsupported layout node kind.");
    }

    public static void Validate(EditorLayoutDocument document)
    {
        var panels = new HashSet<string>(StringComparer.Ordinal); var count = 0;
        void Visit(EditorLayoutNode node, int depth)
        {
            if (++count > 32 || depth > 16 || !double.IsFinite(node.Proportion) || node.Proportion <= 0 || node.Proportion > 1e6)
                throw new InvalidDataException("Layout tree size or proportions are invalid.");
            switch (node)
            {
                case EditorTabLayout tabs:
                    if (tabs.Panels.Count is < 1 or > 5 || !tabs.Panels.Contains(tabs.Active, StringComparer.Ordinal))
                        throw new InvalidDataException("Active tab must belong to its nonempty group.");
                    foreach (var panel in tabs.Panels)
                        if (!PanelNames.Contains(panel, StringComparer.Ordinal) || !panels.Add(panel))
                            throw new InvalidDataException("Unknown or duplicate panel: "+panel);
                    break;
                case EditorSplitLayout split:
                    if (split.Orientation is not ("horizontal" or "vertical") || split.Children.Count is < 2 or > 5)
                        throw new InvalidDataException("Invalid split orientation or child count.");
                    foreach (var child in split.Children) Visit(child, depth+1);
                    break;
                default: throw new InvalidDataException("Unsupported layout node.");
            }
        }
        if (document.Main is not null) Visit(document.Main, 0);
        if (document.Floating.Count > 5) throw new InvalidDataException("Too many floating windows.");
        foreach (var window in document.Floating) { ValidateBounds(window.Bounds); Visit(window.Content, 0); }
        if (panels.Count != PanelNames.Count) throw new InvalidDataException("Layout must contain each of the five editor panels exactly once.");
    }
    public static void ValidateBounds(EditorFloatBounds value)
    {
        if (!double.IsFinite(value.X) || !double.IsFinite(value.Y) || Math.Abs(value.X) > 1e7 || Math.Abs(value.Y) > 1e7
            || !double.IsFinite(value.Width) || !double.IsFinite(value.Height) || value.Width is < 64 or > 16384 || value.Height is < 64 or > 16384)
            throw new InvalidDataException("Floating window bounds are invalid.");
    }
    public static JsonObject Encode(EditorLayoutDocument document)
    {
        JsonObject Node(EditorLayoutNode node) => node switch
        {
            EditorTabLayout tabs => new() { ["kind"] = "tabs", ["proportion"] = tabs.Proportion,
                ["panels"] = new JsonArray(tabs.Panels.Select(name => (JsonNode?)JsonValue.Create(name)).ToArray()), ["active"] = tabs.Active },
            EditorSplitLayout split => new() { ["kind"] = "split", ["proportion"] = split.Proportion,
                ["orientation"] = split.Orientation, ["children"] = new JsonArray(split.Children.Select(child => (JsonNode?)Node(child)).ToArray()) },
            _ => throw new InvalidDataException("Unsupported layout node.")
        };
        return new() { ["format"] = "poima.editor-layout", ["version"] = 1,
            ["main"] = document.Main is null ? null : Node(document.Main),
            ["floating"] = new JsonArray(document.Floating.Select(window => (JsonNode?)new JsonObject {
                ["content"] = Node(window.Content), ["bounds"] = new JsonObject { ["x"] = window.Bounds.X, ["y"] = window.Bounds.Y,
                    ["width"] = window.Bounds.Width, ["height"] = window.Bounds.Height } }).ToArray()) };
    }
}
