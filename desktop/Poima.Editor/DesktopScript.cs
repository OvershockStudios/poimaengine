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
    private readonly JsonArray actions;
    private int next, frameOffset;
    private int? deferredAt;
    private long deferredStarted;
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
                    case "save_layout": window.SaveLayout(); result["layout"] = window.InspectLayout(); break;
                    case "load_layout": window.LoadLayout(); break;
                    case "scene_frame": window.Navigation.FrameSelection(); break;
                    case "scene_step": window.Navigation.Tick(action["seconds"]!.GetValue<double>()); break;
                    case "scene_input":
                        var hwnd = window.Navigation.Window;
                        if (hwnd == IntPtr.Zero) throw new InvalidOperationException("Scene HWND is unavailable.");
                        var message = Text("message"); result["message"] = message;
                        var dimensions = window.Navigation.Inspect();
                        var x = (int)((action["x"]?.GetValue<double>() ?? .5)*dimensions["width"]!.GetValue<int>());
                        var y = (int)((action["y"]?.GetValue<double>() ?? .5)*dimensions["height"]!.GetValue<int>());
                        var point = (nint)((uint)(ushort)x | ((uint)(ushort)y << 16));
                        var code = message switch { "right_down" => 0x0204u, "right_up" => 0x0205u, "left_down" => 0x0201u, "left_up" => 0x0202u,
                            "middle_down" => 0x0207u, "middle_up" => 0x0208u, "move" => 0x0200u,
                            "key_down" => 0x0100u, "key_up" => 0x0101u, "cancel" => 0x001Fu, "wheel" => 0x020Au,
                            _ => throw new ArgumentException("Unsupported local scene input message.") };
                        nuint parameter = action["key"]?.GetValue<uint>() ?? 0;
                        if (code == 0x020A)
                        {
                            var screen = new NativePoint { X=x,Y=y };
                            if (!ClientToScreen(hwnd,ref screen)) throw new InvalidOperationException("Cannot map Scene input coordinates.");
                            point = (nint)((uint)(ushort)screen.X | ((uint)(ushort)screen.Y << 16));
                            parameter = (nuint)((uint)(ushort)(action["delta"]?.GetValue<int>() ?? 120) << 16);
                        }
                        SendMessage(hwnd,code,parameter,code is 0x0100u or 0x0101u ? 1 : point);
                        result["navigation"] = window.Navigation.Inspect(); result["camera"] = model.Host.Call("desktop.inspect")["camera"]!.DeepClone();
                        break;
                    case "select": window.SelectEntity(Text("id")); break;
                    case "create": result["id"] = model.Create(Text("kind")); break;
                    case "draft_name": model.SetName(Text("name")); break;
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
                    case "assert_scene_error":
                        var errorExpected = action["expected"]!.GetValue<bool>();
                        if ((model.Host.LastError != null) != errorExpected)
                            throw new InvalidOperationException("Unexpected scene error state: " + model.Host.LastError);
                        result["scene_error"] = model.Host.LastError;
                        result["native"] = model.Host.Call("desktop.inspect");
                        if (result["native"]!["graphics_error"] is not null)
                            throw new InvalidOperationException("Snapshot preparation poisoned the renderer.");
                        break;
                    case "draft_component": model.SetComponent(Text("type"), action["value"]!.AsObject()); break;
                    case "apply": model.Apply(); break;
                    case "reload": model.Reload(); break;
                    case "undo": model.History(false); break;
                    case "redo": model.History(true); break;
                    case "runtime_toggle": model.PlayStop(); break;
                    case "step": model.Step(action["ticks"]?.GetValue<int>() ?? 1); break;
                    case "float": window.FloatPanel(Text("panel")); break;
                    case "reset_layout": window.ResetLayout(); break;
                    case "rpc": result["result"] = model.Host.Call(Text("method"), action["params"]?.AsObject()); break;
                    case "inspect":
                        result["layout"] = window.InspectLayout(); result["navigation"] = window.Navigation.Inspect();
                        result["draft"] = window.InspectDraft(); result["native"] = model.Host.Call("desktop.inspect");
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
}
