// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text.Json.Nodes;

namespace Poima.Editor;

/// <summary>Uncommitted human drafts; all execution and committed state belong to the native service.</summary>
public sealed class GameplayEditorModel : IDisposable
{
    private readonly EditorModel editor;
    private JsonObject? profile;
    private readonly Dictionary<string, string> configBaseline = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> valuesBaseline = new(StringComparer.Ordinal);
    private bool refreshing;
    private string reloadBaseline = "";
    public event EventHandler? Changed;
    public Dictionary<string, string> Configuration { get; } = new(StringComparer.Ordinal);
    public Dictionary<string, string> Values { get; } = new(StringComparer.Ordinal);
    public JsonArray Fields { get; private set; } = new();
    public JsonObject? Observation { get; private set; }
    public long Generation { get; private set; }
    public long CurrentGeneration => editor.Host.State["gameplay"]?["generation"]?.GetValue<long>() ?? Generation;
    public long DraftVersion { get; private set; }
    public string ReloadAssembly { get; set; } = "";
    public string? Error { get; private set; }
    public bool Configured => profile is not null;
    public bool ConfigDirty => Configuration.Any(pair => configBaseline.GetValueOrDefault(pair.Key) != pair.Value);
    public bool ValuesDirty => Values.Any(pair => valuesBaseline.GetValueOrDefault(pair.Key) != pair.Value);
    public bool ReloadDirty => ReloadAssembly != reloadBaseline;
    public bool Dirty => ConfigDirty || ValuesDirty || ReloadDirty;
    public bool ConfigConflict => Generation != CurrentGeneration;
    public bool LiveConflict => Observation is not null &&
        (Observation["session_id"]?.GetValue<string>() != editor.RuntimeId || Observation["tick"]?.GetValue<long>() != editor.Tick ||
         Observation["structure_revision"]?.GetValue<long>() != editor.Host.State["runtime"]?["structure_revision"]?.GetValue<long>() ||
         Observation["revision"]?.GetValue<long>() != editor.Host.State["gameplay"]?["runtime"]?["revision"]?.GetValue<long>());
    public bool Paused => editor.RuntimeId is not null && editor.Paused;
    public bool Stopped => editor.RuntimeId is null;
    public GameplayEditorModel(EditorModel editor)
    {
        this.editor = editor;
        RevertConfiguration();
        editor.Host.StateChanged += HostChanged;
    }
    private void HostChanged(object? sender, EventArgs args)
    {
        if (editor.Host.State["closing"]?.GetValue<bool>() == true) return;
        if (CurrentGeneration != Generation && !Dirty) { RevertConfiguration(); return; }
        // Full schema/values are fetched only by explicit user actions. Tick
        // notifications update status without replacing text or allocating fields.
        Changed?.Invoke(this, EventArgs.Empty);
    }
    private void Notify() { Changed?.Invoke(this, EventArgs.Empty); }
    public void SetConfiguration(string name, string text) { Configuration[name] = text; Error = null; Notify(); }
    public void SetValue(string name, string text) { Values[name] = text; Error = null; Notify(); }
    public void SetReloadAssembly(string text) { ReloadAssembly = text; Error = null; Notify(); }
    public void Fail(Exception error) { Error = error.Message; editor.Note(error.Message); Notify(); }
    public void RequireClean()
    {
        if (Dirty) throw new InvalidOperationException("Apply or explicitly revert C# Gameplay drafts first.");
    }
    public void RevertConfiguration()
    {
        var state = editor.Host.Call("desktop.gameplay.inspect");
        Generation = state["generation"]!.GetValue<long>();
        profile = state["profile"] is JsonObject value ? EditorModel.Clone(value) : null;
        Configuration.Clear();
        Configuration["Assembly"] = profile?["assembly"]?.GetValue<string>() ?? "";
        Configuration["Game type"] = profile?["type"]?.GetValue<string>() ?? "";
        Configuration["Host runtime"] = profile?["hostfxr"]?.GetValue<string>() ?? Path.Combine(AppContext.BaseDirectory, "hostfxr.dll");
        Configuration["Managed bridge"] = profile?["bridge"]?.GetValue<string>() ?? Path.Combine(AppContext.BaseDirectory, "gameplay", "Poima.ManagedBridge.dll");
        Configuration["Initial values JSON"] = profile?["values"]?.ToJsonString() ?? "{}";
        configBaseline.Clear(); foreach (var pair in Configuration) configBaseline.Add(pair.Key, pair.Value);
        if (!ReloadDirty || ReloadAssembly.Length == 0) ReloadAssembly = reloadBaseline = Configuration["Assembly"];
        Error = null; ++DraftVersion; Notify();
    }
    public void Configure(bool clear = false)
    {
        if (!Stopped) throw new InvalidOperationException("Stop playback before configuring its launch profile.");
        editor.RequireInspectorClean();
        if (ValuesDirty) throw new InvalidOperationException("Revert the previous live gameplay draft first.");
        if (ReloadDirty) throw new InvalidOperationException("Revert the independent reload assembly draft first.");
        JsonObject? candidate = null;
        if (!clear)
        {
            var text = Configuration["Initial values JSON"];
            if (text.Length > 65536) throw new InvalidOperationException("Initial values JSON exceeds 64 KiB.");
            var values = JsonNode.Parse(text) as JsonObject ?? throw new InvalidOperationException("Initial values must be a JSON object.");
            candidate = new() { ["hostfxr"] = Configuration["Host runtime"], ["bridge"] = Configuration["Managed bridge"],
                ["assembly"] = Configuration["Assembly"], ["type"] = Configuration["Game type"], ["values"] = values };
        }
        editor.Host.Call("desktop.gameplay.configure", new() { ["request_id"] = EditorModel.NewId(), ["expected_generation"] = Generation, ["profile"] = candidate });
        editor.Host.RefreshState(); RevertConfiguration();
    }
    public void RefreshValues()
    {
        ReadValues(true);
    }
    private void ReadValues(bool discardReload)
    {
        if (refreshing) return;
        refreshing = true;
        try
        {
            JsonObject? observation = editor.RuntimeId is null ? null : editor.Host.Call("runtime.gameplay.inspect", new()
                { ["session_id"] = editor.RuntimeId, ["tick"] = editor.Tick, ["include_schema"] = true });
            Observation = observation;
            if (discardReload || !ReloadDirty)
                ReloadAssembly = reloadBaseline = observation?["module"]?["assembly"]?.GetValue<string>() ?? Configuration["Assembly"];
            Fields = observation?["module"]?["schema"]?["fields"] is JsonArray fields ? (JsonArray)fields.DeepClone() : new();
            Values.Clear(); valuesBaseline.Clear();
            foreach (var item in Fields)
            {
                var name = item!["name"]!.GetValue<string>(); var value = observation!["module"]!["values"]![name]!;
                var text = value is JsonValue scalar && scalar.TryGetValue<string>(out var stringValue) ? stringValue : value.ToJsonString();
                Values[name] = text; valuesBaseline[name] = text;
            }
            Error = null; ++DraftVersion;
        }
        finally { refreshing = false; }
        Notify();
    }
    private void RequireLive()
    {
        editor.RequireInspectorClean();
        if (!Paused) throw new InvalidOperationException("Pause playback before editing or reloading C# gameplay.");
        if (ConfigDirty || ConfigConflict) throw new InvalidOperationException("Revert stale or unapplied launch configuration first.");
        if (Observation is null || Observation["session_id"]?.GetValue<string>() != editor.RuntimeId)
            throw new InvalidOperationException("Load live values for the current runtime first.");
    }
    private static JsonNode ParseField(JsonObject field, string text)
    {
        var name = field["name"]!.GetValue<string>(); var kind = field["kind"]!.GetValue<string>();
        switch (kind)
        {
            case "int32": if (int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var i)) return JsonValue.Create(i)!; break;
            case "int64": if (long.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var l)) return JsonValue.Create(l.ToString(CultureInfo.InvariantCulture))!; break;
            case "float32": if (float.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var f) && float.IsFinite(f)) return JsonValue.Create(f)!; break;
            case "float64": if (double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) && double.IsFinite(d)) return JsonValue.Create(d)!; break;
            case "entity": if (text.Length == 32 && text.All(Uri.IsHexDigit)) return JsonValue.Create(text.ToLowerInvariant())!; break;
        }
        throw new InvalidOperationException($"{name} must be a valid {kind}" + (kind.StartsWith("float", StringComparison.Ordinal) ? " (finite number)." : kind == "entity" ? " (32 hexadecimal digits)." : "."));
    }
    public bool Invalid(string name)
    {
        var field = Fields.OfType<JsonObject>().FirstOrDefault(item => item["name"]?.GetValue<string>() == name);
        if (field is null) return false;
        try { _ = ParseField(field, Values[name]); return false; } catch (InvalidOperationException) { return true; }
    }
    public void ApplyValues()
    {
        RequireLive();
        var patch = new JsonObject();
        foreach (var field in Fields.OfType<JsonObject>())
        {
            var name = field["name"]!.GetValue<string>();
            if (Values[name] != valuesBaseline[name]) patch[name] = ParseField(field, Values[name]);
        }
        if (patch.Count == 0) throw new InvalidOperationException("No changed gameplay values to apply.");
        editor.Host.Call("runtime.gameplay.edit", new() { ["session_id"] = Observation!["session_id"]!.DeepClone(),
            ["request_id"] = EditorModel.NewId(), ["expected_tick"] = Observation["tick"]!.DeepClone(), ["expected_structure_revision"] = Observation["structure_revision"]!.DeepClone(),
            ["expected_revision"] = Observation["revision"]!.DeepClone(), ["values"] = patch });
        editor.Host.RefreshState(); ReadValues(false);
    }
    public void Reload()
    {
        RequireLive();
        if (ValuesDirty) throw new InvalidOperationException("Apply or revert live gameplay values before reloading the assembly.");
        if (profile is null) throw new InvalidOperationException("Configure a launch profile first.");
        editor.Host.Call("runtime.gameplay.load", new() { ["session_id"] = Observation!["session_id"]!.DeepClone(),
            ["request_id"] = EditorModel.NewId(), ["expected_tick"] = Observation["tick"]!.DeepClone(), ["expected_structure_revision"] = Observation["structure_revision"]!.DeepClone(),
            ["expected_revision"] = Observation["revision"]!.DeepClone(), ["hostfxr"] = profile["hostfxr"]!.DeepClone(),
            ["bridge"] = profile["bridge"]!.DeepClone(), ["assembly"] = ReloadAssembly, ["type"] = profile["type"]!.DeepClone(), ["values"] = new JsonObject() });
        editor.Host.RefreshState(); RefreshValues();
    }
    public JsonObject Inspect() => new() { ["generation"] = Generation, ["current_generation"] = CurrentGeneration,
        ["config_dirty"] = ConfigDirty, ["config_conflict"] = ConfigConflict, ["values_dirty"] = ValuesDirty,
        ["reload_dirty"] = ReloadDirty, ["live_conflict"] = LiveConflict, ["error"] = Error, ["observation"] = Observation?.DeepClone() };
    public void Dispose() { editor.Host.StateChanged -= HostChanged; Changed = null; }
}
