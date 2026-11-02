// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct ProceduralHandoffState
{
    public EntityId Actor,Camera,Rig,Root,Middle,End,OtherRig,OtherEnd;
    public int Mode,Ticks,Dead,PoseChecks,StagedChecks,OwnWrites,OtherWrites,OtherChecks;
    public int HistoricalReads,HistoricalInitialized,MalformedChecks,ObservedMode,HasOrientation;
    public int HistoricalEnabled,HistoricalSpace,HistoricalClamped,HistoricalFallback,HistoricalHasOrientation;
    public long FailAt,ObservedTick,DeathTick,RootBody,MiddleBody,EndBody;
    public double CommittedYaw,CommittedPitch,ObservedYaw,ObservedPitch;
    public double ActorX,ActorY,ActorZ,CameraX,CameraY,CameraZ;
    public double TargetX,TargetY,TargetZ,EndX,EndY,EndZ,TargetError,OtherEndX,OtherEndY,OtherEndZ;
    public double HistoricalWeight,HistoricalTargetX,HistoricalTargetY,HistoricalTargetZ;
    public double HistoricalPoleX,HistoricalPoleY,HistoricalPoleZ,HistoricalEndX,HistoricalEndY,HistoricalEndZ,HistoricalError;
    public double HistoricalGoalX,HistoricalGoalY,HistoricalGoalZ,HistoricalGoalW,HistoricalOrientationWeight;
    public double HistoricalActualX,HistoricalActualY,HistoricalActualZ,HistoricalActualW,HistoricalAngularError;
    public double OrientationGoalX,OrientationGoalY,OrientationGoalZ,OrientationGoalW,OrientationWeight,AngularError,CallbackBoneGap;
}

