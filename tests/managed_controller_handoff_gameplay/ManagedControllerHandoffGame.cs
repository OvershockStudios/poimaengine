// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct ControllerHandoffState
{
    public EntityId Actor,Camera,Rig,Control,Region,Target;
    public int Mode,Ticks,Dead,CheckGeometry,PoseChecks,GeometryChecks,StagedChecks,Writes,ObservedMode,Crouched;
    public long FailAt,ObservedTick,RootBody,TipBody;
    public double CommittedYaw,CommittedPitch,ControlYaw,ControlPitch,ObservedYaw,ObservedPitch;
    public double ActorX,ActorY,ActorZ,ActorFx,ActorFy,ActorFz,Vx,Vy,Vz;
    public double CameraX,CameraY,CameraZ,CameraFx,CameraFy,CameraFz;
    public double BaseX,BaseY,BaseZ,BaseFx,BaseFy,BaseFz;
    public double EffectX,EffectY,EffectZ,RegionDistance,GeometryDistance;
}

[GameModule("poima.test.controller-handoff")]
public sealed class ControllerHandoffGame:Game<ControllerHandoffState>,IPhysicalRigGame,
    ICharacterInputGame,ICharacterStanceGame,ICameraPoseGame,IHitRegionsGame
{
    public override void Initialize(ref ControllerHandoffState s)
    {
        s=default;s.Actor=new(0,1);s.Camera=new(0,2);s.Rig=new(0,100);
        s.Control=new(0,20);s.Region=new(0,30);s.Target=new(0,600);s.FailAt=-1;
    }
    static void Near(double actual,double expected,double tolerance=3e-6)
    {
        if(!double.IsFinite(actual) || !double.IsFinite(expected) || Math.Abs(actual-expected)>tolerance)
            throw new InvalidOperationException($"Handoff independent geometry differs: {actual:R} != {expected:R}");
    }
    static void Rotation(WorldTransform matrix,double yaw,double pitch)
    {
        double y=yaw*Math.PI/180,p=pitch*Math.PI/180,cy=Math.Cos(y),sy=Math.Sin(y),cp=Math.Cos(p),sp=Math.Sin(p);
        // Independent analytic Y-heading followed by X-pitch, column-major.
        double[] expected=[cy,0,-sy,0, sy*sp,cp,cy*sp,0, sy*cp,-sp,cy*cp,0];
        for(int c=0;c<3;++c)for(int r=0;r<4;++r)Near(matrix[c,r],expected[c*4+r]);
        Near(matrix[3,3],1);
    }
    public override void Tick(ref ControllerHandoffState s,GameContext c)
    {
        ++s.Ticks;s.ObservedTick=checked((long)c.Tick);
        double yaw=s.CommittedYaw,pitch=s.CommittedPitch,controlYaw=s.ControlYaw,controlPitch=s.ControlPitch;
        foreach(var input in c.Inputs)
        {
            if(input.Entity==s.Actor && s.Dead==0)
            {yaw=Math.IEEERemainder(yaw+input.LookYaw,360);pitch=Math.Clamp(pitch+input.LookPitch,-85,85);}
            if(input.Entity==s.Control)
            {controlYaw=Math.IEEERemainder(controlYaw+input.LookYaw,360);controlPitch=Math.Clamp(controlPitch+input.LookPitch,-85,85);}
        }
        var actor=c.Get(s.Actor);var camera=c.Get(s.Camera);var control=c.Get(s.Control);
        var pose=c.GetCameraPose(s.Camera) ?? throw new InvalidOperationException("Handoff camera absent");
        var stance=c.GetCharacterStance(s.Actor) ?? throw new InvalidOperationException("Handoff stance absent");
        Rotation(actor.Transform,yaw,0);Rotation(camera.Transform,yaw,pitch);Rotation(pose.BaseWorld,yaw,pitch);
        Rotation(control.Transform,controlYaw,0);
        Near(pose.BaseWorld.Position.X-actor.Transform.Position.X,0);
        Near(pose.BaseWorld.Position.Y-actor.Transform.Position.Y,1.6);
        Near(pose.BaseWorld.Position.Z-actor.Transform.Position.Z,0);
        // The authored effect is caller-controlled. BaseWorld excludes effects;
        // Get(Camera) must include that same effect exactly once.
        var effect=pose.World.Position;
        Near(camera.Transform.Position.X,pose.BaseWorld.Position.X+pose.BaseWorld[0,0]*effect.X+pose.BaseWorld[1,0]*effect.Y+pose.BaseWorld[2,0]*effect.Z,1e-10);
        Near(camera.Transform.Position.Y,pose.BaseWorld.Position.Y+pose.BaseWorld[0,1]*effect.X+pose.BaseWorld[1,1]*effect.Y+pose.BaseWorld[2,1]*effect.Z,1e-10);
        Near(camera.Transform.Position.Z,pose.BaseWorld.Position.Z+pose.BaseWorld[0,2]*effect.X+pose.BaseWorld[1,2]*effect.Y+pose.BaseWorld[2,2]*effect.Z,1e-10);
        if(stance.Crouched)throw new InvalidOperationException("Suppressed death input changed capsule posture");
        ++s.PoseChecks;s.ObservedYaw=yaw;s.ObservedPitch=pitch;s.Crouched=stance.Crouched?1:0;
        s.ActorX=actor.Transform.Position.X;s.ActorY=actor.Transform.Position.Y;s.ActorZ=actor.Transform.Position.Z;
        s.ActorFx=actor.Transform.Forward.X;s.ActorFy=actor.Transform.Forward.Y;s.ActorFz=actor.Transform.Forward.Z;
        s.Vx=actor.Velocity.X;s.Vy=actor.Velocity.Y;s.Vz=actor.Velocity.Z;
        s.CameraX=camera.Transform.Position.X;s.CameraY=camera.Transform.Position.Y;s.CameraZ=camera.Transform.Position.Z;
        s.CameraFx=camera.Transform.Forward.X;s.CameraFy=camera.Transform.Forward.Y;s.CameraFz=camera.Transform.Forward.Z;
        s.BaseX=pose.BaseWorld.Position.X;s.BaseY=pose.BaseWorld.Position.Y;s.BaseZ=pose.BaseWorld.Position.Z;
        s.BaseFx=pose.BaseWorld.Forward.X;s.BaseFy=pose.BaseWorld.Forward.Y;s.BaseFz=pose.BaseWorld.Forward.Z;
        s.EffectX=effect.X;s.EffectY=effect.Y;s.EffectZ=effect.Z;
        if(s.CheckGeometry!=0)
        {
            Near(yaw,90);Near(pitch,0);
            var region=c.HitTest(pose.BaseWorld.Position,pose.BaseWorld.Forward,20)
                ?? throw new InvalidOperationException("New-look attached region missed");
            if(region.Entity!=s.Actor || region.RegionEntity!=s.Region || region.Region!="aim-witness")
                throw new InvalidOperationException("New-look region owner/attachment differs");
            Near(region.Distance,3.8,2e-5);
            var geometry=c.Raycast(pose.BaseWorld.Position,pose.BaseWorld.Forward,20,[s.Actor])
                ?? throw new InvalidOperationException("New-look native geometry missed");
            if(geometry.Entity!=s.Target)throw new InvalidOperationException("New-look geometry identity differs");
            Near(geometry.Distance,7.5,2e-5);s.RegionDistance=region.Distance;s.GeometryDistance=geometry.Distance;
            ++s.GeometryChecks;s.CheckGeometry=0;
        }
        var rig=c.GetPhysicalRig(s.Rig) ?? throw new InvalidOperationException("Handoff rig absent");
        var root=c.GetPhysicalRigPart(s.Rig,"root") ?? throw new InvalidOperationException("Handoff root absent");
        var tip=c.GetPhysicalRigPart(s.Rig,"tip") ?? throw new InvalidOperationException("Handoff tip absent");
        s.ObservedMode=(int)rig.Mode;s.RootBody=root.BodyId;s.TipBody=tip.BodyId;
        int mode=s.Mode;s.Mode=0;
        if(mode is 1 or 2 or 6)
        {
            if(mode==2){c.SetCharacterInput(s.Actor,1,1,90,20,true);++s.Writes;}
            c.SetCharacterStance(s.Actor,true);c.ActivatePhysicalRig(s.Rig);s.Writes+=2;
            if(mode==6)throw new InvalidOperationException("Handoff immediate callback rollback");
        }
        else if(mode==3){c.ActivatePhysicalRig(s.Rig);c.ActivatePhysicalRig(s.Rig);s.Writes+=2;}
        else if(mode!=0)throw new InvalidOperationException("Unknown handoff fixture mode");
        if(c.GetPhysicalRig(s.Rig)!=rig || c.GetPhysicalRigPart(s.Rig,"root")!=root || c.GetPhysicalRigPart(s.Rig,"tip")!=tip ||
            c.GetCameraPose(s.Camera)!=pose || c.Get(s.Actor).Transform!=actor.Transform)
            throw new InvalidOperationException("Staged handoff leaked into callback observations");
        ++s.StagedChecks;
        if(mode is 1 or 2)s.Dead=1;
        else if(s.Dead==0){s.CommittedYaw=yaw;s.CommittedPitch=pitch;}
        s.ControlYaw=controlYaw;s.ControlPitch=controlPitch;
        if(checked((long)c.Tick)==s.FailAt)throw new InvalidOperationException("Handoff later-Tick callback rollback");
    }
}
