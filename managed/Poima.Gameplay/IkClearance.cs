// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;

namespace Poima;

/// <summary>Declares native arm-corridor queries over committed animation and physics.</summary>
public interface IIkClearanceGame : ITwoBoneIkGame, ICapsuleCastGame { }

public readonly record struct IkChainPoints(Vector3d Root,Vector3d Middle,Vector3d End);
public enum IkClearanceSegment : uint { Upper=1, Lower=2 }
public readonly record struct IkClearanceBlocker(EntityId Entity,IkClearanceSegment Segment,uint Interval,
    double Fraction,double PenetrationDepth);
public readonly record struct IkClearance(IkChainPoints Current,IkChainPoints Candidate,IkClearanceBlocker? Blocker);

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeIkClearanceV2
{
    public uint Version,Bytes;
    public double UpperRadius,LowerRadius,Weight;
    public Vector3d Target,Pole;
    public EntityId* Ignore;
    public uint IgnoreCount,Space,Enabled,Reserved,Segment,Interval;
    public EntityId Obstacle;
    public double Fraction,PenetrationDepth;
    public Vector3d CandidateRoot,CandidateMiddle,CandidateEnd,CurrentRoot,CurrentMiddle,CurrentEnd;
}

internal static unsafe class IkClearanceAbi
{
    internal static bool LayoutValid()
    {
        NativeIkClearanceV2 value=default;
        return sizeof(NativeIkClearanceV2)==288 && (byte*)&value.Ignore-(byte*)&value==80 &&
            (byte*)&value.Segment-(byte*)&value==104 && (byte*)&value.Obstacle-(byte*)&value==112 &&
            (byte*)&value.CandidateRoot-(byte*)&value==144 && (byte*)&value.CurrentRoot-(byte*)&value==216;
    }

    internal static IkClearance Decode(in NativeIkClearanceV2 value)
    {
        if(value.Version!=2 || value.Bytes!=288 || value.Reserved!=0 || value.Ignore!=null || value.IgnoreCount!=0 ||
            value.Segment>2 || !TwoBoneIkServiceAbi.Point(value.CandidateRoot) ||
            !TwoBoneIkServiceAbi.Point(value.CandidateMiddle) || !TwoBoneIkServiceAbi.Point(value.CandidateEnd) ||
            !TwoBoneIkServiceAbi.Point(value.CurrentRoot) || !TwoBoneIkServiceAbi.Point(value.CurrentMiddle) ||
            !TwoBoneIkServiceAbi.Point(value.CurrentEnd) ||
            (value.Segment==0 ? value.Obstacle!=default || value.Interval!=0 || value.Fraction!=0 || value.PenetrationDepth!=0 :
                value.Obstacle==default || value.Interval is <1 or >4 ||
                !double.IsFinite(value.Fraction) || value.Fraction is <0 or >1 ||
                !double.IsFinite(value.PenetrationDepth) || value.PenetrationDepth<0))
            throw new InvalidOperationException("Invalid native IK clearance result.");
        IkClearanceBlocker? blocker=value.Segment==0 ? null :
            new(value.Obstacle,(IkClearanceSegment)value.Segment,value.Interval,value.Fraction,value.PenetrationDepth);
        return new(new(value.CurrentRoot,value.CurrentMiddle,value.CurrentEnd),
            new(value.CandidateRoot,value.CandidateMiddle,value.CandidateEnd),blocker);
    }
}

public readonly unsafe ref partial struct GameContext
{
    /// <summary>Tests linear joint-point motion to a proposed pose on the current committed sample.
    /// Does not stage the goal. Own actor and contact-target exclusions are explicit.
    /// Reserves eight casts from the shared Tick capsule budget.</summary>
    public IkClearance ProbeIkClearance(in IkCommand goal,double upperRadius,double lowerRadius,
        ReadOnlySpan<EntityId> ignore=default)
    {
        RequireFeature(GameplayRequiredFeatures.IkClearance,nameof(IIkClearanceGame));
        if(goal.Entity==default || goal.Slot is <1 or >8 || goal.Space is not IkSpace.World and not IkSpace.Rig ||
            !TwoBoneIkServiceAbi.Point(goal.Target) || !TwoBoneIkServiceAbi.Point(goal.Pole) ||
            !double.IsFinite(goal.Weight) || goal.Weight is <0 or >1)
            throw new ArgumentException("IK clearance requires a valid two-bone goal.",nameof(goal));
        if(!double.IsFinite(upperRadius) || upperRadius is <.001 or >1 ||
            !double.IsFinite(lowerRadius) || lowerRadius is <.001 or >1)
            throw new ArgumentOutOfRangeException(nameof(upperRadius),"Both segment radii must be .001..1 metres.");
        if(ignore.Length>128)throw new ArgumentException("At most 128 ignored entities.",nameof(ignore));
        var api=TwoBoneIkServiceAbi.Validate(services);
        NativeError error=default;
        EntityId entity=goal.Entity;
        fixed(EntityId* excluded=ignore)
        {
            NativeIkClearanceV2 result=new()
            {
                Version=2,Bytes=288,UpperRadius=upperRadius,LowerRadius=lowerRadius,Weight=goal.Weight,
                Target=goal.Target,Pole=goal.Pole,Space=(uint)goal.Space,Enabled=goal.Enabled ? 1u : 0u,
                Ignore=excluded,IgnoreCount=(uint)ignore.Length
            };
            Check(api->IkGet(services->Context,&entity,goal.Slot,(NativeTwoBoneIkStateV1*)&result,&error),&error);
            return IkClearanceAbi.Decode(result);
        }
    }
}
