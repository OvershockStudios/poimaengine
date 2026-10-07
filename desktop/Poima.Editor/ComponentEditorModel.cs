// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Poima.Editor;

/// <summary>Field text stays human-owned; native declarations and validators own committed data.</summary>
public static class ComponentFields
{
    public const string Unset = "00000000000000000000000000000000";
    public static string Text(JsonNode? value) => value is JsonValue scalar && scalar.TryGetValue<string>(out var text) ? text : value?.ToJsonString() ?? "";
    public static JsonNode Parse(JsonObject field, string text)
    {
        var kind = field["kind"]!.GetValue<string>();
        switch (kind)
        {
            case "array": return ParseArray(field, text);
            case "int32": if (int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var i)) return JsonValue.Create(i)!; break;
            case "int64": if (long.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var l)) return JsonValue.Create(l.ToString(CultureInfo.InvariantCulture))!; break;
            case "float32": if (float.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var f) && float.IsFinite(f)) return JsonValue.Create(f == 0 ? 0f : f)!; break;
            case "float64": if (double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) && double.IsFinite(d)) return JsonValue.Create(d == 0 ? 0d : d)!; break;
            case "entity": if (text.Length == 32 && text.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f')) return JsonValue.Create(text)!; break;
        }
        throw new InvalidOperationException($"{field["name"]} must be a valid {kind}. Floating values must be finite.");
    }
    private static JsonArray ParseArray(JsonObject field, string text)
    {
        const string invalid = "Array values require bounded JSON with the declared scalar element kind.";
        try
        {
            if (text.Length > 16 * 1024 || field["capacity"] is not JsonValue capacityValue ||
                !capacityValue.TryGetValue<int>(out var capacity) || capacity is < 1 or > 31)
                throw new InvalidOperationException(invalid);
            var kind = field["element_kind"]?.GetValue<string>();
            if (kind is not ("int32" or "int64" or "float32" or "float64" or "entity"))
                throw new InvalidOperationException(invalid);
            using var document = JsonDocument.Parse(text, new JsonDocumentOptions { MaxDepth = 2 });
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Array || root.GetArrayLength() > capacity)
                throw new InvalidOperationException(invalid);
            var result = new JsonArray();
            foreach (var item in root.EnumerateArray())
            {
                switch (kind)
                {
                    case "int32" when item.ValueKind == JsonValueKind.Number && item.TryGetInt32(out var integer):
                        result.Add(integer); break;
                    case "int64" when item.ValueKind == JsonValueKind.String:
                        var decimalText = item.GetString()!;
                        if (!long.TryParse(decimalText, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var wide) ||
                            wide.ToString(CultureInfo.InvariantCulture) != decimalText) throw new InvalidOperationException(invalid);
                        result.Add(decimalText); break;
                    case "float32" when item.ValueKind == JsonValueKind.Number && item.TryGetSingle(out var single) && float.IsFinite(single):
                        result.Add(single == 0 ? 0f : single); break;
                    case "float64" when item.ValueKind == JsonValueKind.Number && item.TryGetDouble(out var number) && double.IsFinite(number):
                        result.Add(number == 0 ? 0d : number); break;
                    case "entity" when item.ValueKind == JsonValueKind.String:
                        var entity = item.GetString()!;
                        if (entity.Length != 32 || !entity.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f'))
                            throw new InvalidOperationException(invalid);
                        result.Add(entity); break;
                    default: throw new InvalidOperationException(invalid);
                }
            }
            return result;
        }
        catch (JsonException error) { throw new InvalidOperationException(invalid, error); }
    }
    public static JsonObject Defaults(JsonObject schema)
    {
        var values = new JsonObject();
        foreach (var field in schema["fields"]!.AsArray().OfType<JsonObject>()) values[field["id"]!.GetValue<string>()] = field["default"]!.DeepClone();
        return values;
    }
}

