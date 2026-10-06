// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;

namespace Poima.Editor;

public sealed record EntityRow(string Id, string Name, string? Parent, string[] Components)
{
    public string Icon => Components.Contains("Camera") ? "camera" : (Components.Contains("Light") || Components.Contains("LightingEnvironment")) ? "light" :
        Components.Contains("AudioEmitter") ? "audio" : Components.Contains("AnimationRig") ? "rig" :
        Components.Any(x => x is "MeshRenderer" or "StaticMesh" or "SkinnedMesh") ? "cube" : "entity";
}
public sealed record CameraChoice(string Id, string Label);
public sealed record AnimationClipChoice(int? Index, string Label)
{
    public override string ToString() => Label;
}

public sealed class EditorModel : IDisposable
{
    public NativeHost Host { get; }
    public GameplayEditorModel? Gameplay { get; set; }
    public ComponentEditorModel? Components { get; set; }
    public JsonObject Schemas { get; private set; } = new();
    public JsonObject DraftSchemas { get; private set; } = new();
    public event EventHandler? Changed;
    public event EventHandler? PlaybackChanged;
    public event Action? SceneChanging;
    public event Action? GameCameraChanging;
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
    public string PlaybackState { get; private set; } = "stopped";
    public bool Paused => PlaybackState == "paused";
    public string? PlaybackError { get; private set; }
    public string? PlaybackSuspended { get; private set; }
    public string? GameCamera { get; private set; }
    public IReadOnlyList<CameraChoice> Cameras { get; private set; } = [];
    private string? cameraSource;
    private bool gameCameraInitialized;
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
    private void HostChanged(object? sender, EventArgs args)
    {
        // Shutdown pumps must not trigger camera/entity RPCs or live Inspector queries.
        if (Host.State["closing"]?.GetValue<bool>() == true) { SyncPlayback(); return; }
        var authoredChanged = Host.State["revision"]?.GetValue<long>() != Revision;
        var selectionChanged = Host.State["selected"]?.GetValue<string>() != observedSelection;
        if (authoredChanged || selectionChanged) Refresh();
        else { SyncPlayback(); RefreshCameras(); PlaybackChanged?.Invoke(this, EventArgs.Empty); }
    }
    private void SyncPlayback()
    {
        var runtime = Host.State["runtime"] as JsonObject;
        var playback = Host.State["playback"] as JsonObject;
        RuntimeId = runtime?["active"]?.GetValue<bool>() == true ? runtime["session_id"]?.GetValue<string>() : null;
        Tick = runtime?["tick"]?.GetValue<long>() ?? 0;
        PlaybackState = playback?["state"]?.GetValue<string>() ?? (RuntimeId is null ? "stopped" : "paused");
        PlaybackError = playback?["last_error"]?.GetValue<string>();
        PlaybackSuspended = playback?["suspended"]?.GetValue<string>();
        GameCamera = Host.State["views"]?["game"]?["camera"]?.GetValue<string>();
        gameCameraInitialized |= GameCamera is not null;
    }
    private void RefreshCameras()
    {
        var source = RuntimeId is null ? "authored:" + Revision : "runtime:" + RuntimeId;
        if (cameraSource == source) return;
        var cameras = Host.Call("desktop.cameras")["cameras"]!.AsArray();
        Cameras = cameras.Select(camera =>
        {
            var id = camera!["id"]!.GetValue<string>();
            var label = Entities.FirstOrDefault(entity => entity.Id == id)?.Name ?? id[..Math.Min(id.Length, 8)];
            return new CameraChoice(id, label);
        }).ToArray();
        cameraSource = source;
        if (!gameCameraInitialized && Cameras.Count != 0)
        {
            // One initial real camera choice. Later deletions and explicit None
            // remain visible choices, never silently jump to a different camera.
            GameCamera = Cameras[0].Id; gameCameraInitialized = true;
            Host.Call("desktop.game.camera", new() { ["camera"] = GameCamera });
        }
    }
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
            SyncPlayback();
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
            var schemas = Host.Call("component.schemas")["schemas"]!.AsArray();
            Schemas = new JsonObject();
            foreach (var schema in schemas.OfType<JsonObject>()) Schemas[schema["id"]!.GetValue<string>()] = schema.DeepClone();
            RefreshCameras();
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
        DraftSchemas = Clone(Schemas);
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
    public void RequireAuthoredClean()
    {
        if (Dirty) throw new InvalidOperationException("Apply or reload Inspector changes first.");
    }
    private void RequireClean() { RequireAuthoredClean(); Components?.RequireClean(); }
    public void RequireInspectorClean() => RequireClean();
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
        RequireStopped(); Components?.RequireClean();
        if (invalid.Count != 0) throw new InvalidOperationException("Fix invalid Inspector fields before applying.");
        if (!Dirty || Selected is null) return;
        SceneChanging?.Invoke();
        var ops = new JsonArray();
        if (DraftName != baselineName) ops.Add(new JsonObject { ["op"] = "entity.rename", ["id"] = Selected, ["name"] = DraftName });
        foreach (var pair in baseline)
            if (!DraftComponents.ContainsKey(pair.Key)) ops.Add(new JsonObject { ["op"] = "component.remove", ["id"] = Selected, ["type"] = pair.Key });
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
    public void AddCustomComponent(string type)
    {
        RequireStopped(); Components?.RequireClean();
        if (Selected is null || DraftSchemas[type] is not JsonObject schema) throw new InvalidOperationException("Select an object and an imported component schema first.");
        var key = "game:" + type;
        if (DraftComponents.ContainsKey(key)) throw new InvalidOperationException("This component is already attached.");
        SceneChanging?.Invoke(); DraftComponents[key] = ComponentFields.Defaults(schema);
        ++DraftGeneration; Changed?.Invoke(this, EventArgs.Empty);
    }
    public void RemoveCustomComponent(string type)
    {
        RequireStopped(); Components?.RequireClean();
        var key = "game:" + type;
        if (Selected is null || !DraftComponents.ContainsKey(key)) throw new InvalidOperationException("This component is not attached.");
        SceneChanging?.Invoke(); DraftComponents.Remove(key);
        invalid.RemoveWhere(field => field.StartsWith(key + ":", StringComparison.Ordinal));
        foreach (var field in rawFields.Keys.Where(field => field.StartsWith(key + ":", StringComparison.Ordinal)).ToArray()) rawFields.Remove(field);
        ++DraftGeneration; Changed?.Invoke(this, EventArgs.Empty);
    }
    public void ImportComponentManifest(string path)
    {
        RequireStopped(); RequireClean(); Gameplay?.RequireClean();
        // Read the bounded bytes once: retry identity is the inline declaration, never a mutable filename.
        using var input = new System.IO.FileStream(path, System.IO.FileMode.Open, System.IO.FileAccess.Read, System.IO.FileShare.Read);
        using var bytes = new System.IO.MemoryStream();
        var buffer = new byte[8192];
        while (true) { var count = input.Read(buffer); if (count == 0) break; if (bytes.Length + count > 512 * 1024) throw new InvalidOperationException("Component manifest exceeds 512 KiB."); bytes.Write(buffer, 0, count); }
        using var document = System.Text.Json.JsonDocument.Parse(bytes.ToArray(), new System.Text.Json.JsonDocumentOptions { MaxDepth = 32 });
        static void Unique(System.Text.Json.JsonElement node)
        {
            if (node.ValueKind == System.Text.Json.JsonValueKind.Object)
            {
                var names = new HashSet<string>(StringComparer.Ordinal);
                foreach (var field in node.EnumerateObject()) { if (!names.Add(field.Name)) throw new InvalidOperationException("Component manifest contains duplicate JSON fields."); Unique(field.Value); }
            }
            else if (node.ValueKind == System.Text.Json.JsonValueKind.Array) foreach (var child in node.EnumerateArray()) Unique(child);
        }
        Unique(document.RootElement);
        var manifest = JsonNode.Parse(document.RootElement.GetRawText()) as JsonObject ?? throw new InvalidOperationException("Component manifest must be an object.");
        Host.Call("component.schema.import", new() { ["request_id"] = NewId(), ["base_revision"] = Revision, ["manifest"] = manifest });
        Refresh(); Note("Imported component declarations. Add a component to an object in the Inspector.");
    }
    public void AttachMeshCollider()
    {
        RequireStopped(); RequireClean();
        if (Selected is null || DraftComponents["StaticMesh"] is not JsonObject mesh)
            throw new InvalidOperationException("Select an object with a StaticMesh first.");
        if (DraftComponents.ContainsKey("MeshCollider"))
            throw new InvalidOperationException("This object already has a MeshCollider.");
        if (DraftComponents.ContainsKey("BoxCollider") || DraftComponents.ContainsKey("CharacterController"))
            throw new InvalidOperationException("Static mesh collision cannot share an object with BoxCollider or CharacterController.");
        Transact(new JsonArray(new JsonObject
        {
            ["op"] = "component.set", ["id"] = Selected, ["type"] = "MeshCollider",
            ["value"] = new JsonObject { ["asset"] = mesh["asset"]!.DeepClone(), ["primitive"] = mesh["primitive"]!.DeepClone(), ["friction"] = .5, ["restitution"] = 0 }
        }));
    }
    public void RemoveMeshCollider()
    {
        RequireStopped(); RequireClean();
        if (Selected is null || !DraftComponents.ContainsKey("MeshCollider"))
            throw new InvalidOperationException("Select an object with a MeshCollider first.");
        Transact(new JsonArray(new JsonObject { ["op"] = "component.remove", ["id"] = Selected, ["type"] = "MeshCollider" }));
    }
    public JsonObject SkyDefaults() => Clone(Host.Call("world.describe")["components"]!["LightingEnvironment"]!["properties"]!["sky"]!["default"]!.AsObject());
    public string Create(string kind)
    {
        if (kind == "Environment") return CreateEnvironment();
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
    private string CreateEnvironment()
    {
        RequireStopped(); RequireClean();
        var existing = Entities.FirstOrDefault(entity => entity.Components.Contains("LightingEnvironment"));
        if (existing is not null) { Select(existing.Id); return existing.Id; }
        var environment = NewId(); var sun = NewId();
        var sky = SkyDefaults(); sky["enabled"] = true; sky["sun"] = sun;
        var pitch = -155 * Math.PI / 360; var yaw = -20 * Math.PI / 360;
        JsonObject Transform(JsonArray rotation) => new() { ["position"] = new JsonArray(0, 0, 0), ["rotation"] = rotation, ["scale"] = new JsonArray(1, 1, 1) };
        JsonObject Component(string id, string type, JsonObject value) => new() { ["op"] = "component.set", ["id"] = id, ["type"] = type, ["value"] = value };
        Transact(new JsonArray(
            new JsonObject { ["op"] = "entity.create", ["id"] = environment, ["name"] = "Environment" },
            Component(environment, "Transform", Transform(new JsonArray(0, 0, 0, 1))),
            Component(environment, "LightingEnvironment", new JsonObject { ["ambient"] = new JsonArray(.12, .14, .18), ["exposure"] = 1, ["sky"] = sky }),
            new JsonObject { ["op"] = "entity.create", ["id"] = sun, ["name"] = "Sun" },
            Component(sun, "Transform", Transform(new JsonArray(Math.Sin(pitch)*Math.Cos(yaw), Math.Cos(pitch)*Math.Sin(yaw), -Math.Sin(pitch)*Math.Sin(yaw), Math.Cos(pitch)*Math.Cos(yaw)))),
            Component(sun, "Light", new JsonObject { ["kind"] = "directional", ["color"] = new JsonArray(1, .95, .85), ["intensity"] = 3.5, ["enabled"] = true, ["shadow"] = new JsonObject { ["enabled"] = true } })));
        Select(environment); return environment;
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
        Gameplay?.RequireClean(); Components?.RequireClean();
        SceneChanging?.Invoke();
        if (RuntimeId is not null) Host.Call("desktop.play.stop", new() { ["session_id"] = RuntimeId });
        else
        {
            RequireClean();
            var parameters = new JsonObject { ["session_id"] = NewId(), ["revision"] = Revision };
            if (Gameplay is not null) parameters["expected_gameplay_generation"] = Gameplay.Generation;
            Host.Call("desktop.play.start", parameters);
        }
        Host.RefreshState();
    }
    public void Pause()
    {
        Components?.RequireClean();
        if (RuntimeId is null) throw new InvalidOperationException("Start playback first.");
        Host.Call(Paused ? "desktop.play.resume" : "desktop.play.pause", new() { ["session_id"] = RuntimeId });
        Host.RefreshState();
    }
    public void Step(int ticks = 1)
    {
        Components?.RequireClean();
        if (RuntimeId is null || !Paused) throw new InvalidOperationException("Pause playback before stepping.");
        Host.Call("desktop.play.step", new() { ["session_id"] = RuntimeId, ["request_id"] = NewId(), ["expected_tick"] = Tick, ["expected_structure_revision"] = Host.State["runtime"]!["structure_revision"]!.DeepClone(), ["ticks"] = ticks });
        Host.RefreshState();
    }
    public JsonObject InspectSelectedAnimation()
    {
        if (RuntimeId is null || Selected is null) throw new InvalidOperationException("Select a rig in a running simulation first.");
        return Host.Call("runtime.entity", new() { ["session_id"] = RuntimeId, ["id"] = Selected, ["tick"] = Tick });
    }
    public IReadOnlyList<AnimationClipChoice> AnimationClips(string asset)
    {
        var result = new List<AnimationClipChoice> { new(null, "Rest pose") };
        var parameters = new JsonObject { ["asset"] = asset, ["section"] = "animations", ["limit"] = 64 };
        do
        {
            var page = Host.Call("asset.inspect", parameters);
            foreach (var clip in page["items"]!.AsArray())
            {
                var index = clip!["index"]!.GetValue<int>();
                result.Add(new(index, $"{index} · {clip["name"]!.GetValue<string>()}"));
            }
            parameters["offset"] = page["next_offset"]?.DeepClone();
        } while (parameters["offset"] is not null);
        return result;
    }
    public void StepAnimation(string entity, string session, long observedTick, long observedStructure, JsonObject command, int blendTicks)
    {
        RequireClean();
        if (Selected != entity || RuntimeId != session) throw new InvalidOperationException("Animation selection or runtime changed. Load live state again.");
        if (!Paused) throw new InvalidOperationException("Pause playback before changing live animation.");
        if (blendTicks < 0 || blendTicks > 3600) throw new ArgumentOutOfRangeException(nameof(blendTicks));
        var value = Clone(command); value["entity"] = entity; value["blend_ticks"] = blendTicks;
        Host.Call("desktop.play.step", new() { ["session_id"] = session, ["request_id"] = NewId(),
            ["expected_tick"] = observedTick, ["expected_structure_revision"] = observedStructure, ["ticks"] = 1, ["animations"] = new JsonArray(value) });
        Host.RefreshState();
    }
    public void SetGameCamera(string? camera)
    {
        GameCameraChanging?.Invoke();
        Host.Call("desktop.game.camera", new() { ["camera"] = camera });
        gameCameraInitialized = true; Host.RefreshState();
    }
    public JsonObject InspectPlayback() => new() { ["state"] = PlaybackState, ["session_id"] = RuntimeId, ["tick"] = Tick,
        ["paused"] = Paused, ["suspended"] = PlaybackSuspended, ["last_error"] = PlaybackError,
        ["camera"] = GameCamera, ["draft_generation"] = DraftGeneration };
    public JsonObject InspectDraft() => new() { ["entity"] = Selected, ["name"] = DraftName, ["components"] = DraftComponents.DeepClone(), ["invalid_fields"] = new JsonArray(invalid.Order().Select(x => (JsonNode?)JsonValue.Create(x)).ToArray()), ["dirty"] = Dirty, ["conflict"] = Conflict, ["base_revision"] = BaseRevision, ["revision"] = Revision };
    public void Dispose() { Host.StateChanged -= HostChanged; SceneChanging = null; GameCameraChanging = null; }
}
