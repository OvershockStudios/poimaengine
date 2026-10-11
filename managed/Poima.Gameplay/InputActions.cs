// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into Tick-only named action observations, independently of movement or other service extensions.</summary>
public interface IInputActionsGame { }

/// <summary>Committed input for this Tick. A quick tap can have both Pressed and Released set. Missing named records have all four values false.</summary>
public readonly record struct InputActionState(bool Held,bool Pressed,bool Released,bool Present);

[StructLayout(LayoutKind.Sequential)] internal struct NativeInputActionV1
{
    public uint Version,Bytes,Flags,Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeInputActionServicesV1
{
    public NativePlayerPreferenceServicesV1 Preferences;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,byte*,uint,NativeInputActionV1*,NativeError*,int> Get;
}
internal static unsafe class InputActionServiceAbi
{
    internal const uint RequiredBytes=264;
    internal static NativeInputActionServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Gameplay input actions require services epoch 7 with at least 264 bytes.");
        var extended=(NativeInputActionServicesV1*)services;
        if(extended->Get==null)throw new ArgumentException("Gameplay input action callback is absent.");
        return extended;
    }
    internal static bool LayoutValid()
    {
        NativeInputActionServicesV1 services=default;NativeInputActionV1 result=default;
        return sizeof(NativeInputActionServicesV1)==264 && sizeof(NativeInputActionV1)==16 &&
            (byte*)&services.Get-(byte*)&services==256 && (byte*)&result.Flags-(byte*)&result==8 &&
            (byte*)&result.Reserved-(byte*)&result==12;
    }
}

public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads one native named action for a live entity during Tick. It does not stage input, advance simulation or infer a binding for an absent record.</summary>
    public InputActionState GetInputAction(EntityId entity,string name)
    {
        RequireFeature(GameplayRequiredFeatures.InputActions,nameof(IInputActionsGame));
        ArgumentNullException.ThrowIfNull(name);
        if(name.Length is <1 or >64 || name[0] is <'a' or >'z' ||
            name.Any(c=>!((c>='a' && c<='z') || (c>='0' && c<='9') || c is '_' or '.' or '-')))
            throw new ArgumentException("Action name must be ASCII [a-z][a-z0-9_.-]{0,63}.",nameof(name));
        var extended=InputActionServiceAbi.Validate(services);
        Span<byte> bytes=stackalloc byte[name.Length];
        for(int i=0;i<name.Length;++i)bytes[i]=(byte)name[i];
        NativeInputActionV1 result=new(){Version=1,Bytes=16};NativeError error=default;
        fixed(byte* text=bytes)Check(extended->Get(services->Context,&entity,text,(uint)bytes.Length,&result,&error),&error);
        if(result.Version!=1 || result.Bytes!=16 || result.Reserved!=0 || (result.Flags & ~15u)!=0 ||
            ((result.Flags & 8u)==0 && result.Flags!=0))
            throw new InvalidOperationException("Invalid native input action state.");
        return new((result.Flags&1u)!=0,(result.Flags&2u)!=0,(result.Flags&4u)!=0,(result.Flags&8u)!=0);
    }
}
