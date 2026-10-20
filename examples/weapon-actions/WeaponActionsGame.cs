// SPDX-License-Identifier: Apache-2.0
using Poima;
using Poima.Fps;
namespace Poima.Examples;

public struct WeaponActionsState
{
 public int Magazine,Chamber,Reserve,Phase,TicksRemaining,Cooldown,BurstRemaining,TriggerHeld;
 public int FireMode,Active,PendingCommands,LastEvents,DryFires,Hits,Misses,AimHeld,Crouched;
 public long AcceptedShots,TickCount,RecoilImpulses;
 public double AimPitch,AimPitchVelocity,AimYaw,AimYawVelocity;
 public double VisualPitch,VisualPitchVelocity,WeaponPitch,WeaponPitchVelocity,WeaponBack,WeaponBackVelocity;
 public double LastAimX,LastAimY,LastAimZ;
}

/// <summary>A small optional-package consumer, not an engine-owned weapon framework.</summary>
[GameModule("poima.example.weapon-actions")]
public sealed partial class WeaponActionsGame : Game<WeaponActionsState>,IInputActionsGame,ICharacterInputGame,ICharacterStanceGame,ICameraLensGame,IHitRegionsGame,ICameraPoseGame
{
 static EntityId E(ulong id)=>new(0,id);
 static UiId U(ulong id)=>new(0,id);
 static readonly ulong[] HiddenControls=[505,506,508];
 static readonly Vector3d[] AimPattern=[new(.012,-.002,0),new(.015,.003,0),new(.017,-.004,0)];
 static readonly Vector3d[] VisualPattern=[new(.035,.0,.0),new(.045,.0,.0),new(.040,.0,.0)];
 public override void Initialize(ref WeaponActionsState s)
 {s=default;s.Magazine=4;s.Chamber=1;s.Reserve=12;s.Phase=(int)WeaponPhase.Ready;}
 static WeaponDefinition Definition(in WeaponActionsState s)=>new(4,3,3,4,2,2,(WeaponFireMode)s.FireMode,3);
 static WeaponState Read(in WeaponActionsState s)=>new(s.Magazine,s.Chamber,s.Reserve,(WeaponPhase)s.Phase,s.TicksRemaining,s.Cooldown,s.BurstRemaining,s.TriggerHeld!=0,s.AcceptedShots);
 static void Store(ref WeaponActionsState s,WeaponStep step)
 {
  var w=step.State;s.Magazine=w.Magazine;s.Chamber=w.Chamber;s.Reserve=w.Reserve;s.Phase=(int)w.Phase;
  s.TicksRemaining=w.TicksRemaining;s.Cooldown=w.Cooldown;s.BurstRemaining=w.BurstRemaining;s.TriggerHeld=w.TriggerHeld?1:0;s.AcceptedShots=w.AcceptedShots;s.LastEvents=(int)step.Events;
 }
 static void Ui(in WeaponActionsState s,GameContext c)
 {
  c.SetUi(U(501),s.Active==0?"WEAPON ACTIONS / BEGIN":s.Active==2?"WEAPON ACTIONS / PAUSED":"WEAPON ACTIONS / LIVE");
  c.SetUi(U(502),$"{(WeaponFireMode)s.FireMode} / {(WeaponPhase)s.Phase} {s.TicksRemaining}\nMAG {s.Magazine}+{s.Chamber} / RESERVE {s.Reserve} / SHOTS {s.AcceptedShots}\nHITS {s.Hits} / MISSES {s.Misses} / DRY {s.DryFires} / QUEUED {s.PendingCommands}");
  c.SetUi(U(503),visible:s.Active==0);c.SetUi(U(504),visible:s.Active==1);c.SetUi(U(507),visible:s.Active==2);
  foreach(ulong id in HiddenControls)c.SetUi(U(id),visible:false);
  c.SetUi(U(510),$"Mode: {(WeaponFireMode)s.FireMode}",visible:true);
  c.SetUi(U(511),visible:true);c.SetUi(U(512),visible:true);c.SetUi(U(513),visible:true);c.SetUi(U(514),visible:true);
 }
 static void Shot(ref WeaponActionsState s,GameContext c)
 {
  // Logical aim uses BaseWorld and saved aim recoil only. Visual camera shake
  // and foreground kick are intentionally excluded from native hit testing.
  var basis=(c.GetCameraPose(E(3))??throw new InvalidOperationException("Camera3 pose required.")).BaseWorld;
  var direction=FpsPoseMath.BaseDirection(in basis,FpsPoseMath.Rotation(new(s.AimPitch,s.AimYaw,0)));
  s.LastAimX=direction.X;s.LastAimY=direction.Y;s.LastAimZ=direction.Z;
  var hit=c.HitTest(basis.Position,direction,20,[E(2)]);
  if(hit?.Region=="target")++s.Hits;else ++s.Misses;
  ulong ordinal=(ulong)(s.AcceptedShots-1);
  var aim=Recoil.AngularKick(AimPattern,ordinal,137,new(.002,.001,0),RecoilMode.Hybrid,stream:0);
  var visual=Recoil.AngularKick(VisualPattern,ordinal,137,new(.007,0,0),RecoilMode.Hybrid,stream:1);
  s.AimPitchVelocity+=aim.X*2*Math.PI*6*Math.E;s.AimYawVelocity+=aim.Y*2*Math.PI*6*Math.E;
  s.VisualPitchVelocity+=visual.X*2*Math.PI*9*Math.E;s.WeaponPitchVelocity+=visual.X*2*Math.PI*8*Math.E;
  s.WeaponBackVelocity+=.03*2*Math.PI*8*Math.E;s.RecoilImpulses=checked(s.RecoilImpulses+1);
 }
 static void Spring(ref double position,ref double velocity,double hz,double dt)
 {var next=Feedback.Step(new ScalarSpring(position,velocity),0,hz,dt);position=next.Position;velocity=next.Velocity;}
 public override void Tick(ref WeaponActionsState s,GameContext c)
 {
  c.SetCharacterInput(E(200),0,0);
  var animation=c.GetAnimation(E(201));if(animation is {} a && a.Playing)c.SetAnimation(E(201),a.Clip,a.Time,a.Speed,a.Loop,false);
  if(s.Active!=1){Ui(in s,c);return;}
  s.TickCount=checked(s.TickCount+1);
  s.AimHeld=c.GetInputAction(E(2),"aim").Held?1:0;s.Crouched=c.GetInputAction(E(2),"crouch").Held?1:0;
  c.SetCharacterStance(E(2),s.Crouched!=0);
  c.SetCameraLens(E(3),new(s.AimHeld!=0?35:70,.1,80),new(s.AimHeld!=0?55:75,.01,2));
  int pending=s.PendingCommands;s.PendingCommands=0;
  var input=new WeaponInput(c.GetInputAction(E(2),"fire").Held,c.GetInputAction(E(2),"reload").Pressed||(pending&8)!=0,(pending&1)!=0,(pending&2)!=0,(pending&4)!=0);
  var definition=Definition(in s);var current=Read(in s);var step=Weapons.Step(in definition,in current,in input);Store(ref s,step);
  if((step.Events&WeaponEvents.DryFire)!=0)++s.DryFires;
  if((step.Events&WeaponEvents.Shot)!=0)Shot(ref s,c);
  Spring(ref s.AimPitch,ref s.AimPitchVelocity,6,c.DeltaTime);Spring(ref s.AimYaw,ref s.AimYawVelocity,6,c.DeltaTime);
  Spring(ref s.VisualPitch,ref s.VisualPitchVelocity,9,c.DeltaTime);Spring(ref s.WeaponPitch,ref s.WeaponPitchVelocity,8,c.DeltaTime);Spring(ref s.WeaponBack,ref s.WeaponBackVelocity,8,c.DeltaTime);
  var aimRotation=FpsPoseMath.Rotation(new(s.AimPitch,s.AimYaw,0));
  var visualRotation=FpsPoseMath.Rotation(new(s.VisualPitch,0,0));
  c.SetCameraPose(E(3),new(new(0,0,0),FpsPoseMath.Product(aimRotation,visualRotation)),new CameraPoseOffset(new(0,0,s.WeaponBack),FpsPoseMath.Rotation(new(s.WeaponPitch,0,0))));
  Ui(in s,c);
 }
 public override void Control(ref WeaponActionsState s,ControlContext c)
 {
  switch(c.Action)
  {
   case "begin":if(s.Active==0){s.Active=1;c.RequestResume();}break;
   case "menu":if(s.Active==1){s.Active=2;c.RequestPause();}break;
   case "resume":if(s.Active==2){s.Active=1;c.RequestResume();}break;
   case "weapon-mode":if(s.BurstRemaining==0)s.FireMode=(s.FireMode+1)%3;break;
   case "weapon-cancel":s.PendingCommands|=1;break;
   case "weapon-holster":s.PendingCommands|=2;break;
   case "weapon-equip":s.PendingCommands|=4;break;
   case "weapon-reload":s.PendingCommands|=8;break;
  }
  c.SetUi(U(501),s.Active==0?"WEAPON ACTIONS / BEGIN":s.Active==2?"WEAPON ACTIONS / PAUSED":"WEAPON ACTIONS / LIVE");
  c.SetUi(U(503),visible:s.Active==0);c.SetUi(U(504),visible:s.Active==1);c.SetUi(U(507),visible:s.Active==2);
  c.SetUi(U(510),$"Mode: {(WeaponFireMode)s.FireMode}");
 }
}
