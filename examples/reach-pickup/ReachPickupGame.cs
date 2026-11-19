// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using Poima;

namespace Poima.Examples;

public struct ReachPickupState
{
    public EntityId Actor, Rig, Hand, Item, FingerMesh;
    public int Vertex1, Vertex2, Vertex3, Vertex4;
    public int AimTicks, RetryTicks, ReturnTicks, FingerLayer, OpenClip;
    public double GripX, GripY, GripZ, GripQx, GripQy, GripQz, GripQw;
    public double PoleX, PoleY, PoleZ, OpenTime, FrameLimit, AngleLimit, SurfaceLimit;
    public int TicksInPhase, Phase, Attempts, Misses, Cancellations;
    public long PendingTick, PickupTick;
    public double Weight, Opening, ReturnWeight, ReturnOpening;
    public double TargetX, TargetY, TargetZ, TargetQx, TargetQy, TargetQz, TargetQw;
}

[GameModule("poima.example.reach-pickup")]
public sealed class ReachPickupGame : Game<ReachPickupState>,
    IItemAttachmentPickupGame, ITwoBoneIkOrientationGame, IMaskedAnimationGame, IInputActionsGame
{
    internal const int Idle=0, Reaching=1, Returning=2, Held=3;
    internal const double AimDistance=2;

    public override void Initialize(ref ReachPickupState state) { }

    public override void Tick(ref ReachPickupState state, GameContext context) => Update(ref state,context,false);

    internal static void Update(ref ReachPickupState state, GameContext context, bool aim, EntityId aimCamera=default)
    {
        if(state.PendingTick!=0 && context.GetItemTransferOutcome(state.Item) is { } outcome &&
            outcome.SampledTick==(ulong)state.PendingTick)
        {
            state.PendingTick=0;
            if(outcome.Status==ItemTransferStatus.Transferred)
            {
                state.PickupTick=(long)outcome.SampledTick;
                BeginReturn(ref state);
            }
            else ++state.Misses;
        }
        if(state.Phase==Held)return;

        bool cancel=context.GetInputAction(state.Actor,"cancel_pickup").Pressed;
        if(state.Phase==Reaching && (cancel || (aim && !TargetVisible(state,context,aimCamera))))
        {
            ++state.Cancellations;
            BeginReturn(ref state);
        }
        if(state.Phase==Idle)
        {
            if(cancel || !context.GetInputAction(state.Actor,"pickup").Pressed)return;
            if(aim)
            {
                if(AimedItem(state,context,aimCamera) is not { } selected)return;
                state.Item=selected;
            }
            ValidateSettings(state);
            state.Phase=Reaching;
            state.TicksInPhase=0;
        }

        if(state.Phase==Reaching)
        {
            ++state.TicksInPhase;
            if(state.TicksInPhase>state.AimTicks && state.TicksInPhase-state.AimTicks>state.RetryTicks)
                BeginReturn(ref state);
            else
            {
                double progress=Math.Min(1.0,(double)state.TicksInPhase/state.AimTicks);
                state.Weight=Ease(progress);
                state.Opening=Ease(Math.Min(progress/.35,(1-progress)/.35));
                ReadTarget(ref state,context.Get(state.Item).Transform);
                if(progress==1)
                {
                    var itemFrame=Grip(state);
                    var guard=new ItemAttachmentContact(itemFrame,ItemGripFrame.Identity,state.FrameLimit,state.AngleLimit);
                    ItemSurfaceProbe[] probes=[
                        Probe(state,state.Vertex1),Probe(state,state.Vertex2),
                        Probe(state,state.Vertex3),Probe(state,state.Vertex4)
                    ];
                    context.AttemptItemPickup(state.Item,state.Hand,guard,probes);
                    ++state.Attempts;
                    state.PendingTick=checked((long)context.Tick+1);
                }
                ApplyPose(state,context);
                return;
            }
        }
        if(state.Phase==Returning)
        {
            double remaining=1-Ease(Math.Min(1.0,(double)++state.TicksInPhase/state.ReturnTicks));
            state.Weight=remaining*state.ReturnWeight;
            state.Opening=remaining*state.ReturnOpening;
            ApplyPose(state,context);
            if(remaining==0)state.Phase=state.PickupTick==0 ? Idle : Held;
        }
    }

    internal static EntityId? AimedItem(in ReachPickupState state,GameContext context,EntityId aimCamera)
    {
        var eye=context.Get(aimCamera==default ? state.Rig : aimCamera).Transform;
        return context.AimReleasedItem(eye.Position,eye.Forward,AimDistance,[state.Actor]).Item;
    }

    private static void BeginReturn(ref ReachPickupState state)
    {
        state.Phase=Returning;
        state.TicksInPhase=0;
        state.ReturnWeight=state.Weight;
        state.ReturnOpening=state.Opening;
    }

    private static void ValidateSettings(ReachPickupState state)
    {
        if(state.AimTicks<1 || state.RetryTicks<1 || state.ReturnTicks<1 ||
            state.FingerLayer is <1 or >4 || state.OpenClip<0 ||
            state.Vertex1<0 || state.Vertex2<0 || state.Vertex3<0 || state.Vertex4<0 ||
            !double.IsFinite(state.OpenTime) || state.OpenTime<0 ||
            !double.IsFinite(state.FrameLimit) || state.FrameLimit<=0 ||
            !double.IsFinite(state.AngleLimit) || state.AngleLimit<=0 ||
            !double.IsFinite(state.SurfaceLimit) || state.SurfaceLimit<=0)
            throw new InvalidOperationException("Configure an authored IK chain, finger layer and calibrated grip.");
    }

    private static double Ease(double t) => t*t*(3-2*t);
    private static ItemGripFrame Grip(ReachPickupState s) =>
        new(new(s.GripX,s.GripY,s.GripZ),new(s.GripQx,s.GripQy,s.GripQz,s.GripQw));
    private static ItemSurfaceProbe Probe(ReachPickupState s,int vertex) =>
        new(new(s.FingerMesh,(uint)vertex),s.Item,s.SurfaceLimit);

    private static bool TargetVisible(ReachPickupState state,GameContext context,EntityId aimCamera)
    {
        if(context.GetItemAttachment(state.Item)?.Anchor is not { } proxy)return false;
        var eye=context.Get(aimCamera==default ? state.Rig : aimCamera).Transform;
        var origin=eye.Position;
        var target=context.Get(proxy).Transform.Position;
        Vector3d direction=new(target.X-origin.X,target.Y-origin.Y,target.Z-origin.Z);
        if(direction==default)direction=eye.Forward;
        return context.AimReleasedItem(origin,direction,AimDistance,[state.Actor]).Item==state.Item;
    }

    private static Vector3d TransformedPoint(WorldTransform m,Vector3d point) => new(
        m[3,0]+m[0,0]*point.X+m[1,0]*point.Y+m[2,0]*point.Z,
        m[3,1]+m[0,1]*point.X+m[1,1]*point.Y+m[2,1]*point.Z,
        m[3,2]+m[0,2]*point.X+m[1,2]*point.Y+m[2,2]*point.Z);

    private static Quaternion Rotation(WorldTransform m)
    {
        var x=Vector3.Normalize(new((float)m[0,0],(float)m[0,1],(float)m[0,2]));
        var y=Vector3.Normalize(new((float)m[1,0],(float)m[1,1],(float)m[1,2]));
        var z=Vector3.Normalize(new((float)m[2,0],(float)m[2,1],(float)m[2,2]));
        return Quaternion.Normalize(Quaternion.CreateFromRotationMatrix(new(
            x.X,x.Y,x.Z,0,y.X,y.Y,y.Z,0,z.X,z.Y,z.Z,0,0,0,0,1)));
    }

    private static Quaternion Rotation(Quaterniond q) => new((float)q.X,(float)q.Y,(float)q.Z,(float)q.W);

    private static void ReadTarget(ref ReachPickupState state,WorldTransform item)
    {
        var grip=Grip(state);
        var target=TransformedPoint(item,grip.Position);
        var rotation=Quaternion.Normalize(Rotation(item)*Rotation(grip.Rotation));
        (state.TargetX,state.TargetY,state.TargetZ)=(target.X,target.Y,target.Z);
        (state.TargetQx,state.TargetQy,state.TargetQz,state.TargetQw)=(rotation.X,rotation.Y,rotation.Z,rotation.W);
    }

    private static void ApplyPose(ReachPickupState state,GameContext context)
    {
        var rig=context.Get(state.Rig).Transform;
        var pole=TransformedPoint(rig,new(state.PoleX,state.PoleY,state.PoleZ));
        var world=new Quaternion((float)state.TargetQx,(float)state.TargetQy,(float)state.TargetQz,(float)state.TargetQw);
        var local=Quaternion.Normalize(Quaternion.Inverse(Rotation(rig))*world);
        IkEndOrientationGoal? orientation=state.Weight>0
            ? new(new(local.X,local.Y,local.Z,local.W),state.Weight) : null;
        context.SetTwoBoneIkOrientation(new(state.Rig,1,new(state.TargetX,state.TargetY,state.TargetZ),
            pole,state.Weight>0,IkSpace.World,state.Weight,orientation));
        context.SetAnimationLayer(state.Rig,(uint)state.FingerLayer,state.OpenClip,1,
            time:state.OpenTime*state.Opening,loop:false,playing:false);
    }
}

[GameModule("poima.example.aimed-reach-pickup")]
public sealed class AimedReachPickupGame : Game<ReachPickupState>,
    IItemAttachmentAimGame, IItemAttachmentPickupGame, ITwoBoneIkOrientationGame, IMaskedAnimationGame, IInputActionsGame
{
    public override void Initialize(ref ReachPickupState state) { }
    public override void Tick(ref ReachPickupState state, GameContext context) =>
        ReachPickupGame.Update(ref state,context,true);
}