[GameModule("poima.test.procedural-handoff")]
public sealed class ProceduralHandoffGame:Game<ProceduralHandoffState>,IPhysicalRigGame,ITwoBoneIkOrientationGame
{
    public override void Initialize(ref ProceduralHandoffState s)
    {s=default;s.Actor=new(0,1);s.Camera=new(0,2);s.Rig=new(0,100);s.OtherRig=new(0,200);s.FailAt=-1;s.DeathTick=-1;}
    static void Near(double actual,double expected,double tolerance=3e-6)
    {
        if(!double.IsFinite(actual)||!double.IsFinite(expected)||Math.Abs(actual-expected)>tolerance)
            throw new InvalidOperationException($"Procedural handoff independent geometry differs: {actual:R} != {expected:R}");
    }
    static double Length(Vector3d p)=>Math.Sqrt(p.X*p.X+p.Y*p.Y+p.Z*p.Z);
    static Vector3d Delta(Vector3d a,Vector3d b)=>new(a.X-b.X,a.Y-b.Y,a.Z-b.Z);
    static void Point(Vector3d a,Vector3d b,double tolerance=2e-7)
    {Near(a.X,b.X,tolerance);Near(a.Y,b.Y,tolerance);Near(a.Z,b.Z,tolerance);}
    static void Rotation(WorldTransform actual,double yaw,double pitch)
    {
        double y=yaw*Math.PI/180,p=pitch*Math.PI/180,cy=Math.Cos(y),sy=Math.Sin(y),cp=Math.Cos(p),sp=Math.Sin(p);
        double[] expected=[cy,0,-sy,0,sy*sp,cp,cy*sp,0,sy*cp,-sp,cy*cp,0];
        for(int column=0;column<3;++column)for(int row=0;row<4;++row)Near(actual[column,row],expected[4*column+row]);Near(actual[3,3],1);
    }
    static void OrientationFrame(WorldTransform wrapper,WorldTransform end,Quaterniond q)
    {
        // Independent transpose composition of normalized FK basis vectors.
        // The fixture has positive uniform scale; no solver/quaternion helper
        // participates in the expected logical rig-space orientation matrix.
        double x=q.X,y=q.Y,z=q.Z,w=q.W;
        double[] expected=[1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y),
            2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x),2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y)];
        for(int column=0;column<3;++column)for(int row=0;row<3;++row)
        {
            double a=0,b=0,value=0;
            for(int k=0;k<3;++k){a+=wrapper[row,k]*wrapper[row,k];b+=end[column,k]*end[column,k];value+=wrapper[row,k]*end[column,k];}
            Near(value/Math.Sqrt(a*b),expected[column*3+row]);
        }
    }
    static void Cache(ref ProceduralHandoffState s,IkOrientedState state)
    {
        var p=state.Point;s.HistoricalEnabled=p.Enabled?1:0;s.HistoricalSpace=(int)p.Space;s.HistoricalClamped=p.Clamped?1:0;s.HistoricalFallback=p.PoleFallback?1:0;
        s.HistoricalWeight=p.Weight;s.HistoricalTargetX=p.Target.X;s.HistoricalTargetY=p.Target.Y;s.HistoricalTargetZ=p.Target.Z;
        s.HistoricalPoleX=p.Pole.X;s.HistoricalPoleY=p.Pole.Y;s.HistoricalPoleZ=p.Pole.Z;
        s.HistoricalEndX=p.EndPosition.X;s.HistoricalEndY=p.EndPosition.Y;s.HistoricalEndZ=p.EndPosition.Z;s.HistoricalError=p.TargetError;
        s.HistoricalHasOrientation=state.EndOrientation.HasValue?1:0;
        if(state.EndOrientation is {} o)
        {
            s.HistoricalGoalX=o.Goal.Rotation.X;s.HistoricalGoalY=o.Goal.Rotation.Y;s.HistoricalGoalZ=o.Goal.Rotation.Z;s.HistoricalGoalW=o.Goal.Rotation.W;
            s.HistoricalOrientationWeight=o.Goal.Weight;s.HistoricalActualX=o.ActualRotation.X;s.HistoricalActualY=o.ActualRotation.Y;
            s.HistoricalActualZ=o.ActualRotation.Z;s.HistoricalActualW=o.ActualRotation.W;s.HistoricalAngularError=o.AngularError;
        }
        s.HistoricalInitialized=1;
    }
    static void Historical(ProceduralHandoffState s,IkOrientedState state)
    {
        var expected=new IkState(s.Rig,1,0,1,2,s.HistoricalEnabled!=0,(IkSpace)s.HistoricalSpace,s.HistoricalWeight,
            new(s.HistoricalTargetX,s.HistoricalTargetY,s.HistoricalTargetZ),new(s.HistoricalPoleX,s.HistoricalPoleY,s.HistoricalPoleZ),
            new(s.HistoricalEndX,s.HistoricalEndY,s.HistoricalEndZ),s.HistoricalError,s.HistoricalClamped!=0,s.HistoricalFallback!=0);
        IkEndOrientationState? orientation=s.HistoricalHasOrientation==0?null:new(new(new(s.HistoricalGoalX,s.HistoricalGoalY,s.HistoricalGoalZ,s.HistoricalGoalW),s.HistoricalOrientationWeight),
            new(s.HistoricalActualX,s.HistoricalActualY,s.HistoricalActualZ,s.HistoricalActualW),s.HistoricalAngularError);
        if(state.Point!=expected||state.EndOrientation!=orientation)throw new InvalidOperationException("Passive historical IK changed or was recomputed from current physics");
    }
    static void PointWrite(ref ProceduralHandoffState s,GameContext c,IkState before)
    {c.SetTwoBoneIk(s.Rig,1,new(before.Target.X+.025,before.Target.Y,before.Target.Z),before.Pole);++s.OwnWrites;}
    static void OrientedWrite(ref ProceduralHandoffState s,GameContext c,IkState before)
    {
        var goal=new IkEndOrientationGoal(new(0,Math.Sin(.125),0,Math.Cos(.125)),.65);
        c.SetTwoBoneIkOrientation(new(s.Rig,1,new(before.Target.X+.015,before.Target.Y,before.Target.Z),before.Pole,true,IkSpace.World,1,goal));++s.OwnWrites;
    }
    public override void Tick(ref ProceduralHandoffState s,GameContext c)
    {
        ++s.Ticks;s.ObservedTick=checked((long)c.Tick);
        var physical=c.GetPhysicalRig(s.Rig)??throw new InvalidOperationException("Procedural physical rig absent");
        var point=c.GetTwoBoneIk(s.Rig,1)??throw new InvalidOperationException("Procedural point IK absent");
        var oriented=c.GetTwoBoneIkOrientation(s.Rig,1)??throw new InvalidOperationException("Procedural oriented IK absent");
        if(point!=oriented.Point||point.Entity!=s.Rig||point.Slot!=1||point.Root!=0||point.Middle!=1||point.End!=2)
            throw new InvalidOperationException("Point/oriented SDK identity or joined diagnostics differ");
        s.ObservedMode=(int)physical.Mode;
        var rootPart=c.GetPhysicalRigPart(s.Rig,"root")??throw new InvalidOperationException("Physical root absent");
        var middlePart=c.GetPhysicalRigPart(s.Rig,"middle")??throw new InvalidOperationException("Physical middle absent");
        var endPart=c.GetPhysicalRigPart(s.Rig,"end")??throw new InvalidOperationException("Physical end absent");
        s.RootBody=rootPart.BodyId;s.MiddleBody=middlePart.BodyId;s.EndBody=endPart.BodyId;
        double yaw=s.CommittedYaw,pitch=s.CommittedPitch;
        foreach(var input in c.Inputs)if(input.Entity==s.Actor&&physical.Mode==PhysicalRigMode.Animated)
        {yaw=Math.IEEERemainder(yaw+input.LookYaw,360);pitch=Math.Clamp(pitch+input.LookPitch,-85,85);}
        var actor=c.Get(s.Actor);var camera=c.Get(s.Camera);Rotation(actor.Transform,yaw,0);Rotation(camera.Transform,yaw,pitch);
        double radians=yaw*Math.PI/180;
        Point(camera.Transform.Position,new(actor.Transform.Position.X+.05*Math.Cos(radians)+.02*Math.Sin(radians),actor.Transform.Position.Y+1.6,
            actor.Transform.Position.Z-.05*Math.Sin(radians)+.02*Math.Cos(radians)),3e-6);
        s.ActorX=actor.Transform.Position.X;s.ActorY=actor.Transform.Position.Y;s.ActorZ=actor.Transform.Position.Z;
        s.CameraX=camera.Transform.Position.X;s.CameraY=camera.Transform.Position.Y;s.CameraZ=camera.Transform.Position.Z;
        s.ObservedYaw=yaw;s.ObservedPitch=pitch;s.TargetX=point.Target.X;s.TargetY=point.Target.Y;s.TargetZ=point.Target.Z;
        s.EndX=point.EndPosition.X;s.EndY=point.EndPosition.Y;s.EndZ=point.EndPosition.Z;s.TargetError=point.TargetError;
        s.HasOrientation=oriented.EndOrientation.HasValue?1:0;
        s.OrientationGoalX=oriented.EndOrientation?.Goal.Rotation.X??0;s.OrientationGoalY=oriented.EndOrientation?.Goal.Rotation.Y??0;
        s.OrientationGoalZ=oriented.EndOrientation?.Goal.Rotation.Z??0;s.OrientationGoalW=oriented.EndOrientation?.Goal.Rotation.W??0;
        s.OrientationWeight=oriented.EndOrientation?.Goal.Weight??0;s.AngularError=oriented.EndOrientation?.AngularError??0;
        if(physical.Mode==PhysicalRigMode.Animated)
        {
            if(s.Dead!=0||!point.Enabled||point.Weight!=1||point.Space!=IkSpace.World||point.Clamped)throw new InvalidOperationException("Animated procedural precondition differs");
            var root=c.Get(s.Root);var middle=c.Get(s.Middle);var end=c.Get(s.End);var wrapper=c.Get(s.Rig);
            Point(end.Transform.Position,point.EndPosition);Point(end.Transform.Position,point.Target,2e-6);
            double scale=Math.Sqrt(root.Transform[0,0]*root.Transform[0,0]+root.Transform[0,1]*root.Transform[0,1]+root.Transform[0,2]*root.Transform[0,2]);
            Near(Length(Delta(middle.Transform.Position,root.Transform.Position)),.5*scale,3e-7);
            Near(Length(Delta(end.Transform.Position,middle.Transform.Position)),.5*scale,3e-7);
            if(oriented.EndOrientation is {} o)OrientationFrame(wrapper.Transform,end.Transform,o.ActualRotation);
            s.CallbackBoneGap=Length(Delta(end.Transform.Position,point.EndPosition));++s.PoseChecks;
        }
        else
        {
            if(s.Dead!=1)throw new InvalidOperationException("Unexpected physical owner without compiled activation");
            if(s.HistoricalInitialized==0)Cache(ref s,oriented);Historical(s,oriented);++s.HistoricalReads;
        }
        var other=c.GetTwoBoneIk(s.OtherRig,1)??throw new InvalidOperationException("Independent IK absent");
        var otherEnd=c.Get(s.OtherEnd);Point(otherEnd.Transform.Position,other.EndPosition);
        Point(other.EndPosition,new(-4+other.Target.X,4+other.Target.Y,other.Target.Z),2e-6);
        s.OtherEndX=other.EndPosition.X;s.OtherEndY=other.EndPosition.Y;s.OtherEndZ=other.EndPosition.Z;++s.OtherChecks;
        c.SetTwoBoneIk(s.OtherRig,1,new(.4+.002*s.Ticks,.6,.1),new(0,0,1),space:IkSpace.Rig);++s.OtherWrites;
        int mode=s.Mode;s.Mode=0;
        switch(mode)
        {
            case 0:break;
            case 1:case 8:PointWrite(ref s,c,point);break;
            case 2:OrientedWrite(ref s,c,point);break;
            case 3:c.ActivatePhysicalRig(s.Rig);s.Dead=1;s.DeathTick=checked((long)c.Tick);break;
            case 4:PointWrite(ref s,c,point);c.ActivatePhysicalRig(s.Rig);s.Dead=1;break;
            case 5:OrientedWrite(ref s,c,point);throw new InvalidOperationException("Procedural handoff immediate callback rollback");
            case 6:PointWrite(ref s,c,point);PointWrite(ref s,c,point);break;
            case 7:
                bool caught=false;try{c.SetTwoBoneIk(s.Rig,1,new(double.NaN,0,0),point.Pole);}catch(ArgumentException){caught=true;}
                if(!caught)throw new InvalidOperationException("Malformed managed IK setter was accepted");++s.MalformedChecks;break;
            default:throw new InvalidOperationException("Unknown procedural handoff mode");
        }
        if(c.GetTwoBoneIk(s.Rig,1)!=point||c.GetTwoBoneIkOrientation(s.Rig,1)!=oriented||c.GetPhysicalRig(s.Rig)!=physical||
            c.GetTwoBoneIk(s.OtherRig,1)!=other||c.Get(s.Actor).Transform!=actor.Transform)
            throw new InvalidOperationException("Staged IK/activation became visible in the same callback");
        ++s.StagedChecks;
        if(s.Dead==0){s.CommittedYaw=yaw;s.CommittedPitch=pitch;}
        if(checked((long)c.Tick)==s.FailAt)throw new InvalidOperationException("Procedural handoff later-Tick callback rollback");
    }
}
