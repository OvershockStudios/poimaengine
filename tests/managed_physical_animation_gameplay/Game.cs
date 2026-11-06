// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct PhysicalAnimationGameState
{
    public EntityId Rig;
    public int Phase,Joint,Clip,LayerClip,TransitionMode,BlendTicks,WeightBlendTicks,Source,Ticks,Writes,CommittedReads,RejectAfterWrite;
    public int ObservedMode,ObservedClip,ObservedLayerClip,ObservedSource,SamplePresent,HingeProjected,TwistClamped,SwingClamped,ObservedTransition,ObservedLayerTransition;
    public long ObservedTick,SampledTick,FailAt;
    public double Time,Speed,Weight,Strength,TargetAngle;
    public double ObservedTime,ObservedSpeed,LayerTime,LayerSpeed,LayerWeight,LayerTargetWeight,Progress,LayerProgress,Correction,ChartCondition;
    public double RequestedX,RequestedY,RequestedZ,RequestedW,FittedX,FittedY,FittedZ,FittedW,EffectiveX,EffectiveY,EffectiveZ,EffectiveW,EffectiveStrength;
}

// Real callback consumer. Target fitting, clock advancement and physical
// response remain native; the test controller only selects ordinary commands.
[GameModule("poima.test.physical-rig-animation")]
public sealed class PhysicalAnimationGame : Game<PhysicalAnimationGameState>,IPhysicalRigAnimationGame
{
    public override void Initialize(ref PhysicalAnimationGameState s)
    {
        s=default;s.Rig=new(0,0x10);s.Clip=1;s.LayerClip=2;s.Source=1;
        s.Time=.25;s.Speed=1;s.Weight=1;s.Strength=.6;s.TargetAngle=.2;s.FailAt=-1;
    }
    private static string JointName(int selected)=>selected switch
    {0=>"shoulder",1=>"elbow",_=>throw new ArgumentException("Invalid target fixture joint.")};

