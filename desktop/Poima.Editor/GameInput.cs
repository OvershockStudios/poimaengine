// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;

namespace Poima.Editor;

// The GUI owns focus and physical device events. Bindings and fixed-tick
// consumption live in the native bridge, shared with external clients.
public sealed class GameInput : IViewportInteraction
{
    private readonly EditorModel model;
    private ViewportInput? viewport;
    private ViewportModifiers lastModifiers;
    private int lastX, lastY;
    private string? reportedError;
    public IntPtr Window { get; private set; }
    public long ErrorCount { get; private set; }
    public event Action<string>? Error;
    public GameInput(EditorModel model)
    {
        this.model = model; model.SceneChanging += Release; model.GameCameraChanging += Release;
    }
    private void Guard(Action operation)
    {
        try { operation(); }
        catch (Exception error)
        {
            try { Release(); } catch (Exception cleanup) { model.Note(cleanup.Message); }
            ++ErrorCount; model.Note(error.Message); Error?.Invoke(error.Message);
        }
    }
    private string? capturedSession;
    private bool releasing;
    private Dictionary<uint, string>? keyboard;
    private string? profilePath;
    private bool replaceProfile;
    public bool Captured => capturedSession is not null && viewport?.GameCapture == true;
    public string? ProfilePath => profilePath;
    public event EventHandler? Changed;

    public void Attach(ViewportInput input, IntPtr window) { Release(); viewport = input; Window = window; reportedError = null; }
    public void Detach(ViewportInput input) { if (viewport != input) return; Release(); viewport = null; Window = IntPtr.Zero; }
    public void ValidateCapture() => Guard(() => viewport?.ValidateGameCapture());

    public void SelectProfile(string? path)
    {
        // Check before changing the active selection or releasing a valid input
        // session. The native configure operation checks the profile again.
        if (path is not null && model.Host.Call("input.inspect", new() { ["path"] = path })["persisted"]?.GetValue<bool>() != true)
            throw new InvalidOperationException("Choose an existing input profile.");
        Release(); profilePath = path; replaceProfile = true;
        Changed?.Invoke(this, EventArgs.Empty);
    }

    public void Release()
    {
        if (releasing) return;
        releasing = true;
        try
        {
            var session = capturedSession; capturedSession = null;
            try
            {
                if (session is not null && model.Host.Call("desktop.input.inspect")["session_id"]?.GetValue<string>() == session)
                    model.Host.Call("desktop.input.focus", new() { ["session_id"] = session, ["focused"] = false });
            }
            finally { viewport?.EndGameCapture(); }
            if (session is not null) Changed?.Invoke(this, EventArgs.Empty);
        }
        finally { releasing = false; }
    }

    private void Engage()
    {
        if (viewport is null || model.RuntimeId is not string session || model.PlaybackState != "playing") return;
        var controllers = model.Host.Call("desktop.controllers")["controllers"]!.AsArray();
        var controller = controllers.FirstOrDefault(value => value?["camera"]?.GetValue<string>() == model.GameCamera)?["id"]?.GetValue<string>()
            ?? throw new InvalidOperationException("This camera has no CharacterController. Choose a player's camera to control it.");
        var state = model.Host.Call("desktop.input.inspect");
        if (replaceProfile || state["session_id"]?.GetValue<string>() != session || state["controller"]?.GetValue<string>() != controller)
        {
            var parameters = new JsonObject { ["session_id"] = session, ["controller"] = controller };
            if (profilePath is not null) parameters["input_profile"] = profilePath;
            state = model.Host.Call("desktop.input.configure", parameters);
            replaceProfile = false;
        }
        profilePath = state["profile"]?["path"]?.GetValue<string>();
        if (keyboard is null)
        {
            keyboard = model.Host.Call("input.describe")["controls"]!.AsArray()
                .Where(value => value!["device"]!.GetValue<string>() == "keyboard" && !value["reserved"]!.GetValue<bool>())
                .ToDictionary(value => value!["code"]!.GetValue<uint>(), value => value!["id"]!.GetValue<string>());
        }
        viewport.BeginGameCapture();
        try { model.Host.Call("desktop.input.focus", new() { ["session_id"] = session, ["focused"] = true }); }
        catch { viewport.EndGameCapture(); throw; }
        capturedSession = session;
        Changed?.Invoke(this, EventArgs.Empty);
    }

    private void Send(JsonObject value)
    {
        model.Host.Call("desktop.input.events", new() { ["session_id"] = capturedSession, ["request_id"] = EditorModel.NewId(), ["events"] = new JsonArray(value) });
    }

    public void Handle(ViewportInputEvent e) => Guard(() => HandleCore(e));
    private void HandleCore(ViewportInputEvent e)
    {
        lastModifiers = e.Modifiers; lastX = e.X; lastY = e.Y;
        if (e.Kind is ViewportInputKind.FocusLost or ViewportInputKind.CaptureLost or ViewportInputKind.Resized)
        { Release(); return; }
        if (e.Kind == ViewportInputKind.KeyDown && e.VirtualKey is 0x1B or 0x09)
        { Release(); return; }
        if (model.PlaybackState != "playing") { Release(); return; }
        if (!Captured)
        {
            // The acquisition click never fires a gameplay action.
            if (e.Kind == ViewportInputKind.PointerDown && e.Button == ViewportMouseButton.Left) Engage();
            return;
        }
        if (e.Kind is ViewportInputKind.KeyDown or ViewportInputKind.KeyUp)
        {
            if ((e.Kind == ViewportInputKind.KeyUp || !e.Repeat) && keyboard!.TryGetValue(e.ScanCode, out var control))
                Send(new() { ["control"] = control, ["down"] = e.Kind == ViewportInputKind.KeyDown });
        }
        else if (e.Kind is ViewportInputKind.PointerDown or ViewportInputKind.PointerUp)
        {
            var code = e.Button switch { ViewportMouseButton.Left => 1, ViewportMouseButton.Middle => 2,
                ViewportMouseButton.Right => 3, ViewportMouseButton.X1 => 4, ViewportMouseButton.X2 => 5, _ => 0 };
            if (code != 0) Send(new() { ["control"] = "mouse." + code, ["down"] = e.Kind == ViewportInputKind.PointerDown });
        }
        else if (e.Kind == ViewportInputKind.RelativeMotion)
            Send(new() { ["motion"] = new JsonArray(e.DeltaX, e.DeltaY) });
    }

    public void Tick() => Guard(() =>
    {
        if (viewport?.LastError is string error && reportedError != error)
        { reportedError = error; throw new InvalidOperationException(error); }
        if (capturedSession is null) return;
        var state = model.Host.State["input"];
        if (model.PlaybackState != "playing" || model.RuntimeId != capturedSession
            || viewport?.GameCapture != true || state?["focused"]?.GetValue<bool>() != true
            || state?["session_id"]?.GetValue<string>() != capturedSession)
            Release();
    });

    public JsonObject Inspect() => new() { ["attached"] = viewport is not null, ["width"] = viewport?.Width ?? 0, ["height"] = viewport?.Height ?? 0,
        ["last_input_modifiers"] = (int)lastModifiers, ["last_input_x"] = lastX, ["last_input_y"] = lastY,
        ["input_error"] = viewport?.LastError, ["error_count"] = ErrorCount,
        ["qualification_input"] = viewport?.QualificationInput ?? false, ["ignored_interactive_messages"] = viewport?.IgnoredInteractiveMessages ?? 0,
        ["captured"] = Captured, ["session_id"] = capturedSession,
        ["profile_path"] = profilePath, ["native"] = model.Host.Call("desktop.input.inspect") };
}