/// <summary>Paused live component values are deliberately separate from authored Inspector drafts.</summary>
public sealed class ComponentEditorModel : IDisposable
{
    private readonly EditorModel editor;
    private readonly Dictionary<string, string> baseline = new(StringComparer.Ordinal);
    public event EventHandler? Changed;
    public JsonObject? Observation { get; private set; }
    public JsonArray RuntimeSchemas { get; private set; } = new();
    public string? SchemaSession { get; private set; }
    public Dictionary<string, string> Values { get; } = new(StringComparer.Ordinal);
    public long DraftVersion { get; private set; }
    public string? Error { get; private set; }
    public bool Dirty => Values.Any(value => baseline.GetValueOrDefault(value.Key) != value.Value);
    public bool Conflict => Observation is not null && (Observation["session_id"]?.GetValue<string>() != editor.RuntimeId ||
        Observation["tick"]?.GetValue<long>() != editor.Tick || Observation["structure_revision"]?.GetValue<long>() != editor.Host.State["runtime"]?["structure_revision"]?.GetValue<long>() || Observation["component_revision"]?.GetValue<long>() != editor.Host.State["components"]?["revision"]?.GetValue<long>());
    public ComponentEditorModel(EditorModel editor) { this.editor = editor; editor.Host.StateChanged += HostChanged; }
    private void HostChanged(object? sender, EventArgs args) => Changed?.Invoke(this, EventArgs.Empty);
    public void Fail(Exception error) { Error = error.Message; editor.Note(error.Message); Changed?.Invoke(this, EventArgs.Empty); }
    public void RequireClean() { if (Dirty) throw new InvalidOperationException("Apply or explicitly reload/discard live component changes first."); }
    public void Set(string field, string value) { Values[field] = value; Error = null; Changed?.Invoke(this, EventArgs.Empty); }
    public bool Invalid(JsonObject field)
    {
        try { _ = ComponentFields.Parse(field, Values[field["id"]!.GetValue<string>()]); return false; }
        catch (InvalidOperationException) { return true; }
    }
    public void RefreshTypes()
    {
        RequireClean();
        if (editor.RuntimeId is null || !editor.Paused) throw new InvalidOperationException("Pause playback before discovering runtime components.");
        var result = editor.Host.Call("runtime.components", new() { ["session_id"] = editor.RuntimeId });
        RuntimeSchemas = (JsonArray)result["schemas"]!.DeepClone(); SchemaSession = editor.RuntimeId;
        Error = null; ++DraftVersion; Changed?.Invoke(this, EventArgs.Empty);
    }
    public void Load(string entity, string type)
    {
        editor.RequireAuthoredClean();
        if (Dirty && (Observation?["id"]?.GetValue<string>() != entity || Observation?["type"]?.GetValue<string>() != type)) RequireClean();
        if (editor.RuntimeId is null || !editor.Paused) throw new InvalidOperationException("Pause playback before inspecting live components.");
        var candidate = editor.Host.Call("runtime.component.get", new() { ["session_id"] = editor.RuntimeId, ["tick"] = editor.Tick, ["id"] = entity, ["type"] = type });
        if (candidate["values"] is not JsonObject values) throw new InvalidOperationException("This component is absent from the frozen runtime entity.");
        Observation = candidate; Values.Clear(); baseline.Clear();
        foreach (var value in values) Values[value.Key] = baseline[value.Key] = ComponentFields.Text(value.Value);
        Error = null; ++DraftVersion; Changed?.Invoke(this, EventArgs.Empty);
    }
    public void Reload()
    {
        if (Observation is null) return;
        if (Observation["session_id"]?.GetValue<string>() != editor.RuntimeId) throw new InvalidOperationException("The observed runtime was replaced. Discard this draft before loading another runtime.");
        Load(Observation["id"]!.GetValue<string>(), Observation["type"]!.GetValue<string>());
    }
    public void Discard() { Observation = null; Values.Clear(); baseline.Clear(); Error = null; ++DraftVersion; Changed?.Invoke(this, EventArgs.Empty); }
    public void Apply()
    {
        editor.RequireAuthoredClean(); editor.Gameplay?.RequireClean();
        if (!editor.Paused || Observation is null || Observation["session_id"]?.GetValue<string>() != editor.RuntimeId)
            throw new InvalidOperationException("Load live component values for the current paused runtime first.");
        if (!Dirty) return;
        var values = new JsonObject();
        foreach (var field in Observation["schema"]!["fields"]!.AsArray().OfType<JsonObject>()) values[field["id"]!.GetValue<string>()] = ComponentFields.Parse(field, Values[field["id"]!.GetValue<string>()]);
        editor.Host.Call("runtime.component.edit", new() { ["session_id"] = Observation["session_id"]!.DeepClone(), ["request_id"] = EditorModel.NewId(),
            ["expected_tick"] = Observation["tick"]!.DeepClone(), ["expected_structure_revision"] = Observation["structure_revision"]!.DeepClone(), ["expected_revision"] = Observation["component_revision"]!.DeepClone(),
            ["id"] = Observation["id"]!.DeepClone(), ["type"] = Observation["type"]!.DeepClone(), ["values"] = values });
        editor.Host.RefreshState(); Reload();
    }
    public JsonObject Inspect() => new() { ["observation"] = Observation?.DeepClone(), ["dirty"] = Dirty, ["conflict"] = Conflict, ["error"] = Error,
        ["draft_version"] = DraftVersion, ["schema_session"] = SchemaSession, ["schemas"] = RuntimeSchemas.DeepClone(), ["values"] = new JsonObject(Values.Select(pair => new KeyValuePair<string, JsonNode?>(pair.Key, JsonValue.Create(pair.Value)))) };
    public void Dispose() { editor.Host.StateChanged -= HostChanged; Changed = null; }
}
