// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into requested automatic-foot controls before game construction.</summary>
public interface IGroundingControlGame : ITwoBoneIkOrientationGame { }
/// <summary>Requested enable/multiplier, independent of cached effective contact weight.</summary>
public readonly record struct GroundingControl(bool Enabled,double Weight);
/// <summary>Complete replacement for a frozen automatically owned foot slot.</summary>
public readonly record struct GroundingControlCommand(EntityId Entity,uint Slot,bool Enabled,double Weight);

[StructLayout(LayoutKind.Sequential)] internal struct NativeGroundingCommandV1
{
    public uint Version,Bytes;public EntityId Entity;public uint Slot,Enabled;
    public double Weight;public ulong Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeGroundingStateV1
{
    public uint Version,Bytes,Present,Reserved;public EntityId Entity;
    public uint Slot,Enabled;public double Weight;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeGroundingServicesV1
{
    public NativeTwoBoneIkOrientationServicesV1 Orientation;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,uint,NativeGroundingStateV1*,NativeError*,int> GroundingGet;
    public delegate* unmanaged[Cdecl]<void*,NativeGroundingCommandV1*,NativeError*,int> GroundingSet;
}
internal static unsafe class GroundingControlServiceAbi
{
    internal const uint RequiredBytes=368;
    internal static bool LayoutValid()
    {
        NativeGroundingServicesV1 services=default;NativeGroundingCommandV1 command=default;NativeGroundingStateV1 state=default;
        return TwoBoneIkOrientationServiceAbi.LayoutValid() && sizeof(NativeGroundingServicesV1)==368 &&
            (byte*)&services.Orientation-(byte*)&services==0 && (byte*)&services.GroundingGet-(byte*)&services==352 && (byte*)&services.GroundingSet-(byte*)&services==360 &&
            sizeof(NativeGroundingCommandV1)==48 && (byte*)&command.Version-(byte*)&command==0 && (byte*)&command.Bytes-(byte*)&command==4 &&
            (byte*)&command.Entity-(byte*)&command==8 && (byte*)&command.Slot-(byte*)&command==24 && (byte*)&command.Enabled-(byte*)&command==28 &&
            (byte*)&command.Weight-(byte*)&command==32 && (byte*)&command.Reserved-(byte*)&command==40 &&
            sizeof(NativeGroundingStateV1)==48 && (byte*)&state.Version-(byte*)&state==0 && (byte*)&state.Bytes-(byte*)&state==4 &&
            (byte*)&state.Present-(byte*)&state==8 && (byte*)&state.Reserved-(byte*)&state==12 && (byte*)&state.Entity-(byte*)&state==16 &&
            (byte*)&state.Slot-(byte*)&state==32 && (byte*)&state.Enabled-(byte*)&state==36 && (byte*)&state.Weight-(byte*)&state==40;
    }
    internal static NativeGroundingServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Gameplay grounding control requires services epoch7 with at least368bytes.");
        var extended=(NativeGroundingServicesV1*)services;
        if(extended->GroundingGet==null || extended->GroundingSet==null)
            throw new ArgumentException("Gameplay grounding control callback is absent.");
        return extended;
    }
    internal static GroundingControl? Decode(NativeGroundingStateV1* state,EntityId entity,uint slot)
    {
        if(state->Version!=1 || state->Bytes!=48 || state->Reserved!=0 || state->Present>1)
            throw new InvalidOperationException("Invalid native grounding state header.");
        if(state->Present==0)
        {
            foreach(byte value in new ReadOnlySpan<byte>((byte*)state+16,32))
                if(value!=0)throw new InvalidOperationException("Absent native grounding state is not canonical.");
            return null;
        }
        if(state->Entity!=entity || state->Slot!=slot || state->Enabled>1 ||
            !double.IsFinite(state->Weight) || state->Weight<0 || state->Weight>1)
            throw new InvalidOperationException("Invalid native grounding control.");
        return new(state->Enabled!=0,state->Weight);
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads the committed requested control; absent manual slots return null. Tick only.</summary>
    public GroundingControl? GetGroundingControl(EntityId entity,uint slot)
    {
        RequireFeature(GameplayRequiredFeatures.GroundingControl,nameof(IGroundingControlGame));
        if(slot is <1 or >8)throw new ArgumentOutOfRangeException(nameof(slot));
        var extended=GroundingControlServiceAbi.Validate(services);
        NativeGroundingStateV1 state=new(){Version=1,Bytes=48};NativeError error=default;
        Check(extended->GroundingGet(services->Context,&entity,slot,&state,&error),&error);
        return GroundingControlServiceAbi.Decode(&state,entity,slot);
    }
    /// <summary>Stages a complete enable/multiplier replacement; automatic slot ownership remains frozen.</summary>
    public void SetGroundingControl(in GroundingControlCommand source)
    {
        RequireFeature(GameplayRequiredFeatures.GroundingControl,nameof(IGroundingControlGame));
        if(source.Slot is <1 or >8)throw new ArgumentOutOfRangeException(nameof(source.Slot));
        if(!double.IsFinite(source.Weight) || source.Weight<0 || source.Weight>1)throw new ArgumentOutOfRangeException(nameof(source.Weight));
        var extended=GroundingControlServiceAbi.Validate(services);
        NativeGroundingCommandV1 command=new(){Version=1,Bytes=48,Entity=source.Entity,Slot=source.Slot,Enabled=source.Enabled?1u:0u,Weight=source.Weight};
        NativeError error=default;Check(extended->GroundingSet(services->Context,&command,&error),&error);
    }
}
