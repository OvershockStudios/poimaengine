using Poima;
namespace Switchback;
public struct RangeState
{
 public int Phase, Supply, ClearedMask, Ammo, Shots, Rejected, Health, Timer, MenuOpen, ReloadRemaining;
 public int AimTarget, AdsElapsed, HazardInside, PatrolDirection, HasPreviousB, SaveStatus;
 public double AdsStartWorld, AdsTargetWorld, AdsWorld, AdsStartForeground, AdsTargetForeground, AdsForeground, PreviousBX, PreviousBZ;
 public long SaveEpochHigh, SaveEpochLow, SaveSequence;
}
[GameModule("switchback.range")]
public sealed class RangeGame : Game<RangeState>, IInputActionsGame, ICharacterInputGame, ICharacterStanceGame, ICameraLensGame, IHitRegionsGame
{
 static EntityId E(ulong n)=>new(0,n);
 static UiId U(ulong n)=>new(0,n);
 const string Slot="switchback-checkpoint";
 public override void Initialize(ref RangeState s)
 {
  s=default;s.Health=3;s.PatrolDirection=1;s.AdsWorld=s.AdsStartWorld=s.AdsTargetWorld=70;s.AdsForeground=s.AdsStartForeground=s.AdsTargetForeground=75;
 }
 static bool Near(Vector3d p,double x,double z,double radius)=>Math.Abs(p.Y)<=.05 && (p.X-x)*(p.X-x)+(p.Z-z)*(p.Z-z)<=radius*radius;
 static string SaveText(in RangeState s,GameContext c)
 {
  if(s.SaveSequence==0)return c.Saves.Enabled?"No checkpoint yet":"Saving unavailable";
  var ticket=new SaveTicket(s.SaveEpochHigh,s.SaveEpochLow,s.SaveSequence);
  var result=c.GetSaveResult(ticket);
  if(result.State==SaveOperationState.Expired && c.Saves.LastRestore.HasValue)return "Checkpoint restored";
  return "Checkpoint: "+result.State.ToString();
 }
 static void Remember(ref RangeState s,SaveRequestResult r)
 {
  if(r.Accepted){s.SaveEpochHigh=r.Ticket.EpochHigh;s.SaveEpochLow=r.Ticket.EpochLow;s.SaveSequence=r.Ticket.Sequence;s.SaveStatus=1;}
  else s.SaveStatus=-(int)r.Rejection;
 }
 static void Ui(ref RangeState s,GameContext c)
 {
  var stance=c.GetCharacterStance(E(2));
  string phase=s.Phase switch {0=>"WELCOME / SWITCHBACK RANGE",1=>s.MenuOpen!=0?"PAUSED / CHECKPOINT MENU":"SWITCHBACK / LIVE RANGE",2=>"DOWN / RETRY CHECKPOINT",_=>"RANGE CLEAR / COMPLETE"};
  c.SetUi(U(501),phase+"\nCrouch under the green supply lintel. Clear both targets. Return to blue.");
  c.SetUi(U(502),$"AMMO {s.Ammo}/4   TARGETS {((s.ClearedMask&1)!=0?1:0)+((s.ClearedMask&2)!=0?1:0)}/2   HEALTH {s.Health}   RELOAD {s.ReloadRemaining}\n{(stance?.Crouched==true?"CROUCHED":"STANDING")} {(stance?.StandBlocked==true?"/ STAND BLOCKED":"")}  {SaveText(s,c)}");
  c.SetUi(U(503),visible:s.Phase==0);c.SetUi(U(504),visible:s.Phase==1&&s.MenuOpen==0);
  c.SetUi(U(505),visible:s.Phase==1&&s.MenuOpen!=0);c.SetUi(U(506),visible:s.Phase==0||s.Phase==2||s.MenuOpen!=0);
  c.SetUi(U(507),visible:s.Phase==1&&s.MenuOpen!=0);c.SetUi(U(508),visible:s.Phase==2);
 }
 static void Freeze(GameContext c)
 {
  c.SetCharacterInput(E(200),0,0);
  var a=c.GetAnimation(E(201));if(a is {} x && x.Playing)c.SetAnimation(E(201),x.Clip,x.Time,x.Speed,x.Loop,false);
 }
 public override void Tick(ref RangeState s,GameContext c)
 {
  bool active=s.Phase==1&&s.MenuOpen==0;
  if(!active){Freeze(c);Ui(ref s,c);return;}
  ++s.Timer;
  c.SetCharacterStance(E(2),c.GetInputAction(E(2),"crouch").Held);
  int aim=c.GetInputAction(E(2),"aim").Held?1:0;
  if(aim!=s.AimTarget){s.AimTarget=aim;s.AdsStartWorld=s.AdsWorld;s.AdsStartForeground=s.AdsForeground;s.AdsTargetWorld=aim==1?35:70;s.AdsTargetForeground=aim==1?55:75;s.AdsElapsed=0;}
  else if(s.AdsElapsed<12)++s.AdsElapsed;
  s.AdsWorld=CameraFovTransition.Evaluate(s.AdsStartWorld,s.AdsTargetWorld,(uint)s.AdsElapsed,12);
  s.AdsForeground=CameraFovTransition.Evaluate(s.AdsStartForeground,s.AdsTargetForeground,(uint)s.AdsElapsed,12);
  c.SetCameraLens(E(3),new(s.AdsWorld,.1,80),new(s.AdsForeground,.01,2));
  if(s.ReloadRemaining>0 && --s.ReloadRemaining==0)s.Ammo=4;
  if(c.GetInputAction(E(2),"reload").Pressed){if(s.Supply==1&&s.Ammo<4&&s.ReloadRemaining==0)s.ReloadRemaining=12;else ++s.Rejected;}
  var p=c.Get(E(2)).Transform.Position;
  bool hazard=Near(p,0,-4,.4);
  if(hazard&&s.HazardInside==0)--s.Health;
  s.HazardInside=hazard?1:0;
  if(s.Health<=0){s.Phase=2;Freeze(c);Ui(ref s,c);return;}
  if(c.Pressed(E(2),GameAction.Use))
  {
   if(s.Supply==0&&Near(p,-3,0,.5)&&c.GetCharacterStance(E(2))?.Crouched==true){s.Supply=1;s.Ammo=4;c.PlaySound(E(400));}
   else if(s.Supply==1&&s.ClearedMask==3&&Near(p,0,5,.6)){s.Phase=3;c.PlaySound(E(402));Freeze(c);Ui(ref s,c);return;}
   else ++s.Rejected;
  }
  if(c.GetInputAction(E(2),"fire").Pressed)
  {
   if(s.Ammo==0||s.ReloadRemaining>0)++s.Rejected;
   else
   {
    --s.Ammo;++s.Shots;
    var cam=c.Get(E(3)).Transform;
    var hit=c.HitTest(cam.Position,cam.Forward,20,[E(2)]);
    int bit=hit?.Region=="target"?(hit.Value.Entity==E(100)?1:hit.Value.Entity==E(200)?2:0):0;
    if(s.Supply==1&&bit!=0&&(s.ClearedMask&bit)==0){s.ClearedMask|=bit;c.PlaySound(E(401));}
    else ++s.Rejected;
   }
  }
  var b=c.Get(E(200)).Transform.Position;
  bool moving=s.HasPreviousB!=0&&((b.X-s.PreviousBX)*(b.X-s.PreviousBX)+(b.Z-s.PreviousBZ)*(b.Z-s.PreviousBZ)>1e-10);
  s.PreviousBX=b.X;s.PreviousBZ=b.Z;s.HasPreviousB=1;
  bool cleared=(s.ClearedMask&2)!=0;
  if(b.X>=3.5)s.PatrolDirection=-1;else if(b.X<=2.5)s.PatrolDirection=1;
  c.SetCharacterInput(E(200),cleared?0:s.PatrolDirection,0);
  int clip=!cleared&&moving?1:0;
  var animation=c.GetAnimation(E(201));
  if(animation is {} a && (a.Clip!=clip||!a.Playing))c.SetAnimation(E(201),clip,a.Clip==clip?a.Time:0,1,true,true);
  Ui(ref s,c);
 }
 public override void Control(ref RangeState s,ControlContext c)
 {
  switch(c.Action)
  {
   case "begin":if(s.Phase==0){s.Phase=1;s.MenuOpen=0;Remember(ref s,c.TryRequestSave(Slot));c.RequestResume();}break;
   case "menu":if(s.Phase==1&&s.MenuOpen==0){s.MenuOpen=1;c.RequestPause();}break;
   case "resume":if(s.Phase==1&&s.MenuOpen!=0){s.MenuOpen=0;c.RequestResume();}break;
   case "save":if(s.Phase==1&&s.MenuOpen!=0)Remember(ref s,c.TryRequestSave(Slot));break;
   case "retry":case "load":c.TryRequestLoad(Slot);break;
  }
  c.SetUi(U(501),s.Phase==0?"WELCOME / SWITCHBACK RANGE":s.MenuOpen!=0?"PAUSED / CHECKPOINT MENU":"SWITCHBACK RANGE");
  c.SetUi(U(503),visible:s.Phase==0);c.SetUi(U(504),visible:s.Phase==1&&s.MenuOpen==0);c.SetUi(U(505),visible:s.Phase==1&&s.MenuOpen!=0);c.SetUi(U(506),visible:s.Phase==0||s.Phase==2||s.MenuOpen!=0);c.SetUi(U(507),visible:s.Phase==1&&s.MenuOpen!=0);c.SetUi(U(508),visible:s.Phase==2);
  if(s.SaveSequence!=0){var result=c.GetSaveResult(new(s.SaveEpochHigh,s.SaveEpochLow,s.SaveSequence));c.SetUi(U(502),"Checkpoint: "+result.State);}
 }
}
