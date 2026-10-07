// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
using Poima.Editor;
var checks = new List<string>();
void Check(bool condition,string label) { if (!condition) throw new InvalidOperationException(label); checks.Add(label); }
void Near(double a,double b,string label,double tolerance=1e-9) => Check(Math.Abs(a-b)<tolerance,label);
(EditorModel model, SceneNavigation nav) Fixture()
{
    var model = new EditorModel(); var nav = new SceneNavigation(model);
    nav.Attach(new ViewportInput(),new IntPtr(1)); return (model,nav);
}
void Wheel(SceneNavigation nav,int delta=120,ViewportModifiers modifiers=ViewportModifiers.None) =>
    nav.Handle(new(ViewportInputKind.PointerWheel,X:320,Y:200,WheelDelta:delta,Modifiers:modifiers));
void Fly(SceneNavigation nav) { nav.Handle(new(ViewportInputKind.PointerDown,Button:ViewportMouseButton.Right)); nav.Handle(new(ViewportInputKind.KeyDown,VirtualKey:87)); }
// Compare the real integrated camera at different dispatcher rates, then verify
// controlled coast while RMB is held and immediate stop on capture/focus loss.
double Flight(int fps)
{
    var (model,nav)=Fixture(); Fly(nav);
    for(var i=0;i<fps;++i)nav.Tick(1.0/fps);
    Check(model.Host.Z<10 && model.Host.Z>5,"Acceleration bounds "+fps);
    nav.Handle(new(ViewportInputKind.KeyUp,VirtualKey:87)); var before=model.Host.Z;
    for(var i=0;i<fps;++i)nav.Tick(1.0/fps);
    Check(model.Host.Z<before && before-model.Host.Z<.401,"Bounded deceleration "+fps);
    return model.Host.Z;
}
var thirty=Flight(30);Near(thirty,Flight(60),"30/60 Hz integrated flight equivalence");Near(thirty,Flight(144),"30/144 Hz integrated flight equivalence");
foreach(var kind in new[]{ViewportInputKind.FocusLost,ViewportInputKind.CaptureLost,ViewportInputKind.Resized})
{
    var (model,nav)=Fixture();Fly(nav);nav.Tick(.1);Wheel(nav);
    nav.Handle(new(kind));var before=model.Host.Z;for(var i=0;i<30;++i)nav.Tick(1.0/30);
    Near(model.Host.Z,before,"No drift after "+kind);Check(!nav.Flying,"Gesture canceled "+kind);
}
{
    var(model,nav)=Fixture();Wheel(nav);Near(model.Host.Z,10,"Wheel starts without a jump");nav.Tick(.05);
    Check(model.Host.Z<10 && model.Host.Z>10*Math.Exp(-.12),"Zoom approaches target monotonically");
    var before=model.Host.Z;Wheel(nav,-120);nav.Tick(.05);Check(model.Host.Z>before,"Wheel reversal responds immediately");
    nav.Cancel();before=model.Host.Z;nav.Tick(.1);Near(model.Host.Z,before,"Cancel clears pending zoom");
}
{
    var(model,nav)=Fixture();Wheel(nav);Wheel(nav);for(var i=0;i<60;++i)nav.Tick(1.0/30);
    Near(model.Host.Z,10*Math.Exp(-.24),"Repeated wheel accumulates distance",1e-5);
    Check(!nav.Inspect()["zoom_pending"]!.GetValue<bool>(),"Zoom settles without an endless tail");
}
{
    var(model,nav)=Fixture();Wheel(nav,modifiers:ViewportModifiers.Control);Wheel(nav,modifiers:ViewportModifiers.Alt);
    nav.Handle(new(ViewportInputKind.PointerDown,Button:ViewportMouseButton.Middle));Wheel(nav);nav.Tick(.1);
    Near(model.Host.Z,10,"Modified wheel and pan never enqueue zoom");
}
foreach(var zoom in new[]{false,true})foreach(var cut in new[]{false,true})
{
    var(model,nav)=Fixture();if(zoom)Wheel(nav);else{Fly(nav);nav.Tick(.1);}
    if(cut)model.Host.Cut++;else model.Host.Teleport(42);
    var before=model.Host.Z;var writes=model.Host.Writes;nav.Tick(.1);
    Near(model.Host.Z,before,$"External {(cut?"cut":"pose")} cancels {(zoom?"zoom":"flight")}");
    Check(model.Host.Writes==writes,"External camera never overwritten "+zoom+cut);
}
{
    var(model,nav)=Fixture();Fly(nav);nav.Tick(.1);model.ChangeScene();var before=model.Host.Z;nav.Tick(.1);
    Near(model.Host.Z,before,"Scene edits cancel flight");
    nav.SetSmoothing(false);Wheel(nav);Near(model.Host.Z,before+10*(Math.Exp(-.12)-1),"Immediate wheel mode");
    Fly(nav);before=model.Host.Z;nav.Tick(.1);Near(model.Host.Z,before-.5,"Immediate flight mode");
}
{
    var motion=new SceneCameraMotion();motion.QueueZoom(10,.05);var distance=10.0;
    for(var i=0;i<100;++i)distance=motion.StepZoom(distance,.1);
    Check(distance>=.05,"Minimum zoom distance");motion.QueueZoom(distance,1e100);
    for(var i=0;i<100;++i)distance=motion.StepZoom(distance,.1);
    Check(distance<=1e6,"Maximum zoom distance");motion.Cancel();Near(motion.Speed,0,"Cancel resets velocity");
    Check(motion.StepFlight([0,0,-1],5,0).All(v=>v==0),"Zero dt has no displacement");
    try {motion.StepZoom(10,double.NaN);throw new Exception("NaN accepted");}catch(ArgumentException){checks.Add("Nonfinite time rejected");}
}
Console.WriteLine(JsonSerializer.Serialize(new {passed=true,checks,scope="Production camera math and navigation with stand-in host/input; no renderer or physical input qualification."},new JsonSerializerOptions{WriteIndented=true}));
