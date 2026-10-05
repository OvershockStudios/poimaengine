// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Poima.Editor;

if (args.Length != 2 || args[0] != "--output")
{
    Console.Error.WriteLine("Use: Poima.LayoutStore.Contract --output evidence-directory");
    return 2;
}
var root = Path.Combine(Path.GetFullPath(args[1]), "run-"+Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
var cases = new JsonArray(); var failures = 0;

void Check(bool value, string message)
{ if (!value) throw new InvalidOperationException(message); }
void Case(string name, Action<string> action)
{
    var folder = Path.Combine(root, name); Directory.CreateDirectory(folder);
    var watch = Stopwatch.StartNew(); string? error = null;
    try { action(folder); }
    catch (Exception failure) { ++failures; error = failure.ToString(); }
    cases.Add(new JsonObject { ["name"] = name, ["passed"] = error is null, ["elapsed_ms"] = watch.Elapsed.TotalMilliseconds, ["error"] = error });
    Console.WriteLine((error is null ? "PASS " : "FAIL ")+name);
}
EditorLayoutDocument Fixture() => new(
    new EditorSplitLayout(1, "horizontal", new EditorLayoutNode[] {
        new EditorTabLayout(.3, new[] { "Hierarchy" }, "Hierarchy"),
        new EditorSplitLayout(.7, "vertical", new EditorLayoutNode[] {
            new EditorTabLayout(.8, new[] { "Scene" }, "Scene"),
            new EditorTabLayout(.2, new[] { "Inspector", "Project" }, "Project") }) }),
    new[] { new EditorFloatingLayout(new EditorTabLayout(1, new[] { "Console" }, "Console"), new(-1200, 120, 640, 480)) });
JsonObject Encoded() => EditorLayoutStore.Encode(Fixture());
EditorLayoutStore Store(string folder) => new(Path.Combine(folder, "project"), Path.Combine(folder, "prefs", "layout.json"));
void NoTemporaryFiles(string folder) => Check(!Directory.EnumerateFiles(folder, ".layout-*.tmp", SearchOption.AllDirectories).Any(), "A save leaked a temporary preference file.");
void RejectedFile(string folder, byte[] bytes)
{
    var store = Store(folder); Directory.CreateDirectory(Path.GetDirectoryName(store.FilePath)!);
    File.WriteAllBytes(store.FilePath, bytes);
    Check(!store.TryLoad(out var document, out var loadError) && document is null && !string.IsNullOrWhiteSpace(loadError), "Invalid file was restored or lacked a diagnostic.");
    Check(File.ReadAllBytes(store.FilePath).AsSpan().SequenceEqual(bytes), "Failed load modified original bytes.");
    Check(!store.TrySave(Fixture(), out var saveError) && !string.IsNullOrWhiteSpace(saveError), "A valid save overwrote a corrupt/unsupported preference file.");
    Check(File.ReadAllBytes(store.FilePath).AsSpan().SequenceEqual(bytes), "Failed save modified original bytes.");
    NoTemporaryFiles(folder);
}
void RejectedJson(string name, Action<JsonObject> mutate) => Case(name, folder => {
    var json = Encoded(); mutate(json); RejectedFile(folder, Encoding.UTF8.GetBytes(json.ToJsonString()));
});
JsonArray SecondaryPanels(JsonObject json) => json["main"]!["children"]![1]!["children"]![1]!["panels"]!.AsArray();

Case("missing-file-no-write", folder => {
    var store = Store(folder);
    Check(!store.TryLoad(out var document, out var error) && document is null && error is null, "Missing preferences should mean default layout without an error.");
    Check(!Directory.Exists(Path.GetDirectoryName(store.FilePath)), "Read-only loading created a preference directory.");
});
Case("round-trip-custom-path", folder => {
    var project = Path.Combine(folder, "project"); Directory.CreateDirectory(project);
    var sentinel = Path.Combine(project, "world.json"); File.WriteAllText(sentinel, "untouched world bytes");
    var custom = Path.Combine(folder, "preferences with spaces", "chosen-layout.json");
    var store = new EditorLayoutStore(project, custom);
    Check(store.FilePath == Path.GetFullPath(custom), "Custom file path was not honored.");
    Check(store.TrySave(Fixture(), out var saveError), saveError ?? "Save failed.");
    Check(store.TryLoad(out var restored, out var loadError) && restored is not null, loadError ?? "Load failed.");
    Check(JsonNode.DeepEquals(EditorLayoutStore.Encode(Fixture()), EditorLayoutStore.Encode(restored!)), "Round trip changed split proportions, active tab, panel order, or floating bounds.");
    Check(File.ReadAllText(sentinel) == "untouched world bytes" && Directory.EnumerateFileSystemEntries(project).Count() == 1, "Preferences touched project storage.");
    NoTemporaryFiles(folder);
});
Case("replace-existing-valid-file", folder => {
    var store = Store(folder); Check(store.TrySave(Fixture(), out var firstError), firstError ?? "Initial save failed.");
    var replacement = new EditorLayoutDocument(new EditorTabLayout(1, new[] { "Console", "Scene", "Hierarchy", "Project", "Inspector" }, "Scene"), Array.Empty<EditorFloatingLayout>());
    Check(store.TrySave(replacement, out var saveError), saveError ?? "Replacement failed.");
    Check(store.TryLoad(out var restored, out var loadError) && restored is not null, loadError ?? "Replacement could not be loaded.");
    Check(JsonNode.DeepEquals(EditorLayoutStore.Encode(replacement), EditorLayoutStore.Encode(restored!)), "Existing preference file was not replaced completely.");
    NoTemporaryFiles(folder);
});
Case("atomic-replace-open-reader", folder => {
    var store = Store(folder); Check(store.TrySave(Fixture(), out var firstError), firstError ?? "Initial save failed.");
    var original = File.ReadAllBytes(store.FilePath);
    using var reader = new FileStream(store.FilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
    var replacement = new EditorLayoutDocument(new EditorTabLayout(1, new[] { "Console", "Scene", "Hierarchy", "Project", "Inspector" }, "Scene"), Array.Empty<EditorFloatingLayout>());
    Check(store.TrySave(replacement, out var saveError), saveError ?? "Replacement failed.");
    Check(store.TryLoad(out var restored, out var loadError) && restored is not null, loadError ?? "Replacement could not be loaded.");
    Check(JsonNode.DeepEquals(EditorLayoutStore.Encode(replacement), EditorLayoutStore.Encode(restored!)), "Target path did not contain the complete replacement.");
    var retained = new byte[original.Length]; reader.ReadExactly(retained);
    Check(retained.AsSpan().SequenceEqual(original) && reader.ReadByte() == -1, "Replacement modified the already-open previous file instead of replacing its identity.");
    NoTemporaryFiles(folder);
});
Case("all-panels-floating", folder => {
    var store = Store(folder);
    var layout = new EditorLayoutDocument(null, EditorLayoutStore.PanelNames.Select((panel, i) => new EditorFloatingLayout(new EditorTabLayout(1, new[] { panel }, panel), new(i*20, i*30, 500, 300))).ToArray());
    Check(store.TrySave(layout, out var error), error ?? "All-floating save failed.");
    Check(store.TryLoad(out var restored, out error) && restored is not null && restored.Main is null && restored.Floating.Count == 5, error ?? "All-floating layout was not restored.");
});
Case("default-path-project-isolation", folder => {
    var first = new EditorLayoutStore(Path.Combine(folder, "project-a"));
    var same = new EditorLayoutStore(Path.Combine(folder, "project-a", "."));
    var other = new EditorLayoutStore(Path.Combine(folder, "project-b"));
    Check(first.FilePath == same.FilePath && first.FilePath != other.FilePath, "Project preference identity is unstable or collides.");
    Check(!first.FilePath.StartsWith(Path.GetFullPath(folder)+Path.DirectorySeparatorChar, StringComparison.Ordinal), "Default preferences are stored in the project/test folder.");
    Check(first.FilePath.Contains(Path.Combine("Poima", "Editor", "Layouts"), StringComparison.Ordinal), "Default preference path is not user-local editor storage.");
});
Case("malformed-json", folder => RejectedFile(folder, Encoding.UTF8.GetBytes("{not json")));
Case("duplicate-json-field", folder => RejectedFile(folder, Encoding.UTF8.GetBytes(Encoded().ToJsonString().Replace("\"version\":1", "\"version\":1,\"version\":1", StringComparison.Ordinal))));
RejectedJson("unknown-version", json => json["version"] = 999);
RejectedJson("boolean-version", json => json["version"] = true);
RejectedJson("unknown-format", json => json["format"] = "another.layout");
RejectedJson("unknown-field", json => json["future"] = 1);
RejectedJson("duplicate-panel", json => SecondaryPanels(json)[0] = "Hierarchy");
RejectedJson("missing-panel", json => SecondaryPanels(json).RemoveAt(0));
RejectedJson("unknown-panel", json => SecondaryPanels(json)[0] = "UnrecognizedTool");
RejectedJson("invalid-active-tab", json => json["main"]!["children"]![0]!["active"] = "Console");
RejectedJson("invalid-orientation", json => json["main"]!["orientation"] = "diagonal");
RejectedJson("zero-proportion", json => json["main"]!["proportion"] = 0);
RejectedJson("bad-floating-bounds", json => json["floating"]![0]!["bounds"]!["width"] = -10);
Case("overflow-number-in-file", folder => RejectedFile(folder, Encoding.UTF8.GetBytes(Encoded().ToJsonString().Replace("\"proportion\":1", "\"proportion\":1e999", StringComparison.Ordinal))));
Case("nonfinite-token-in-file", folder => RejectedFile(folder, Encoding.UTF8.GetBytes(Encoded().ToJsonString().Replace("\"proportion\":1", "\"proportion\":NaN", StringComparison.Ordinal))));
Case("oversize-file", folder => RejectedFile(folder, new byte[65537]));
Case("nonfinite-authored-save", folder => {
    var store = Store(folder); Check(store.TrySave(Fixture(), out var error), error ?? "Initial save failed.");
    var original = File.ReadAllBytes(store.FilePath);
    foreach (var value in new[] { double.NaN, double.PositiveInfinity, double.NegativeInfinity })
    {
        var invalid = new EditorLayoutDocument(new EditorTabLayout(value, EditorLayoutStore.PanelNames, "Scene"), Array.Empty<EditorFloatingLayout>());
        Check(!store.TrySave(invalid, out error) && !string.IsNullOrWhiteSpace(error), "Nonfinite proportion was saved.");
        var fixture = Fixture();
        invalid = fixture with { Floating = new[] { fixture.Floating[0] with { Bounds = new(value, 0, 640, 480) } } };
        Check(!store.TrySave(invalid, out error) && !string.IsNullOrWhiteSpace(error), "Nonfinite window coordinate was saved.");
        Check(File.ReadAllBytes(store.FilePath).AsSpan().SequenceEqual(original), "Invalid authored save modified valid preferences.");
    }
    NoTemporaryFiles(folder);
});
Case("directory-path-preserved", folder => {
    var store = Store(folder); Directory.CreateDirectory(store.FilePath);
    var sentinel = Path.Combine(store.FilePath, "keep.txt"); File.WriteAllText(sentinel, "keep");
    Check(!store.TryLoad(out _, out var error) && !string.IsNullOrWhiteSpace(error), "Directory accepted as a layout file.");
    Check(!store.TrySave(Fixture(), out error) && !string.IsNullOrWhiteSpace(error), "Save replaced a directory.");
    Check(File.ReadAllText(sentinel) == "keep", "Preference directory contents changed.");
});
var evidence = new JsonObject { ["passed"] = failures == 0, ["tests"] = cases.Count, ["failures"] = failures,
    ["framework"] = RuntimeInformation.FrameworkDescription, ["os"] = RuntimeInformation.OSDescription,
    ["assembly_sha256"] = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(typeof(EditorLayoutStore).Assembly.Location))).ToLowerInvariant(),
    ["cases"] = cases,
    ["limitations"] = new JsonArray("Headless storage contract only; Dock restore lifecycle and display clamping require separate desktop qualification.") };
var report = Path.Combine(root, "result.json");
File.WriteAllText(report, evidence.ToJsonString(new JsonSerializerOptions { WriteIndented = true })+"\n");
Console.WriteLine($"{cases.Count-failures}/{cases.Count} passed; evidence: {report}");
return failures == 0 ? 0 : 1;
