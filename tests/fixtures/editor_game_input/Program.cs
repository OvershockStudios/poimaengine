// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
namespace Poima.Editor;
public enum ViewportInputKind
{
    PointerDown, PointerUp, PointerMove, PointerWheel,
    KeyDown, KeyUp, FocusLost, CaptureLost, Resized, RelativeMotion
}

public enum ViewportMouseButton { None, Left, Middle, Right, X1, X2 }

[Flags]
public enum ViewportMouseButtons { None = 0, Left = 1, Right = 2, Middle = 4, X1 = 8, X2 = 16 }

[Flags]
public enum ViewportModifiers { None = 0, Shift = 1, Control = 2, Alt = 4 }

public readonly record struct ViewportInputEvent(
    ViewportInputKind Kind,
    int X = 0, int Y = 0,
    ViewportMouseButton Button = ViewportMouseButton.None,
    ViewportMouseButtons Buttons = ViewportMouseButtons.None,
    int WheelDelta = 0, bool HorizontalWheel = false,
    uint VirtualKey = 0, bool Repeat = false, int ClickCount = 0,
    ViewportModifiers Modifiers = ViewportModifiers.None,
    int Width = 0, int Height = 0,
    int DeltaX = 0, int DeltaY = 0, uint ScanCode = 0,
    uint NativeScanCode = 0, bool ExtendedKey = false, bool Synthetic = false)
{
    public bool Shift => (Modifiers & ViewportModifiers.Shift) != 0;
    public bool Control => (Modifiers & ViewportModifiers.Control) != 0;
    public bool Alt => (Modifiers & ViewportModifiers.Alt) != 0;
}


