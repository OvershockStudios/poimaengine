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
    private long configurationGeneration;
    private (string Session, string Camera, long Generation)? configurationAttempt;
    public string? ConfigurationError { get; private set; }
    public bool UiKeyboardOwned { get; private set; }
    private string? uiSession;
    private bool uiAcceptHeld;
    public bool Captured => capturedSession is not null && viewport?.GameCapture == true;
    public string? ProfilePath => profilePath;
    public string Defaults { get; private set; } = "keyboard_mouse";
    public string GamepadMode { get; private set; } = "disabled";
    public uint GamepadId { get; private set; }
    public string ProfileFormat { get; private set; } = "poima.input.v1";
    public bool ConfigurationPending => replaceProfile;
    public event EventHandler? Changed;

    public void Attach(ViewportInput input, IntPtr window) { Release(); viewport = input; Window = window; reportedError = null; }
    public void Detach(ViewportInput input) { if (viewport != input) return; Release(); viewport = null; Window = IntPtr.Zero; }
    public void ValidateCapture() => Guard(() => viewport?.ValidateGameCapture());

    public void SelectProfile(string? path) => SelectConfiguration("keyboard_mouse", path, "disabled", 0);

    public void SelectConfiguration(string defaults, string? path, string mode, uint id)
    {
        if (defaults is not ("keyboard_mouse" or "keyboard_mouse_gamepad")) throw new ArgumentException("Choose a built-in binding set.");
        if (mode is not ("disabled" or "only_connected" or "explicit") || (mode == "explicit") != (id != 0))
            throw new ArgumentException("Choose a gamepad assignment and, for Explicit device, an available device.");
        var format = defaults == "keyboard_mouse_gamepad" ? "poima.input.v2" : "poima.input.v1";
        if (path is not null)
        {
            var profile = model.Host.Call("input.inspect", new() { ["path"] = path });
            if (profile["persisted"]?.GetValue<bool>() != true) throw new InvalidOperationException("Choose an existing input profile.");
            format = profile["format"]!.GetValue<string>();
        }
        if (mode != "disabled" && format != "poima.input.v2")
            throw new InvalidOperationException("Gamepad input needs a v2 profile. Choose Keyboard, mouse and gamepad or load a v2 profile.");
        if (model.RuntimeId is null && mode == "explicit"
            && !model.Host.Call("desktop.input.devices")["devices"]!.AsArray().Any(value => value!["id"]!.GetValue<uint>() == id))
            throw new InvalidOperationException("The selected gamepad is disconnected. Refresh devices and choose an available device.");
        JsonObject? applied = null;
        if (model.RuntimeId is string session && model.GameCamera is string camera)
            applied = model.Host.Call("desktop.input.configure", Configuration(session, FindController(), camera, defaults, path, mode, id));
        // Only retire managed capture once native validation/acquisition succeeded.
        Release();
        profilePath = path; Defaults = defaults; GamepadMode = mode; GamepadId = id; ProfileFormat = format;
        replaceProfile = applied is null; ++configurationGeneration; ConfigurationError = null;
        configurationAttempt = applied is not null && model.RuntimeId is string currentSession && model.GameCamera is string currentCamera
            ? (currentSession, currentCamera, configurationGeneration) : null;
        if (applied is not null) Adopt(applied);
        Changed?.Invoke(this, EventArgs.Empty);
    }

    private string? FindController() => model.Host.Call("desktop.controllers")["controllers"]!.AsArray()
        .FirstOrDefault(value => value?["camera"]?.GetValue<string>() == model.GameCamera)?["id"]?.GetValue<string>();

    private static JsonObject Configuration(string session, string? controller, string camera, string defaults, string? path, string mode, uint id)
    {
        var selection = new JsonObject { ["mode"] = mode }; if (mode == "explicit") selection["id"] = id;
        var result = new JsonObject { ["session_id"] = session, ["gamepad"] = selection };
        if (controller is null) result["camera"] = camera; else result["controller"] = controller;
        if (path is not null) result["input_profile"] = path; else result["defaults"] = defaults;
        return result;
    }

    private JsonNode? ApplyPendingConfiguration(JsonNode? state)
    {
        if (model.RuntimeId is not string session || model.GameCamera is not string camera) return state;
        var context = (session, camera, configurationGeneration);
        if (configurationAttempt == context) return state;
        // Remember the attempt before device acquisition. A disconnected device
        // or invalid profile reports once; Apply retries explicitly, and a new
        // session/camera retries the stored preference without a per-poll loop.
        configurationAttempt = context;
        if (!replaceProfile && state?["configured"]?.GetValue<bool>() == true
            && state["session_id"]?.GetValue<string>() == session && state["camera"]?.GetValue<string>() == camera)
            return state;
        replaceProfile = true;
        try
        {
            var applied = model.Host.Call("desktop.input.configure", Configuration(session, FindController(), camera, Defaults, profilePath, GamepadMode, GamepadId));
            Release(); replaceProfile = false; ConfigurationError = null; Adopt(applied);
            Changed?.Invoke(this, EventArgs.Empty);
            return applied;
        }
        catch (Exception error) { ConfigurationError = error.Message; throw; }
    }

    private void Adopt(JsonNode state)
    {
        if (state["configured"]?.GetValue<bool>() != true) return;
        profilePath = state["profile"]?["path"]?.GetValue<string>();
        ProfileFormat = state["profile"]?["format"]?.GetValue<string>() ?? "poima.input.v1";
        Defaults = ProfileFormat == "poima.input.v2" ? "keyboard_mouse_gamepad" : "keyboard_mouse";
        GamepadMode = state["gamepad"]?["policy"]?.GetValue<string>() ?? "disabled";
        GamepadId = state["gamepad"]?["requested_id"]?.GetValue<uint>() ?? 0;
    }

    private void ResetUi()
    {
        UiKeyboardOwned = false; uiAcceptHeld = false; uiSession = null;
        if (model.Host.State["closing"]?.GetValue<bool>() != true) model.Host.Call("desktop.ui.reset");
    }
    public void Release() { ResetUi(); ReleaseGameplay(); }
    private void ReleaseGameplay()
    {
        if (releasing) return;
        releasing = true;
        try
        {
            var session = capturedSession; capturedSession = null;
            try
            {
                if (session is not null && model.Host.State["closing"]?.GetValue<bool>() != true && model.Host.Call("desktop.input.inspect")["session_id"]?.GetValue<string>() == session)
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
        var controller = FindController();
        if (controller is null) return; // Playing menus need no player controller.
        var state = model.Host.Call("desktop.input.inspect");
        if (replaceProfile || state["session_id"]?.GetValue<string>() != session || state["controller"]?.GetValue<string>() != controller)
        {
            state = model.Host.Call("desktop.input.configure", Configuration(session, controller, model.GameCamera!, Defaults, profilePath, GamepadMode, GamepadId));
            replaceProfile = false;
        }
        Adopt(state);
        if (keyboard is null)
        {
            keyboard = model.Host.Call("input.describe")["controls"]!.AsArray()
                .Where(value => value!["device"]!.GetValue<string>() == "keyboard" && !value["reserved"]!.GetValue<bool>())
                .ToDictionary(value => value!["code"]!.GetValue<uint>(), value => value!["id"]!.GetValue<string>());
        }
        // A deliberate click outside UI transfers keyboard ownership back to
        // gameplay; stale UI focus must not swallow the new player's keys.
        ResetUi();
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

    private bool RouteUi(ViewportInputEvent e)
    {
        if (model.RuntimeId is not string session || model.GameCamera is not string camera) return false;
        if (uiSession != session) { UiKeyboardOwned = false; uiAcceptHeld = false; uiSession = session; }
        string? kind = e.Kind switch
        {
            ViewportInputKind.PointerMove when !Captured => "pointer_move",
            ViewportInputKind.PointerDown when e.Button == ViewportMouseButton.Left => "pointer_down",
            ViewportInputKind.PointerUp when e.Button == ViewportMouseButton.Left => "pointer_up",
            ViewportInputKind.PointerWheel when !e.HorizontalWheel && !Captured => "pointer_wheel",
            ViewportInputKind.KeyDown when e.VirtualKey == 0x09 && !e.Repeat => e.Shift ? "focus_previous" : "focus_next",
            ViewportInputKind.KeyDown when e.VirtualKey == 0x0D && !e.Repeat && !uiAcceptHeld => "accept_down",
            ViewportInputKind.KeyUp when e.VirtualKey == 0x0D && uiAcceptHeld => "accept_up",
            ViewportInputKind.KeyDown when e.VirtualKey == 0x1B && !e.Repeat => "cancel",
            _ => null
        };
        if (kind is null) return UiKeyboardOwned && e.Kind is ViewportInputKind.KeyDown or ViewportInputKind.KeyUp;
        var response = model.Host.Call("desktop.ui.input", new() { ["session_id"] = session, ["camera"] = camera,
            ["request_id"] = EditorModel.NewId(), ["kind"] = kind, ["x"] = e.X, ["y"] = e.Y,
            ["delta"] = kind == "pointer_wheel" ? -e.WheelDelta / 120f : 0 });
        UiKeyboardOwned = response["keyboard_owned"]?.GetValue<bool>() == true;
        bool consumed = response["consumed"]?.GetValue<bool>() == true;
        if (kind == "accept_down") uiAcceptHeld = consumed;
        if (kind == "accept_up" || kind == "cancel") uiAcceptHeld = false;
        if (consumed && Captured) ReleaseGameplay();
        return consumed;
    }

    public void Handle(ViewportInputEvent e) => Guard(() => HandleCore(e));
    private void HandleCore(ViewportInputEvent e)
    {
        // EndGameCapture synchronously emits its own cleanup notification. A
        // deliberate gameplay-to-UI handoff must retain the new UI gesture.
        if (releasing && e.Kind == ViewportInputKind.CaptureLost) return;
        if (model.Host.State["closing"]?.GetValue<bool>() == true) { Release(); return; }
        lastModifiers = e.Modifiers; lastX = e.X; lastY = e.Y;
        if (e.Kind is ViewportInputKind.FocusLost or ViewportInputKind.CaptureLost or ViewportInputKind.Resized)
        { Release(); return; }
        if (RouteUi(e)) return;
        if (e.Kind == ViewportInputKind.KeyDown && e.VirtualKey is 0x1B or 0x09)
        { Release(); return; }
        if (model.PlaybackState != "playing") { ReleaseGameplay(); return; }
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
        var state = ApplyPendingConfiguration(model.Host.State["input"]);
        if (uiSession is not null && model.RuntimeId != uiSession) ResetUi();
        UiKeyboardOwned = state?["ui_keyboard"]?.GetValue<bool>() == true;
        if (!replaceProfile && state is not null) Adopt(state);
        if (capturedSession is null) return;
        if (model.PlaybackState != "playing" || model.RuntimeId != capturedSession
            || viewport?.GameCapture != true || state?["focused"]?.GetValue<bool>() != true
            || state?["session_id"]?.GetValue<string>() != capturedSession)
            ReleaseGameplay();
    });

    public JsonObject Inspect() => new() { ["attached"] = viewport is not null, ["width"] = viewport?.Width ?? 0, ["height"] = viewport?.Height ?? 0,
        ["last_input_modifiers"] = (int)lastModifiers, ["last_input_x"] = lastX, ["last_input_y"] = lastY,
        ["input_error"] = viewport?.LastError, ["error_count"] = ErrorCount,
        ["qualification_input"] = viewport?.QualificationInput ?? false, ["ignored_interactive_messages"] = viewport?.IgnoredInteractiveMessages ?? 0,
        ["captured"] = Captured, ["ui_keyboard_owned"] = UiKeyboardOwned, ["session_id"] = capturedSession,
        ["profile_path"] = profilePath, ["defaults"] = Defaults, ["gamepad_mode"] = GamepadMode, ["gamepad_id"] = GamepadId,
        ["profile_format"] = ProfileFormat, ["configuration_pending"] = replaceProfile, ["configuration_error"] = ConfigurationError, ["native"] = model.Host.State["closing"]?.GetValue<bool>() == true ? model.Host.State["input"]?.DeepClone() : model.Host.Call("desktop.input.inspect") };
}
