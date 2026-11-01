// SPDX-License-Identifier: Apache-2.0
using Poima;
using Poima.Ai;
using System.Numerics;
namespace Poima.Examples;

// Every retained value is a supported primitive field. Sight results and the memory record are transient and
// rebuilt each Tick from these scalars. Configuration fields can be changed through typed gameplay edits.
public struct PerceptionYardState
{
 public int Station,PendingStation,RayBudget,Sampling,TargetMode,SelfMode,MotionTicks,SightStatus,RaysCast,FirstVisible;
 public int S0Status,S0Reason,S1Status,S1Reason,MemoryPhase,MemoryEvents,SearchStatus;
 public long Ticks,FailAt,LastTick,RetainTicks,TotalRays,FirstSeenTick,LastSeenTick,RetainUntilTick,MemoryUpdateTick,Reacquisitions;
 public EntityId S0Hit,S1Hit,MemoryTarget,SearchHit;
 public double HorizontalHalfAngle,VerticalHalfAngle,MinRange,MaxRange,GazeX,GazeZ;
 public double TorsoX,TorsoY,TorsoZ,HeadX,HeadY,HeadZ;
 public double S0Distance,S0HitDistance,S0Azimuth,S0Elevation,S1Distance,S1HitDistance,S1Azimuth,S1Elevation;
 public double LastKnownX,LastKnownY,LastKnownZ,SearchHitDistance;
}

/// <summary>One sentry watching a two-box intruder with native rays and last-seen memory.
/// Sample policy only: the sentry holds its gaze while seeing, turns to the last known position while
/// remembering and returns to rest after expiry. This is not a general NPC brain.</summary>
[GameModule("poima.example.perception-yard")]
public sealed class PerceptionYardGame:Game<PerceptionYardState>
{
 const double TorsoY=.8,HeadY=1.55,MetresPerTick=.5;
 static EntityId E(ulong id)=>new(0,id);
 static UiId U(ulong id)=>new(0,id);
 // The eye sits inside the sentry post (10); the visor (11) is in front of it. Both are self colliders.
 static Vector3d Eye=>new(0,1.6,6);
 static Vector3d Up=>new(0,1,0);

 public override void Initialize(ref PerceptionYardState s)
 {
  s=default;s.Station=2;s.FailAt=-1;s.RayBudget=4;s.RetainTicks=90;s.FirstVisible=-1;
  s.HorizontalHalfAngle=.7;s.VerticalHalfAngle=.45;s.MinRange=.25;s.MaxRange=12;s.GazeZ=-1;
 }

 static void Move(ref PerceptionYardState s,GameContext c)
 {
  var (x,z)=s.PendingStation switch
  {
   1=>(0.0,-2.0),2=>(3.0,-2.0),3=>(-3.0,-2.0),4=>(-7.0,4.0),5=>(0.0,-8.0),
   _=>throw new InvalidOperationException("Unknown Perception Yard station.")
  };
  var current=c.Get(E(20)).Transform.Position;
  double dx=x-current.X,dy=TorsoY-current.Y,dz=z-current.Z;
  // At most0.5m per Tick (30m/s), below the native kinematic limit; torso and head move together.
  uint ticks=(uint)Math.Max(1,Math.Ceiling(Math.Sqrt(dx*dx+dy*dy+dz*dz)/MetresPerTick-1e-6));
  c.MoveKinematic(E(20),new(x,TorsoY,z),Quaternion.Identity,ticks);
  c.MoveKinematic(E(21),new(x,HeadY,z),Quaternion.Identity,ticks);
  s.Station=s.PendingStation;s.PendingStation=0;
 }

 static void Record(in SightSample sample,out int status,out int reason,out EntityId hit,out double distance,out double hitDistance,out double azimuth,out double elevation)
 {
  status=(int)sample.Status;reason=(int)sample.Geometry.Reject;hit=sample.HitEntity;hitDistance=sample.HitDistance;
  distance=sample.Geometry.Distance;azimuth=sample.Geometry.Azimuth;elevation=sample.Geometry.Elevation;
 }

