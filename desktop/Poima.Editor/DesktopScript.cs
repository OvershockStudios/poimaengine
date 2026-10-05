// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using System.Runtime.InteropServices;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.VisualTree;

namespace Poima.Editor;

// Bounded semantic qualification. This does not simulate physical pointer/keyboard input.
internal sealed class DesktopScript
{
    [DllImport("user32.dll", EntryPoint = "SendMessageW")]
    private static extern nint SendMessage(nint hwnd, uint message, nuint wparam, nint lparam);
    [StructLayout(LayoutKind.Sequential)] private struct NativePoint { public int X,Y; }
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ClientToScreen(nint hwnd, ref NativePoint point);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetKeyboardState([Out] byte[] state);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetKeyboardState([In] byte[] state);
    [DllImport("user32.dll")] private static extern short GetKeyState(int key);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool IsWindow(nint hwnd);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool IsWindowVisible(nint hwnd);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool IsIconic(nint hwnd);
    [DllImport("user32.dll")] private static extern nint GetAncestor(nint hwnd, uint flags);
    [DllImport("user32.dll")] private static extern nint GetParent(nint hwnd);
    [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetClientRect(nint hwnd, out NativeRect rectangle);
    private readonly JsonArray actions;
    private int next, frameOffset;
    private int? deferredAt;
    private long deferredStarted;
    private double gizmoX, gizmoY;
    public JsonArray Results { get; } = [];
    public string? Error { get; private set; }
    public bool Completed => next == actions.Count && deferredAt is null;
    public DesktopScript(string? path, int frames)
    {
        if (path == null) { actions = []; return; }
        if (new FileInfo(path).Length > 1024 * 1024) throw new ArgumentException("Script exceeds 1 MiB.");
        var root = JsonNode.Parse(File.ReadAllText(path), documentOptions: new() { MaxDepth = 64 })!.AsObject();
        actions = root["actions"]!.AsArray();
        if (actions.Count > 256) throw new ArgumentException("Script exceeds 256 actions.");
        var previous = 0;
        foreach (var entry in actions)
        {
            var frame = entry!["frame"]!.GetValue<int>();
            if (frame < previous || frame >= frames - 1) throw new ArgumentException("Script frames must be ordered and leave a final presentation frame.");
            previous = frame;
        }
    }
    public void Tick(int frame, MainWindow window)
    {
        while (next < actions.Count && actions[next]!["frame"]!.GetValue<int>() + frameOffset <= frame && Error == null)
        {
            var action = actions[next++]!.AsObject();
            var op = action["op"]!.GetValue<string>(); var model = window.Model;
            var result = new JsonObject { ["frame"] = frame, ["op"] = op };
            string Text(string key) => action[key]!.GetValue<string>();
            try
            {
                switch (op)
                {
                    case "resize_window":
                        var width = action["width"]!.GetValue<double>(); var height = action["height"]!.GetValue<double>();
                        if (!double.IsFinite(width) || !double.IsFinite(height) || width < 960 || width > 2400 || height < 620 || height > 1600) throw new ArgumentException("Qualification window size is outside bounds.");
                        window.Width = width; window.Height = height; break;
                    case "save_layout": window.SaveLayout(); result["layout"] = window.InspectLayout(); break;
                    case "load_layout": window.LoadLayout(); break;
                    case "scene_frame": window.Navigation.FrameSelection(); break;
                    case "scene_step": window.Navigation.Tick(action["seconds"]!.GetValue<double>()); break;
                    case "gizmo_configure": result["gizmo"] = window.Navigation.ConfigureGizmo(Text("mode"), action["space"]?.GetValue<string>()); break;
                    case "gizmo_inspect": result["gizmo"] = window.Navigation.InspectGizmo(); break;
                    case "gizmo_begin":
                        (gizmoX, gizmoY) = GizmoPoint(action, window.Navigation);
                        result["point"] = new JsonArray(gizmoX, gizmoY);
                        result["gizmo"] = window.Navigation.BeginGizmo(gizmoX, gizmoY); break;
                    case "gizmo_update":
                        gizmoX = action["x"]?.GetValue<double>() ?? gizmoX+(action["dx"]?.GetValue<double>() ?? 0);
                        gizmoY = action["y"]?.GetValue<double>() ?? gizmoY+(action["dy"]?.GetValue<double>() ?? 0);
                        result["gizmo"] = window.Navigation.UpdateGizmo(gizmoX, gizmoY, action["snap"]?.GetValue<bool>() ?? false); break;
                    case "gizmo_commit": result["result"] = window.Navigation.CommitGizmo(); break;
                    case "gizmo_cancel": result["gizmo"] = window.Navigation.CancelGizmo(); break;
                    case "scene_input":
                    case "game_input":
                        var gameTarget = op == "game_input";
                        JsonObject InputState() => gameTarget ? window.Game.Inspect() : window.Navigation.Inspect();
                        var hwnd = gameTarget ? window.Game.Window : window.Navigation.Window;
                        if (hwnd == IntPtr.Zero) throw new InvalidOperationException("Requested viewport HWND is unavailable.");
                        var message = Text("message"); result["message"] = message;
                        var dimensions = InputState();
                        var x = (int)((action["x"]?.GetValue<double>() ?? .5)*dimensions["width"]!.GetValue<int>());
                        var y = (int)((action["y"]?.GetValue<double>() ?? .5)*dimensions["height"]!.GetValue<int>());
                        if (action["axis"] is not null)
                        {
                            (gizmoX, gizmoY) = GizmoPoint(action, window.Navigation);
                            x = (int)Math.Round(gizmoX); y = (int)Math.Round(gizmoY);
                        }
                        x = action["pixel_x"]?.GetValue<int>() ?? x;
                        y = action["pixel_y"]?.GetValue<int>() ?? y;
                        if (action["gizmo_relative"]?.GetValue<bool>() == true)
                        { x = (int)Math.Round(gizmoX+(action["dx"]?.GetValue<double>() ?? 0)); y = (int)Math.Round(gizmoY+(action["dy"]?.GetValue<double>() ?? 0)); }
                        var point = (nint)((uint)(ushort)x | ((uint)(ushort)y << 16));
                        var code = message switch { "right_down" => 0x0204u, "right_up" => 0x0205u, "left_down" => 0x0201u, "left_up" => 0x0202u,
                            "middle_down" => 0x0207u, "middle_up" => 0x0208u, "move" => 0x0200u,
                            "key_down" => 0x0100u, "key_up" => 0x0101u, "cancel" => 0x001Fu, "focus_lost" => 0x0008u, "wheel" => 0x020Au,
                            _ => throw new ArgumentException("Unsupported local scene input message.") };
                        nuint parameter = action["key"]?.GetValue<uint>() ?? 0;
                        if (code == 0x020A)
                        {
                            var screen = new NativePoint { X=x,Y=y };
                            if (!ClientToScreen(hwnd,ref screen)) throw new InvalidOperationException("Cannot map Scene input coordinates.");
                            point = (nint)((uint)(ushort)screen.X | ((uint)(ushort)screen.Y << 16));
                            parameter = (nuint)((uint)(ushort)(action["delta"]?.GetValue<int>() ?? 120) << 16);
                        }
                        var requestedModifiers = (action["shift"]?.GetValue<bool>() == true ? ViewportModifiers.Shift : 0)
                            | (action["control"]?.GetValue<bool>() == true ? ViewportModifiers.Control : 0)
                            | (action["alt"]?.GetValue<bool>() == true ? ViewportModifiers.Alt : 0);
                        result["requested_modifiers"] = (int)requestedModifiers;
                        var savedKeyboard = new byte[256];
                        if (!GetKeyboardState(savedKeyboard)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                        result["saved_thread_modifiers"] = ((savedKeyboard[0x10] & 0x80) != 0 ? 1 : 0)
                            | ((savedKeyboard[0x11] & 0x80) != 0 ? 2 : 0) | ((savedKeyboard[0x12] & 0x80) != 0 ? 4 : 0);
                        var scriptedKeyboard = (byte[])savedKeyboard.Clone();
                        void Modifier(ViewportModifiers flag, int generic, int left, int right)
                        {
                            foreach (var key in new[] { generic, left, right }) scriptedKeyboard[key] &= 0x7f;
                            if ((requestedModifiers & flag) != 0) { scriptedKeyboard[generic] |= 0x80; scriptedKeyboard[left] |= 0x80; }
                        }
                        Modifier(ViewportModifiers.Shift, 0x10, 0xA0, 0xA1);
                        Modifier(ViewportModifiers.Control, 0x11, 0xA2, 0xA3);
                        Modifier(ViewportModifiers.Alt, 0x12, 0xA4, 0xA5);
                        if (!SetKeyboardState(scriptedKeyboard)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                        try
                        {
                            // This table belongs only to the calling UI thread;
                            // no global input is injected or another app altered.
                            // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setkeyboardstate
                            var deliveredModifiers = ((GetKeyState(0x10) & 0x8000) != 0 ? ViewportModifiers.Shift : 0)
                                | ((GetKeyState(0x11) & 0x8000) != 0 ? ViewportModifiers.Control : 0)
                                | ((GetKeyState(0x12) & 0x8000) != 0 ? ViewportModifiers.Alt : 0);
                            result["thread_modifiers"] = (int)deliveredModifiers;
                            if (deliveredModifiers != requestedModifiers) throw new InvalidOperationException("Semantic modifier state was not applied to the UI thread.");
                            var keyFlags = 1L | ((long)(action["scan"]?.GetValue<byte>() ?? 0) << 16)
                                | (action["extended"]?.GetValue<bool>() == true ? 1L << 24 : 0)
                                | (action["repeat"]?.GetValue<bool>() == true ? 1L << 30 : 0)
                                | (code == 0x0101u ? 3L << 30 : 0);
                            ViewportInput.DispatchQualification(hwnd, () => SendMessage(hwnd,code,parameter,code is 0x0100u or 0x0101u ? (nint)keyFlags : point));
                            var observedModifiers = InputState()["last_input_modifiers"]!.GetValue<int>();
                            result["delivered_modifiers"] = observedModifiers;
                            if (observedModifiers != (int)requestedModifiers) throw new InvalidOperationException("Viewport received different modifiers from the semantic action.");
                        }
                        finally
                        {
                            if (!SetKeyboardState(savedKeyboard)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Cannot restore the UI thread keyboard state.");
                        }
                        result["navigation"] = window.Navigation.Inspect(); result["game_input"] = window.Game.Inspect(); result["camera"] = model.Host.Call("desktop.inspect")["camera"]!.DeepClone();
                        result["requested_pointer_x"] = x; result["requested_pointer_y"] = y;
                        break;
                    case "game_motion":
                        ViewportInput.DispatchQualificationRelative(window.Game.Window, action["dx"]!.GetValue<int>(), action["dy"]!.GetValue<int>());
                        result["game_input"] = window.Game.Inspect(); break;
                    case "game_profile":
                        window.Game.SelectProfile(action["path"]?.GetValue<string>());
                        result["game_input"] = window.Game.Inspect(); break;
                    case "select": window.SelectEntity(Text("id")); break;
                    case "create": result["id"] = model.Create(Text("kind")); break;
                    case "draft_name": model.SetName(Text("name")); break;
                    case "click_control":
                    case "check_control":
                    case "choose_control":
                        var controlLifetime = (IClassicDesktopStyleApplicationLifetime)Avalonia.Application.Current!.ApplicationLifetime!;
                        var controls = controlLifetime.Windows.Where(w => w.IsVisible).SelectMany(w => w.GetVisualDescendants()).OfType<Control>()
                            .Where(control => control.IsVisible && AutomationProperties.GetName(control) == Text("control")).ToArray();
                        if (controls.Length != 1 || !controls[0].IsEnabled) throw new InvalidOperationException("Expected one enabled visible control: " + Text("control"));
                        if (op == "click_control" && controls[0] is Button button)
                            button.RaiseEvent(new Avalonia.Interactivity.RoutedEventArgs(Button.ClickEvent));
                        else if (op == "check_control" && controls[0] is CheckBox check)
                        { check.IsChecked = action["checked"]!.GetValue<bool>(); result["checked"] = check.IsChecked; }
                        else if (op == "choose_control" && controls[0] is ComboBox combo)
                        {
                            var options = combo.Items.Cast<object>().Select(item => item.ToString() ?? "").ToArray();
                            result["options"] = new JsonArray(options.Select(option => (JsonNode?)JsonValue.Create(option)).ToArray());
                            var matches = options.Select((label,index) => (label,index)).Where(item => item.label == Text("choice")).ToArray();
                            if (matches.Length != 1) throw new InvalidOperationException("Combo option must match exactly one item: " + Text("choice"));
                            combo.SelectedIndex = matches[0].index; result["selected"] = combo.SelectedItem?.ToString();
                        }
                        else throw new InvalidOperationException("Control type does not support " + op);
                        break;
                    case "wait_text":
                    case "assert_text":
                    case "set_text":
                        var lifetime = (IClassicDesktopStyleApplicationLifetime)Avalonia.Application.Current!.ApplicationLifetime!;
                        var edits = lifetime.Windows.Where(w => w.IsVisible).SelectMany(w => w.GetVisualDescendants()).OfType<TextBox>()
                            .Where(w => w.IsVisible && AutomationProperties.GetName(w) == Text("control")).ToArray();
                        if (op == "wait_text" && (edits.Length != 1 || edits[0].Text != Text("text")))
                        {
                            if (deferredAt is null) { deferredAt = frame; deferredStarted = System.Diagnostics.Stopwatch.GetTimestamp(); }
                            if (System.Diagnostics.Stopwatch.GetElapsedTime(deferredStarted).TotalSeconds < 3)
                            { --next; return; }
                            throw new InvalidOperationException($"Inspector readiness timed out for {Text("control")}; matches={edits.Length}, draft={window.InspectDraft()}");
                        }
                        if (edits.Length != 1) throw new InvalidOperationException($"Expected one visible {Text("control")} field; found {edits.Length}.");
                        var edit = edits[0];
                        result["control_width"] = edit.Bounds.Width; result["control_text"] = edit.Text;
                        if (deferredAt is int started)
                        {
                            result["wait_pumps"] = frame-started;
                            frameOffset += frame-started; deferredAt = null;
                        }
                        if (op == "set_text") edit.Text = Text("text");
                        else if (edit.Text != Text("text")) throw new InvalidOperationException("Field text was not preserved: " + Text("control"));
                        break;
                    case "close_guard":
                        window.Close();
                        if (!window.IsVisible) throw new InvalidOperationException("Dirty close was not cancelled.");
                        break;
                    case "wait_scene_error":
                    case "assert_scene_error":
                        var errorExpected = action["expected"]!.GetValue<bool>();
                        var errorView = action["view"]?.GetValue<string>() ?? "scene";
                        if (errorView is not ("scene" or "game")) throw new ArgumentException("Unknown viewport.");
                        var viewError = model.Host.ViewError(errorView);
                        result["scene_error"] = viewError;
                        var sceneState = model.Host.Call("desktop.inspect");
                        result["native"] = sceneState;
                        var details = sceneState["views"]![errorView]!;
                        var sceneWindow = InspectSceneWindow(errorView == "scene" ? window.Navigation.Window : window.Game.Window);
                        result["scene_window"] = sceneWindow;
                        if (details["graphics_error"] is not null)
                            throw new InvalidOperationException("Snapshot preparation poisoned the renderer.");
                        var sceneErrorMatches = (viewError != null) == errorExpected;
                        var sceneReady = sceneErrorMatches && (errorExpected || JsonNode.DeepEquals(details["presented_revision"], sceneState["revision"]));
                        if (op == "wait_scene_error" && !sceneReady)
                        {
                            if (deferredAt is null) { deferredAt = frame; deferredStarted = System.Diagnostics.Stopwatch.GetTimestamp(); }
                            if (System.Diagnostics.Stopwatch.GetElapsedTime(deferredStarted).TotalSeconds < 3)
                            { --next; return; }
                            throw new InvalidOperationException($"Scene recovery presentation timed out: view={errorView}; error={viewError}; revision={sceneState["revision"]}; presented_revision={details["presented_revision"]}; frames_presented={details["frames_presented"]}; window={sceneWindow.ToJsonString()}");
                        }
                        if (op == "assert_scene_error" && !sceneErrorMatches)
                            throw new InvalidOperationException("Unexpected view error state: " + viewError);
                        if (deferredAt is int sceneWaitStarted)
                        {
                            result["wait_pumps"] = frame-sceneWaitStarted;
                            frameOffset += frame-sceneWaitStarted; deferredAt = null;
                        }
                        break;
                    case "draft_component": model.SetComponent(Text("type"), action["value"]!.AsObject()); break;
                    case "apply": model.Apply(); break;
                    case "reload": model.Reload(); break;
                    case "undo": model.History(false); break;
                    case "redo": model.History(true); break;
                    case "runtime_toggle": model.PlayStop(); break;
                    case "runtime_pause": model.Pause(); break;
                    case "runtime_rpc":
                        // Bypass the model, as an external client does. The next
                        // native poll must synchronize the visible controls.
                        result["result"] = model.Host.Call("desktop.play." + Text("command"), new() { ["session_id"] = model.RuntimeId });
                        break;
                    case "runtime_entity":
                        result["result"] = model.Host.Call("runtime.entity", new() { ["session_id"] = model.RuntimeId, ["id"] = Text("id") });
                        break;
                    case "game_camera": model.SetGameCamera(action["camera"]?.GetValue<string>()); break;
                    case "show_panel": window.ShowPanel(Text("panel")); break;
                    case "step": model.Step(action["ticks"]?.GetValue<int>() ?? 1); break;
                    case "float": window.FloatPanel(Text("panel")); break;
                    case "reset_layout": window.SetTallLayout(action["split_views"]?.GetValue<bool>() ?? false); break;
                    case "rpc": result["result"] = model.Host.Call(Text("method"), action["params"]?.AsObject()); break;
                    case "inspect":
                        result["layout"] = window.InspectLayout(); result["navigation"] = window.Navigation.Inspect(); result["game_input"] = window.Game.Inspect();
                        result["scene_window"] = InspectSceneWindow(window.Navigation.Window); result["game_window"] = InspectSceneWindow(window.Game.Window);
                        result["draft"] = window.InspectDraft(); result["native"] = model.Host.Call("desktop.inspect");
                        result["playback"] = model.InspectPlayback();
                        result["playback_controls"] = window.InspectPlaybackControls();
                        if (Avalonia.Application.Current?.ApplicationLifetime is Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop)
                            result["windows"] = new JsonArray(desktop.Windows.Select(w => (JsonNode?)new JsonObject {
                                ["title"] = w.Title, ["visible"] = w.IsVisible, ["width"] = w.Bounds.Width, ["height"] = w.Bounds.Height }).ToArray());
                        break;
                    case "assert_draft":
                        var draft = window.InspectDraft();
                        foreach (var field in action["expected"]!.AsObject())
                            if (!JsonNode.DeepEquals(draft[field.Key], field.Value)) throw new InvalidOperationException($"Draft assertion failed for {field.Key}: {draft}");
                        result["draft"] = draft; break;
                    default: throw new ArgumentException("Unknown semantic action: " + op);
                }
                if (action.ContainsKey("error_contains")) throw new InvalidOperationException("Expected failure did not occur.");
            }
            catch (Exception e)
            {
                result["error"] = e.Message;
                if (action["error_contains"] is JsonValue expected && e.Message.Contains(expected.GetValue<string>(), StringComparison.Ordinal))
                    result["expected_error"] = true;
                else Error = e.Message;
            }
            Results.Add(result);
        }
    }
    private static JsonObject InspectSceneWindow(nint hwnd)
    {
        var valid = hwnd != IntPtr.Zero && IsWindow(hwnd);
        var root = valid ? GetAncestor(hwnd, 2) : IntPtr.Zero; // GA_ROOT
        var parent = valid ? GetParent(hwnd) : IntPtr.Zero;
        var client = default(NativeRect);
        var measured = valid && GetClientRect(hwnd, out client);
        var error = valid && !measured ? Marshal.GetLastWin32Error() : 0;
        return new JsonObject { ["hwnd"] = hwnd.ToInt64(), ["valid"] = valid,
            ["visible"] = valid && IsWindowVisible(hwnd), ["root_hwnd"] = root.ToInt64(),
            ["root_visible"] = root != IntPtr.Zero && IsWindowVisible(root),
            ["root_minimized"] = root != IntPtr.Zero && IsIconic(root), ["parent_hwnd"] = parent.ToInt64(),
            ["client_measured"] = measured, ["client_width"] = measured ? client.Right-client.Left : (int?)null,
            ["client_height"] = measured ? client.Bottom-client.Top : (int?)null, ["client_error"] = error };
    }
    private static (double X, double Y) GizmoPoint(JsonObject action, SceneNavigation navigation)
    {
        if (action["axis"] is not JsonValue axis) return (action["x"]!.GetValue<double>(), action["y"]!.GetValue<double>());
        var geometry = navigation.InspectGizmo();
        var handle = geometry["handles"]!.AsArray().SingleOrDefault(value => value?["axis"]?.GetValue<string>() == axis.GetValue<string>())?.AsObject()
            ?? throw new InvalidOperationException("Requested gizmo axis is absent.");
        if (handle["visible"]?.GetValue<bool>() != true) throw new InvalidOperationException("Requested gizmo axis is not visible.");
        var points = handle["points"]!.AsArray();
        if (points.Count < 2) throw new InvalidOperationException("Requested gizmo handle lacks a hit-test path.");
        if (points.Count == 2 && action["point_index"] is null)
        {
            var first = points[0]!.AsArray(); var last = points[1]!.AsArray();
            return (first[0]!.GetValue<double>()*.3+last[0]!.GetValue<double>()*.7,
                    first[1]!.GetValue<double>()*.3+last[1]!.GetValue<double>()*.7);
        }
        var index = action["point_index"]?.GetValue<int>() ?? points.Count/4;
        if (index < 0 || index >= points.Count) throw new ArgumentException("Gizmo point_index is outside the handle path.");
        return (points[index]![0]!.GetValue<double>(), points[index]![1]!.GetValue<double>());
    }
}
