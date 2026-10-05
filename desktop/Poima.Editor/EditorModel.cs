// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;

namespace Poima.Editor;

public sealed record EntityRow(string Id, string Name, string? Parent, string[] Components)
{
    public string Icon => Components.Contains("Camera") ? "camera" : Components.Contains("Light") ? "light" :
        Components.Contains("AudioEmitter") ? "audio" : Components.Contains("AnimationRig") ? "rig" :
        Components.Any(x => x is "MeshRenderer" or "StaticMesh" or "SkinnedMesh") ? "cube" : "entity";
}

public sealed class EditorModel : IDisposable
{
    public NativeHost Host { get; }
    public event EventHandler? Changed;
    public event Action? SceneChanging;
    public List<EntityRow> Entities { get; } = [];
    public List<string> Log { get; } = [];
    public long Revision { get; private set; }
    public long BaseRevision { get; private set; }
    public long DraftGeneration { get; private set; }
    public string? Selected { get; private set; }
    public string DraftName { get; private set; } = "";
    public JsonObject DraftComponents { get; private set; } = new();
    public bool Dirty => Selected is not null && (invalid.Count != 0 || DraftName != baselineName || !JsonNode.DeepEquals(DraftComponents, baseline));
    public bool Conflict => Dirty && BaseRevision != Revision;
    public int UndoDepth { get; private set; }
    public int RedoDepth { get; private set; }
    public string? RuntimeId { get; private set; }
    public long Tick { get; private set; }
    public bool Paused { get; private set; } = true;
    private string baselineName = "";
    private JsonObject baseline = new();
    private bool refreshing;
    private string? observedSelection;
    private readonly HashSet<string> invalid = [];
    private readonly Dictionary<string, string> rawFields = new(StringComparer.Ordinal);

