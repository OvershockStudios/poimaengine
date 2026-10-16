// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;

namespace Poima.Editor;

// The transport models RPC data, not the selection reconciliation algorithm.
// Cached State deliberately changes only through Publish/RefreshState.
public sealed class NativeHost
{
    public const string A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    public const string B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    public const string C = "cccccccccccccccccccccccccccccccc";
    public event EventHandler? StateChanged;
    public JsonObject State { get; private set; }
    public string? ActualSelected { get; private set; } = A;
    public long Revision { get; private set; } = 1;
    public int UndoCount { get; private set; }
    public bool RuntimeActive { get; private set; }
    public long ComponentRevision { get; private set; } = 9;
    public Dictionary<string, JsonObject> Documents { get; } = new(StringComparer.Ordinal);
    public List<(string Method, JsonObject Parameters)> Calls { get; } = [];
    public List<JsonObject> Transactions { get; } = [];
    public List<JsonObject> ComponentEdits { get; } = [];
    public JsonObject ComponentValues { get; private set; } = new() { ["count"] = 1, ["exact"] = "9007199254740993" };
    private readonly Dictionary<string, int> failures = new(StringComparer.Ordinal);
    public static JsonObject Clone(JsonObject value) => value.DeepClone().AsObject();
    public static JsonObject InventorySchema() => new()
    {
        ["id"] = "inventory", ["name"] = "Inventory", ["fields"] = new JsonArray(
            new JsonObject { ["id"] = "count", ["name"] = "Count", ["kind"] = "int32", ["default"] = 1 },
            new JsonObject { ["id"] = "exact", ["name"] = "Exact", ["kind"] = "int64", ["default"] = "9007199254740993" })
    };
    public NativeHost()
    {
        foreach (var (id, name, x) in new[] { (A, "Alpha", 1), (B, "Bravo", 2), (C, "Charlie", 3) })
            Documents[id] = new JsonObject
            {
                ["id"] = id, ["name"] = name, ["parent"] = null,
                ["components"] = new JsonObject
                {
                    ["Transform"] = new JsonObject { ["position"] = new JsonArray(x, 0, 0), ["rotation"] = new JsonArray(0, 0, 0), ["scale"] = new JsonArray(1, 1, 1) },
                    ["game:inventory"] = Clone(ComponentValues)
                }
            };
        State = Snapshot();
    }
    private JsonObject Snapshot() => new()
    {
        ["selected"] = ActualSelected, ["revision"] = Revision,
        ["runtime"] = new JsonObject { ["active"] = RuntimeActive, ["session_id"] = "selection-session", ["tick"] = 17L, ["structure_revision"] = 4L },
        ["components"] = new JsonObject { ["revision"] = ComponentRevision },
        ["playback"] = new JsonObject { ["state"] = RuntimeActive ? "paused" : "stopped" },
        ["views"] = new JsonObject { ["game"] = new JsonObject { ["camera"] = null } }
    };
    public void Publish() { State = Snapshot(); StateChanged?.Invoke(this, EventArgs.Empty); }
    public void RefreshState() => Publish();
    public void RemoteSelect(string? id, bool publish = true) { ActualSelected = id; if (publish) Publish(); }
    public void StartRuntime() { RuntimeActive = true; Publish(); }
    public void FailOnce(string method) => failures[method] = failures.GetValueOrDefault(method) + 1;
    public void ExternalRevision() { ++Revision; Publish(); }
    public void Delete(string id) { Documents.Remove(id); ++Revision; Publish(); }
    public int Count(string method) => Calls.Count(call => call.Method == method);
    public JsonObject AuthoredSnapshot() => new()
    {
        ["revision"] = Revision, ["undo"] = UndoCount,
        ["documents"] = new JsonArray(Documents.OrderBy(pair => pair.Key, StringComparer.Ordinal).Select(pair => (JsonNode?)pair.Value.DeepClone()).ToArray()),
        ["transactions"] = new JsonArray(Transactions.Select(value => (JsonNode?)value.DeepClone()).ToArray())
    };
    private static void Require(bool valid, string message) { if (!valid) throw new InvalidOperationException(message); }
    private static void Keys(JsonObject value, params string[] keys) => Require(value.Count == keys.Length && keys.All(value.ContainsKey), "Unexpected request envelope.");
    private static bool RequestId(JsonNode? value) => value is JsonValue scalar && scalar.TryGetValue<string>(out var text) &&
        text.Length == 32 && text.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');
    public JsonObject Call(string method, JsonObject? parameters = null)
    {
        var args = Clone(parameters ?? new()); Calls.Add((method, Clone(args)));
        if (failures.GetValueOrDefault(method) > 0) { --failures[method]; throw new InvalidOperationException("Injected failure: " + method); }
        switch (method)
        {
            case "desktop.inspect": Keys(args); return Snapshot();
            case "world.inspect": Keys(args); return new() { ["revision"] = Revision };
            case "desktop.cameras": Keys(args); return new() { ["cameras"] = new JsonArray() };
            case "component.schemas": Keys(args); return new() { ["schemas"] = new JsonArray(InventorySchema()) };
            case "world.history": Keys(args); return new() { ["undo_count"] = UndoCount, ["redo_count"] = 0 };
            case "entity.query":
                Require(args["revision"]!.GetValue<long>() == Revision && args["limit"]!.GetValue<int>() == 256, "Invalid query guard.");
                return new() { ["entities"] = new JsonArray(Documents.OrderBy(pair => pair.Key, StringComparer.Ordinal).Select(pair => (JsonNode?)new JsonObject
                    { ["id"] = pair.Key, ["name"] = pair.Value["name"]!.DeepClone(), ["parent"] = null,
                        ["components"] = new JsonArray(pair.Value["components"]!.AsObject().Select(field => (JsonNode?)JsonValue.Create(field.Key)).ToArray()) }).ToArray()), ["next_after"] = null };
            case "entity.get":
                Keys(args, "id"); return new() { ["value"] = Documents.TryGetValue(args["id"]!.GetValue<string>(), out var entity) ? entity.DeepClone() : throw new InvalidOperationException("Entity absent.") };
            case "desktop.select":
                Keys(args, "id"); var selected = args["id"]?.GetValue<string>(); Require(selected is null || Documents.ContainsKey(selected), "Entity absent.");
                ActualSelected = selected; return new() { ["selected"] = selected };
            case "world.transact": return Transact(args);
            case "desktop.gizmo.commit": Keys(args, "drag_id", "request_id"); Require(RequestId(args["request_id"]), "Bad gizmo request ID."); return new() { ["committed"] = true };
            case "runtime.component.get":
                Keys(args, "session_id", "tick", "id", "type");
                Require(RuntimeActive && args["session_id"]!.GetValue<string>() == "selection-session" && args["tick"]!.GetValue<long>() == 17 &&
                    args["id"]!.GetValue<string>() == A && args["type"]!.GetValue<string>() == "inventory", "Invalid component observation target.");
                return new() { ["session_id"] = "selection-session", ["tick"] = 17L, ["structure_revision"] = 4L, ["component_revision"] = ComponentRevision,
                    ["id"] = A, ["type"] = "inventory", ["schema"] = InventorySchema(), ["values"] = ComponentValues.DeepClone() };
            case "runtime.component.edit": return EditComponent(args);
            default: throw new InvalidOperationException("Unexpected RPC: " + method);
        }
    }
    private JsonObject Transact(JsonObject args)
    {
        Keys(args, "request_id", "base_revision", "ops"); Require(RequestId(args["request_id"]), "Bad transaction request ID.");
        Require(args["base_revision"]!.GetValue<long>() == Revision, "Stale authored revision.");
        var next = Documents.ToDictionary(pair => pair.Key, pair => Clone(pair.Value), StringComparer.Ordinal);
        foreach (var op in args["ops"]!.AsArray().OfType<JsonObject>())
        {
            var target = next[op["id"]!.GetValue<string>()];
            switch (op["op"]!.GetValue<string>())
            {
                case "entity.rename": Keys(op, "op", "id", "name"); target["name"] = op["name"]!.DeepClone(); break;
                case "component.set": Keys(op, "op", "id", "type", "value"); target["components"]![op["type"]!.GetValue<string>()] = op["value"]!.DeepClone(); break;
                case "component.remove": Keys(op, "op", "id", "type"); target["components"]!.AsObject().Remove(op["type"]!.GetValue<string>()); break;
                default: throw new InvalidOperationException("Unexpected operation.");
            }
        }
        Documents.Clear(); foreach (var pair in next) Documents.Add(pair.Key, pair.Value);
        Transactions.Add(Clone(args)); ++Revision; ++UndoCount; return new() { ["revision"] = Revision };
    }
    private JsonObject EditComponent(JsonObject args)
    {
        Keys(args, "session_id", "request_id", "expected_tick", "expected_structure_revision", "expected_revision", "id", "type", "values");
        Require(RequestId(args["request_id"]) && args["session_id"]!.GetValue<string>() == "selection-session" && args["id"]!.GetValue<string>() == A &&
            args["type"]!.GetValue<string>() == "inventory" && args["expected_tick"]!.GetValue<long>() == 17 &&
            args["expected_structure_revision"]!.GetValue<long>() == 4 && args["expected_revision"]!.GetValue<long>() == ComponentRevision, "Stale/wrong component guard.");
        var values = args["values"]!.AsObject(); Keys(values, "count", "exact");
        _ = values["count"]!.GetValue<int>(); Require(values["exact"]!.GetValue<string>() == "9007199254740993", "Wide field changed.");
        ComponentValues = Clone(values); ComponentEdits.Add(Clone(args)); ++ComponentRevision; return new() { ["revision"] = ComponentRevision };
    }
}
public sealed class GameplayEditorModel { public long Generation => 1; public void RequireClean() {} }

