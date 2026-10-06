// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;

namespace Poima.Editor;

/// <summary>Human save drafts and explicit observations; the native service owns every checkpoint.</summary>
public sealed class SaveEditorModel : IDisposable
{
    private readonly EditorModel editor;
    private string rootBaseline = "";
    private bool observationSuperseded;
    public event EventHandler? Changed;
    public string RootDraft { get; private set; } = "";
    public string SlotDraft { get; private set; } = "quick";
    public JsonObject ConfigObservation { get; private set; } = new();
    public JsonObject? SlotObservation { get; private set; }
    public JsonObject? RuntimeObservation { get; private set; }
    public JsonObject? GameplayObservation { get; private set; }
    public JsonObject? Pending { get; private set; }
    public JsonObject? LastResult { get; private set; }
    public string? Error { get; private set; }
    public long DraftVersion { get; private set; }
    public long Generation => ConfigObservation["generation"]?.GetValue<long>() ?? 0;
    public long CurrentGeneration => editor.Host.State["saves"]?["generation"]?.GetValue<long>() ?? Generation;
    public bool RootDirty => RootDraft != rootBaseline;
    public bool Dirty => RootDirty || Pending is not null;
    public bool ConfigConflict => Generation != CurrentGeneration;
    public bool Playing => editor.PlaybackState == "playing";
    public bool CanWrite => !Playing && editor.RuntimeId is not null && SlotObservation is not null;
    public bool CanLoad => !Playing && SlotObservation is not null;
    public bool AcknowledgeRecovery { get; private set; }
    public bool AllowRecovery { get; private set; }
    public bool UseConfiguredGameplay { get; private set; } = true;
    public bool ObservedRecovery => SlotObservation?["recovered"]?.GetValue<bool>() == true;
    public bool ObservationStale => SlotObservation is not null &&
        (observationSuperseded || ConfigConflict || SlotObservation["configuration_generation"]?.GetValue<long>() != Generation ||
         SlotDraft != SlotObservation["slot"]?.GetValue<string>() || RootDirty ||
         RuntimeObservation?["session_id"]?.GetValue<string>() != editor.RuntimeId ||
         RuntimeObservation?["tick"]?.GetValue<long>() != (editor.RuntimeId is null ? null : editor.Tick) ||
         RuntimeObservation?["control_sequence"]?.GetValue<long>() != editor.Host.State["runtime"]?["control_sequence"]?.GetValue<long>() ||
         RuntimeObservation?["ui_revision"]?.GetValue<long>() != editor.Host.State["runtime"]?["ui_revision"]?.GetValue<long>() ||
         RuntimeObservation?["structure_revision"]?.GetValue<long>() != editor.Host.State["runtime"]?["structure_revision"]?.GetValue<long>() ||
         RuntimeObservation?["authored_revision"]?.GetValue<long>() != editor.Revision ||
         RuntimeObservation?["gameplay_revision"]?.GetValue<long>() != editor.Host.State["gameplay"]?["runtime"]?["revision"]?.GetValue<long>() ||
         RuntimeObservation?["component_revision"]?.GetValue<long>() != (editor.RuntimeId is null ? null : editor.Host.State["components"]?["revision"]?.GetValue<long>()) ||
         GameplayObservation?["generation"]?.GetValue<long>() != editor.Host.State["gameplay"]?["generation"]?.GetValue<long>());