 public override void Tick(ref PerceptionYardState s,GameContext c)
 {
  s.Ticks=checked(s.Ticks+1);
  long tick=SightMemory.Tick(c.Tick);s.LastTick=tick;
  if(s.TargetMode is <0 or >1 || s.SelfMode is <0 or >2 || s.Sampling is <0 or >1 || s.MemoryPhase is <0 or >3)
   throw new InvalidOperationException("Perception Yard modes: target 0..1, self 0..2, sampling 0..1, memory phase 0..3.");
  if(s.PendingStation!=0)Move(ref s,c);
  // Queries and reads see committed poses; motion staged above appears after this Tick's physics.
  var torsoBody=c.Get(E(20));var torso=torsoBody.Transform.Position;var head=c.Get(E(21)).Transform.Position;
  s.MotionTicks=checked((int)torsoBody.MotionRemainingTicks);
  s.TorsoX=torso.X;s.TorsoY=torso.Y;s.TorsoZ=torso.Z;s.HeadX=head.X;s.HeadY=head.Y;s.HeadZ=head.Z;

  Span<EntityId> self=stackalloc EntityId[2];self[0]=E(10);self[1]=E(11);
  Span<EntityId> parts=stackalloc EntityId[2];parts[0]=E(20);parts[1]=E(21);
  ReadOnlySpan<EntityId> ignore=self[..(s.SelfMode==0 ? 2 : s.SelfMode==1 ? 1 : 0)];
  ReadOnlySpan<EntityId> targets=parts[..(s.TargetMode==0 ? 2 : 1)];
  var settings=new SightSettings(Math.PI,s.HorizontalHalfAngle,s.VerticalHalfAngle,s.MinRange,s.MaxRange,(SightSampling)s.Sampling);
  Span<Vector3d> points=stackalloc Vector3d[2];points[0]=torso;points[1]=head;
  Span<SightSample> samples=stackalloc SightSample[2];
  var sight=Sight.Evaluate(new SightObserver(Eye,new(s.GazeX,0,s.GazeZ),Up),settings,points,targets,ignore,s.RayBudget,samples,c);
  s.SightStatus=(int)sight.Status;s.RaysCast=sight.RaysCast;s.FirstVisible=sight.FirstVisible;s.TotalRays=checked(s.TotalRays+sight.RaysCast);
  Record(samples[0],out s.S0Status,out s.S0Reason,out s.S0Hit,out s.S0Distance,out s.S0HitDistance,out s.S0Azimuth,out s.S0Elevation);
  Record(samples[1],out s.S1Status,out s.S1Reason,out s.S1Hit,out s.S1Distance,out s.S1HitDistance,out s.S1Azimuth,out s.S1Elevation);

  var memory=new SightMemoryState((SightMemoryPhase)s.MemoryPhase,s.MemoryTarget,new(s.LastKnownX,s.LastKnownY,s.LastKnownZ),
   s.FirstSeenTick,s.LastSeenTick,s.RetainUntilTick,s.MemoryUpdateTick,s.Reacquisitions);
  // Inconclusive is simply "not seen this Tick"; it never refreshes memory and is never treated as occlusion.
  var step=SightMemory.Update(memory,E(20),sight.Visible ? torso : null,tick,s.RetainTicks);
  var m=step.State;
  s.MemoryPhase=(int)m.Phase;s.MemoryTarget=m.Target;s.LastKnownX=m.LastKnownPosition.X;s.LastKnownY=m.LastKnownPosition.Y;s.LastKnownZ=m.LastKnownPosition.Z;
  s.FirstSeenTick=m.FirstSeenTick;s.LastSeenTick=m.LastSeenTick;s.RetainUntilTick=m.RetainUntilTick;s.MemoryUpdateTick=m.LastUpdateTick;
  s.Reacquisitions=m.Reacquisitions;s.MemoryEvents=(int)step.Events;

  if(m.Phase==SightMemoryPhase.Remembering)
  {
   // Search: face the remembered location, then check whether that spot itself is in view. An empty target
   // list makes Clear the only visible outcome; any collider in the way, including the intruder, occludes.
   double dx=m.LastKnownPosition.X-Eye.X,dz=m.LastKnownPosition.Z-Eye.Z,length=Math.Sqrt(dx*dx+dz*dz);
   if(length>1e-6){s.GazeX=dx/length;s.GazeZ=dz/length;}
   Span<Vector3d> spot=stackalloc Vector3d[1];spot[0]=m.LastKnownPosition;
   Span<SightSample> found=stackalloc SightSample[1];
   var search=Sight.Evaluate(new SightObserver(Eye,new(s.GazeX,0,s.GazeZ),Up),settings,spot,ReadOnlySpan<EntityId>.Empty,ignore,s.RayBudget,found,c);
   s.SearchStatus=(int)found[0].Status;s.SearchHit=found[0].HitEntity;s.SearchHitDistance=found[0].HitDistance;
   s.TotalRays=checked(s.TotalRays+search.RaysCast);
  }
  else
  {
   if(m.Phase!=SightMemoryPhase.Seeing){s.GazeX=0;s.GazeZ=-1;}
   s.SearchStatus=0;s.SearchHit=default;s.SearchHitDistance=0;
  }
  c.SetUi(U(502),$"STATION {s.Station} / {sight.Status} / RAYS {sight.RaysCast}\nMEMORY {m.Phase} / REACQUIRED {m.Reacquisitions}\nSEARCH {(SightSampleStatus)s.SearchStatus}");
  if(s.FailAt>=0 && s.Ticks==s.FailAt)
   throw new InvalidOperationException("Perception second-Tick rollback fixture");
 }

 public override void Control(ref PerceptionYardState s,ControlContext c)
 {
  switch(c.Action)
  {
   case "open":s.PendingStation=1;break;
   case "hide":s.PendingStation=2;break;
   case "low":s.PendingStation=3;break;
   case "flank":s.PendingStation=4;break;
   case "far":s.PendingStation=5;break;
   case "fault":s.FailAt=checked(s.Ticks+2);break;
   case "clear-fault":s.FailAt=-1;break;
   case "reset":
    // Explicit game policy: forget the target and return the gaze to rest.
    s.MemoryPhase=s.MemoryEvents=s.SearchStatus=0;s.MemoryTarget=s.SearchHit=default;
    s.LastKnownX=s.LastKnownY=s.LastKnownZ=s.SearchHitDistance=0;
    s.FirstSeenTick=s.LastSeenTick=s.RetainUntilTick=s.MemoryUpdateTick=s.Reacquisitions=0;
    s.GazeX=0;s.GazeZ=-1;break;
   default:throw new ArgumentException("Unknown Perception Yard control.");
  }
 }
}
