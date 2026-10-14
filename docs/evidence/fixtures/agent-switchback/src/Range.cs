using Poima;
namespace Switchback;
public struct RangeState
{
 public int Phase,Supply,ClearedMask,Ammo,Shots,Rejected,Health,Timer,MenuOpen,ReloadRemaining;
 public int AimTarget,AdsElapsed,AdsDuration;
 public double WorldStart,WorldTarget,ForegroundStart,ForegroundTarget,WorldFov,ForegroundFov;
 public int HazardInside,PatrolDirection,AnimationClip,HasPreviousB;
 public double PreviousBX,PreviousBZ;
 public long SaveEpochHigh,SaveEpochLow,SaveSequence;
 public int SaveKind,SaveStatus;
 public long SaveGeneration;
}
[GameModule("switchback.range")]
public sealed class RangeGame:Game<RangeState>,IInputActionsGame,IHitRegionsGame,ICharacterInputGame,ICharacterStanceGame,ICameraLensGame
{
 static EntityId E(ulong n)=>new(0,n);
 static UiId U(ulong n)=>new(0,n);
 const string Slot="switchback-checkpoint";
 public override void Initialize(ref RangeState s)
 {
  s=default;s.Health=3;s.PatrolDirection=1;s.AnimationClip=0;
  s.WorldStart=s.WorldTarget=s.WorldFov=70;s.ForegroundStart=s.ForegroundTarget=s.ForegroundFov=75;s.AdsDuration=12;
 }
 static bool Near(Vector3d p,double x,double z,double radius)=>Math.Abs(p.Y)<=.05 && (p.X-x)*(p.X-x)+(p.Z-z)*(p.Z-z)<=radius*radius;
 static void Store(ref RangeState s,SaveRequestResult q,int kind)
 {
  if(!q.Accepted){s.SaveStatus=4;return;}
  s.SaveEpochHigh=q.Ticket.EpochHigh;s.SaveEpochLow=q.Ticket.EpochLow;s.SaveSequence=q.Ticket.Sequence;s.SaveKind=kind;s.SaveStatus=1;
 }
 static void Reconcile(ref RangeState s,GameContext c)
 {
  if(s.SaveStatus is not (1 or 2))return;
  var ticket=new SaveTicket(s.SaveEpochHigh,s.SaveEpochLow,s.SaveSequence);
  var result=c.GetSaveResult(ticket);
  if(result.State==SaveOperationState.Expired)
  {
   // A restored checkpoint contains the initiating write ticket. Native restore
   // metadata proves that this snapshot was published; never enqueue it again.
   if(c.Saves.LastRestore is {} restored){s.SaveStatus=3;s.SaveGeneration=(long)restored.Generation;}
   else s.SaveStatus=0;
  }
  else {s.SaveStatus=(int)result.State;if(result.State==SaveOperationState.Succeeded)s.SaveGeneration=(long)result.Generation;}
 }
 static void Reconcile(ref RangeState s,ControlContext c)
 {
  if(s.SaveStatus is not (1 or 2))return;
  var ticket=new SaveTicket(s.SaveEpochHigh,s.SaveEpochLow,s.SaveSequence);
  var result=c.GetSaveResult(ticket);
  if(result.State==SaveOperationState.Expired)
  {
   // A restored checkpoint contains the initiating write ticket. Native restore
   // metadata proves that this snapshot was published; never enqueue it again.
   if(c.Saves.LastRestore is {} restored){s.SaveStatus=3;s.SaveGeneration=(long)restored.Generation;}
   else s.SaveStatus=0;
  }
  else {s.SaveStatus=(int)result.State;if(result.State==SaveOperationState.Succeeded)s.SaveGeneration=(long)result.Generation;}
 }
 static string Status(in RangeState s)=>s.Phase switch {0=>"WELCOME / SWITCHBACK RANGE",2=>"RANGE FAILED / RETRY CHECKPOINT",3=>"RANGE CLEAR / COMPLETE",_=>s.MenuOpen==1?"PAUSED / CHECKPOINT MENU":"SWITCHBACK / LIVE RANGE"};
 static string Hud(in RangeState s)=>$"AMMO {s.Ammo}/4   TARGETS {((s.ClearedMask&1)!=0?1:0)+((s.ClearedMask&2)!=0?1:0)}/2   HEALTH {s.Health}/3   RELOAD {s.ReloadRemaining}\nSUPPLY {s.Supply}   SHOTS {s.Shots}   REJECTED {s.Rejected}   TICK {s.Timer}\nSAVE {s.SaveStatus switch {1=>"queued",2=>"resolving",3=>"complete",4=>"failed",_=>"none"}} / generation {s.SaveGeneration}";
 static void Ui(ref RangeState s,GameContext c)
 {
  c.SetUi(U(100),Status(s));c.SetUi(U(101),Hud(s));
  c.SetUi(U(110),visible:s.Phase==0);c.SetUi(U(111),visible:s.Phase==0||s.MenuOpen==1||s.Phase==2);
  c.SetUi(U(112),visible:s.Phase==1&&s.MenuOpen==0);c.SetUi(U(113),visible:s.MenuOpen==1);
  c.SetUi(U(114),visible:s.MenuOpen==1);c.SetUi(U(115),visible:s.Phase==2);
  var stance=c.GetCharacterStance(E(2));
  c.SetUi(U(102),stance is {} v?$"{(v.Crouched?"CROUCHED":"STANDING")} {(v.StandBlocked?"/ STAND BLOCKED":"")}":"STANCE UNKNOWN");
 }
 static void UiControl(ref RangeState s,ControlContext c)
 {
  c.SetUi(U(100),Status(s));c.SetUi(U(101),Hud(s));
  c.SetUi(U(110),visible:s.Phase==0);c.SetUi(U(111),visible:s.Phase==0||s.MenuOpen==1||s.Phase==2);
  c.SetUi(U(112),visible:s.Phase==1&&s.MenuOpen==0);c.SetUi(U(113),visible:s.MenuOpen==1);
  c.SetUi(U(114),visible:s.MenuOpen==1);c.SetUi(U(115),visible:s.Phase==2);
 }
 public override void Control(ref RangeState s,ControlContext c)
 {
  Reconcile(ref s,c);
  switch(c.Action)
  {
   case "begin" when s.Phase==0:s.Phase=1;Store(ref s,c.TryRequestSave(Slot),1);c.RequestResume();break;
   case "menu" when s.Phase==1&&s.MenuOpen==0:s.MenuOpen=1;c.RequestPause();break;
   case "resume" when s.Phase==1&&s.MenuOpen==1:s.MenuOpen=0;c.RequestResume();break;
   case "save" when s.MenuOpen==1:Store(ref s,c.TryRequestSave(Slot,s.SaveGeneration>0?s.SaveGeneration:null),1);break;
   case "load" when s.Phase==0||s.Phase==2||s.MenuOpen==1:Store(ref s,c.TryRequestLoad(Slot),2);break;
   case "retry" when s.Phase==2:Store(ref s,c.TryRequestLoad(Slot),2);c.RequestResume();break;
  }
  UiControl(ref s,c);
 }
 static void HoldAnimation(GameContext c)
 {
  if(c.GetAnimation(E(31)) is {} a && a.Playing)c.SetAnimation(E(31),a.Clip,a.Time,a.Speed,a.Loop,false);
 }
 static void Patrol(ref RangeState s,GameContext c)
 {
  var p=c.Get(E(30)).Transform.Position;
  bool moving=s.HasPreviousB!=0 && Math.Abs(p.X-s.PreviousBX)+Math.Abs(p.Z-s.PreviousBZ)>.00001;
  int clip=(s.ClearedMask&2)!=0?0:moving?1:0;
  var a=c.GetAnimation(E(31));
  if(a is {} anim && anim.Clip==clip){if(!anim.Playing)c.SetAnimation(E(31),clip,anim.Time,1,true,true);}
  else c.SetAnimation(E(31),clip);
  s.AnimationClip=clip;s.HasPreviousB=1;s.PreviousBX=p.X;s.PreviousBZ=p.Z;
  if(p.X>=3.5)s.PatrolDirection=-1;if(p.X<=2.5)s.PatrolDirection=1;
  float intent=(float)(s.PatrolDirection*Math.Min(1,Math.Abs((s.PatrolDirection>0?3.5:2.5)-p.X)*60));
  c.SetCharacterInput(E(30),(s.ClearedMask&2)!=0?0:intent,0);
 }
 public override void Tick(ref RangeState s,GameContext c)
 {
  Reconcile(ref s,c);
  bool active=s.Phase==1&&s.MenuOpen==0;
  if(!active){HoldAnimation(c);Ui(ref s,c);return;}
  ++s.Timer;
  c.SetCharacterStance(E(2),c.GetInputAction(E(2),"crouch").Held);
  int aim=c.GetInputAction(E(2),"aim").Held?1:0;
  if(aim!=s.AimTarget){s.WorldStart=s.WorldFov;s.ForegroundStart=s.ForegroundFov;s.WorldTarget=aim==1?35:70;s.ForegroundTarget=aim==1?55:75;s.AdsElapsed=0;s.AimTarget=aim;}
  else s.AdsElapsed=Math.Min(s.AdsDuration,s.AdsElapsed+1);
  s.WorldFov=CameraFovTransition.Evaluate(s.WorldStart,s.WorldTarget,(uint)s.AdsElapsed,(uint)s.AdsDuration);
  s.ForegroundFov=CameraFovTransition.Evaluate(s.ForegroundStart,s.ForegroundTarget,(uint)s.AdsElapsed,(uint)s.AdsDuration);
  c.SetCameraLens(E(3),new(s.WorldFov,.1,80),new(s.ForegroundFov,.01,3));
  if(s.ReloadRemaining>0&&--s.ReloadRemaining==0)s.Ammo=4;
  if(c.GetInputAction(E(2),"reload").Pressed){if(s.Supply==1&&s.Ammo<4&&s.ReloadRemaining==0)s.ReloadRemaining=12;else ++s.Rejected;}
  var p=c.Get(E(2)).Transform.Position;
  bool hazard=Near(p,0,-4,.4);
  if(hazard&&s.HazardInside==0){--s.Health;if(s.Health==0)s.Phase=2;}
  s.HazardInside=hazard?1:0;
  if(s.Phase==2){HoldAnimation(c);Ui(ref s,c);return;}
  if(c.Pressed(E(2),GameAction.Use))
  {
   if(s.Supply==0&&Near(p,-3,0,.5)&&c.GetCharacterStance(E(2)) is {Crouched:true}){s.Supply=1;s.Ammo=4;c.PlaySound(E(50));}
   else if(s.Supply==1&&s.ClearedMask==3&&Near(p,0,5,.6)){s.Phase=3;c.PlaySound(E(51));}
   else ++s.Rejected;
  }
  if(s.Phase==3){HoldAnimation(c);Ui(ref s,c);return;}
  if(c.GetInputAction(E(2),"fire").Pressed)
  {
   if(s.Ammo==0||s.ReloadRemaining>0)++s.Rejected;
   else
   {
    --s.Ammo;++s.Shots;var camera=c.Get(E(3)).Transform;
    var hit=c.HitTest(camera.Position,camera.Forward,20,[E(2)]);
    int bit=hit is {Region:"target"} h?(h.Entity==E(20)?1:h.Entity==E(30)?2:0):0;
    if(s.Supply==1&&bit!=0&&(s.ClearedMask&bit)==0){s.ClearedMask|=bit;c.PlaySound(E(50));}
    else ++s.Rejected;
   }
  }
  Patrol(ref s,c);Ui(ref s,c);
 }
}