    public SaveEditorModel(EditorModel editor)
    {
        this.editor = editor;
        RefreshConfiguration();
        editor.Host.StateChanged += HostChanged;
    }
    private void HostChanged(object? sender, EventArgs args) => Notify();
    private void Notify() => Changed?.Invoke(this, EventArgs.Empty);
    public void Fail(Exception error) { Error = error.Message; editor.Note(error.Message); Notify(); }
    public void SetRoot(string value) { if (value == RootDraft) return; RootDraft = value; Error = null; ResetRecovery(); Notify(); }
    public void SetSlot(string value) { if (value == SlotDraft) return; SlotDraft = value; Error = null; ResetRecovery(); Notify(); }
    public void SetAcknowledgeRecovery(bool value)
    {
        if (value && (!ObservedRecovery || !ObservationMatchesSlot())) throw new InvalidOperationException("Inspect this recovered slot before acknowledging a recovery write.");
        AcknowledgeRecovery = value; Notify();
    }
    public void SetAllowRecovery(bool value)
    {
        if (value && (!ObservedRecovery || !ObservationMatchesSlot())) throw new InvalidOperationException("Inspect this recovered slot before allowing a recovered load.");
        AllowRecovery = value; Notify();
    }
    public void SetUseConfiguredGameplay(bool value) { UseConfiguredGameplay = value; Notify(); }
    private void ResetRecovery() { AcknowledgeRecovery = false; AllowRecovery = false; }
    public void RequireClean()
    {
        if (Dirty) throw new InvalidOperationException("Configure or refresh the Save root draft, and retry or dismiss any pending Save operation before closing.");
    }
    public void RefreshConfiguration()
    {
        var state = editor.Host.Call("save.status");
        ConfigObservation = EditorModel.Clone(state);
        RootDraft = rootBaseline = state["root"]?.GetValue<string>() ?? "";
        ResetRecovery(); Error = null; ++DraftVersion; Notify();
    }
    public void InspectSlot()
    {
        if (RootDirty) throw new InvalidOperationException("Configure or refresh the root draft before inspecting a slot.");
        // Keep observations intact if any part of the new inspection fails.
        var status = editor.Host.Call("save.status");
        if (!JsonNode.DeepEquals(status, ConfigObservation)) throw new InvalidOperationException("Save configuration changed. Refresh configuration explicitly before inspecting.");
        var slot = editor.Host.Call("save.inspect", new() { ["slot"] = SlotDraft });
        var world = editor.Host.Call("world.inspect");
        var runtime = editor.Host.Call("runtime.status");
        var gameplay = editor.Host.Call("desktop.gameplay.inspect");
        var components = editor.Host.Call("desktop.inspect")["components"];
        var active = runtime["active"]?.GetValue<bool>() == true;
        var structure = active ? editor.Host.Call("runtime.inspect", new() { ["session_id"] = runtime["session_id"]!.DeepClone() }) : null;
        var observation = new JsonObject
        {
            ["authored_revision"] = world["revision"]!.DeepClone(),
            ["session_id"] = active ? runtime["session_id"]?.DeepClone() : null,
            ["tick"] = active ? runtime["tick"]?.DeepClone() : null,
            ["gameplay_revision"] = active ? gameplay["runtime"]?["revision"]?.DeepClone() : null,
            ["component_revision"] = active ? components?["revision"]?.DeepClone() : null,
            ["structure_revision"] = structure?["structure_revision"]?.DeepClone(),
            ["ui_revision"] = structure?["ui_revision"]?.DeepClone(),
            ["control_sequence"] = structure?["control_sequence"]?.DeepClone()
        };
        SlotObservation = EditorModel.Clone(slot); RuntimeObservation = observation; GameplayObservation = EditorModel.Clone(gameplay);
        observationSuperseded = false; ResetRecovery(); Error = null; ++DraftVersion; Notify();
    }
    private void RequireIdle()
    {
        if (Playing) throw new InvalidOperationException("Pause playback before changing save storage or saving/loading a checkpoint.");
    }
    private void RequireNoPending()
    {
        if (Pending is not null) throw new InvalidOperationException("Retry the retained Save operation or explicitly dismiss it before creating another operation.");
    }
    public void Configure(bool clear = false)
    {
        RequireIdle(); RequireNoPending();
        var request = new JsonObject { ["request_id"] = EditorModel.NewId(), ["expected_generation"] = Generation,
            ["root"] = clear ? null : JsonValue.Create(RootDraft) };
        Execute("save.configure", request, null);
    }
    private bool ObservationMatchesSlot() => SlotObservation is not null && !RootDirty && !ConfigConflict &&
        SlotObservation["configuration_generation"]?.GetValue<long>() == Generation && SlotObservation["slot"]?.GetValue<string>() == SlotDraft;
    private void RequireOperationObservation()
    {
        RequireIdle(); RequireNoPending(); editor.RequireInspectorClean(); editor.Gameplay?.RequireClean();
        if (SlotObservation is null || RuntimeObservation is null) throw new InvalidOperationException("Inspect the slot to observe its generation and current runtime first.");
        if (!ObservationMatchesSlot()) throw new InvalidOperationException("The slot or storage root differs from the observation. Refresh configuration or inspect the intended slot explicitly.");
        // Stale runtime/slot guards intentionally reach native validation. Do not
        // replace them with current counters before a destructive operation.
    }
    public void Write()
    {
        RequireOperationObservation();
        if (RuntimeObservation!["session_id"] is null) throw new InvalidOperationException("Start and pause Play, then inspect the slot before saving.");
        var request = new JsonObject
        {
            ["request_id"] = EditorModel.NewId(), ["configuration_generation"] = Generation, ["slot"] = SlotDraft,
            ["expected_generation"] = SlotObservation!["generation"]!.DeepClone(),
            ["session_id"] = RuntimeObservation["session_id"]!.DeepClone(), ["expected_tick"] = RuntimeObservation["tick"]!.DeepClone(),
            ["expected_gameplay_revision"] = RuntimeObservation["gameplay_revision"]!.DeepClone(), ["acknowledge_recovery"] = AcknowledgeRecovery
        };
        request["expected_component_revision"] = RuntimeObservation["component_revision"]!.DeepClone();
        request["expected_structure_revision"] = RuntimeObservation["structure_revision"]!.DeepClone();
        request["expected_ui_revision"] = RuntimeObservation["ui_revision"]!.DeepClone();
        request["expected_control_sequence"] = RuntimeObservation["control_sequence"]!.DeepClone();
        Execute("save.write", request, rootBaseline);
    }
    public void Load()
    {
        RequireOperationObservation();
        var request = new JsonObject
        {
            ["request_id"] = EditorModel.NewId(), ["configuration_generation"] = Generation, ["slot"] = SlotDraft,
            ["expected_generation"] = SlotObservation!["generation"]!.DeepClone(), ["revision"] = RuntimeObservation!["authored_revision"]!.DeepClone(),
            ["expected_session_id"] = RuntimeObservation["session_id"]?.DeepClone(), ["expected_tick"] = RuntimeObservation["tick"]?.DeepClone(),
            ["expected_gameplay_revision"] = RuntimeObservation["gameplay_revision"]?.DeepClone(), ["new_session_id"] = EditorModel.NewId(),
            ["allow_recovery"] = AllowRecovery
        };
        if (RuntimeObservation["session_id"] is not null) request["expected_component_revision"] = RuntimeObservation["component_revision"]!.DeepClone();
        if (RuntimeObservation["session_id"] is not null) request["expected_structure_revision"] = RuntimeObservation["structure_revision"]!.DeepClone();
        if (RuntimeObservation["session_id"] is not null) request["expected_ui_revision"] = RuntimeObservation["ui_revision"]!.DeepClone();
        if (RuntimeObservation["session_id"] is not null) request["expected_control_sequence"] = RuntimeObservation["control_sequence"]!.DeepClone();
        if (RuntimeObservation["session_id"] is null)
        {
            var current = editor.Host.Call("desktop.gameplay.inspect");
            if (UseConfiguredGameplay && !JsonNode.DeepEquals(current["profile"], GameplayObservation?["profile"]))
                throw new InvalidOperationException("Trusted gameplay configuration changed. Inspect the slot again before loading.");
            JsonObject? profile = UseConfiguredGameplay && GameplayObservation?["profile"] is JsonObject configured ? EditorModel.Clone(configured) : null;
            profile?.Remove("values"); request["gameplay"] = profile;
        }
        Execute("save.load", request, rootBaseline);
    }
    private void Execute(string method, JsonObject parameters, string? root)
    {
        Pending = new() { ["method"] = method, ["params"] = EditorModel.Clone(parameters), ["root"] = root, ["root_draft"] = RootDraft };
        Notify();
        RunPending();
    }
    private void RunPending()
    {
        var pending = Pending ?? throw new InvalidOperationException("No retained Save operation.");
        var method = pending["method"]!.GetValue<string>();
        var result = editor.Host.Call(method, EditorModel.Clone((JsonObject)pending["params"]!));
        LastResult = EditorModel.Clone(result); Pending = null; Error = null;
        if (method is "save.write" or "save.load") observationSuperseded = true;
        // Refresh presentation only after native success. Keep the historical
        // slot/runtime observation until the user deliberately inspects again.
        try
        {
            editor.Host.RefreshState();
            if (method == "save.configure")
            {
                var independentDraft = RootDraft != pending["root_draft"]?.GetValue<string>() ? RootDraft : null;
                RefreshConfiguration();
                if (independentDraft is not null) SetRoot(independentDraft);
            }
            if (method == "save.load") editor.Refresh();
        }
        catch (Exception refreshError)
        {
            Error = $"{method} succeeded; the view refresh failed: {refreshError.Message}. The completed result is retained. Refresh explicitly before another operation.";
            editor.Note(Error);
        }
        Notify();
    }
    public void RetryPending()
    {
        RequireIdle();
        var pending = Pending ?? throw new InvalidOperationException("No retained Save operation.");
        editor.RequireInspectorClean(); editor.Gameplay?.RequireClean();
        var method = pending["method"]!.GetValue<string>();
        if (method == "save.write")
        {
            // The durable receipt excludes this session-local routing counter.
            // Never let a retained request address a different storage root.
            var status = editor.Host.Call("save.status");
            if (RootDirty || ConfigConflict || !JsonNode.DeepEquals(status, ConfigObservation) ||
                status["root"]?.GetValue<string>() != pending["root"]?.GetValue<string>())
                throw new InvalidOperationException("Refresh the same configured storage root before retrying this checkpoint write.");
            pending["params"]!["configuration_generation"] = Generation;
        }
        RunPending();
    }
    public void DiscardPending() { Pending = null; Error = null; Notify(); }
    public JsonObject Inspect() => new()
    {
        ["root_draft"] = RootDraft, ["slot_draft"] = SlotDraft, ["config_observation"] = ConfigObservation.DeepClone(),
        ["slot_observation"] = SlotObservation?.DeepClone(), ["runtime_observation"] = RuntimeObservation?.DeepClone(),
        ["gameplay_observation"] = GameplayObservation?.DeepClone(), ["generation"] = Generation, ["current_generation"] = CurrentGeneration,
        ["root_dirty"] = RootDirty, ["dirty"] = Dirty, ["config_conflict"] = ConfigConflict, ["observation_stale"] = ObservationStale,
        ["pending"] = Pending?.DeepClone(), ["last_result"] = LastResult?.DeepClone(), ["error"] = Error,
        ["acknowledge_recovery"] = AcknowledgeRecovery, ["allow_recovery"] = AllowRecovery, ["use_configured_gameplay"] = UseConfiguredGameplay
    };
    public void Dispose() => editor.Host.StateChanged -= HostChanged;
}
