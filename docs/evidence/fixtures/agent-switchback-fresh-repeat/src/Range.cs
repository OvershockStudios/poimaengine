using Poima;
namespace Switchback;
[GameModule("switchback-range-original")]
public sealed class Range : Game<State>, IInputActionsGame, ICharacterInputGame, ICharacterStanceGame, ICameraLensGame, IHitRegionsGame {
 static EntityId E(ulong n)=>new(0,n);
 static UiId U(ulong n)=>new(0,n);
 public override void Initialize(ref State s) {
  s=default;s.Health=3;s.WorldStart=s.WorldTarget=70;s.ForegroundStart=s.ForegroundTarget=75;s.AdsDuration=12;s.PatrolDirection=1;s.PreviousBX=3;s.PreviousBZ=-6;
 }
 static bool Inside(Vector3d p,double x,double z,double radius)=>Math.Abs(p.Y)<=.05 && (p.X-x)*(p.X-x)+(p.Z-z)*(p.Z-z)<=radius*radius;
 static void Ticket(ref State s,SaveRequestResult r) {
  if(!r.Accepted){s.SaveState=4;return;}
  s.SaveHigh=r.Ticket.EpochHigh;s.SaveLow=r.Ticket.EpochLow;s.SaveSequence=r.Ticket.Sequence;s.SaveState=1;
 }
 static void ObserveSave(ref State s,GameContext c) {
  if(s.SaveSequence==0)return;
  var r=c.GetSaveResult(new(s.SaveHigh,s.SaveLow,s.SaveSequence));
  if(r.State==SaveOperationState.Expired && c.Saves.LastRestore is {} restore){s.SaveState=3;s.SaveGeneration=(long)restore.Generation;}
  else {s.SaveState=(int)r.State;if(r.State==SaveOperationState.Succeeded)s.SaveGeneration=(long)r.Generation;}
 }
 static string Status(in State s)=>$"SWITCHBACK | Ammo {s.Ammo}/4 | Targets {((s.ClearedMask&1)!=0?1:0)+((s.ClearedMask&2)!=0?1:0)}/2 | Health {s.Health} | Reload {s.ReloadRemaining} | Save {(s.SaveState==3?"completed":s.SaveState==4?"failed":s.SaveState==1||s.SaveState==2?"pending":"none")} | {(s.Phase==0?"Welcome: crouch under amber lintel and Use supply":s.Phase==2?"Dead":s.Phase==3?"Range complete":s.MenuOpen!=0?"Paused":"Running")}";
 static void Ui(ref State s,GameContext c) {
  var stance=c.GetCharacterStance(E(2));
  c.SetUi(U(100),Status(s)+$" | {(stance?.Crouched==true?"crouched":"standing")} {(stance?.StandBlocked==true?"stand blocked":"")}");
  c.SetUi(U(101),visible:s.Phase==0);c.SetUi(U(102),visible:s.Phase==0||s.Phase==2||s.MenuOpen!=0);
  c.SetUi(U(103),visible:s.Phase==1&&s.MenuOpen==0);c.SetUi(U(104),visible:s.MenuOpen!=0);c.SetUi(U(105),visible:s.MenuOpen!=0);c.SetUi(U(106),visible:s.Phase==2);
 }
 public override void Control(ref State s,ControlContext c) {
  if(s.SaveSequence!=0){var r=c.GetSaveResult(new(s.SaveHigh,s.SaveLow,s.SaveSequence));if(r.State==SaveOperationState.Expired&&c.Saves.LastRestore is {} restore){s.SaveState=3;s.SaveGeneration=(long)restore.Generation;}else {s.SaveState=(int)r.State;if(r.State==SaveOperationState.Succeeded)s.SaveGeneration=(long)r.Generation;}}
  switch(c.Action){
   case "begin" when s.Phase==0:s.Phase=1;Ticket(ref s,c.TryRequestSave("switchback-checkpoint"));c.RequestResume();break;
   case "menu" when s.Phase==1:s.MenuOpen=1;c.RequestPause();break;
   case "resume" when s.Phase==1:s.MenuOpen=0;c.RequestResume();break;
   case "save" when s.MenuOpen!=0:Ticket(ref s,c.TryRequestSave("switchback-checkpoint",s.SaveGeneration==0?null:s.SaveGeneration));break;
   case "load":case "retry":Ticket(ref s,c.TryRequestLoad("switchback-checkpoint"));break;
  }
  c.SetUi(U(100),Status(s));c.SetUi(U(101),visible:s.Phase==0);c.SetUi(U(102),visible:s.Phase==0||s.Phase==2||s.MenuOpen!=0);
  c.SetUi(U(103),visible:s.Phase==1&&s.MenuOpen==0);c.SetUi(U(104),visible:s.MenuOpen!=0);c.SetUi(U(105),visible:s.MenuOpen!=0);c.SetUi(U(106),visible:s.Phase==2);
 }
 public override void Tick(ref State s,GameContext c) {
  ObserveSave(ref s,c);
  bool active=s.Phase==1&&s.MenuOpen==0;
  if(!active){Ui(ref s,c);return;}
  s.Timer++;
  c.SetCharacterStance(E(2),c.GetInputAction(E(2),"crouch").Held);
  int aim=c.GetInputAction(E(2),"aim").Held?1:0;
  if(aim!=s.AimTarget){s.WorldStart=CameraFovTransition.Evaluate(s.WorldStart,s.WorldTarget,(uint)s.AdsElapsed,(uint)s.AdsDuration);s.ForegroundStart=CameraFovTransition.Evaluate(s.ForegroundStart,s.ForegroundTarget,(uint)s.AdsElapsed,(uint)s.AdsDuration);s.WorldTarget=aim==1?35:70;s.ForegroundTarget=aim==1?55:75;s.AdsElapsed=0;s.AimTarget=aim;}
  else if(s.AdsElapsed<s.AdsDuration)s.AdsElapsed++;
  c.SetCameraLens(E(3),new(CameraFovTransition.Evaluate(s.WorldStart,s.WorldTarget,(uint)s.AdsElapsed,(uint)s.AdsDuration),.1,80),new(CameraFovTransition.Evaluate(s.ForegroundStart,s.ForegroundTarget,(uint)s.AdsElapsed,(uint)s.AdsDuration),.01,3));
  if(s.ReloadRemaining>0&&--s.ReloadRemaining==0)s.Ammo=4;
  if(c.GetInputAction(E(2),"reload").Pressed){if(s.Supply==1&&s.Ammo<4&&s.ReloadRemaining==0)s.ReloadRemaining=12;else s.Rejected++;}
  var p=c.Get(E(2)).Transform.Position;
  bool hazard=Inside(p,0,-4,.4);
  if(hazard&&s.HazardInside==0){s.Health--;if(s.Health==0){s.Phase=2;s.HazardInside=1;Ui(ref s,c);return;}}
  s.HazardInside=hazard?1:0;
  if(c.Pressed(E(2),GameAction.Use)){
   if(s.Supply==0&&Inside(p,-3,0,.5)&&c.GetCharacterStance(E(2))?.Crouched==true){s.Supply=1;s.Ammo=4;c.PlaySound(E(50));}
   else if(Inside(p,0,5,.6)&&s.Supply==1&&s.ClearedMask==3){s.Phase=3;c.PlaySound(E(51));Ui(ref s,c);return;}
   else s.Rejected++;
  }
  if(c.GetInputAction(E(2),"fire").Pressed){
   if(s.Ammo==0||s.ReloadRemaining>0)s.Rejected++;
   else {s.Ammo--;s.Shots++;var camera=c.Get(E(3)).Transform;var h=c.HitTest(camera.Position,camera.Forward,20,new EntityId[]{E(2)});int bit=h?.Region=="target"?(h.Value.Entity==E(20)?1:h.Value.Entity==E(30)?2:0):0;
    if(s.Supply==1&&bit!=0&&(s.ClearedMask&bit)==0){s.ClearedMask|=bit;c.PlaySound(E(50));}else s.Rejected++;
   }
  }
  var b=c.Get(E(30)).Transform.Position;
  int clip=(s.ClearedMask&2)==0&&(Math.Abs(b.X-s.PreviousBX)+Math.Abs(b.Z-s.PreviousBZ)>.00001)?1:0;
  if(c.GetAnimation(E(31)) is {} anim&&(anim.Clip!=clip||!anim.Playing))c.SetAnimation(E(31),clip);
  s.AnimationClip=clip;s.PreviousBX=b.X;s.PreviousBZ=b.Z;
  if(b.X>=3.5)s.PatrolDirection=-1;else if(b.X<=2.5)s.PatrolDirection=1;
  float patrol=(float)Math.Clamp((s.PatrolDirection>0?3.5-b.X:2.5-b.X)*60,-1,1);
  c.SetCharacterInput(E(30),(s.ClearedMask&2)!=0?0:patrol,0);
  Ui(ref s,c);
 }
}
