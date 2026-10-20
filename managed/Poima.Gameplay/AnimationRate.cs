// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into speed-only base and configured-layer clock changes without replacing fades.</summary>
public interface IAnimationRateGame : IInertialAnimationGame { }
[StructLayout(LayoutKind.Sequential)] internal struct NativeAnimationRateCommandV1
{
    public uint Version,Bytes;public EntityId Entity;public uint Layer,Reserved;public double Speed;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeAnimationRateServicesV1
{
    public NativeGroundingServicesV1 Grounding;
    public delegate* unmanaged[Cdecl]<void*,NativeAnimationRateCommandV1*,NativeError*,int> AnimationRateSet;
}
internal static unsafe class AnimationRateServiceAbi
{
    internal const uint RequiredBytes=376;
    internal static bool LayoutValid()
    {
        NativeAnimationRateServicesV1 services=default;NativeAnimationRateCommandV1 command=default;
        return GroundingControlServiceAbi.LayoutValid() && sizeof(NativeAnimationRateServicesV1)==376 &&
            (byte*)&services.Grounding-(byte*)&services==0 && (byte*)&services.AnimationRateSet-(byte*)&services==368 &&
            sizeof(NativeAnimationRateCommandV1)==40 && (byte*)&command.Version-(byte*)&command==0 &&
            (byte*)&command.Bytes-(byte*)&command==4 && (byte*)&command.Entity-(byte*)&command==8 &&
            (byte*)&command.Layer-(byte*)&command==24 && (byte*)&command.Reserved-(byte*)&command==28 &&
            (byte*)&command.Speed-(byte*)&command==32;
    }
    internal static NativeAnimationRateServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Animation rate service ABI mismatch: animation_rate_v1 requires epoch7 with at least376 bytes. Declare IAnimationRateGame.");
        var extended=(NativeAnimationRateServicesV1*)services;
        if(extended->AnimationRateSet==null)throw new ArgumentException("Animation rate service callback is absent.");
        return extended;
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Stages a base-clock speed change. It does not seek, resume playback or restart a transition.</summary>
    public void SetAnimationSpeed(EntityId entity,double speed)=>SetAnimationClockSpeed(entity,0,speed);
    /// <summary>Stages a configured-layer speed change; clip and weight fades remain unchanged.</summary>
    public void SetAnimationLayerSpeed(EntityId entity,uint slot,double speed)
    {
        if(slot is <1 or >4)throw new ArgumentOutOfRangeException(nameof(slot));
        SetAnimationClockSpeed(entity,slot,speed);
    }
    private void SetAnimationClockSpeed(EntityId entity,uint layer,double speed)
    {
        RequireFeature(GameplayRequiredFeatures.AnimationRate,nameof(IAnimationRateGame));
        if(!double.IsFinite(speed) || speed<0 || speed>8)throw new ArgumentOutOfRangeException(nameof(speed));
        var extended=AnimationRateServiceAbi.Validate(services);
        NativeAnimationRateCommandV1 command=new(){Version=1,Bytes=40,Entity=entity,Layer=layer,Speed=speed};
        NativeError error=default;Check(extended->AnimationRateSet(services->Context,&command,&error),&error);
    }
}
