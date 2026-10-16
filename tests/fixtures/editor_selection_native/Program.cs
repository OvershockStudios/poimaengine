// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using Poima.Editor;

internal static class Program
{
    private const string A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    private const string B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    private static string editorDirectory = "", runtimeDirectory = "", output = "";
    private static readonly JsonArray cases = [], resolutions = [];
    private static int checks;
    private static JsonObject report = new();
    private static IntPtr desktopLibrary;
    private static bool ownsOutput;

    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(string fileName, IntPtr reserved, uint flags);

    private static string DirectoryArgument(string path, bool existing)
    {
        if (!Path.IsPathFullyQualified(path)) throw new ArgumentException("All directory arguments must be absolute.");
        var full = Path.GetFullPath(path);
        if (existing && !Directory.Exists(full)) throw new ArgumentException("Required directory is absent: " + full);
        for (var current = new DirectoryInfo(existing ? full : Path.GetDirectoryName(full)!); current is not null; current = current.Parent)
            if (!current.Exists || (current.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new ArgumentException("Directories must exist and cannot traverse reparse points: " + full);
        return full;
    }
    private static string FileHash(string file) => Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(file)));
    private static JsonObject Identity(string file) => new() { ["filename"] = Path.GetFileName(file), ["bytes"] = new FileInfo(file).Length, ["sha256"] = FileHash(file) };
    private static Assembly? ResolveManaged(AssemblyLoadContext context, AssemblyName requested)
    {
        var name = requested.Name;
        if (name is null || name != Path.GetFileName(name)) return null;
        var path = Path.Combine(editorDirectory, name + ".dll");
        if (!File.Exists(path)) return null;
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new InvalidOperationException("Managed dependency cannot be a reparse point.");
        var identity = Identity(path); identity["requested"] = requested.FullName; resolutions.Add(identity);
        return context.LoadFromAssemblyPath(path);
    }
    private static IntPtr ResolveNative(string name, Assembly assembly, DllImportSearchPath? search)
    {
        if (name != "poima_desktop") return IntPtr.Zero;
        if (desktopLibrary != IntPtr.Zero) return desktopLibrary;
        var path = Path.Combine(runtimeDirectory, "bin", "poima_desktop.dll");
        // Per-load search rules: dependency DLLs may resolve beside this DLL or
        // in the standard system/application locations. No global search change.
        desktopLibrary = LoadLibraryExW(path, IntPtr.Zero, 0x00000100 | 0x00001000);
        if (desktopLibrary == IntPtr.Zero) throw new InvalidOperationException("Loading installed poima_desktop.dll failed with Win32 error " + Marshal.GetLastPInvokeError());
        return desktopLibrary;
    }
    private static void Check(bool condition, string message) { ++checks; if (!condition) throw new InvalidOperationException(message); }
    private static void Equal(JsonNode? actual, JsonNode? expected, string message) => Check(JsonNode.DeepEquals(actual, expected), message);
    private static JsonObject Clone(JsonObject value) => value.DeepClone().AsObject();
    private static void WriteNew(string path, JsonObject value)
    {
        using var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        JsonSerializer.Serialize(file, value, new JsonSerializerOptions { WriteIndented = true }); file.Flush(true);
    }

