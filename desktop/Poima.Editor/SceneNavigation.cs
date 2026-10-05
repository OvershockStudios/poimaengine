// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;

namespace Poima.Editor;

// Gestures only change the inspection camera. Picking returns an entity through
// the native scene service; the usual Inspector draft guard owns selection.
public sealed class SceneNavigation(EditorModel model)
{
    private ViewportInput? input;
    private readonly HashSet<uint> keys = [];
    private ViewportMouseButton drag;
    private int lastX, lastY, downX, downY, clickCount;
    private string? reportedInputError;
    private bool orbit, moved, fast;
    private double orbitDistance = 10;
    public double FlySpeed { get; private set; } = 5;
    public IntPtr Window { get; private set; }
    public event EventHandler? Changed;
    public event Action<string>? Error;
    public long ErrorCount { get; private set; }
    public void Attach(ViewportInput value, IntPtr window)
    {
        Cancel(); input = value; Window = window; reportedInputError = null;
    }
    public void Detach(ViewportInput value)
    {
        if (input != value) return;
        Cancel(); input = null; Window = IntPtr.Zero;
    }
    public void Cancel()
    {
        drag = ViewportMouseButton.None; keys.Clear(); orbit = false; moved = false;
        input?.CancelCapture();
    }
    private void Guard(Action action)
    {
        try { action(); }
        catch (Exception error) { Cancel(); ++ErrorCount; model.Note(error.Message); Error?.Invoke(error.Message); }
    }
    private JsonObject Camera() => model.Host.Call("desktop.inspect")["camera"]!.AsObject();
    private static double[] Position(JsonObject camera) => camera["position"]!.AsArray().Select(n => n!.GetValue<double>()).ToArray();
    private static (double[] right, double[] up, double[] back) Basis(double yaw, double pitch)
    {
        var y = yaw*Math.PI/180; var p = pitch*Math.PI/180;
        var cy = Math.Cos(y); var sy = Math.Sin(y); var cp = Math.Cos(p); var sp = Math.Sin(p);
        return ([cy, 0, -sy], [sy*sp, cp, cy*sp], [sy*cp, -sp, cy*cp]);
    }
    private static JsonArray Array(double[] values) => new(values.Select(v => (JsonNode?)JsonValue.Create(v)).ToArray());
    private void SetCamera(double[] position, double yaw, double pitch)
    {
        model.Host.Call("desktop.camera", new() { ["position"] = Array(position), ["yaw"] = yaw, ["pitch"] = pitch });
    }
    public void SetSpeed(double speed)
    {
        if (!double.IsFinite(speed) || speed < .1 || speed > 200) throw new ArgumentException("Camera speed must be 0.1–200 units/s.");
        FlySpeed = speed; Changed?.Invoke(this, EventArgs.Empty);
    }
    public void FrameSelection()
    {
        if (model.Selected is null) throw new InvalidOperationException("Select an object to frame.");
        if (input is null || input.Height < 1) throw new InvalidOperationException("The Scene viewport is unavailable.");
        var result = model.Host.Call("desktop.frame", new() { ["revision"] = model.Revision, ["id"] = model.Selected,
            ["aspect"] = (double)input.Width/input.Height });
        orbitDistance = result["distance"]!.GetValue<double>();
    }
    private void Pick(int x, int y, bool frame)
    {
        if (input is null || input.Width < 1 || input.Height < 1 || x < 0 || y < 0 || x >= input.Width || y >= input.Height) return;
        var result = model.Host.Call("desktop.pick", new() { ["revision"] = model.Revision,
            ["x"] = (x+.5)/input.Width, ["y"] = (y+.5)/input.Height, ["aspect"] = (double)input.Width/input.Height });
        model.Select(result["id"]?.GetValue<string>());
        if (frame && model.Selected is not null) FrameSelection();
    }
    public void Handle(ViewportInputEvent e) => Guard(() =>
    {
        fast = e.Shift;
        switch (e.Kind)
        {
            case ViewportInputKind.FocusLost:
            case ViewportInputKind.CaptureLost:
                drag = ViewportMouseButton.None; keys.Clear(); orbit = false; moved = false; break;
            case ViewportInputKind.KeyDown:
                keys.Add(e.VirtualKey);
                if (e.VirtualKey == 0x1B) Cancel();
                else if (e.VirtualKey == 0x46 && !e.Repeat && !e.Control && !e.Alt) FrameSelection();
                break;
            case ViewportInputKind.KeyUp: keys.Remove(e.VirtualKey); break;
            case ViewportInputKind.PointerDown:
                if (drag != ViewportMouseButton.None || e.Button is not (ViewportMouseButton.Left or ViewportMouseButton.Middle or ViewportMouseButton.Right)) break;
                drag = e.Button; orbit = e.Alt && drag == ViewportMouseButton.Left;
                lastX = downX = e.X; lastY = downY = e.Y; moved = false; clickCount = e.ClickCount;
                break;
            case ViewportInputKind.PointerUp:
                if (drag != e.Button) break;
                moved |= Math.Abs(e.X-downX) + Math.Abs(e.Y-downY) > 5;
                var select = drag == ViewportMouseButton.Left && !orbit && !moved;
                drag = ViewportMouseButton.None; orbit = false;
                if (select) Pick(e.X, e.Y, clickCount > 1);
                break;
            case ViewportInputKind.PointerMove:
                if (drag == ViewportMouseButton.None) break;
                var dx = e.X-lastX; var dy = e.Y-lastY; lastX = e.X; lastY = e.Y;
                moved |= Math.Abs(e.X-downX) + Math.Abs(e.Y-downY) > 5;
                if (dx == 0 && dy == 0 || drag == ViewportMouseButton.Left && !orbit) break;
                var camera = Camera(); var pos = Position(camera);
                var yaw = camera["yaw"]!.GetValue<double>(); var pitch = camera["pitch"]!.GetValue<double>();
                var basis = Basis(yaw, pitch);
                if (drag == ViewportMouseButton.Right || orbit)
                {
                    yaw = Math.IEEERemainder(yaw-dx*.15, 360); pitch = Math.Clamp(pitch-dy*.15, -89, 89);
                    if (orbit)
                    {
                        var back = Basis(yaw,pitch).back;
                        for (int i=0;i<3;++i) pos[i] += (back[i]-basis.back[i])*orbitDistance;
                    }
                }
                else if (drag == ViewportMouseButton.Middle)
                {
                    var units = 2*orbitDistance*Math.Tan(camera["vertical_fov"]!.GetValue<double>()*Math.PI/360)/Math.Max(1,input?.Height ?? 1);
                    for (int i=0;i<3;++i) pos[i] += (-basis.right[i]*dx+basis.up[i]*dy)*units;
                }
                SetCamera(pos,yaw,pitch); break;
            case ViewportInputKind.PointerWheel:
                if (e.HorizontalWheel || e.WheelDelta == 0) break;
                if (drag != ViewportMouseButton.Right && (input is null || e.X < 0 || e.Y < 0 || e.X >= input.Width || e.Y >= input.Height)) break;
                var multiplier = Math.Exp(Math.Clamp(-e.WheelDelta/120.0*.12,-3,3));
                if (drag == ViewportMouseButton.Right) { SetSpeed(Math.Clamp(FlySpeed/multiplier,.1,200)); break; }
                var wheelCamera = Camera(); var wheelPosition = Position(wheelCamera);
                var wheelYaw = wheelCamera["yaw"]!.GetValue<double>(); var wheelPitch = wheelCamera["pitch"]!.GetValue<double>();
                var wheelBack = Basis(wheelYaw,wheelPitch).back;
                var distance = Math.Clamp(orbitDistance*multiplier,.05,1e6);
                for (int i=0;i<3;++i) wheelPosition[i] += wheelBack[i]*(distance-orbitDistance);
                SetCamera(wheelPosition,wheelYaw,wheelPitch); orbitDistance = distance; break;
        }
    });
    public void Tick(double seconds) => Guard(() =>
    {
        if (input?.LastError is string error)
        {
            if (reportedInputError != error) { reportedInputError = error; throw new InvalidOperationException(error); }
            return;
        }
        if (input is null || drag != ViewportMouseButton.Right || !double.IsFinite(seconds)) return;
        int Axis(uint positive,uint negative) => (keys.Contains(positive) ? 1 : 0)-(keys.Contains(negative) ? 1 : 0);
        var f=Axis(0x57,0x53); var r=Axis(0x44,0x41); var u=Axis(0x45,0x51);
        if (f==0 && r==0 && u==0) return;
        var camera=Camera(); var position=Position(camera); var yaw=camera["yaw"]!.GetValue<double>(); var pitch=camera["pitch"]!.GetValue<double>();
        var basis=Basis(yaw,pitch); var direction=new double[3];
        for(int i=0;i<3;++i) direction[i]=-basis.back[i]*f+basis.right[i]*r+(i==1 ? u : 0);
        var length=Math.Sqrt(direction.Sum(v=>v*v));
        var speed=FlySpeed*(fast || keys.Contains(0x10) || keys.Contains(0xA0) || keys.Contains(0xA1) ? 4 : 1)*Math.Clamp(seconds,0,.1);
        if(length>1e-8) for(int i=0;i<3;++i) position[i] += direction[i]/length*speed;
        SetCamera(position,yaw,pitch);
    });
    public JsonObject Inspect() => new() { ["attached"] = input is not null, ["width"] = input?.Width ?? 0, ["height"] = input?.Height ?? 0,
        ["fly_speed"] = FlySpeed, ["orbit_distance"] = orbitDistance, ["drag"] = drag.ToString(), ["pressed_keys"] = keys.Count,
        ["input_error"] = input?.LastError, ["error_count"] = ErrorCount };
}