    public override void Tick(ref PhysicalAnimationGameState s,GameContext c)
    {
        string joint=JointName(s.Joint);
        var rig=c.GetPhysicalRig(s.Rig)??throw new InvalidOperationException("Physical fixture rig absent.");
        var animation=c.GetPhysicalRigAnimation(s.Rig)??throw new InvalidOperationException("Physical target player absent.");
        var layer=c.GetPhysicalRigAnimationLayer(s.Rig,1)??throw new InvalidOperationException("Physical target layer absent.");
        var source=c.GetPhysicalRigDriveSource(s.Rig,joint)??throw new InvalidOperationException("Physical target source absent.");
        var effective=c.GetPhysicalRigDrive(s.Rig,joint)??throw new InvalidOperationException("Physical effective drive absent.");
        ++s.Ticks;s.ObservedTick=checked((long)c.Tick);s.SampledTick=checked((long)source.SampledTick);
        s.ObservedMode=(int)rig.Mode;s.ObservedClip=animation.State.Clip??-1;s.ObservedLayerClip=layer.Playback.State.Clip??-1;
        s.ObservedSource=(int)source.Source;s.ObservedTime=animation.State.Time;s.ObservedSpeed=animation.State.Speed;
        s.LayerTime=layer.Playback.State.Time;s.LayerSpeed=layer.Playback.State.Speed;s.LayerWeight=layer.Weight;s.LayerTargetWeight=layer.TargetWeight;
        s.ObservedTransition=animation.Mode is {} mode?(int)mode:-1;s.Progress=animation.Progress??0;
        s.ObservedLayerTransition=layer.Playback.Mode is {} layerMode?(int)layerMode:-1;s.LayerProgress=layer.Playback.Progress??0;
        s.EffectiveX=effective.TargetRotation.X;s.EffectiveY=effective.TargetRotation.Y;s.EffectiveZ=effective.TargetRotation.Z;s.EffectiveW=effective.TargetRotation.W;
        s.EffectiveStrength=effective.Strength;s.SamplePresent=source.Sample.HasValue?1:0;
        if(source.Sample is {} sample)
        {
            s.RequestedX=sample.RequestedRotation.X;s.RequestedY=sample.RequestedRotation.Y;s.RequestedZ=sample.RequestedRotation.Z;s.RequestedW=sample.RequestedRotation.W;
            s.FittedX=sample.TargetRotation.X;s.FittedY=sample.TargetRotation.Y;s.FittedZ=sample.TargetRotation.Z;s.FittedW=sample.TargetRotation.W;
            s.Correction=sample.CorrectionRadians;s.ChartCondition=sample.ChartCondition;
            s.HingeProjected=sample.HingeProjected?1:0;s.TwistClamped=sample.TwistClamped?1:0;s.SwingClamped=sample.SwingClamped?1:0;
        }
        else
        {
            s.RequestedX=s.RequestedY=s.RequestedZ=s.RequestedW=s.FittedX=s.FittedY=s.FittedZ=s.FittedW=0;
            s.Correction=s.ChartCondition=0;s.HingeProjected=s.TwistClamped=s.SwingClamped=0;
        }
        var transition=(AnimationTransitionMode)s.TransitionMode;
        switch(s.Phase)
        {
            case 0:break;
            case 1:
                c.ActivatePhysicalRig(s.Rig);
                c.SetPhysicalRigAnimation(s.Rig,s.Clip,time:s.Time,speed:s.Speed,loop:false);
                c.SetPhysicalRigDriveSource(s.Rig,"shoulder",PhysicalRigDriveSource.Animation,s.Strength);
                c.SetPhysicalRigDriveSource(s.Rig,"elbow",PhysicalRigDriveSource.Animation,s.Strength);s.Writes+=4;break;
            case 2:
                c.SetPhysicalRigAnimation(s.Rig,s.Clip,transition,time:s.Time,speed:s.Speed,loop:false,blendTicks:checked((uint)s.BlendTicks));++s.Writes;break;
            case 3:
                c.SetPhysicalRigAnimationLayer(s.Rig,1,s.LayerClip,s.Weight,transition,time:s.Time,speed:s.Speed,loop:false,playing:false,
                    blendTicks:checked((uint)s.BlendTicks),weightBlendTicks:checked((uint)s.WeightBlendTicks));++s.Writes;break;
            case 4:c.SetPhysicalRigAnimationSpeed(s.Rig,s.Speed);++s.Writes;break;
            case 5:c.SetPhysicalRigAnimationLayerSpeed(s.Rig,1,s.Speed);++s.Writes;break;
            case 6:c.SetPhysicalRigDriveSource(s.Rig,joint,(PhysicalRigDriveSource)s.Source,s.Strength);++s.Writes;break;
            case 7:
                c.SetPhysicalRigDrive(s.Rig,joint,new Quaterniond(Math.Sin(s.TargetAngle/2),0,0,Math.Cos(s.TargetAngle/2)),s.Strength);++s.Writes;break;
            case 8:
                c.SetPhysicalRigAnimationSpeed(s.Rig,s.Speed);
                c.SetPhysicalRigDriveSource(s.Rig,joint,(PhysicalRigDriveSource)s.Source,s.Strength);s.Writes+=2;break;
            case 9:
                c.SetPhysicalRigDriveSource(s.Rig,"shoulder",PhysicalRigDriveSource.Manual,0);
                c.SetPhysicalRigDriveSource(s.Rig,"elbow",PhysicalRigDriveSource.Manual,0);s.Writes+=2;break;
            default:throw new ArgumentException("Unknown physical animation phase.");
        }
        s.Phase=0;
        if(c.GetPhysicalRig(s.Rig)!=rig||c.GetPhysicalRigAnimation(s.Rig)!=animation||c.GetPhysicalRigAnimationLayer(s.Rig,1)!=layer||
            c.GetPhysicalRigDriveSource(s.Rig,joint)!=source||c.GetPhysicalRigDrive(s.Rig,joint)!=effective)
            throw new InvalidOperationException("Staged physical animation leaked into committed reads.");
        ++s.CommittedReads;
        if(s.RejectAfterWrite!=0)throw new InvalidOperationException("Physical animation fixture rejected after staging.");
        if(checked((long)c.Tick)==s.FailAt)throw new InvalidOperationException("Physical animation fixture later-Tick failure.");
    }
}