internal static class Program
{
    private static int checks;
    private static readonly JsonArray results = [];
    private sealed class Context : IDisposable
    {
        public NativeHost Host { get; } = new();
        public EditorModel Model { get; }
        public ComponentEditorModel Components { get; }
        public Context() { Model = new(Host); Components = new(Model); Model.Components = Components; }
        public void Dispose() { Components.Dispose(); Model.Dispose(); }
    }
    private static void Check(bool value, string message) { ++checks; if (!value) throw new InvalidOperationException(message); }
    private static void Equal(JsonNode? actual, JsonNode? expected, string message) => Check(JsonNode.DeepEquals(actual, expected), message);
    private static void Throws(Action action, string message)
    {
        try { action(); } catch (InvalidOperationException) { ++checks; return; }
        throw new InvalidOperationException(message);
    }
    private static JsonObject Draft(EditorModel model) => new()
    {
        ["draft"] = model.InspectDraft(), ["schemas"] = model.DraftSchemas.DeepClone(), ["generation"] = model.DraftGeneration,
        ["raw"] = model.FieldText("Transform:position", "<absent>")
    };
    private static void Edit(Context c, bool invalid = false)
    {
        c.Model.SetName("Edited Alpha"); var transform = c.Model.DraftComponents["Transform"]!.DeepClone().AsObject();
        transform["position"] = new JsonArray(7, 8, 9); c.Model.SetComponent("Transform", transform);
        c.Model.SetFieldText("Transform:position", invalid ? "not-a-vector" : "7, 8, 9");
        if (invalid) c.Model.SetInvalid("Transform:position", true);
    }
    private static void Selected(Context c, string? id)
    {
        Check(c.Model.Selected == id && c.Host.ActualSelected == id, "Model/native selection differ.");
        Check(!c.Model.SelectionNeedsRefresh, "Resolved selection still requires refresh.");
        Check(!c.Model.Dirty, "Resolved Inspector is dirty.");
        if (id is not null) Equal(c.Model.DraftComponents, c.Host.Documents[id]["components"], "Resolved components differ.");
    }
    private static void Case(string name, Action action)
    {
        var before = checks;
        try { action(); results.Add(new JsonObject { ["scenario"] = name, ["status"] = "pass", ["checks"] = checks - before }); }
        catch (Exception error) { results.Add(new JsonObject { ["scenario"] = name, ["status"] = "fail", ["checks"] = checks - before, ["error"] = error.Message }); }
    }
    private static void ReloadDraft()
    {
        using var c = new Context(); Edit(c, invalid: true); var draft = Draft(c.Model); var world = c.Host.AuthoredSnapshot();
        c.Host.RemoteSelect(NativeHost.B); Equal(Draft(c.Model), draft, "Remote selection destroyed dirty/invalid draft.");
        c.Model.Reload(); Selected(c, NativeHost.B); Check(c.Model.FieldText("Transform:position", "missing") == "missing", "Reload retained discarded raw text.");
        Equal(c.Host.AuthoredSnapshot(), world, "Reload/selection authored world or history.");
    }
    private static void ApplyOriginal()
    {
        using var c = new Context(); Edit(c); var baseRevision = c.Model.BaseRevision; var b = c.Host.Documents[NativeHost.B].DeepClone(); var other = c.Host.Documents[NativeHost.C].DeepClone();
        c.Host.RemoteSelect(NativeHost.B); c.Model.Apply(); Selected(c, NativeHost.B);
        Check(c.Host.Transactions.Count == 1, "Apply did not commit exactly once."); var sent = c.Host.Transactions.Single();
        Check(sent["base_revision"]!.GetValue<long>() == baseRevision, "Apply replaced original revision guard.");
        var ops = sent["ops"]!.AsArray(); Check(ops.Count == 2 && ops.All(op => op!["id"]!.GetValue<string>() == NativeHost.A), "Apply targeted deferred entity or omitted original operations.");
        Check(c.Host.Documents[NativeHost.A]["name"]!.GetValue<string>() == "Edited Alpha", "Original rename missing.");
        Equal(c.Host.Documents[NativeHost.A]["components"]!["Transform"]!["position"], new JsonArray(7, 8, 9), "Original transform missing.");
        Equal(c.Host.Documents[NativeHost.B], b, "Deferred target mutated."); Equal(c.Host.Documents[NativeHost.C], other, "Unrelated entity mutated.");
        Check(c.Host.UndoCount == 1 && c.Host.Revision == baseRevision + 1, "Unexpected authored history.");
    }
    private static void ApplyFailure(string kind)
    {
        using var c = new Context(); Edit(c, invalid: kind == "invalid"); c.Host.RemoteSelect(NativeHost.B);
        if (kind == "stale") c.Host.ExternalRevision();
        if (kind == "native") c.Host.FailOnce("world.transact");
        var draft = Draft(c.Model); var world = c.Host.AuthoredSnapshot(); var calls = c.Host.Count("world.transact");
        Throws(c.Model.Apply, "Failed Apply unexpectedly succeeded."); Equal(Draft(c.Model), draft, "Failed Apply changed exact draft/raw fields.");
        Equal(c.Host.AuthoredSnapshot(), world, "Failed Apply changed authored state/history.");
        Check(c.Model.Selected == NativeHost.A && c.Host.ActualSelected == NativeHost.B, "Failed Apply lost deferred selection.");
        Check(c.Host.Count("world.transact") == calls + (kind == "invalid" ? 0 : 1), "Invalid/stale/native Apply wrong dispatch count.");
        if (kind == "native") { c.Model.Apply(); Selected(c, NativeHost.B); Check(c.Host.Transactions.Count == 1, "Recovery double committed."); }
        else { c.Model.Reload(); Selected(c, NativeHost.B); Check(c.Host.Transactions.Count == 0, "Discard recovery authored changes."); }
    }
    private static void Latest(string name, params string?[] choices)
    {
        Case(name, () => { using var c = new Context(); Edit(c); var draft = Draft(c.Model); var world = c.Host.AuthoredSnapshot();
            foreach (var choice in choices) { c.Host.RemoteSelect(choice); Equal(Draft(c.Model), draft, "Intermediate selection changed draft."); }
            c.Model.Reload(); Selected(c, choices[^1]); Equal(c.Host.AuthoredSnapshot(), world, "Selection sequence authored changes."); });
    }
    private static void Deleted()
    {
        using var c = new Context(); Edit(c); c.Host.RemoteSelect(NativeHost.B); c.Host.Delete(NativeHost.B); var world = c.Host.AuthoredSnapshot();
        c.Model.Reload(); Selected(c, null); Equal(c.Host.AuthoredSnapshot(), world, "Clearing deleted selection authored changes.");
        var clear = c.Host.Calls.Last(call => call.Method == "desktop.select"); Check(clear.Parameters.ContainsKey("id") && clear.Parameters["id"] is null, "Deleted selection not explicitly cleared.");
    }
    private static void ReadFailure(string method, bool dirty)
    {
        using var c = new Context(); if (dirty) Edit(c, invalid: true);
        c.Host.RemoteSelect(NativeHost.B, publish: false); var draft = Draft(c.Model); var world = c.Host.AuthoredSnapshot(); c.Host.FailOnce(method);
        Throws(dirty ? c.Model.Reload : c.Model.Refresh, "Injected read failure unexpectedly succeeded.");
        Equal(Draft(c.Model), draft, "Failed read destroyed draft or generation."); Equal(c.Host.AuthoredSnapshot(), world, "Failed read authored changes.");
        var commits = c.Host.Count("desktop.gizmo.commit"); Throws(() => c.Model.CommitGizmo(42), "Failed selection sync permitted gizmo commit.");
        Check(c.Host.Count("desktop.gizmo.commit") == commits, "Unsafe gizmo reached transport.");
        c.Model.Reload(); Selected(c, NativeHost.B); Equal(c.Host.AuthoredSnapshot(), world, "Read recovery authored changes.");
    }
    private static void PostApplyReadFailure()
    {
        using var c = new Context(); Edit(c); c.Host.RemoteSelect(NativeHost.B, publish: false); c.Host.FailOnce("desktop.inspect");
        Throws(c.Model.Apply, "Post-commit inspection failure missing."); Check(c.Host.Transactions.Count == 1 && !c.Model.Dirty, "Committed draft not retained cleanly.");
        var before = c.Host.Count("desktop.gizmo.commit"); Throws(() => c.Model.CommitGizmo(42), "Unknown native selection allowed gizmo.");
        Check(c.Host.Count("desktop.gizmo.commit") == before, "Unknown-selection commit dispatched.");
        c.Model.Refresh(); Selected(c, NativeHost.B); Check(c.Host.Transactions.Count == 1, "Refresh replayed committed transaction.");
    }
    private static void LocalCached()
    {
        using var c = new Context(); var cached = c.Host.State; var world = c.Host.AuthoredSnapshot(); c.Model.Select(NativeHost.B);
        Check(ReferenceEquals(cached, c.Host.State) && c.Host.State["selected"]!.GetValue<string>() == NativeHost.A, "Fake cache advanced during Call.");
        c.Model.Refresh(); Selected(c, NativeHost.B);
        c.Host.RemoteSelect(NativeHost.A); Check(!ReferenceEquals(cached, c.Host.State), "New native observation did not replace cache.");
        Selected(c, NativeHost.A); Equal(c.Host.AuthoredSnapshot(), world, "Local selection authored changes.");
    }
    private static void LocalSupersedes()
    {
        using var c = new Context(); c.Model.SetName("Temporary"); c.Host.RemoteSelect(NativeHost.B); c.Model.SetName("Alpha");
        Check(!c.Model.Dirty, "Edit-back did not clean authored draft."); var world = c.Host.AuthoredSnapshot();
        c.Model.Select(NativeHost.C); c.Model.Refresh(); Selected(c, NativeHost.C); Equal(c.Host.AuthoredSnapshot(), world, "Explicit selection authored changes.");
    }
    private static void AuthoredEditBack()
    {
        using var c = new Context(); c.Model.SetName("Temporary"); c.Host.RemoteSelect(NativeHost.B);
        var world = c.Host.AuthoredSnapshot(); c.Model.SetName("Alpha"); c.Model.ReconcileSelection();
        Selected(c, NativeHost.B); Equal(c.Host.AuthoredSnapshot(), world, "Edit-back reconciliation authored changes.");
    }
    private static void CompoundFieldCompletion()
    {
        using var c = new Context(); c.Model.SetInvalid("Transform:position", true); c.Host.RemoteSelect(NativeHost.B);
        var world = c.Host.AuthoredSnapshot(); var original = c.Model.DraftComponents["Transform"]!.DeepClone().AsObject();
        c.Model.SetInvalid("Transform:position", false);
        Check(c.Model.Selected == NativeHost.A, "Validity notification switched selection mid-field edit.");
        var changed = original.DeepClone().AsObject(); changed["position"] = new JsonArray(9, 8, 7);
        c.Model.SetComponent("Transform", changed); c.Model.ReconcileSelection();
        Check(c.Model.Selected == NativeHost.A && c.Model.Dirty, "Completed field edit wrote through to another entity.");
        Equal(c.Model.DraftComponents["Transform"], changed, "Complete field draft was lost.");
        c.Model.SetComponent("Transform", original); c.Model.ReconcileSelection(); Selected(c, NativeHost.B);
        Equal(c.Host.AuthoredSnapshot(), world, "Compound field reconciliation authored changes.");
    }
    private static void AutomaticReadFailure(bool live)
    {
        using var c = new Context();
        if (live) { c.Host.StartRuntime(); c.Components.Load(NativeHost.A, "inventory"); c.Components.Set("count", "2"); }
        else c.Model.SetName("Temporary");
        c.Host.RemoteSelect(NativeHost.B); var world = c.Host.AuthoredSnapshot();
        var notices = 0;
        c.Model.Changed += (_, _) => { ++notices; Check(notices < 5, "Automatic error notification recursed."); c.Model.ReconcileSelection(); };
        c.Host.FailOnce("entity.get"); var reads = c.Host.Count("entity.get"); var logs = c.Model.Log.Count;
        if (live) c.Components.Set("count", "1");
        else { c.Model.SetName("Alpha"); c.Model.ReconcileSelection(); }
        Check(c.Host.Count("entity.get") == reads + 1 && notices == 1, "Automatic failure retried through its notification.");
        Check(c.Model.SelectionNeedsRefresh, "Automatic failure omitted explicit refresh state.");
        Check(c.Model.Log.Count == logs + 1 && c.Model.Log.Last().StartsWith("Inspector selection refresh failed:", StringComparison.Ordinal), "Automatic failure was hidden.");
        Check(c.Model.Selected == NativeHost.A && !c.Model.Dirty && !c.Components.Dirty, "Automatic failure changed retained clean context.");
        var commits = c.Host.Count("desktop.gizmo.commit"); Throws(() => c.Model.CommitGizmo(42), "Automatic failure permitted unsafe gizmo work.");
        Check(c.Host.Count("desktop.gizmo.commit") == commits, "Automatic failure dispatched gizmo work.");
        Equal(c.Host.AuthoredSnapshot(), world, "Automatic failure authored changes.");
        c.Model.Refresh(); Selected(c, NativeHost.B); Equal(c.Host.AuthoredSnapshot(), world, "Automatic recovery authored changes.");
    }
    private static void CancellationOrder()
    {
        using var c = new Context(); var events = new List<string>();
        c.Model.SceneChanging += () => events.Add("cancel:" + c.Model.Selected);
        c.Model.Changed += (_, _) => events.Add("changed:" + c.Model.Selected);
        var world = c.Host.AuthoredSnapshot(); c.Host.RemoteSelect(NativeHost.B);
        var changed = events.FindIndex(item => item == "changed:" + NativeHost.B);
        Check(changed >= 0 && events.Take(changed).Any(item => item == "cancel:" + NativeHost.A), "Selection published before cancelling old gesture.");
        Selected(c, NativeHost.B); Equal(c.Host.AuthoredSnapshot(), world, "Cancellation/selection authored changes.");
    }
    private static void ComponentResolution(string action)
    {
        using var c = new Context(); c.Host.StartRuntime(); c.Components.Load(NativeHost.A, "inventory"); c.Components.Set("count", "2");
        var draft = c.Components.Inspect(); var world = c.Host.AuthoredSnapshot(); c.Host.RemoteSelect(NativeHost.B);
        Check(c.Model.Selected == NativeHost.A && c.Components.Dirty, "Remote selection bypassed live draft guard.");
        Equal(c.Components.Inspect(), draft, "Remote selection destroyed live draft.");
        var changes = 0; c.Model.Changed += (_, _) => { ++changes; Check(changes < 10, "Selection/component Changed recursion."); };
        switch (action)
        {
            case "apply": c.Components.Apply(); break;
            case "reload": c.Components.Reload(); break;
            case "discard": c.Components.Discard(); break;
            case "revert": c.Components.Set("count", "1"); break;
            default: throw new InvalidOperationException(action);
        }
        Selected(c, NativeHost.B); Check(!c.Components.Dirty && changes > 0 && changes < 10, "Live resolution failed to reconcile once boundedly.");
        Equal(c.Host.AuthoredSnapshot(), world, "Live component resolution authored changes.");
        Check(c.Host.ComponentEdits.Count == (action == "apply" ? 1 : 0), "Unexpected component mutation count.");
        if (action == "apply")
        {
            var edit = c.Host.ComponentEdits.Single(); Check(edit["id"]!.GetValue<string>() == NativeHost.A && edit["expected_revision"]!.GetValue<long>() == 9, "Component Apply lost original target/guards.");
            Check(c.Host.ComponentValues["count"]!.GetValue<int>() == 2, "Live edited value missing.");
            Equal(edit["values"]!["exact"], JsonValue.Create("9007199254740993"), "Untouched wide value lost.");
        }
    }
    private static void ComponentFailure(string kind)
    {
        using var c = new Context(); c.Host.StartRuntime(); c.Components.Load(NativeHost.A, "inventory"); c.Components.Set("count", kind == "invalid" ? "bad" : "2");
        c.Host.RemoteSelect(NativeHost.B); var draft = c.Components.Inspect(); var world = c.Host.AuthoredSnapshot();
        if (kind == "edit") c.Host.FailOnce("runtime.component.edit");
        if (kind == "reload") c.Host.FailOnce("runtime.component.get");
        Throws(kind == "reload" ? c.Components.Reload : c.Components.Apply, "Failed component operation unexpectedly succeeded.");
        Equal(c.Components.Inspect(), draft, "Failed component operation destroyed draft.");
        Check(c.Model.Selected == NativeHost.A && c.Host.ActualSelected == NativeHost.B, "Failed component operation adopted deferred target.");
        Check(c.Host.ComponentEdits.Count == 0, "Failed component edit mutated native data."); Equal(c.Host.AuthoredSnapshot(), world, "Component failure authored changes.");
        c.Components.Discard(); Selected(c, NativeHost.B);
    }
    private static void AuthoredReloadDuringLiveDraft()
    {
        using var c = new Context(); c.Host.StartRuntime(); c.Components.Load(NativeHost.A, "inventory"); c.Components.Set("count", "2");
        c.Model.SetName("Temporary authored name"); c.Host.RemoteSelect(NativeHost.B); var component = c.Components.Inspect(); var world = c.Host.AuthoredSnapshot();
        c.Model.Reload(); Check(!c.Model.Dirty && c.Model.Selected == NativeHost.A && c.Components.Dirty, "Authored discard lost retained live context.");
        Equal(c.Components.Inspect(), component, "Authored Reload destroyed live draft.");
        c.Components.Apply(); Selected(c, NativeHost.B); Equal(c.Host.AuthoredSnapshot(), world, "Cross-draft resolution authored changes.");
        Check(c.Host.ComponentEdits.Count == 1, "Cross-draft resolution prevented original live Apply.");
    }
    private static void DeletedRetainedOriginal()
    {
        using var c = new Context(); c.Host.StartRuntime(); c.Components.Load(NativeHost.A, "inventory"); c.Components.Set("count", "2");
        c.Host.RemoteSelect(NativeHost.B); var component = c.Components.Inspect(); c.Host.Delete(NativeHost.A); var world = c.Host.AuthoredSnapshot();
        Check(c.Host.ActualSelected == NativeHost.B && c.Model.Selected != NativeHost.B, "Deleting retained original clobbered valid remote selection or bypassed live draft.");
        Equal(c.Components.Inspect(), component, "Deleting authored original destroyed frozen live draft.");
        c.Components.Discard(); Selected(c, NativeHost.B); Equal(c.Host.AuthoredSnapshot(), world, "Resolving deleted original authored changes.");
    }
    private static void DeletedClearFailure()
    {
        using var c = new Context(); Edit(c, invalid: true); c.Host.RemoteSelect(NativeHost.B); c.Host.Delete(NativeHost.B);
        var draft = Draft(c.Model); var world = c.Host.AuthoredSnapshot(); c.Host.FailOnce("desktop.select");
        Throws(c.Model.Reload, "Native clear failure unexpectedly succeeded."); Equal(Draft(c.Model), draft, "Failed deleted-target clear destroyed original draft.");
        Check(c.Host.ActualSelected == NativeHost.B, "Failed clear mutated native selection.");
        c.Model.Reload(); Selected(c, null); Equal(c.Host.AuthoredSnapshot(), world, "Deleted-target clear recovery authored changes.");
    }
    private static void LocalAcceptedReadFailure()
    {
        using var c = new Context(); var draft = Draft(c.Model); var world = c.Host.AuthoredSnapshot(); c.Host.FailOnce("entity.get");
        Throws(() => c.Model.Select(NativeHost.B), "Accepted local selection read failure missing.");
        Check(c.Host.ActualSelected == NativeHost.B, "Local selection was not accepted before failed read.");
        Equal(Draft(c.Model), draft, "Failed post-selection read destroyed original clean Inspector.");
        var commits = c.Host.Count("desktop.gizmo.commit"); Throws(() => c.Model.CommitGizmo(42), "Incomplete local selection permitted gizmo.");
        Check(c.Host.Count("desktop.gizmo.commit") == commits, "Incomplete local selection dispatched commit.");
        c.Model.Refresh(); Selected(c, NativeHost.B); Equal(c.Host.AuthoredSnapshot(), world, "Local read recovery authored changes.");
    }
    private static void ComponentCommittedReadFailure()
    {
        using var c = new Context(); c.Host.StartRuntime(); c.Components.Load(NativeHost.A, "inventory"); c.Components.Set("count", "2");
        c.Host.RemoteSelect(NativeHost.B); c.Host.FailOnce("runtime.component.get");
        Throws(c.Components.Apply, "Post-component-commit reload failure missing.");
        Check(c.Host.ComponentEdits.Count == 1 && c.Host.ComponentValues["count"]!.GetValue<int>() == 2, "Successful live mutation missing before read failure.");
        Check(c.Components.Dirty && c.Model.Selected == NativeHost.A && c.Host.ActualSelected == NativeHost.B, "Read failure lost component draft or pending choice.");
        c.Components.Reload(); Selected(c, NativeHost.B); Check(c.Host.ComponentEdits.Count == 1, "Read recovery replayed component mutation.");
    }
    private static int Main()
    {
        Case("dirty_reload", ReloadDraft); Case("original_entity_apply", ApplyOriginal);
        foreach (var kind in new[] { "invalid", "stale", "native" }) Case("authored_apply_" + kind, () => ApplyFailure(kind));
        Latest("remote_null", (string?)null); Latest("latest_b_c", NativeHost.B, NativeHost.C);
        Latest("latest_b_null", NativeHost.B, null); Latest("latest_b_a", NativeHost.B, NativeHost.A);
        Case("deleted_pending_target", Deleted);
        foreach (var method in new[] { "desktop.inspect", "entity.get" }) foreach (var dirty in new[] { false, true })
            Case("read_failure_" + method + (dirty ? "_dirty" : "_clean"), () => ReadFailure(method, dirty));
        Case("post_apply_unknown_selection", PostApplyReadFailure); Case("local_ack_stale_cache_later_return", LocalCached);
        Case("local_supersedes_deferred", LocalSupersedes); Case("cancel_before_selection_notification", CancellationOrder);
        Case("authored_edit_back_completion", AuthoredEditBack); Case("compound_field_completion", CompoundFieldCompletion);
        Case("automatic_authored_reconciliation_failure", () => AutomaticReadFailure(false));
        Case("automatic_live_reconciliation_failure", () => AutomaticReadFailure(true));
        foreach (var action in new[] { "apply", "reload", "discard", "revert" }) Case("live_component_" + action, () => ComponentResolution(action));
        foreach (var kind in new[] { "invalid", "edit", "reload" }) Case("live_component_failure_" + kind, () => ComponentFailure(kind));
        Case("authored_reload_during_live_draft", AuthoredReloadDuringLiveDraft);
        Case("deleted_retained_original_valid_pending", DeletedRetainedOriginal); Case("deleted_pending_clear_failure", DeletedClearFailure);
        Case("local_accepted_read_failure", LocalAcceptedReadFailure); Case("live_component_committed_read_failure", ComponentCommittedReadFailure);
        var failed = results.Count(item => item!["status"]!.GetValue<string>() == "fail");
        Console.WriteLine(new JsonObject { ["format"] = "poima.editor-selection-model", ["version"] = 1,
            ["scope"] = "linked production EditorModel and ComponentEditorModel; fake RPC transport; no native/GUI/input qualification",
            ["scenarios"] = results.Count, ["checks"] = checks, ["failed"] = failed, ["results"] = results }.ToJsonString());
        return failed == 0 ? 0 : 1;
    }
}
