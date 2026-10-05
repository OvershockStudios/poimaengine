// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;

namespace Poima.Editor;

// Presentation-only observation. Native owns recording and capture identity.
public sealed class ProfilerModel : IDisposable
{
    private readonly EditorModel editor;
    private bool disposed;
    private int loadVersion;
    private JsonObject? pendingStart;
    public event EventHandler? Changed;
    public JsonObject Status { get; private set; } = new();
    public JsonObject? Summary { get; private set; }
    public JsonArray Events { get; private set; } = new();
    public string? LoadedCapture { get; private set; }
    public JsonObject? Selected { get; private set; }
    public string? Error { get; private set; }
    public bool Loading { get; private set; }
    public string State => Status["state"]?.GetValue<string>() ?? "empty";
    public string? Capture => Status["capture_id"]?.GetValue<string>();
    public bool Stale => LoadedCapture is not null && LoadedCapture != Capture;
    public ProfilerModel(EditorModel editor) { this.editor = editor; RefreshStatus(); }
    private void Notify() { if (!disposed) Changed?.Invoke(this, EventArgs.Empty); }
    public void Fail(Exception error) { Error = error.Message; editor.Note(error.Message); Notify(); }
    public void ObserveStatus()
    {
        Status = editor.Host.Call("profiler.status"); Notify();
    }
    public void RefreshStatus()
    {
        Status = editor.Host.Call("profiler.status"); Error = null; Notify();
    }
    public void Record(int capacity = 16384)
    {
        if (Loading) throw new InvalidOperationException("Wait for capture loading to finish.");
        // Retain uncertain start parameters; a retry cannot accidentally create a second capture.
        pendingStart ??= new() { ["capture_id"] = EditorModel.NewId(), ["expected_capture_id"] = Capture, ["capacity"] = capacity };
        Status = editor.Host.Call("profiler.start", pendingStart);
        pendingStart = null; Error = null; Notify();
    }
    public void DiscardStart() { pendingStart = null; RefreshStatus(); }
    public async Task StopAsync()
    {
        var capture = Capture ?? throw new InvalidOperationException("Refresh to observe a capture first.");
        Status = editor.Host.Call("profiler.stop", new() { ["capture_id"] = capture });
        Error = null; Notify(); await LoadAsync();
    }
    public async Task LoadAsync()
    {
        if (Loading) throw new InvalidOperationException("Capture loading is already in progress.");
        RefreshStatus();
        if (State is not ("stopped" or "full")) return;
        if ((Status["open"]?.GetValue<long>() ?? 0) != 0) throw new InvalidOperationException("Capture scopes are still completing. Refresh again once they have closed.");
        var capture = Capture!; var version = ++loadVersion; Loading = true; Notify();
        try
        {
            var summary = editor.Host.Call("profiler.summary", new() { ["capture_id"] = capture });
            var events = new JsonArray(); int offset = 0;
            while (true)
            {
                if (disposed || version != loadVersion) return;
                var page = editor.Host.Call("profiler.events", new() { ["capture_id"] = capture, ["offset"] = offset, ["limit"] = 1024 });
                if (page["capture_id"]?.GetValue<string>() != capture || page["total"]!.GetValue<int>() > 65536)
                    throw new InvalidOperationException("Unexpected capture page identity or size.");
                foreach (var entry in page["events"]!.AsArray()) events.Add(entry!.DeepClone());
                if (events.Count > 65536) throw new InvalidOperationException("Capture exceeds the display budget.");
                if (page["next_offset"] is not { } next) break;
                var nextOffset = next.GetValue<int>();
                if (nextOffset <= offset || nextOffset != events.Count) throw new InvalidOperationException("Capture pagination did not progress.");
                offset = nextOffset;
                // Yield between bounded native pages; never call the world from a worker.
                await Task.Delay(1);
            }
            if (disposed || version != loadVersion) return;
            RefreshStatus();
            if (Capture != capture || State is not ("stopped" or "full"))
                throw new InvalidOperationException("Capture changed elsewhere while loading. Refresh to inspect the new capture.");
            Events = events; Summary = summary; LoadedCapture = capture; Selected = null; Error = null;
        }
        finally { Loading = false; Notify(); }
    }
    public void Select(long id)
    {
        Selected = Events.OfType<JsonObject>().FirstOrDefault(value => value["id"]!.GetValue<long>() == id)
            ?? throw new ArgumentException("Event is not in the loaded capture.");
        Notify();
    }
    public JsonObject Export(string destination)
    {
        var capture = LoadedCapture ?? throw new InvalidOperationException("Load a sealed capture first.");
        var reply = editor.Host.Call("profiler.export", new() { ["capture_id"] = capture });
        if (reply["capture_id"]?.GetValue<string>() != capture) throw new InvalidOperationException("Export capture identity changed.");
        var path = Path.GetFullPath(destination);
        if (!Directory.Exists(Path.GetDirectoryName(path))) throw new ArgumentException("Choose an existing export directory.");
        using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        using var writer = new System.Text.Json.Utf8JsonWriter(output);
        reply["trace"]!.WriteTo(writer); writer.Flush(); output.Flush(true);
        return new() { ["capture_id"] = capture, ["path"] = path, ["bytes"] = output.Length };
    }
    public JsonObject Inspect() => new() { ["status"] = Status.DeepClone(), ["loaded_capture"] = LoadedCapture,
        ["loaded_events"] = Events.Count, ["loading"] = Loading, ["stale"] = Stale, ["error"] = Error,
        ["selected"] = Selected?.DeepClone(), ["summary"] = Summary?.DeepClone(), ["pending_start"] = pendingStart?.DeepClone() };
    public void Dispose() { disposed = true; ++loadVersion; Changed = null; }
}