// The fixture links production GameInput; only the HWND host/transport are fakes.
public interface IViewportInteraction { void Attach(ViewportInput input, IntPtr window); void Detach(ViewportInput input); void Handle(ViewportInputEvent e); }
public sealed class ViewportInput
{
    public bool GameCapture { get; private set; }
    public string? LastError => null;
    public int Width => 960;
    public int Height => 540;
    public bool QualificationInput => true;
    public long IgnoredInteractiveMessages => 0;
    public void ValidateGameCapture() {}
    public void BeginGameCapture() { GameCapture = true; }
    public Action<ViewportInputEvent>? Dispatch;
    public void EndGameCapture() { if (GameCapture) { GameCapture = false; Dispatch?.Invoke(new(ViewportInputKind.CaptureLost)); } }
}
public sealed class FakeHost
{
    public JsonObject State { get; } = new() { ["input"] = new JsonObject() };
    public readonly List<(string Method, JsonObject Params)> Calls = new();
    public bool UiConsumes=true, HasController=false, FailConfiguration=false;
    public string Session="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", Camera="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    public JsonObject Call(string method, JsonObject? request = null)
    {
        Calls.Add((method,request?.DeepClone().AsObject() ?? new()));
        switch(method)
        {
            case "desktop.ui.reset": State["input"]!["ui_keyboard"]=false; return new();
            case "desktop.ui.input":
                bool owns=UiConsumes && request!["kind"]!.GetValue<string>()!="cancel";
                State["input"]!["ui_keyboard"]=owns;
                return new() { ["consumed"]=UiConsumes,["keyboard_owned"]=owns };
            case "desktop.controllers": return new() { ["controllers"]=HasController ? new JsonArray(new JsonObject { ["id"]="cccccccccccccccccccccccccccccccc",["camera"]=Camera }) : new JsonArray() };
            case "desktop.input.inspect": return State["input"]!.DeepClone().AsObject();
            case "desktop.input.devices": return new() { ["devices"]=new JsonArray(new JsonObject { ["id"]=17u }) };
            case "desktop.input.configure":
                if (FailConfiguration) throw new InvalidOperationException("Disconnected test device");
                State["input"]=new JsonObject { ["configured"]=true,["session_id"]=Session,["camera"]=Camera,
                    ["binding"]=request!.ContainsKey("controller") ? "controller" : "ui",["controller"]=request["controller"]?.DeepClone(),
                    ["gamepad"]=new JsonObject { ["policy"]=request["gamepad"]!["mode"]!.DeepClone(),["requested_id"]=request["gamepad"]?["id"]?.DeepClone() },
                    ["profile"]=new JsonObject { ["format"]=request["defaults"]?.GetValue<string>()=="keyboard_mouse_gamepad" ? "poima.input.v2" : "poima.input.v1" } };
                return State["input"]!.DeepClone().AsObject();
            case "desktop.input.focus": State["input"]!["focused"]=request!["focused"]!.DeepClone();return new();
            case "desktop.input.events": return new();
            case "input.describe": return new() { ["controls"]=new JsonArray(new JsonObject { ["device"]="keyboard",["reserved"]=false,["code"]=26u,["id"]="keyboard.w" }) };
            default: throw new InvalidOperationException(method);
        }
    }
}
public sealed class EditorModel
{
    public FakeHost Host { get; }=new();
    public event Action? SceneChanging;
    public event Action? GameCameraChanging;
    public string PlaybackState { get; set; }="paused";
    public bool Active=true, CameraAvailable=true;
    public string? RuntimeId => Active ? Host.Session : null;
    public string? GameCamera => CameraAvailable ? Host.Camera : null;
    public void SceneChange() => SceneChanging?.Invoke();
    public void CameraChange() => GameCameraChanging?.Invoke();
    public static string NewId()=>Guid.NewGuid().ToString("N");
    public readonly List<string> Notes=new();
    public void Note(string text)=>Notes.Add(text);
}
internal static class Program
{
    static void Check(bool ok,string message) { if(!ok)throw new Exception(message); }
    static int Main()
    {
        var editor=new EditorModel();var game=new GameInput(editor);var viewport=new ViewportInput { Dispatch=game.Handle };game.Attach(viewport,new IntPtr(1));
        game.Handle(new(ViewportInputKind.PointerDown,X:200,Y:80,Button:ViewportMouseButton.Left));
        game.Handle(new(ViewportInputKind.PointerUp,X:200,Y:80,Button:ViewportMouseButton.Left));
        Check(game.ErrorCount==0 && !game.Captured,"Paused menu required a controller or captured gameplay");
        Check(!editor.Host.Calls.Any(c=>c.Method=="desktop.controllers"),"Paused UI queried a controller");
        Check(editor.Host.Calls.Count(c=>c.Method=="desktop.ui.input")==2,"UI lost mouse press/release");
        Check(game.UiKeyboardOwned,"UI focus did not own keyboard");
        game.Handle(new(ViewportInputKind.KeyDown,VirtualKey:0x09,Modifiers:ViewportModifiers.Shift));
        Check(editor.Host.Calls.Last().Params["kind"]!.GetValue<string>()=="focus_previous","Shift Tab did not navigate backwards");
        game.Handle(new(ViewportInputKind.KeyDown,VirtualKey:0x0D));
        int count=editor.Host.Calls.Count;
        game.Handle(new(ViewportInputKind.KeyDown,VirtualKey:0x0D,Repeat:true));
        game.Handle(new(ViewportInputKind.KeyDown,VirtualKey:0x20));
        game.Handle(new(ViewportInputKind.KeyUp,VirtualKey:0x20));
        Check(editor.Host.Calls.Count==count,"Repeat or Space interfered with Enter confirm ownership");
        game.Handle(new(ViewportInputKind.KeyUp,VirtualKey:0x0D));
        Check(editor.Host.Calls.Last().Params["kind"]!.GetValue<string>()=="accept_up","Enter release lost");
        game.Handle(new(ViewportInputKind.PointerWheel,WheelDelta:120));
        Check(editor.Host.Calls.Last().Params["delta"]!.GetValue<float>()==-1,"Wheel physical units/sign changed");
        game.Handle(new(ViewportInputKind.FocusLost));Check(!game.UiKeyboardOwned,"Focus loss retained UI keyboard");
        editor.PlaybackState="playing";editor.Host.UiConsumes=false;
        game.Handle(new(ViewportInputKind.PointerDown,Button:ViewportMouseButton.Left));
        Check(game.ErrorCount==0 && !game.Captured,"Playing menu-only background click required a controller");
        editor.Host.HasController=true;
        game.Handle(new(ViewportInputKind.PointerDown,Button:ViewportMouseButton.Left));Check(game.Captured,"Non-UI click no longer acquires gameplay");
        Check(!editor.Host.Calls.Any(c=>c.Method=="desktop.input.events"),"Acquisition click fired gameplay");
        Check(!game.UiKeyboardOwned,"Gameplay capture retained stale UI keyboard focus");
        editor.Host.UiConsumes=true;
        game.Handle(new(ViewportInputKind.PointerDown,Button:ViewportMouseButton.Left));
        Check(!game.Captured && game.UiKeyboardOwned,"UI click failed to release gameplay exclusively");
        Check(!editor.Host.Calls.Any(c=>c.Method=="desktop.input.events"),"UI click leaked into gameplay");
        editor.Host.Session="dddddddddddddddddddddddddddddddd";game.Tick();Check(!game.UiKeyboardOwned,"Runtime replacement retained UI focus");
        game.Handle(new(ViewportInputKind.KeyDown,VirtualKey:0x0D));editor.CameraChange();Check(!game.UiKeyboardOwned,"Camera change retained UI focus");
        Check(game.ErrorCount==0,"Routing emitted an error");
        var pendingEditor=new EditorModel { Active=false, CameraAvailable=false };var pending=new GameInput(pendingEditor);
        pending.SelectConfiguration("keyboard_mouse_gamepad",null,"only_connected",0);
        pending.Tick();pending.Tick();Check(pending.ConfigurationPending,"Stopped selection was not retained");
        Check(!pendingEditor.Host.Calls.Any(c=>c.Method=="desktop.input.configure"),"Stopped editor configured runtime input");
        pendingEditor.Active=true;pending.Tick();Check(pending.ConfigurationPending,"Absent Game camera lost pending selection");
        pendingEditor.CameraAvailable=true;pending.Tick();
        var configured=pendingEditor.Host.Calls.Where(c=>c.Method=="desktop.input.configure").ToList();
        Check(configured.Count==1 && configured[0].Params.ContainsKey("camera") && !configured[0].Params.ContainsKey("controller"),"Menu-only pending selection fabricated controller input");
        Check(!pending.ConfigurationPending && pending.Defaults=="keyboard_mouse_gamepad","Applied menu configuration lost preferences");
        for(int i=0;i<10;++i)pending.Tick();
        Check(pendingEditor.Host.Calls.Count(c=>c.Method=="desktop.input.configure")==1,"Configuration repeated every poll");
        pendingEditor.Host.Camera="eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";pending.Tick();pending.Tick();
        Check(pendingEditor.Host.Calls.Count(c=>c.Method=="desktop.input.configure")==2,"Camera change failed exactly-once rebinding");
        pendingEditor.Host.FailConfiguration=true;pendingEditor.Host.Session="ffffffffffffffffffffffffffffffff";
        pending.Tick();long errors=pending.ErrorCount;for(int i=0;i<10;++i)pending.Tick();
        Check(errors==1 && pending.ErrorCount==errors && pending.ConfigurationError is not null,"Failed pending selection retried or disappeared each poll");
        Check(pendingEditor.Host.Calls.Count(c=>c.Method=="desktop.input.configure")==3,"Failed selection acquisition spammed retries");
        pendingEditor.Host.FailConfiguration=false;pending.SelectConfiguration("keyboard_mouse_gamepad",null,"only_connected",0);pending.Tick();
        Check(!pending.ConfigurationPending && pending.ConfigurationError is null && pendingEditor.Host.Calls.Count(c=>c.Method=="desktop.input.configure")==4,"Explicit Apply did not recover pending configuration once");
        pendingEditor.Host.HasController=true;pendingEditor.Host.Camera="11111111111111111111111111111111";pending.Tick();
        Check(pendingEditor.Host.Calls.Last(c=>c.Method=="desktop.input.configure").Params.ContainsKey("controller"),"Player camera did not retain controller configuration path");
        Console.WriteLine("GameInput: paused/playing controller-free UI, input ownership/cancellation, pending camera binding, one attempt per context, failed acquisition retry and controller fallback passed.");return 0;
    }
}
