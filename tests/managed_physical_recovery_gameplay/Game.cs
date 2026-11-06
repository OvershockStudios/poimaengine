// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;
public struct PhysicalRecoveryGameState
{
    public EntityId Rig;
    public int Action,Ticks,Writes,CommittedReads,RejectAfterWrite;
    public int Status,Physical,TransitionActive,StableTicks,RequiredStableTicks,LastAttemptPresent,LastAttemptResult,ReturnedPresent,SupportMask;
    public long FailAt,ObservedTick,Generation,AssessmentRevision,LastAttemptTick,ReturnedTick,FirstSupportBody,SecondSupportBody;
    public double MaximumLinearSpeed,MaximumAngularSpeed,MinimumUprightDot,WrapperDistance,WrapperAngle;
}
[GameModule("poima.test.physical-recovery")]
public sealed class PhysicalRecoveryGame:Game<PhysicalRecoveryGameState>,IPhysicalRecoveryGame
{
    public override void Initialize(ref PhysicalRecoveryGameState s){s=default;s.Rig=new(0,0x10);s.FailAt=-1;}
    public override void Tick(ref PhysicalRecoveryGameState s,GameContext c)
    {
        var q=c.GetPhysicalRecovery(s.Rig)??throw new InvalidOperationException("Authored recovery fixture is absent.");
        ++s.Ticks;s.ObservedTick=checked((long)q.SampledTick);s.Generation=checked((long)q.Generation);s.AssessmentRevision=checked((long)q.AssessmentRevision);
        s.Status=(int)q.Status;s.Physical=q.Physical?1:0;s.TransitionActive=q.TransitionActive?1:0;s.StableTicks=checked((int)q.StableTicks);s.RequiredStableTicks=checked((int)q.RequiredStableTicks);
        s.LastAttemptPresent=q.LastAttemptTick.HasValue?1:0;s.LastAttemptTick=checked((long)(q.LastAttemptTick??0));s.LastAttemptResult=(int)q.LastAttemptResult;
        s.ReturnedPresent=q.ReturnedTick.HasValue?1:0;s.ReturnedTick=checked((long)(q.ReturnedTick??0));
        s.SupportMask=(q.FirstSupportBody.HasValue?1:0)|(q.SecondSupportBody.HasValue?2:0);s.FirstSupportBody=q.FirstSupportBody??0;s.SecondSupportBody=q.SecondSupportBody??0;
        s.MaximumLinearSpeed=q.MaximumLinearSpeed;s.MaximumAngularSpeed=q.MaximumAngularSpeed;s.MinimumUprightDot=q.MinimumUprightDot;s.WrapperDistance=q.WrapperDistance;s.WrapperAngle=q.WrapperAngle;
        switch(s.Action){case 0:break;case 1:c.ActivatePhysicalRig(s.Rig);++s.Writes;break;case 2:c.RequestPhysicalRecovery(s.Rig);++s.Writes;break;default:throw new ArgumentException("Unknown recovery fixture action.");}
        s.Action=0;
        if(c.GetPhysicalRecovery(s.Rig)!=q)throw new InvalidOperationException("Staged recovery changed committed readback.");++s.CommittedReads;
        if(s.RejectAfterWrite!=0)throw new InvalidOperationException("Recovery fixture rejected after staging.");
        if(checked((long)c.Tick)==s.FailAt)throw new InvalidOperationException("Recovery fixture later-Tick failure.");
    }
}
// A distinct real compiled consumer intentionally lacks recovery permission.
// Matching schema permits a same-image/type-switch test without synthetic pointers.
[GameModule("poima.test.physical-recovery")]
public sealed class PhysicalRecoveryUnmarkedGame:Game<PhysicalRecoveryGameState>,IPhysicalRigGame
{
    public override void Initialize(ref PhysicalRecoveryGameState s){s=default;s.Rig=new(0,0x10);s.FailAt=-1;}
    public override void Tick(ref PhysicalRecoveryGameState s,GameContext c)
    {++s.Ticks;c.RequestPhysicalRecovery(s.Rig);++s.Writes;}
}
