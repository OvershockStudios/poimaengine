// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct PhysicalGameState
{
    public EntityId Rig,RootNode,TipNode;
    public int Mode,Ticks,ObservedMode,BodyCount,JointCount,Writes,CommittedReadChecks;
    public int RootActive,TipActive;
    // Full unsigned native BodyIDs fit losslessly in the supported int64 state.
    public long RootBody,TipBody,FailAt,ObservedTick;
    public double RootX,RootY,RootZ,TipX,TipY,TipZ;
    public double RootVx,RootVy,RootVz,TipVx,TipVy,TipVz;
    public double RootMass,TipMass,ImpulseX,ImpulseY,ImpulseZ;
}

// This fixture uses real native callbacks when loaded by the engine. The
// independent wire-validation executable has separate synthetic callbacks.
[GameModule("poima.test.physical-rig")]
public sealed class PhysicalGame : Game<PhysicalGameState>,IPhysicalRigGame
{
    public override void Initialize(ref PhysicalGameState state)
    {
        state=default;state.Rig=new(0,100);state.FailAt=-1;
        state.ImpulseX=1;state.ImpulseY=.5;
    }

    public override void Tick(ref PhysicalGameState s,GameContext c)
    {
        ++s.Ticks;s.ObservedTick=checked((long)c.Tick);
        var rig=c.GetPhysicalRig(s.Rig) ?? throw new InvalidOperationException("Physical rig absent");
        var root=c.GetPhysicalRigPart(s.Rig,"root") ?? throw new InvalidOperationException("Root part absent");
        var tip=c.GetPhysicalRigPart(s.Rig,"tip") ?? throw new InvalidOperationException("Tip part absent");
        s.ObservedMode=(int)rig.Mode;s.BodyCount=checked((int)rig.BodyCount);s.JointCount=checked((int)rig.JointCount);
        s.RootNode=root.NodeEntity;s.TipNode=tip.NodeEntity;s.RootBody=root.BodyId;s.TipBody=tip.BodyId;
        s.RootX=root.Position.X;s.RootY=root.Position.Y;s.RootZ=root.Position.Z;
        s.TipX=tip.Position.X;s.TipY=tip.Position.Y;s.TipZ=tip.Position.Z;
        s.RootVx=root.LinearVelocity.X;s.RootVy=root.LinearVelocity.Y;s.RootVz=root.LinearVelocity.Z;
        s.TipVx=tip.LinearVelocity.X;s.TipVy=tip.LinearVelocity.Y;s.TipVz=tip.LinearVelocity.Z;
        s.RootMass=root.Mass;s.TipMass=tip.Mass;s.RootActive=root.Active?1:0;s.TipActive=tip.Active?1:0;
        if(s.Mode==1){c.ActivatePhysicalRig(s.Rig);++s.Writes;s.Mode=0;}
        else if(s.Mode==2)
        {
            // Repeated and distinct part impulses must remain separate ordered
            // commands, including the repeated root entry.
            var impulse=new Vector3d(s.ImpulseX,s.ImpulseY,s.ImpulseZ);
            c.ImpulsePhysicalRig(s.Rig,"root",root.Position,impulse);
            c.ImpulsePhysicalRig(s.Rig,"tip",tip.Position,impulse);
            c.ImpulsePhysicalRig(s.Rig,"root",root.Position,impulse);
            s.Writes+=3;s.Mode=0;
        }
        else if(s.Mode==3)
        {
            c.ActivatePhysicalRig(s.Rig);c.ActivatePhysicalRig(s.Rig);s.Writes+=2;
        }
        else if(s.Mode!=0)throw new InvalidOperationException("Unknown physical fixture mode");
        if(c.GetPhysicalRig(s.Rig)!=rig || c.GetPhysicalRigPart(s.Rig,"root")!=root || c.GetPhysicalRigPart(s.Rig,"tip")!=tip)
            throw new InvalidOperationException("Staged physical command leaked into a committed read");
        ++s.CommittedReadChecks;
        if(checked((long)c.Tick)==s.FailAt)throw new InvalidOperationException("Physical fixture later-Tick failure");
    }
}
