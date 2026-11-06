// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct PhysicalDriveGameState
{
    public EntityId Rig,SelectedNode;
    public int Phase,Joint,Part,Ticks,ObservedMode,BodyCount,JointCount,DrivePresent;
    public int PartActive,PartSleeping,Writes,CommittedReadChecks,RejectAfterWrite;
    public long ObservedTick,SelectedBody,FailAt;
    public double TargetAngle,Strength;
    public double ObservedTargetX,ObservedTargetY,ObservedTargetZ,ObservedTargetW,ObservedStrength;
    public double Stiffness,Damping,MaxTorque;
    public double PartX,PartY,PartZ,PartQx,PartQy,PartQz,PartQw;
    public double PartVx,PartVy,PartVz,PartWx,PartWy,PartWz,PartMass;
}

// A prebuilt consumer of real engine callbacks. It does not manufacture joint
// responses or advance physics itself; a native owner supplies the graph and
// records authoritative whole-state behavior separately from these observations.
[GameModule("poima.test.physical-rig-drive")]
public sealed class PhysicalDriveGame : Game<PhysicalDriveGameState>,IPhysicalRigDriveGame
{
    public override void Initialize(ref PhysicalDriveGameState state)
    {
        state=default;state.Rig=new(0,0x10);state.Part=1;state.FailAt=-1;
        state.TargetAngle=.3;state.Strength=.5;
    }
    private static string JointName(int selected)=>selected switch
    {0=>"shoulder",1=>"elbow",2=>"unknown_joint",_=>throw new ArgumentException("Unknown fixture joint selector.")};
    private static string PartName(int selected)=>selected switch
    {0=>"pelvis",1=>"arm",2=>"hand",_=>throw new ArgumentException("Unknown fixture part selector.")};

    public override void Tick(ref PhysicalDriveGameState s,GameContext c)
    {
        ++s.Ticks;s.ObservedTick=checked((long)c.Tick);
        string joint=JointName(s.Joint),part=PartName(s.Part);
        var rig=c.GetPhysicalRig(s.Rig) ?? throw new InvalidOperationException("Physical drive rig is absent.");
        var drive=c.GetPhysicalRigDrive(s.Rig,joint);
        var body=c.GetPhysicalRigPart(s.Rig,part) ?? throw new InvalidOperationException("Selected physical part is absent.");
        s.ObservedMode=(int)rig.Mode;s.BodyCount=checked((int)rig.BodyCount);s.JointCount=checked((int)rig.JointCount);
        s.SelectedNode=body.NodeEntity;s.SelectedBody=body.BodyId;s.PartActive=body.Active?1:0;s.PartSleeping=body.Sleeping?1:0;
        s.PartX=body.Position.X;s.PartY=body.Position.Y;s.PartZ=body.Position.Z;
        s.PartQx=body.Rotation.X;s.PartQy=body.Rotation.Y;s.PartQz=body.Rotation.Z;s.PartQw=body.Rotation.W;
        s.PartVx=body.LinearVelocity.X;s.PartVy=body.LinearVelocity.Y;s.PartVz=body.LinearVelocity.Z;
        s.PartWx=body.AngularVelocity.X;s.PartWy=body.AngularVelocity.Y;s.PartWz=body.AngularVelocity.Z;s.PartMass=body.Mass;
        s.DrivePresent=drive.HasValue?1:0;
        if(drive is {} value)
        {
            s.ObservedTargetX=value.TargetRotation.X;s.ObservedTargetY=value.TargetRotation.Y;
            s.ObservedTargetZ=value.TargetRotation.Z;s.ObservedTargetW=value.TargetRotation.W;
            s.ObservedStrength=value.Strength;s.Stiffness=value.Stiffness;s.Damping=value.Damping;s.MaxTorque=value.MaxTorque;
        }
        else
        {
            s.ObservedTargetX=s.ObservedTargetY=s.ObservedTargetZ=s.ObservedTargetW=s.ObservedStrength=0;
            s.Stiffness=s.Damping=s.MaxTorque=0;
        }

        if(s.Phase is <0 or >3)throw new ArgumentException("Unknown physical drive fixture phase.");
        if(s.Phase is 1 or 3){c.ActivatePhysicalRig(s.Rig);++s.Writes;}
        if(s.Phase is 1 or 2)
        {
            // Native admission still checks the actual authored joint limits.
            if(!double.IsFinite(s.TargetAngle))throw new ArgumentException("TargetAngle must be finite radians.");
            var target=new Quaterniond(Math.Sin(s.TargetAngle/2),0,0,Math.Cos(s.TargetAngle/2));
            c.SetPhysicalRigDrive(s.Rig,joint,target,s.Strength);++s.Writes;
        }
        s.Phase=0;
        if(c.GetPhysicalRig(s.Rig)!=rig || c.GetPhysicalRigDrive(s.Rig,joint)!=drive || c.GetPhysicalRigPart(s.Rig,part)!=body)
            throw new InvalidOperationException("Staged physical drive leaked into committed reads.");
        ++s.CommittedReadChecks;
        // Both failures occur in compiled gameplay after actual callback work;
        // the enclosing native transaction must restore the complete state.
        if(s.RejectAfterWrite!=0)throw new InvalidOperationException("Physical drive fixture rejected after callback staging.");
        if(checked((long)c.Tick)==s.FailAt)throw new InvalidOperationException("Physical drive fixture later-Tick failure.");
    }
}