    private static int Main(string[] arguments)
    {
        var started = Stopwatch.StartNew();
        try
        {
            if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("This fixture requires Windows and the actual Windows editor/native runtime.");
            if (arguments.Length != 6) throw new ArgumentException("Expected --editor-dir PATH --runtime-dir PATH --output FRESH_DIRECTORY.");
            var options = new Dictionary<string, string>(StringComparer.Ordinal);
            for (int i = 0; i < arguments.Length; i += 2)
                if (!options.TryAdd(arguments[i], arguments[i + 1])) throw new ArgumentException("Duplicate option.");
            if (options.Count != 3 || !options.ContainsKey("--editor-dir") || !options.ContainsKey("--runtime-dir") || !options.ContainsKey("--output")) throw new ArgumentException("Unknown or missing option.");
            editorDirectory = DirectoryArgument(options["--editor-dir"], true);
            runtimeDirectory = DirectoryArgument(options["--runtime-dir"], true);
            output = DirectoryArgument(options["--output"], false);
            if (Path.Exists(output)) throw new ArgumentException("Output must be a fresh directory; existing records are never overwritten.");
            foreach (var file in new[] { Path.Combine(editorDirectory, "Poima.Editor.dll"), Path.Combine(runtimeDirectory, "bin", "poima_desktop.dll") })
                if (!File.Exists(file) || (File.GetAttributes(file) & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Required regular assembly is absent.");
            Directory.CreateDirectory(output); ownsOutput = true;
            report = new JsonObject
            {
                ["format"] = "poima.editor-selection-native", ["version"] = 1, ["passed"] = false,
                ["scope"] = "Actual compiled EditorModel, ComponentEditorModel and NativeHost over installed unattached native desktop bridge; no AppBuilder, window, rendering, Pump clock, input injection or physical/GUI qualification.",
                ["editor"] = Identity(Path.Combine(editorDirectory, "Poima.Editor.dll")), ["native_bridge"] = Identity(Path.Combine(runtimeDirectory, "bin", "poima_desktop.dll")),
                ["cases"] = cases, ["managed_resolutions"] = resolutions
            };
            AssemblyLoadContext.Default.Resolving += ResolveManaged;
            var editor = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(editorDirectory, "Poima.Editor.dll"));
            NativeLibrary.SetDllImportResolver(editor, ResolveNative);
            Execute();
            report["passed"] = cases.All(value => value!["passed"]!.GetValue<bool>());
        }
        catch (Exception error)
        {
            report["passed"] = false; report["error"] = error.ToString();
        }
        finally
        {
            started.Stop(); report["checks"] = checks; report["seconds"] = started.Elapsed.TotalSeconds;
            if (ownsOutput && !File.Exists(Path.Combine(output, "report.json"))) WriteNew(Path.Combine(output, "report.json"), report);
            Console.WriteLine(report.ToJsonString());
        }
        return report["passed"]?.GetValue<bool>() == true ? 0 : 1;
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Execute()
    {
        Run("dirty_reload", DirtyReload); Run("original_target_apply", ApplyOriginal);
        Run("remote_clear_and_deleted_pending", ClearDeleted); Run("local_ack_stale_cache", LocalCached);
    }
    private static void Run(string name, Action<Context, JsonObject> action)
    {
        var before = checks; var clock = Stopwatch.StartNew(); var entry = new JsonObject { ["name"] = name, ["passed"] = false };
        cases.Add(entry);
        try { using var context = new Context(name); action(context, entry); entry["passed"] = true; }
        catch (Exception error) { entry["error"] = error.ToString(); }
        finally { clock.Stop(); entry["checks"] = checks - before; entry["seconds"] = clock.Elapsed.TotalSeconds; }
    }
    private sealed class Context : IDisposable
    {
        public NativeHost Host { get; }
        public EditorModel Model { get; }
        public ComponentEditorModel Components { get; }
        public string World { get; }
        public Context(string name)
        {
            var directory = Path.Combine(output, name); Directory.CreateDirectory(directory); World = Path.Combine(directory, "world.json");
            Host = new NativeHost(World, "", -1, 1);
            try
            {
                var ops = new JsonArray();
                foreach (var (id, label, x) in new[] { (A, "Alpha", -2), (B, "Bravo", 2) })
                {
                    ops.Add(new JsonObject { ["op"] = "entity.create", ["id"] = id, ["name"] = label });
                    ops.Add(new JsonObject { ["op"] = "component.set", ["id"] = id, ["type"] = "Transform", ["value"] = new JsonObject
                    { ["position"] = new JsonArray(x, 0, 0), ["rotation"] = new JsonArray(0, 0, 0, 1), ["scale"] = new JsonArray(1, 1, 1) } });
                    ops.Add(new JsonObject { ["op"] = "component.set", ["id"] = id, ["type"] = "MeshRenderer", ["value"] = new JsonObject
                    { ["primitive"] = "box", ["visible"] = true, ["albedo"] = new JsonArray(.3, .4, .5) } });
                }
                Host.Call("world.transact", new() { ["request_id"] = Guid.NewGuid().ToString("N"), ["base_revision"] = 0, ["ops"] = ops });
                Host.RefreshState(); Model = new EditorModel(Host); Components = new ComponentEditorModel(Model); Model.Components = Components;
                Model.Select(A);
            }
            catch { Host.Dispose(); throw; }
        }
        public void Dispose() { Components.Dispose(); Model.Dispose(); Host.Dispose(); }
        public JsonObject Snapshot()
        {
            var disk = File.ReadAllBytes(World);
            var documents = JsonNode.Parse(disk)!.AsObject();
            var entities = new JsonObject();
            foreach (var pair in documents["entities"]!.AsObject()) entities[pair.Key] = Host.Call("entity.get", new() { ["id"] = pair.Key });
            return new() { ["inspect"] = Host.Call("world.inspect"), ["history"] = Host.Call("world.history"), ["entities"] = entities,
                ["disk_bytes"] = disk.Length, ["disk_sha256"] = Convert.ToHexStringLower(SHA256.HashData(disk)), ["document"] = documents };
        }
        public JsonObject Observation() => new() { ["native"] = Host.Call("desktop.inspect"), ["draft"] = Model.InspectDraft(),
            ["draft_schemas"] = Model.DraftSchemas.DeepClone(), ["draft_generation"] = Model.DraftGeneration,
            ["raw"] = Model.FieldText("selection.fixture.raw", "<absent>"), ["live"] = Components.Inspect() };
        public void SelectRemote(string? id) { Host.Call("desktop.select", new() { ["id"] = id }); Host.RefreshState(); }
    }
    private static void Resolved(Context c, string? expected)
    {
        var native = c.Host.Call("desktop.inspect");
        Check(c.Model.Selected == expected && native["selected"]?.GetValue<string>() == expected, "Model/native selected IDs differ from expected.");
        Check(!c.Model.Dirty && !c.Model.SelectionNeedsRefresh, "Resolved Inspector remains dirty or unsynchronized.");
        Check(native["attached"]!.GetValue<bool>() == false && native["frames_presented"]!.GetValue<long>() == 0, "Fixture attached or presented a viewport.");
        Check(native["runtime"]!["active"]!.GetValue<bool>() == false, "Fixture unexpectedly started simulation.");
        if (expected is not null)
        {
            var entity = c.Host.Call("entity.get", new() { ["id"] = expected })["value"]!.AsObject();
            Equal(c.Model.DraftComponents, entity["components"], "Inspector components differ from authoritative selected entity.");
            Check(c.Model.DraftName == entity["name"]!.GetValue<string>(), "Inspector name differs.");
        }
        c.Model.RequireSceneEditable();
    }
    private static void Edit(Context c, bool invalid)
    {
        c.Model.SetName("Edited Alpha"); var transform = Clone(c.Model.DraftComponents["Transform"]!.AsObject());
        transform["position"] = new JsonArray(7, 8, 9); c.Model.SetComponent("Transform", transform);
        c.Model.SetFieldText("selection.fixture.raw", invalid ? "not-a-vector" : "7, 8, 9");
        c.Model.SetInvalid("selection.fixture.raw", invalid);
    }
    private static JsonObject Draft(Context c) => new() { ["draft"] = c.Model.InspectDraft(), ["schemas"] = c.Model.DraftSchemas.DeepClone(),
        ["generation"] = c.Model.DraftGeneration, ["raw"] = c.Model.FieldText("selection.fixture.raw", "<absent>") };
    private static void DirtyReload(Context c, JsonObject entry)
    {
        var before = c.Snapshot(); entry["before"] = before; Edit(c, true); var draft = Draft(c); entry["retained_draft"] = draft;
        c.SelectRemote(B); Equal(Draft(c), draft, "Remote B destroyed the exact dirty draft or invalid raw text.");
        Check(c.Model.Selected == A && c.Host.State["selected"]!.GetValue<string>() == B, "Remote B was not deferred on A.");
        entry["deferred"] = c.Observation(); c.Model.Reload(); Resolved(c, B);
        Check(c.Model.FieldText("selection.fixture.raw", "absent") == "absent", "Reload retained discarded invalid text.");
        entry["resolved"] = c.Observation(); entry["gizmo"] = c.Host.Call("desktop.gizmo.inspect", new() { ["width"] = 800, ["height"] = 600 });
        var after = c.Snapshot(); entry["after"] = after; Equal(after, before, "Selection/Reload/gizmo inspection modified complete authored state or history.");
    }
    private static void ApplyOriginal(Context c, JsonObject entry)
    {
        var before = c.Snapshot(); entry["before"] = before; var baseRevision = c.Model.BaseRevision; Edit(c, false); var draft = Draft(c); entry["draft_before_apply"] = draft;
        c.SelectRemote(B); Equal(Draft(c), draft, "Remote choice changed original Apply draft."); c.Model.Apply(); Resolved(c, B);
        var after = c.Snapshot(); entry["after"] = after; entry["resolved"] = c.Observation();
        var expected = Clone(before["document"]!.AsObject()); var actual = after["document"]!.AsObject();
        expected["entities"]![A]!["name"] = "Edited Alpha"; expected["entities"]![A]!["components"]!["Transform"]!["position"] = new JsonArray(7, 8, 9);
        expected["revision"] = baseRevision + 1;
        var oldReceipts = expected["receipts"]!.AsArray(); var receipts = actual["receipts"]!.AsArray();
        Check(receipts.Count == oldReceipts.Count + 1, "Apply did not create exactly one new transaction receipt.");
        for (int i = 0; i < oldReceipts.Count; ++i) Equal(receipts[i], oldReceipts[i], "Apply rewrote an older transaction receipt.");
        // Keep the complete new receipt as evidence; independently constrain its
        // request guard and operation identities below rather than rewriting it.
        entry["apply_receipt"] = receipts[^1]!.DeepClone();
        expected["receipts"] = receipts.DeepClone(); Equal(actual, expected, "Apply modified data outside original A name/Transform and one native receipt/revision.");
        var receipt = receipts[^1]!.AsObject(); entry["base_revision"] = baseRevision;
        var parameters = receipt["params"]!.AsObject();
        Check(parameters["base_revision"]!.GetValue<long>() == baseRevision, "Native receipt lost original revision guard.");
        var operations = parameters["ops"]!.AsArray();
        Check(operations.Count == 2 && operations.All(operation => operation!["id"]!.GetValue<string>() == A), "Apply receipt targets deferred B or unexpected operations.");
        Equal(operations, new JsonArray(
            new JsonObject { ["op"] = "entity.rename", ["id"] = A, ["name"] = "Edited Alpha" },
            new JsonObject { ["op"] = "component.set", ["id"] = A, ["type"] = "Transform", ["value"] = actual["entities"]![A]!["components"]!["Transform"]!.DeepClone() }), "Native Apply operations differ from original draft.");
        Check(parameters["preview"]!.GetValue<bool>() == false && receipt["result"]!["committed"]!.GetValue<bool>() &&
            receipt["result"]!["replayed"]!.GetValue<bool>() == false, "Apply receipt is not a fresh committed transaction.");
        Equal(receipt["result"]!["changed_ids"], new JsonArray(A), "Native Apply changed IDs differ.");
        Check(after["inspect"]!["revision"]!.GetValue<long>() == baseRevision + 1, "Apply revision was not incremented exactly once.");
        Check(after["history"]!["undo_count"]!.GetValue<long>() == before["history"]!["undo_count"]!.GetValue<long>() + 1, "Apply undo count did not advance once.");
        Check(receipt.Count != 0, "Native Apply receipt is empty.");
    }
    private static void ClearDeleted(Context c, JsonObject entry)
    {
        var beforeClear = c.Snapshot(); c.Model.SetName("Discard for clear"); c.SelectRemote(null); c.Model.Reload(); Resolved(c, null);
        var afterClear = c.Snapshot(); Equal(afterClear, beforeClear, "Remote clear authored changes."); entry["clear_before"] = beforeClear; entry["clear_after"] = afterClear; entry["cleared"] = c.Observation();
        c.Model.Select(A); Edit(c, true); c.SelectRemote(B); var revision = c.Host.Call("world.inspect")["revision"]!.GetValue<long>();
        c.Host.Call("world.transact", new() { ["request_id"] = Guid.NewGuid().ToString("N"), ["base_revision"] = revision,
            ["ops"] = new JsonArray(new JsonObject { ["op"] = "entity.delete", ["id"] = B, ["recursive"] = true }) });
        c.Host.RefreshState(); var before = c.Snapshot(); entry["deleted_before_reload"] = before; entry["deleted_deferred"] = c.Observation();
        c.Model.Reload(); Resolved(c, null); var after = c.Snapshot(); entry["deleted_after_reload"] = after; entry["deleted_resolved"] = c.Observation();
        Equal(after, before, "Resolving deleted target changed complete authored state/history.");
    }
    private static void LocalCached(Context c, JsonObject entry)
    {
        c.Host.RefreshState(); var before = c.Snapshot(); var cached = c.Host.State; entry["before"] = before;
        Check(cached["selected"]!.GetValue<string>() == A, "Initial cached selection is not A.");
        c.Model.Select(B); Check(ReferenceEquals(cached, c.Host.State) && cached["selected"]!.GetValue<string>() == A, "Native Call advanced cached poll state unexpectedly.");
        entry["accepted_before_refresh"] = c.Observation(); c.Model.Refresh(); Resolved(c, B);
        c.SelectRemote(A); Resolved(c, A); entry["later_remote"] = c.Observation();
        var after = c.Snapshot(); entry["after"] = after; Equal(after, before, "Local/remote selection and reads modified authored state/history.");
    }
}
