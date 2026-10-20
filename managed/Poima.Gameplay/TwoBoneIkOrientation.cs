// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into complete point/end-orientation IK controls before game construction.</summary>
public interface ITwoBoneIkOrientationGame : ITwoBoneIkGame { }
[StructLayout(LayoutKind.Sequential)]
public readonly record struct Quaterniond(double X,double Y,double Z,double W)
{
    public static Quaterniond Identity=>new(0,0,0,1);
}
/// <summary>Logical rig-space quaternion goal, independent of the point target space and weight.</summary>
public readonly record struct IkEndOrientationGoal(Quaterniond Rotation,double Weight);
/// <summary>Complete replacement. A null goal clears orientation; all positional arguments are explicit to preserve old overload resolution.</summary>
public readonly record struct IkOrientedCommand(EntityId Entity,uint Slot,Vector3d Target,Vector3d Pole,
    bool Enabled,IkSpace Space,double Weight,IkEndOrientationGoal? EndOrientation);
public readonly record struct IkEndOrientationState(IkEndOrientationGoal Goal,Quaterniond ActualRotation,double AngularError);
/// <summary>Committed point controls plus optional cached logical rig-frame orientation diagnostics.</summary>
public readonly record struct IkOrientedState(IkState Point,IkEndOrientationState? EndOrientation);

