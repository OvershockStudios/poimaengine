// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct GroundedHandoffState
{
    public EntityId Actor,Camera,Rig,LeftRoot,LeftMiddle,LeftEnd,RightRoot,RightMiddle;
    public EntityId RightEnd,OtherRig,OtherEnd,Support;
    public int Mode,Ticks,Dead,PoseChecks,StagedChecks,OwnWrites,OtherChecks,HistoricalReads;
    public int HistoricalInitialized,MalformedChecks,ObservedMode,LeftEnabled,RightEnabled,LeftRequestedEnabled,RightRequestedEnabled,LeftHistoricalEnabled;
    public int LeftHistoricalSpace,LeftHistoricalClamped,LeftHistoricalFallback,LeftHistoricalHasOrientation,LeftHistoricalRequestedEnabled,RightHistoricalEnabled,RightHistoricalSpace,RightHistoricalClamped;
    public int RightHistoricalFallback,RightHistoricalHasOrientation,RightHistoricalRequestedEnabled;
    public long FailAt,ObservedTick,DeathTick,PelvisBody,LeftHipBody,LeftKneeBody,LeftFootBody,RightHipBody;
    public long RightKneeBody,RightFootBody;
    public double CommittedYaw,CommittedPitch,ObservedYaw,ObservedPitch,ActorX,ActorY,ActorZ,CameraX;
    public double CameraY,CameraZ,LeftRequestedWeight,RightRequestedWeight,LeftWeight,RightWeight,LeftTargetError,RightTargetError;
    public double LeftCallbackGap,RightCallbackGap,LeftAngularError,RightAngularError,OtherEndX,OtherEndY,OtherEndZ,LeftHistoricalWeight;
    public double LeftHistoricalTargetX,LeftHistoricalTargetY,LeftHistoricalTargetZ,LeftHistoricalPoleX,LeftHistoricalPoleY,LeftHistoricalPoleZ,LeftHistoricalEndX,LeftHistoricalEndY;
    public double LeftHistoricalEndZ,LeftHistoricalError,LeftHistoricalGoalX,LeftHistoricalGoalY,LeftHistoricalGoalZ,LeftHistoricalGoalW,LeftHistoricalOrientationWeight,LeftHistoricalActualX;
    public double LeftHistoricalActualY,LeftHistoricalActualZ,LeftHistoricalActualW,LeftHistoricalAngularError,LeftHistoricalRequestedWeight,RightHistoricalWeight,RightHistoricalTargetX,RightHistoricalTargetY;
    public double RightHistoricalTargetZ,RightHistoricalPoleX,RightHistoricalPoleY,RightHistoricalPoleZ,RightHistoricalEndX,RightHistoricalEndY,RightHistoricalEndZ,RightHistoricalError;
    public double RightHistoricalGoalX,RightHistoricalGoalY,RightHistoricalGoalZ,RightHistoricalGoalW,RightHistoricalOrientationWeight,RightHistoricalActualX,RightHistoricalActualY,RightHistoricalActualZ;
    public double RightHistoricalActualW,RightHistoricalAngularError,RightHistoricalRequestedWeight;
}

