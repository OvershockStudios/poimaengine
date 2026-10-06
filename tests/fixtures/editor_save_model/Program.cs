// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
namespace Poima.Editor;
public sealed class FakeHost
{
    public JsonObject State { get; } = JsonNode.Parse("""{"runtime":{"control_sequence":0,"ui_revision":0,"structure_revision":0},"components":{"revision":0},"gameplay":{"generation":0,"runtime":{"revision":0}},"saves":{"generation":0}}""")!.AsObject();
    public bool Active = true, FailWrite;
    public JsonObject? Sent;
    public event EventHandler? StateChanged;
    public void RefreshState() => StateChanged?.Invoke(this, EventArgs.Empty);
    public JsonObject Call(string method, JsonObject? request = null)
    {
        if (method is "save.write" or "save.load") { Sent = EditorModel.Clone(request!); if (FailWrite) throw new InvalidOperationException("transport interrupted"); return new(); }
        return method switch
        {
            "save.status" => new() { ["generation"] = 0L, ["root"] = "D:/saves" },
            "save.inspect" => new() { ["generation"] = 0L, ["configuration_generation"] = 0L, ["slot"] = "quick" },
            "world.inspect" => new() { ["revision"] = 0L },
            "runtime.status" => new() { ["active"] = Active, ["session_id"] = Active ? "session" : null, ["tick"] = Active ? JsonValue.Create(0L) : null },
            "runtime.inspect" => new() { ["control_sequence"] = State["runtime"]!["control_sequence"]?.DeepClone(), ["structure_revision"] = 0L, ["ui_revision"] = State["runtime"]!["ui_revision"]?.DeepClone() },
            "desktop.gameplay.inspect" => EditorModel.Clone(State["gameplay"]!.AsObject()),
            "desktop.inspect" => EditorModel.Clone(State),
            _ => throw new InvalidOperationException(method)
        };
    }
}
public sealed class FakeGameplay { public void RequireClean() {} }
public sealed class EditorModel
{
    public FakeHost Host { get; } = new();
    public FakeGameplay? Gameplay => null;
    public string PlaybackState => "paused";
    public string? RuntimeId => Host.Active ? "session" : null;
    public long Tick => 0;
    public long Revision => 0;
    public static JsonObject Clone(JsonObject value) => value.DeepClone().AsObject();
    public static string NewId() => Guid.NewGuid().ToString("N");
    public void RequireInspectorClean() {}
    public void Note(string value) {}
    public void Refresh() {}
}
internal static class Program
{
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); }
    static int Main()
    {
        var editor = new EditorModel(); using var model = new SaveEditorModel(editor);
        model.InspectSlot(); Check(!model.ObservationStale, "Fresh observation was stale.");
        editor.Host.State["runtime"]!["control_sequence"] = 1L;
        Check(model.ObservationStale, "Same-tick control without a UI change did not stale save observation.");
        editor.Host.State["runtime"]!["control_sequence"] = 0L;
        editor.Host.State["runtime"]!["ui_revision"] = 1L;
        Check(model.ObservationStale, "Same-tick logical UI edit did not stale save observation.");
        editor.Host.FailWrite = true;
        try { model.Write(); throw new Exception("Missing simulated failure."); } catch (InvalidOperationException) {}
        Check(editor.Host.Sent!["expected_ui_revision"]!.GetValue<long>() == 0, "Write silently refreshed stale UI guard.");
        Check(editor.Host.Sent!["expected_control_sequence"]!.GetValue<long>() == 0, "Write refreshed original control guard.");
        editor.Host.State["runtime"]!["control_sequence"] = 2L;
        editor.Host.State["runtime"]!["ui_revision"] = 2L; editor.Host.FailWrite = false;
        model.RetryPending(); Check(editor.Host.Sent!["expected_ui_revision"]!.GetValue<long>() == 0, "Retry replaced original UI guard.");
        Check(editor.Host.Sent!["expected_control_sequence"]!.GetValue<long>() == 0, "Retry replaced original control sequence.");
        model.InspectSlot(); model.Load(); Check(editor.Host.Sent!["expected_ui_revision"]!.GetValue<long>() == 2, "Active load omitted observed UI revision.");
        Check(editor.Host.Sent!["expected_control_sequence"]!.GetValue<long>() == 2, "Active load omitted original control sequence.");
        editor.Host.State["runtime"]!["control_sequence"] = null;
        editor.Host.Active = false; editor.Host.State["runtime"]!["ui_revision"] = null; editor.Host.State["runtime"]!["structure_revision"] = null; editor.Host.State["gameplay"]!["runtime"] = null;
        model.InspectSlot(); Check(!model.ObservationStale, "Stopped observation was stale.");
        model.Load(); Check(!editor.Host.Sent!.ContainsKey("expected_ui_revision"), "Stopped load sent an active UI guard.");
        Check(!editor.Host.Sent!.ContainsKey("expected_control_sequence"), "Stopped load sent active control guard.");
        Console.WriteLine("Desktop save UI revision freshness, frozen retry guards and stopped replacement passed."); return 0;
    }
}