[StructLayout(LayoutKind.Sequential)] internal struct NativeTwoBoneIkCommandV2
{
    public NativeTwoBoneIkCommandV1 Point;
    public uint EndOrientationPresent,ReservedEndOrientation;
    public Quaterniond EndRotation;public double EndWeight;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeTwoBoneIkStateV2
{
    public NativeTwoBoneIkStateV1 Point;
    public uint EndOrientationPresent,ReservedEndOrientation;
    public Quaterniond EndRotation;public double EndWeight;
    public Quaterniond ActualEndRotation;public double EndAngularError;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeTwoBoneIkOrientationServicesV1
{
    public NativeCharacterStanceServicesV1 CharacterStance;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,uint,NativeTwoBoneIkStateV2*,NativeError*,int> IkOrientationGet;
    public delegate* unmanaged[Cdecl]<void*,NativeTwoBoneIkCommandV2*,NativeError*,int> IkOrientationSet;
}
internal static unsafe class TwoBoneIkOrientationServiceAbi
{
    internal const uint RequiredBytes=352;
    internal const double QuaternionTolerance=8*2.2204460492503131e-16;
    internal static bool LayoutValid()
    {
        NativeTwoBoneIkOrientationServicesV1 services=default;NativeTwoBoneIkCommandV2 command=default;NativeTwoBoneIkStateV2 state=default;
        return CharacterStanceServiceAbi.LayoutValid() && sizeof(Quaterniond)==32 && sizeof(NativeTwoBoneIkOrientationServicesV1)==352 &&
            sizeof(NativeTwoBoneIkCommandV2)==144 && sizeof(NativeTwoBoneIkStateV2)==248 &&
            (byte*)&services.CharacterStance-(byte*)&services==0 && (byte*)&services.IkOrientationGet-(byte*)&services==336 && (byte*)&services.IkOrientationSet-(byte*)&services==344 &&
            (byte*)&command.Point-(byte*)&command==0 && (byte*)&command.EndOrientationPresent-(byte*)&command==96 &&
            (byte*)&command.ReservedEndOrientation-(byte*)&command==100 && (byte*)&command.EndRotation-(byte*)&command==104 && (byte*)&command.EndWeight-(byte*)&command==136 &&
            (byte*)&state.Point-(byte*)&state==0 && (byte*)&state.EndOrientationPresent-(byte*)&state==160 &&
            (byte*)&state.ReservedEndOrientation-(byte*)&state==164 && (byte*)&state.EndRotation-(byte*)&state==168 &&
            (byte*)&state.EndWeight-(byte*)&state==200 && (byte*)&state.ActualEndRotation-(byte*)&state==208 && (byte*)&state.EndAngularError-(byte*)&state==240;
    }
    internal static NativeTwoBoneIkOrientationServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Gameplay oriented IK requires services epoch 7 with at least 352 bytes.");
        var extended=(NativeTwoBoneIkOrientationServicesV1*)services;
        if(extended->IkOrientationGet==null || extended->IkOrientationSet==null)
            throw new ArgumentException("Gameplay oriented IK callback is absent.");
        return extended;
    }
    internal static double Norm(Quaterniond q)=>double.Hypot(double.Hypot(q.X,q.Y),double.Hypot(q.Z,q.W));
    internal static bool Finite(Quaterniond q)=>double.IsFinite(q.X) && double.IsFinite(q.Y) && double.IsFinite(q.Z) && double.IsFinite(q.W);
    internal static bool Unit(Quaterniond q)=>Finite(q) && Math.Abs(Norm(q)-1)<=QuaternionTolerance;
    internal static bool Canonical(Quaterniond q)
    {
        if(!Unit(q))return false;
        double first=q.W!=0?q.W:q.X!=0?q.X:q.Y!=0?q.Y:q.Z;
        return first>0 && !NegativeZero(q.X) && !NegativeZero(q.Y) && !NegativeZero(q.Z) && !NegativeZero(q.W);
    }
    static bool NegativeZero(double value)=>value==0 && BitConverter.DoubleToInt64Bits(value)<0;
    internal static IkEndOrientationGoal? CanonicalGoal(IkEndOrientationGoal? source)
    {
        if(source is not { } goal)return null;
        var q=goal.Rotation;double norm=Norm(q);
        if(!Finite(q) || !double.IsFinite(norm) || norm<=0 || !double.IsFinite(goal.Weight) || goal.Weight<0 || goal.Weight>1)
            throw new ArgumentOutOfRangeException(nameof(source),"IK orientation needs a finite nonzero quaternion and weight0..1.");
        if(goal.Weight==0)return null;
        if(Math.Abs(norm-1)>QuaternionTolerance)
        {
            double maximum=Math.Max(Math.Max(Math.Abs(q.X),Math.Abs(q.Y)),Math.Max(Math.Abs(q.Z),Math.Abs(q.W)));
            q=new(q.X/maximum,q.Y/maximum,q.Z/maximum,q.W/maximum);
            double scaledNorm=Norm(q);q=new(q.X/scaledNorm,q.Y/scaledNorm,q.Z/scaledNorm,q.W/scaledNorm);
        }
        double sign=q.W!=0?q.W:q.X!=0?q.X:q.Y!=0?q.Y:q.Z;
        if(sign<0)q=new(-q.X,-q.Y,-q.Z,-q.W);
        q=new(q.X==0?0:q.X,q.Y==0?0:q.Y,q.Z==0?0:q.Z,q.W==0?0:q.W);
        return new(q,goal.Weight);
    }
    internal static IkOrientedState? Decode(NativeTwoBoneIkStateV2* source,EntityId entity,uint slot)
    {
        if(source->Point.Version!=2 || source->Point.Bytes!=248 || source->EndOrientationPresent>1 || source->ReservedEndOrientation!=0)
            throw new InvalidOperationException("Invalid native oriented IK state header.");
        var point=source->Point;point.Version=1;point.Bytes=160;
        var decoded=TwoBoneIkServiceAbi.Decode(&point,entity,slot);
        if(source->EndOrientationPresent==0)
        {
            foreach(byte value in new ReadOnlySpan<byte>((byte*)source+168,80))
                if(value!=0)throw new InvalidOperationException("Absent native IK orientation payload is not canonical.");
            return decoded is { } valid ? new(valid,null) : null;
        }
        if(decoded is not { } state || !Canonical(source->EndRotation) || !Canonical(source->ActualEndRotation) ||
            !double.IsFinite(source->EndWeight) || source->EndWeight<=0 || source->EndWeight>1 ||
            !double.IsFinite(source->EndAngularError) || source->EndAngularError<0 || source->EndAngularError>Math.PI)
            throw new InvalidOperationException("Invalid native IK orientation state.");
        return new(state,new(new(source->EndRotation,source->EndWeight),source->ActualEndRotation,source->EndAngularError));
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads cached callback-entry state; shares the 1,024-read Tick budget with GetTwoBoneIk.</summary>
    public IkOrientedState? GetTwoBoneIkOrientation(EntityId entity,uint slot)
    {
        RequireFeature(GameplayRequiredFeatures.TwoBoneIkOrientation,nameof(ITwoBoneIkOrientationGame));
        if(slot is <1 or >8)throw new ArgumentOutOfRangeException(nameof(slot));
        var extended=TwoBoneIkOrientationServiceAbi.Validate(services);
        NativeTwoBoneIkStateV2 state=default;state.Point.Version=2;state.Point.Bytes=248;NativeError error=default;
        Check(extended->IkOrientationGet(services->Context,&entity,slot,&state,&error),&error);
        return TwoBoneIkOrientationServiceAbi.Decode(&state,entity,slot);
    }
    /// <summary>Stages a complete replacement with optional rig-space orientation. Shares the 64-write budget and slot conflicts with V1.</summary>
    public void SetTwoBoneIkOrientation(in IkOrientedCommand source)
    {
        RequireFeature(GameplayRequiredFeatures.TwoBoneIkOrientation,nameof(ITwoBoneIkOrientationGame));
        if(source.Slot is <1 or >8)throw new ArgumentOutOfRangeException(nameof(source.Slot));
        if(source.Space is not IkSpace.World and not IkSpace.Rig)throw new ArgumentOutOfRangeException(nameof(source.Space));
        if(!double.IsFinite(source.Weight) || source.Weight<0 || source.Weight>1)throw new ArgumentOutOfRangeException(nameof(source.Weight));
        if(!TwoBoneIkServiceAbi.Point(source.Target) || !TwoBoneIkServiceAbi.Point(source.Pole))throw new ArgumentOutOfRangeException(nameof(source));
        var goal=TwoBoneIkOrientationServiceAbi.CanonicalGoal(source.EndOrientation);
        var extended=TwoBoneIkOrientationServiceAbi.Validate(services);
        NativeTwoBoneIkCommandV2 command=default;command.Point=new(){Version=2,Bytes=144,Entity=source.Entity,Slot=source.Slot,
            Enabled=source.Enabled?1u:0u,Space=(uint)source.Space,Weight=source.Weight,Target=source.Target,Pole=source.Pole};
        if(goal is { } active){command.EndOrientationPresent=1;command.EndRotation=active.Rotation;command.EndWeight=active.Weight;}
        NativeError error=default;Check(extended->IkOrientationSet(services->Context,&command,&error),&error);
    }
}
