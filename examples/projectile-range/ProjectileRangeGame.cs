// SPDX-License-Identifier: Apache-2.0
using Poima;
using Poima.Fps;
using System.Numerics;
namespace Poima.Examples;

// Every retained operand is a supported primitive. Transient package records
// and CapsuleCastHit are reconstructed each Tick, never nested saved fields.
public struct ProjectileRangeState
{
 public int Mode,PendingMode,Phase,Hit,InitialOverlap,NormalValid,TriangleValid,LastQueries,CenterRayHit,MaxSegments;
 public long Ticks,QueryCount,Launches,FailAt,Triangle;
 public EntityId HitEntity,RayEntity;
 public double PositionX,PositionY,PositionZ,VelocityX,VelocityY,VelocityZ,Age;
 public double AccelerationX,AccelerationY,AccelerationZ,Radius,Lifetime,MaxChordError;
 public double LastConsumedSeconds,LastArcDeviation,Fraction,Distance;
 public double PointOnQueryX,PointOnQueryY,PointOnQueryZ,PointOnObstacleX,PointOnObstacleY,PointOnObstacleZ;
 public double NormalX,NormalY,NormalZ,PenetrationDepth,IncidentVelocityX,IncidentVelocityY,IncidentVelocityZ;
}

/// <summary>One finite-speed sphere projectile using committed native geometry.
/// This example is not a projectile pool, moving-target CCD or a damage system.</summary>
[GameModule("poima.example.projectile-range")]
public sealed class ProjectileRangeGame:Game<ProjectileRangeState>,ICapsuleCastGame
{
 static EntityId E(ulong id)=>new(0,id);
 static UiId U(ulong id)=>new(0,id);
 public override void Initialize(ref ProjectileRangeState s)
 {
  s=default;s.Phase=(int)ProjectilePhase.Expired;s.PositionX=20;s.PositionY=5;
  s.FailAt=-1;s.CenterRayHit=-1;s.Radius=.02;s.Lifetime=2;s.MaxChordError=.0001;s.MaxSegments=8;
 }
 static ProjectileState Read(in ProjectileRangeState s)=>new(new(s.PositionX,s.PositionY,s.PositionZ),new(s.VelocityX,s.VelocityY,s.VelocityZ),s.Age,(ProjectilePhase)s.Phase);
 static void Store(ref ProjectileRangeState s,in ProjectileState value)
 {
  s.PositionX=value.Position.X;s.PositionY=value.Position.Y;s.PositionZ=value.Position.Z;
  s.VelocityX=value.Velocity.X;s.VelocityY=value.Velocity.Y;s.VelocityZ=value.Velocity.Z;
  s.Age=value.Age;s.Phase=(int)value.Phase;
 }
 static void Launch(ref ProjectileRangeState s,GameContext c)
 {
  s.Mode=s.PendingMode;s.PendingMode=0;s.Launches=checked(s.Launches+1);
  s.Hit=s.InitialOverlap=s.NormalValid=s.TriangleValid=s.LastQueries=0;
  s.HitEntity=s.RayEntity=default;s.Triangle=0;s.CenterRayHit=-1;
  s.Fraction=s.Distance=s.PointOnQueryX=s.PointOnQueryY=s.PointOnQueryZ=0;
  s.PointOnObstacleX=s.PointOnObstacleY=s.PointOnObstacleZ=s.NormalX=s.NormalY=s.NormalZ=s.PenetrationDepth=0;
  s.IncidentVelocityX=s.IncidentVelocityY=s.IncidentVelocityZ=0;
  s.AccelerationX=s.AccelerationY=s.AccelerationZ=0;
  s.Radius=.02;s.MaxChordError=.0001;s.MaxSegments=8;s.Lifetime=2;
  Vector3d position,velocity;
  switch(s.Mode)
  {
   case 1:position=new(0,1,0);velocity=new(0,0,-1200);break;
   case 2:position=new(4,3,0);velocity=new(1,2,-5);s.AccelerationY=-9.81;break;
   case 3:position=new(8.115,1,0);velocity=new(0,0,-1200);break;
   case 4:position=new(0,1,-10);velocity=new(0,0,-30);break;
   case 5:position=new(12,3,0);velocity=new(0,0,-1);s.Lifetime=.03125;break;
   default:throw new InvalidOperationException("Unknown projectile launch mode.");
  }
  var flight=Projectiles.Launch(position,velocity);Store(ref s,in flight);
  if(s.Mode==3)
  {
   // At x8.115 the center line lies outside box11's right face x8.1;
   // the physical .02m sphere still grazes it. Marker20 is always ignored.
   Span<EntityId> ignore=stackalloc EntityId[1];ignore[0]=E(20);
   var ray=c.Raycast(position,new(0,0,-1),20,ignore);
   s.CenterRayHit=ray.HasValue?1:0;s.RayEntity=ray?.Entity??default;
  }
 }
 static void Contact(ref ProjectileRangeState s,in CapsuleCastHit h,in ProjectileState flight)
 {
  s.Hit=1;s.HitEntity=h.Entity;s.Fraction=h.Fraction;s.Distance=h.Distance;
  s.PointOnQueryX=h.PointOnQuery.X;s.PointOnQueryY=h.PointOnQuery.Y;s.PointOnQueryZ=h.PointOnQuery.Z;
  s.PointOnObstacleX=h.PointOnObstacle.X;s.PointOnObstacleY=h.PointOnObstacle.Y;s.PointOnObstacleZ=h.PointOnObstacle.Z;
  s.InitialOverlap=h.InitialOverlap?1:0;s.NormalValid=h.Normal.HasValue?1:0;
  s.NormalX=h.Normal?.X??0;s.NormalY=h.Normal?.Y??0;s.NormalZ=h.Normal?.Z??0;s.PenetrationDepth=h.PenetrationDepth;
  s.TriangleValid=h.Triangle.HasValue?1:0;s.Triangle=h.Triangle??0u;
  s.IncidentVelocityX=flight.Velocity.X;s.IncidentVelocityY=flight.Velocity.Y;s.IncidentVelocityZ=flight.Velocity.Z;
 }
 static void Status(in ProjectileRangeState s,GameContext c)
 {
  c.SetUi(U(502),$"MODE {s.Mode} / {(ProjectilePhase)s.Phase}\nAGE {s.Age:F4}s / SWEEPS {s.QueryCount} / LAST {s.LastQueries}\nHIT {s.Hit} / OVERLAP {s.InitialOverlap} / CENTER RAY {s.CenterRayHit}");
 }
 public override void Tick(ref ProjectileRangeState s,GameContext c)
 {
  s.Ticks=checked(s.Ticks+1);
  if(s.PendingMode!=0)Launch(ref s,c);
  s.LastQueries=0;s.LastConsumedSeconds=s.LastArcDeviation=0;
  if(s.Mode!=0)
  {
   if(s.MaxSegments is <1 or >8)throw new InvalidOperationException("Projectile range permits1..8 segments per Tick.");
   var definition=new ProjectileDefinition(new(s.AccelerationX,s.AccelerationY,s.AccelerationZ),s.Radius,s.Lifetime,s.MaxChordError,s.MaxSegments);
   var previous=Read(in s);Span<EntityId> ignore=stackalloc EntityId[1];ignore[0]=E(20);
   var step=Projectiles.Step(in definition,in previous,c.DeltaTime,c,ignore);
   var current=step.State;Store(ref s,in current);s.LastQueries=step.Queries;s.QueryCount=checked(s.QueryCount+step.Queries);
   s.LastConsumedSeconds=step.ConsumedSeconds;s.LastArcDeviation=step.MaxArcDeviation;
   if(step.Hit is {} hit)Contact(ref s,in hit,in current);
   // The native kinematic profile caps speed at100m/s. This marker trails the
   // authoritative flight at no more than90m/s; it is not a fast-flight tracer.
   // The committed pose is read before this Tick's staged write, and its tiny
   // collider is excluded explicitly rather than filtering by name.
   var marker=c.Get(E(20)).Transform.Position;
   double dx=current.Position.X-marker.X,dy=current.Position.Y-marker.Y,dz=current.Position.Z-marker.Z;
   double markerTicks=Math.Max(1,Math.Ceiling(Math.Sqrt(dx*dx+dy*dy+dz*dz)*60/90));
   if(!double.IsFinite(markerTicks)||markerTicks>36000)throw new InvalidOperationException("Marker motion exceeds the native kinematic duration domain.");
   c.MoveKinematic(E(20),current.Position,Quaternion.Identity,(uint)markerTicks);
  }
  Status(in s,c);
  if(s.FailAt>=0&&s.Ticks==s.FailAt)
   throw new InvalidOperationException("Projectile second-Tick staged-marker rollback fixture");
 }
 public override void Control(ref ProjectileRangeState s,ControlContext c)
 {
  switch(c.Action)
  {
   case "fast":s.PendingMode=1;break;
   case "gravity":s.PendingMode=2;break;
   case "graze":s.PendingMode=3;break;
   case "overlap":s.PendingMode=4;break;
   case "expire":s.PendingMode=5;break;
   case "fault":s.FailAt=checked(s.Ticks+2);break;
   case "clear-fault":s.FailAt=-1;break;
   default:throw new ArgumentException("Unknown projectile range control.");
  }
 }
}
