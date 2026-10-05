// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text.Json.Nodes;

namespace Poima.Editor;

// Session preferences only. Native owns output, logical voices and its worker.
public sealed class AudioEditorModel : IDisposable
{
    private readonly EditorModel editor;
    private bool disposed;
    private string baselineVolume = "1";
    public JsonObject Observation { get; private set; } = new();
    public JsonObject? Pending { get; private set; }
    public JsonObject? LastResult { get; private set; }
    public string? Error { get; private set; }
    public bool Enabled { get; private set; }
    public bool Muted { get; private set; }
    public string Volume { get; private set; } = "1";
    public long Generation => Observation["generation"]?.GetValue<long>() ?? 0;
    public JsonNode? Status => editor.Host.State["audio"];
    public long CurrentGeneration => Status?["generation"]?.GetValue<long>() ?? Generation;
    public bool Conflict => CurrentGeneration != Generation;
    public bool Dirty => Pending is not null || Volume != baselineVolume
        || Enabled != (Observation["config"]?["enabled"]?.GetValue<bool>() ?? false)
        || Muted != (Observation["config"]?["muted"]?.GetValue<bool>() ?? false);
    public event EventHandler? Changed;
    public AudioEditorModel(EditorModel editor)
    {
        this.editor = editor; Reload(); editor.Host.StateChanged += HostChanged;
    }
    private void Notify() { if (!disposed) Changed?.Invoke(this, EventArgs.Empty); }
    private void HostChanged(object? sender, EventArgs args)
    {
        if (!Dirty && Status is JsonObject state && state["generation"] is not null && CurrentGeneration != Generation) Adopt(state);
        Notify();
    }
    private void Adopt(JsonObject state)
    {
        Observation = new() { ["generation"] = state["generation"]!.DeepClone(), ["config"] = state["config"]!.DeepClone() };
        Enabled = state["config"]!["enabled"]!.GetValue<bool>(); Muted = state["config"]!["muted"]!.GetValue<bool>();
        baselineVolume = Volume = state["config"]!["volume"]!.GetValue<double>().ToString("G", CultureInfo.InvariantCulture);
    }
    public void Fail(Exception error) { Error = error.Message; editor.Note(error.Message); Notify(); }
    public void SetEnabled(bool value) { Enabled = value; Error = null; Notify(); }
    public void SetMuted(bool value) { Muted = value; Error = null; Notify(); }
    public void SetVolume(string value) { Volume = value; Error = null; Notify(); }
    public void Reload()
    {
        if (Pending is not null) throw new InvalidOperationException("Retry or dismiss the pending audio request before reloading.");
        Adopt(editor.Host.Call("desktop.audio.inspect")); Error = null; Notify();
    }
    public void Apply()
    {
        if (Pending is not null) throw new InvalidOperationException("Retry or dismiss the pending audio request before applying another change.");
        if (!double.TryParse(Volume, NumberStyles.Float, CultureInfo.InvariantCulture, out var volume) || !double.IsFinite(volume) || volume is < 0 or > 1)
            throw new ArgumentException("Volume must be a number from 0 to 1.");
        Pending = new() { ["request_id"] = EditorModel.NewId(), ["expected_generation"] = Generation,
            ["enabled"] = Enabled, ["muted"] = Muted, ["volume"] = volume };
        ExecutePending();
    }
    private void ExecutePending()
    {
        var reply = editor.Host.Call("desktop.audio.configure", Pending ?? throw new InvalidOperationException("No pending audio request."));
        LastResult = EditorModel.Clone(reply); Pending = null; Adopt(reply); Error = null; Notify();
    }
    public void RetryPending() => ExecutePending();
    public void DismissPending() { Pending = null; Error = null; Notify(); }
    public void RetryDevice()
    {
        if (Dirty) throw new InvalidOperationException("Apply or reload Audio changes before retrying output.");
        LastResult = editor.Host.Call("desktop.audio.retry"); Error = null; Notify();
    }
    public void ToggleMute()
    {
        if (Dirty) throw new InvalidOperationException("Apply or reload Audio changes before using the Game mute button.");
        // Read current preferences without advancing a tick. Generation guards a
        // concurrent agent edit; no stale full-config overwrite is possible.
        Reload(); Muted = !Muted; Apply();
    }
    public void RequireClean()
    {
        if (Dirty) throw new InvalidOperationException("Apply or reload Audio changes and resolve pending audio requests before closing.");
    }
    public JsonObject Inspect() => new() { ["observation"] = Observation.DeepClone(), ["status"] = Status?.DeepClone(),
        ["enabled"] = Enabled, ["muted"] = Muted, ["volume"] = Volume, ["generation"] = Generation,
        ["current_generation"] = CurrentGeneration, ["dirty"] = Dirty, ["conflict"] = Conflict,
        ["pending"] = Pending?.DeepClone(), ["last_result"] = LastResult?.DeepClone(), ["error"] = Error };
    public void Dispose() { disposed = true; editor.Host.StateChanged -= HostChanged; Changed = null; }
}