[GameModule("poima.test.grounded-handoff")]
public sealed class GroundedHandoffGame:Game<GroundedHandoffState>,IGroundingControlGame,IPhysicalRigGame
{
    public override void Initialize(ref GroundedHandoffState s)
    {s=default;s.Actor=new(0,100);s.Camera=new(0,109);s.Rig=new(0,101);s.OtherRig=new(0,201);s.FailAt=-1;s.DeathTick=-1;}
    static void Near(double a,double b,double tolerance=3e-6)
    {if(!double.IsFinite(a)||!double.IsFinite(b)||Math.Abs(a-b)>tolerance)throw new InvalidOperationException($"Grounded handoff geometry differs: {a:R} != {b:R}");}
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
        // Independent normalized basis transpose composition strips the
        // positive uniform source scale; no solver quaternion helper is used.
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
    static void Pose(GameContext c,EntityId root,EntityId middle,EntityId end,IkOrientedState state)
    {
        var a=c.Get(root).Transform;var b=c.Get(middle).Transform;var z=c.Get(end).Transform;
        Point(z.Position,state.Point.EndPosition);
        double scale=Math.Sqrt(a[0,0]*a[0,0]+a[0,1]*a[0,1]+a[0,2]*a[0,2]);
        Near(Length(Delta(b.Position,a.Position)),Math.Sqrt(.5*.5+.7*.7)*scale,3e-7);
        Near(Length(Delta(z.Position,b.Position)),Math.Sqrt(.5*.5+.7*.7)*scale,3e-7);
    }
    static void CacheLeft(ref GroundedHandoffState s,IkOrientedState state,GroundingControl requested)
    {
        var p=state.Point;s.LeftHistoricalEnabled=p.Enabled?1:0;s.LeftHistoricalSpace=(int)p.Space;s.LeftHistoricalClamped=p.Clamped?1:0;s.LeftHistoricalFallback=p.PoleFallback?1:0;
        s.LeftHistoricalWeight=p.Weight;s.LeftHistoricalTargetX=p.Target.X;s.LeftHistoricalTargetY=p.Target.Y;s.LeftHistoricalTargetZ=p.Target.Z;
        s.LeftHistoricalPoleX=p.Pole.X;s.LeftHistoricalPoleY=p.Pole.Y;s.LeftHistoricalPoleZ=p.Pole.Z;
        s.LeftHistoricalEndX=p.EndPosition.X;s.LeftHistoricalEndY=p.EndPosition.Y;s.LeftHistoricalEndZ=p.EndPosition.Z;s.LeftHistoricalError=p.TargetError;
        s.LeftHistoricalRequestedEnabled=requested.Enabled?1:0;s.LeftHistoricalRequestedWeight=requested.Weight;s.LeftHistoricalHasOrientation=state.EndOrientation.HasValue?1:0;
        if(state.EndOrientation is {} o){s.LeftHistoricalGoalX=o.Goal.Rotation.X;s.LeftHistoricalGoalY=o.Goal.Rotation.Y;s.LeftHistoricalGoalZ=o.Goal.Rotation.Z;s.LeftHistoricalGoalW=o.Goal.Rotation.W;
            s.LeftHistoricalOrientationWeight=o.Goal.Weight;s.LeftHistoricalActualX=o.ActualRotation.X;s.LeftHistoricalActualY=o.ActualRotation.Y;s.LeftHistoricalActualZ=o.ActualRotation.Z;s.LeftHistoricalActualW=o.ActualRotation.W;s.LeftHistoricalAngularError=o.AngularError;}
    }
    static void HistoricalLeft(GroundedHandoffState s,IkOrientedState actual,GroundingControl requested)
    {
        var expected=new IkState(s.Rig,1,1,2,3,s.LeftHistoricalEnabled!=0,(IkSpace)s.LeftHistoricalSpace,s.LeftHistoricalWeight,
            new(s.LeftHistoricalTargetX,s.LeftHistoricalTargetY,s.LeftHistoricalTargetZ),new(s.LeftHistoricalPoleX,s.LeftHistoricalPoleY,s.LeftHistoricalPoleZ),
            new(s.LeftHistoricalEndX,s.LeftHistoricalEndY,s.LeftHistoricalEndZ),s.LeftHistoricalError,s.LeftHistoricalClamped!=0,s.LeftHistoricalFallback!=0);
        IkEndOrientationState? orientation=s.LeftHistoricalHasOrientation==0?null:new(new(new(s.LeftHistoricalGoalX,s.LeftHistoricalGoalY,s.LeftHistoricalGoalZ,s.LeftHistoricalGoalW),s.LeftHistoricalOrientationWeight),
            new(s.LeftHistoricalActualX,s.LeftHistoricalActualY,s.LeftHistoricalActualZ,s.LeftHistoricalActualW),s.LeftHistoricalAngularError);
        if(actual.Point!=expected||actual.EndOrientation!=orientation||requested!=new GroundingControl(s.LeftHistoricalRequestedEnabled!=0,s.LeftHistoricalRequestedWeight))
            throw new InvalidOperationException("Passive left point/orientation/requested controls changed");
    }
    static void CacheRight(ref GroundedHandoffState s,IkOrientedState state,GroundingControl requested)
    {
        var p=state.Point;s.RightHistoricalEnabled=p.Enabled?1:0;s.RightHistoricalSpace=(int)p.Space;s.RightHistoricalClamped=p.Clamped?1:0;s.RightHistoricalFallback=p.PoleFallback?1:0;
        s.RightHistoricalWeight=p.Weight;s.RightHistoricalTargetX=p.Target.X;s.RightHistoricalTargetY=p.Target.Y;s.RightHistoricalTargetZ=p.Target.Z;
        s.RightHistoricalPoleX=p.Pole.X;s.RightHistoricalPoleY=p.Pole.Y;s.RightHistoricalPoleZ=p.Pole.Z;
        s.RightHistoricalEndX=p.EndPosition.X;s.RightHistoricalEndY=p.EndPosition.Y;s.RightHistoricalEndZ=p.EndPosition.Z;s.RightHistoricalError=p.TargetError;
        s.RightHistoricalRequestedEnabled=requested.Enabled?1:0;s.RightHistoricalRequestedWeight=requested.Weight;s.RightHistoricalHasOrientation=state.EndOrientation.HasValue?1:0;
        if(state.EndOrientation is {} o){s.RightHistoricalGoalX=o.Goal.Rotation.X;s.RightHistoricalGoalY=o.Goal.Rotation.Y;s.RightHistoricalGoalZ=o.Goal.Rotation.Z;s.RightHistoricalGoalW=o.Goal.Rotation.W;
            s.RightHistoricalOrientationWeight=o.Goal.Weight;s.RightHistoricalActualX=o.ActualRotation.X;s.RightHistoricalActualY=o.ActualRotation.Y;s.RightHistoricalActualZ=o.ActualRotation.Z;s.RightHistoricalActualW=o.ActualRotation.W;s.RightHistoricalAngularError=o.AngularError;}
    }
    static void HistoricalRight(GroundedHandoffState s,IkOrientedState actual,GroundingControl requested)
    {
        var expected=new IkState(s.Rig,2,4,5,6,s.RightHistoricalEnabled!=0,(IkSpace)s.RightHistoricalSpace,s.RightHistoricalWeight,
            new(s.RightHistoricalTargetX,s.RightHistoricalTargetY,s.RightHistoricalTargetZ),new(s.RightHistoricalPoleX,s.RightHistoricalPoleY,s.RightHistoricalPoleZ),
            new(s.RightHistoricalEndX,s.RightHistoricalEndY,s.RightHistoricalEndZ),s.RightHistoricalError,s.RightHistoricalClamped!=0,s.RightHistoricalFallback!=0);
        IkEndOrientationState? orientation=s.RightHistoricalHasOrientation==0?null:new(new(new(s.RightHistoricalGoalX,s.RightHistoricalGoalY,s.RightHistoricalGoalZ,s.RightHistoricalGoalW),s.RightHistoricalOrientationWeight),
            new(s.RightHistoricalActualX,s.RightHistoricalActualY,s.RightHistoricalActualZ,s.RightHistoricalActualW),s.RightHistoricalAngularError);
        if(actual.Point!=expected||actual.EndOrientation!=orientation||requested!=new GroundingControl(s.RightHistoricalRequestedEnabled!=0,s.RightHistoricalRequestedWeight))
            throw new InvalidOperationException("Passive right point/orientation/requested controls changed");
    }
    static void Write(ref GroundedHandoffState s,GameContext c)
    {c.SetGroundingControl(new(s.Rig,1,true,.75));c.SetGroundingControl(new(s.Rig,2,true,.65));s.OwnWrites+=2;}
    public override void Tick(ref GroundedHandoffState s,GameContext c)
    {
        ++s.Ticks;s.ObservedTick=checked((long)c.Tick);
        var physical=c.GetPhysicalRig(s.Rig)??throw new InvalidOperationException("Grounded physical owner absent");
        var left=c.GetTwoBoneIkOrientation(s.Rig,1)??throw new InvalidOperationException("Left oriented IK absent");
        var right=c.GetTwoBoneIkOrientation(s.Rig,2)??throw new InvalidOperationException("Right oriented IK absent");
        var requestedLeft=c.GetGroundingControl(s.Rig,1)??throw new InvalidOperationException("Left requested grounding absent");
        var requestedRight=c.GetGroundingControl(s.Rig,2)??throw new InvalidOperationException("Right requested grounding absent");
        for(int n=0;n<16;++n)
            if(c.GetTwoBoneIk(s.Rig,1)!=left.Point||c.GetTwoBoneIk(s.Rig,2)!=right.Point||c.GetTwoBoneIkOrientation(s.Rig,1)!=left||c.GetTwoBoneIkOrientation(s.Rig,2)!=right||
                c.GetGroundingControl(s.Rig,1)!=requestedLeft||c.GetGroundingControl(s.Rig,2)!=requestedRight)
                throw new InvalidOperationException("Repeated prepared point/orientation/requested reads changed");
        if(left.Point.Root!=1||left.Point.Middle!=2||left.Point.End!=3||right.Point.Root!=4||right.Point.Middle!=5||right.Point.End!=6)
            throw new InvalidOperationException("Cooked two-leg chain mapping differs");
        s.ObservedMode=(int)physical.Mode;
        s.PelvisBody=(c.GetPhysicalRigPart(s.Rig,"pelvis")??throw new InvalidOperationException("Pelvis part absent")).BodyId;
        s.LeftHipBody=(c.GetPhysicalRigPart(s.Rig,"left_hip")??throw new InvalidOperationException("Left hip part absent")).BodyId;
        s.LeftKneeBody=(c.GetPhysicalRigPart(s.Rig,"left_knee")??throw new InvalidOperationException("Left knee part absent")).BodyId;
        s.LeftFootBody=(c.GetPhysicalRigPart(s.Rig,"left_foot")??throw new InvalidOperationException("Left foot part absent")).BodyId;
        s.RightHipBody=(c.GetPhysicalRigPart(s.Rig,"right_hip")??throw new InvalidOperationException("Right hip part absent")).BodyId;
        s.RightKneeBody=(c.GetPhysicalRigPart(s.Rig,"right_knee")??throw new InvalidOperationException("Right knee part absent")).BodyId;
        s.RightFootBody=(c.GetPhysicalRigPart(s.Rig,"right_foot")??throw new InvalidOperationException("Right foot part absent")).BodyId;
        double yaw=s.CommittedYaw,pitch=s.CommittedPitch;
        foreach(var input in c.Inputs)if(input.Entity==s.Actor&&physical.Mode==PhysicalRigMode.Animated)
        {yaw=Math.IEEERemainder(yaw+input.LookYaw,360);pitch=Math.Clamp(pitch+input.LookPitch,-85,85);}
        var actor=c.Get(s.Actor);var camera=c.Get(s.Camera);Rotation(actor.Transform,yaw,0);Rotation(camera.Transform,yaw,pitch);
        double radians=yaw*Math.PI/180;Point(camera.Transform.Position,new(actor.Transform.Position.X+.05*Math.Cos(radians)+.02*Math.Sin(radians),actor.Transform.Position.Y+1.6,
            actor.Transform.Position.Z-.05*Math.Sin(radians)+.02*Math.Cos(radians)),3e-6);
        s.ObservedYaw=yaw;s.ObservedPitch=pitch;s.ActorX=actor.Transform.Position.X;s.ActorY=actor.Transform.Position.Y;s.ActorZ=actor.Transform.Position.Z;
        s.CameraX=camera.Transform.Position.X;s.CameraY=camera.Transform.Position.Y;s.CameraZ=camera.Transform.Position.Z;
        s.LeftEnabled=left.Point.Enabled?1:0;s.RightEnabled=right.Point.Enabled?1:0;s.LeftWeight=left.Point.Weight;s.RightWeight=right.Point.Weight;
        s.LeftRequestedEnabled=requestedLeft.Enabled?1:0;s.RightRequestedEnabled=requestedRight.Enabled?1:0;s.LeftRequestedWeight=requestedLeft.Weight;s.RightRequestedWeight=requestedRight.Weight;
        s.LeftTargetError=left.Point.TargetError;s.RightTargetError=right.Point.TargetError;s.LeftAngularError=left.EndOrientation?.AngularError??0;s.RightAngularError=right.EndOrientation?.AngularError??0;
        if(physical.Mode==PhysicalRigMode.Animated)
        {
            if(s.Dead!=0||!left.Point.Enabled||!right.Point.Enabled||left.EndOrientation is null||right.EndOrientation is null)
                throw new InvalidOperationException("Animated native contact/frame precondition absent");
            Pose(c,s.LeftRoot,s.LeftMiddle,s.LeftEnd,left);Pose(c,s.RightRoot,s.RightMiddle,s.RightEnd,right);
            var wrapper=c.Get(s.Rig).Transform;
            if(left.EndOrientation is {} lo)OrientationFrame(wrapper,c.Get(s.LeftEnd).Transform,lo.ActualRotation);
            if(right.EndOrientation is {} ro)OrientationFrame(wrapper,c.Get(s.RightEnd).Transform,ro.ActualRotation);
            s.LeftCallbackGap=Length(Delta(c.Get(s.LeftEnd).Transform.Position,left.Point.EndPosition));s.RightCallbackGap=Length(Delta(c.Get(s.RightEnd).Transform.Position,right.Point.EndPosition));++s.PoseChecks;
        }
        else
        {
            if(s.Dead!=1)throw new InvalidOperationException("Physical owner without actual compiled activation");
            if(s.HistoricalInitialized==0){CacheLeft(ref s,left,requestedLeft);CacheRight(ref s,right,requestedRight);s.HistoricalInitialized=1;}
            HistoricalLeft(s,left,requestedLeft);HistoricalRight(s,right,requestedRight);++s.HistoricalReads;
        }
        var other=c.GetTwoBoneIk(s.OtherRig,1)??throw new InvalidOperationException("Independent live ground IK absent");
        Point(c.Get(s.OtherEnd).Transform.Position,other.EndPosition);s.OtherEndX=other.EndPosition.X;s.OtherEndY=other.EndPosition.Y;s.OtherEndZ=other.EndPosition.Z;++s.OtherChecks;
        int mode=s.Mode;s.Mode=0;
        switch(mode)
        {
            case 0:break;
            case 1:Write(ref s,c);break;
            case 2:c.ActivatePhysicalRig(s.Rig);s.Dead=1;s.DeathTick=checked((long)c.Tick);break;
            case 3:Write(ref s,c);c.ActivatePhysicalRig(s.Rig);s.Dead=1;break;
            case 4:Write(ref s,c);throw new InvalidOperationException("Grounded immediate callback rollback");
            case 5:Write(ref s,c);Write(ref s,c);break;
            case 6:
                bool caught=false;try{c.SetGroundingControl(new(s.Rig,1,true,double.NaN));}catch(ArgumentException){caught=true;}
                if(!caught)throw new InvalidOperationException("Malformed grounding multiplier accepted");++s.MalformedChecks;break;
            case 7:Write(ref s,c);break;
            case 8:c.SetTwoBoneIk(s.Rig,1,left.Point.Target,left.Point.Pole);break;
            case 9:c.Despawn(s.Support);s.Support=default;c.ActivatePhysicalRig(s.Rig);s.Dead=1;s.DeathTick=checked((long)c.Tick);break;
            case 10:c.Despawn(s.Support);s.Support=default;break;
            default:throw new InvalidOperationException("Unknown grounded consumer mode");
        }
        if(c.GetGroundingControl(s.Rig,1)!=requestedLeft||c.GetGroundingControl(s.Rig,2)!=requestedRight||c.GetTwoBoneIkOrientation(s.Rig,1)!=left||
            c.GetTwoBoneIkOrientation(s.Rig,2)!=right||c.GetPhysicalRig(s.Rig)!=physical||c.Get(s.Actor).Transform!=actor.Transform)
            throw new InvalidOperationException("Staged controls/activation leaked into callback reads");
        ++s.StagedChecks;if(s.Dead==0){s.CommittedYaw=yaw;s.CommittedPitch=pitch;}
        if(checked((long)c.Tick)==s.FailAt)throw new InvalidOperationException("Grounded later-Tick callback rollback");
    }
}