    public EditorModel(NativeHost host)
    {
        Host = host;
        host.StateChanged += HostChanged;
        Refresh();
        Note("Ready. Changes use the same native world service as external agents.");
    }
    public static string NewId() => Guid.NewGuid().ToString("N");
    public static JsonObject Clone(JsonObject value) => (JsonObject)value.DeepClone();
    public void Note(string text) { if (Log.Count >= 256) Log.RemoveAt(0); Log.Add(text); Changed?.Invoke(this, EventArgs.Empty); }
    private void HostChanged(object? sender, EventArgs args) { Refresh(); }
    public void Refresh()
    {
        if (refreshing) return;
        refreshing = true;
        try
        {
            Revision = Host.Call("world.inspect")["revision"]!.GetValue<long>();
            var nativeSelection = Host.State["selected"]?.GetValue<string>();
            if (nativeSelection != observedSelection)
            {
                if (!Dirty) Selected = nativeSelection;
                observedSelection = nativeSelection;
            }
            if (Host.State["runtime"] is JsonObject runtime)
            {
                RuntimeId = runtime["active"]?.GetValue<bool>() == true ? runtime["session_id"]?.GetValue<string>() : null;
                Tick = runtime["tick"]?.GetValue<long>() ?? 0;
            }
            var found = new List<EntityRow>();
            var parameters = new JsonObject { ["revision"] = Revision, ["limit"] = 256 };
            do
            {
                var page = Host.Call("entity.query", parameters);
                foreach (var e in page["entities"]!.AsArray())
                    found.Add(new(e!["id"]!.GetValue<string>(), e["name"]!.GetValue<string>(), e["parent"]?.GetValue<string>(),
                        e["components"]!.AsArray().Select(x => x!.GetValue<string>()).ToArray()));
                parameters["after"] = page["next_after"]?.DeepClone();
            } while (parameters["after"] is not null);
            Entities.Clear(); Entities.AddRange(found);
            var history = Host.Call("world.history");
            UndoDepth = history["undo_count"]?.GetValue<int>() ?? 0;
            RedoDepth = history["redo_count"]?.GetValue<int>() ?? 0;
            if (!Dirty) LoadSelected();
        }
        finally { refreshing = false; }
        Changed?.Invoke(this, EventArgs.Empty);
    }
    private void LoadSelected()
    {
        if (!Entities.Any(x => x.Id == Selected)) Selected = null;
        var value = Selected is null ? null : Host.Call("entity.get", new() { ["id"] = Selected })["value"]!.AsObject();
        DraftName = value?["name"]?.GetValue<string>() ?? "";
        DraftComponents = value?["components"] is JsonObject components ? Clone(components) : new();
        baselineName = DraftName; baseline = Clone(DraftComponents); BaseRevision = Revision;
        invalid.Clear(); rawFields.Clear();
        ++DraftGeneration;
    }
    public void Select(string? id)
    {
        if (id == Selected) return;
        RequireClean();
        if (id is not null && !Entities.Any(x => x.Id == id)) throw new InvalidOperationException("Entity no longer exists.");
        SceneChanging?.Invoke();
        Host.Call("desktop.select", new() { ["id"] = id });
        Selected = id; LoadSelected(); Changed?.Invoke(this, EventArgs.Empty);
    }
    public void SetName(string name) { SceneChanging?.Invoke(); DraftName = name; }
    public void SetComponent(string type, JsonObject value) { SceneChanging?.Invoke(); DraftComponents[type] = value.DeepClone(); }
    public string FieldText(string field, string fallback) => rawFields.GetValueOrDefault(field, fallback);
    public void SetFieldText(string field, string text) { rawFields[field] = text; }
    public void SetInvalid(string field, bool value) { if (value) invalid.Add(field); else invalid.Remove(field); }
    public bool HasInvalid(string prefix) => invalid.Any(key => key.StartsWith(prefix, StringComparison.Ordinal));
    public void Reload() { SceneChanging?.Invoke(); LoadSelected(); Changed?.Invoke(this, EventArgs.Empty); }
    private void RequireClean()
    {
        if (Dirty) throw new InvalidOperationException("Apply or reload Inspector changes first.");
    }
    private void RequireStopped()
    {
        if (RuntimeId is not null) throw new InvalidOperationException("Stop the runtime before changing authored entities.");
    }
    public void RequireSceneEditable() { RequireStopped(); RequireClean(); }
    public JsonObject CommitGizmo(long dragId)
    {
        RequireSceneEditable();
        var result = Host.Call("desktop.gizmo.commit", new() { ["drag_id"] = dragId, ["request_id"] = NewId() });
        return result;
    }
    public void Apply()
    {
        RequireStopped();
        if (invalid.Count != 0) throw new InvalidOperationException("Fix invalid Inspector fields before applying.");
        if (!Dirty || Selected is null) return;
        SceneChanging?.Invoke();
        var ops = new JsonArray();
        if (DraftName != baselineName) ops.Add(new JsonObject { ["op"] = "entity.rename", ["id"] = Selected, ["name"] = DraftName });
        foreach (var pair in DraftComponents)
            if (!JsonNode.DeepEquals(pair.Value, baseline[pair.Key]))
                ops.Add(new JsonObject { ["op"] = "component.set", ["id"] = Selected, ["type"] = pair.Key, ["value"] = pair.Value?.DeepClone() });
        Host.Call("world.transact", new() { ["request_id"] = NewId(), ["base_revision"] = BaseRevision, ["ops"] = ops });
        baselineName = DraftName; baseline = Clone(DraftComponents); Refresh();
    }
    private void Transact(JsonArray ops)
    {
        RequireStopped(); RequireClean();
        SceneChanging?.Invoke();
        Host.Call("world.transact", new() { ["request_id"] = NewId(), ["base_revision"] = Revision, ["ops"] = ops });
        Refresh();
    }
    public string Create(string kind)
    {
        var id = NewId();
        var ops = new JsonArray
        {
            new JsonObject { ["op"] = "entity.create", ["id"] = id, ["name"] = kind },
            new JsonObject { ["op"] = "component.set", ["id"] = id, ["type"] = "Transform", ["value"] = JsonNode.Parse("{\"position\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]}") }
        };
        var (type, value) = kind switch
        {
            "Cube" => ("MeshRenderer", "{\"primitive\":\"box\",\"albedo\":[0.55,0.65,0.8],\"visible\":true}"),
            "Camera" => ("Camera", "{\"vertical_fov\":60,\"near\":0.1,\"far\":1000}"),
            "Light" => ("Light", "{\"kind\":\"point\",\"color\":[1,1,1],\"intensity\":100,\"range\":20,\"enabled\":true}"),
            _ => ("", "{}")
        };
        if (type.Length != 0) ops.Add(new JsonObject { ["op"] = "component.set", ["id"] = id, ["type"] = type, ["value"] = JsonNode.Parse(value) });
        Transact(ops); Select(id); return id;
    }
    public void Delete()
    {
        if (Selected is null) return;
        Transact(new JsonArray(new JsonObject { ["op"] = "entity.delete", ["id"] = Selected, ["recursive"] = true }));
    }
    public void History(bool redo)
    {
        RequireStopped(); RequireClean();
        SceneChanging?.Invoke();
        Host.Call(redo ? "world.redo" : "world.undo", new() { ["request_id"] = NewId(), ["base_revision"] = Revision }); Refresh();
    }
    public JsonObject Import(string path)
    {
        var result = Host.Call("asset.import", new() { ["source"] = path });
        Note($"Imported {System.IO.Path.GetFileName(path)} — {result["nodes"]} nodes, {result["triangles"]} triangles.");
        return result;
    }
    public void Instantiate(string asset, string name)
    {
        var id = NewId();
        Transact(new JsonArray(new JsonObject { ["op"] = "asset.instantiate", ["id"] = id, ["asset"] = asset, ["name"] = name })); Select(id);
    }
    public void PlayStop()
    {
        SceneChanging?.Invoke();
        if (RuntimeId is not null) { Host.Call("runtime.stop", new() { ["session_id"] = RuntimeId }); RuntimeId = null; Tick = 0; }
        else { RequireClean(); var id = NewId(); Host.Call("runtime.start", new() { ["session_id"] = id, ["revision"] = Revision }); RuntimeId = id; Tick = 0; Paused = true; }
        Changed?.Invoke(this, EventArgs.Empty);
    }
    public void Pause() { if (RuntimeId is null) return; Paused = !Paused; Changed?.Invoke(this, EventArgs.Empty); }
    public void Step(int ticks = 1)
    {
        if (RuntimeId is null) return;
        Tick = Host.Call("runtime.step", new() { ["session_id"] = RuntimeId, ["request_id"] = NewId(), ["expected_tick"] = Tick, ["ticks"] = ticks })["tick"]!.GetValue<long>();
        Changed?.Invoke(this, EventArgs.Empty);
    }
    public JsonObject InspectDraft() => new() { ["entity"] = Selected, ["name"] = DraftName, ["components"] = DraftComponents.DeepClone(), ["invalid_fields"] = new JsonArray(invalid.Order().Select(x => (JsonNode?)JsonValue.Create(x)).ToArray()), ["dirty"] = Dirty, ["conflict"] = Conflict, ["base_revision"] = BaseRevision, ["revision"] = Revision };
    public void Dispose() { Host.StateChanged -= HostChanged; SceneChanging = null; }
}
