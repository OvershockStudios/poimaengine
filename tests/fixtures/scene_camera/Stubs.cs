// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
namespace Poima.Editor;
// These stand-ins deliberately contain no picking, renderer, Win32 or game logic.
// The fixture exercises the production navigation state machine and motion math.
public interface IViewportInteraction
{
    void Attach(ViewportInput input, IntPtr window);
    void Detach(ViewportInput input);
    void Handle(ViewportInputEvent e);
}
public sealed class ViewportInput
{
    public int Width => 640;
    public int Height => 400;
    public string? LastError => null;
    public bool QualificationInput => true;
    public int IgnoredInteractiveMessages => 0;
    public void CancelCapture() { }
}
public sealed class EditorModel
{
    public FakeHost Host { get; } = new();
    public event Action? SceneChanging;
    public bool Dirty => false;
    public string? RuntimeId => null;
    public long Revision => 0;
    public string? Selected => null;
    public void ChangeScene() => SceneChanging?.Invoke();
    public void Note(string value) => throw new InvalidOperationException(value);
    public void Refresh() { }
    public void RequireSceneEditable() { }
    public void Select(string? id) => throw new NotSupportedException();
    public JsonObject CommitGizmo(long id) => throw new NotSupportedException();
}
public sealed class FakeHost
{
    public JsonObject State { get; } = new();
    public JsonObject Camera { get; private set; } = JsonNode.Parse("{\"position\":[0,0,10],\"yaw\":0,\"pitch\":0,\"vertical_fov\":60}")!.AsObject();
    public long Cut { get; set; }
    public int Writes { get; private set; }
    public JsonObject Call(string method, JsonObject? parameters = null)
    {
        if (method == "desktop.inspect") return new() { ["camera"] = Camera.DeepClone(),
            ["views"] = new JsonObject { ["scene"] = new JsonObject { ["view_cut_generation"] = Cut } } };
        if (method != "desktop.camera") throw new NotSupportedException(method);
        foreach (var (key,value) in parameters!) Camera[key] = value?.DeepClone();
        ++Writes; return Camera.DeepClone().AsObject();
    }
    public double Z => Camera["position"]![2]!.GetValue<double>();
    public void Teleport(double z) => Camera["position"]![2] = z;
}
