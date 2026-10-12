// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Independent Tick-only character stance access, negotiated before game construction.</summary>
public interface ICharacterStanceGame { }
public enum CharacterGroundState : uint { OnGround=0,OnSteepGround=1,NotSupported=2,InAir=3 }
/// <summary>Callback-entry committed stance. Desired standing retries while a ceiling blocks expansion.</summary>
public readonly record struct CharacterStanceState(EntityId Entity,bool RequestedCrouch,bool Crouched,bool StandBlocked,bool Configured,
    CharacterGroundState GroundState,float Radius,float StandingHeight,float CrouchedHeight,float Height,float Speed,float CameraDrop);
[StructLayout(LayoutKind.Sequential)] internal struct NativeCharacterStanceCommandV1
{ public uint Version,Bytes;public EntityId Entity;public uint Crouch,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeCharacterStanceStateV1
{
    public uint Version,Bytes;public EntityId Entity;public uint Present,Flags,GroundState;
    public float Radius,StandingHeight,CrouchedHeight,Height,Speed,CameraDrop;public uint Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeCharacterStanceServicesV1
{
    public NativeCameraLensServicesV1 CameraLens;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativeCharacterStanceStateV1*,NativeError*,int> CharacterStanceGet;
    public delegate* unmanaged[Cdecl]<void*,NativeCharacterStanceCommandV1*,NativeError*,int> CharacterStanceSet;
}
internal static unsafe class CharacterStanceServiceAbi
{
    internal const uint RequiredBytes=336;
    internal static NativeCharacterStanceServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Gameplay character stance requires services epoch 7 with at least336bytes.");
        var extended=(NativeCharacterStanceServicesV1*)services;
        if(extended->CharacterStanceGet==null || extended->CharacterStanceSet==null)
            throw new ArgumentException("Gameplay character stance callback is absent.");
        return extended;
    }
    internal static bool LayoutValid()
    {
        NativeCharacterStanceServicesV1 table=default;NativeCharacterStanceCommandV1 command=default;NativeCharacterStanceStateV1 state=default;
        return sizeof(NativeCharacterStanceServicesV1)==336 && sizeof(NativeCharacterStanceCommandV1)==32 && sizeof(NativeCharacterStanceStateV1)==64 &&
            (byte*)&table.CharacterStanceGet-(byte*)&table==320 && (byte*)&table.CharacterStanceSet-(byte*)&table==328 &&
            (byte*)&command.Entity-(byte*)&command==8 && (byte*)&command.Crouch-(byte*)&command==24 && (byte*)&command.Reserved-(byte*)&command==28 &&
            (byte*)&state.Entity-(byte*)&state==8 && (byte*)&state.Present-(byte*)&state==24 && (byte*)&state.Flags-(byte*)&state==28 &&
            (byte*)&state.GroundState-(byte*)&state==32 && (byte*)&state.Radius-(byte*)&state==36 &&
            (byte*)&state.StandingHeight-(byte*)&state==40 && (byte*)&state.CrouchedHeight-(byte*)&state==44 &&
            (byte*)&state.Height-(byte*)&state==48 && (byte*)&state.Speed-(byte*)&state==52 &&
            (byte*)&state.CameraDrop-(byte*)&state==56 && (byte*)&state.Reserved-(byte*)&state==60;
    }
    internal static CharacterStanceState? Decode(NativeCharacterStanceStateV1* value,EntityId entity)
    {
        if(value->Version!=1 || value->Bytes!=64 || value->Present>1 || value->Reserved!=0 || value->Flags>15 || value->GroundState>3)
            throw new InvalidOperationException("Invalid native character stance header.");
        if(value->Present==0)
        {
            foreach(byte b in new ReadOnlySpan<byte>((byte*)value+8,56))
                if(b!=0)throw new InvalidOperationException("Absent native character stance is not canonical.");
            return null;
        }
        bool requested=(value->Flags&1)!=0,actual=(value->Flags&2)!=0,blocked=(value->Flags&4)!=0,configured=(value->Flags&8)!=0;
        if(entity==default || value->Entity!=entity || !float.IsFinite(value->Radius) || value->Radius<.05f || value->Radius>2 ||
            !float.IsFinite(value->StandingHeight) || value->StandingHeight<=2*value->Radius || value->StandingHeight>4 ||
            !float.IsFinite(value->CrouchedHeight) || value->CrouchedHeight<=0 || value->CrouchedHeight>value->StandingHeight ||
            (configured && (value->CrouchedHeight<=2*value->Radius || value->CrouchedHeight>=value->StandingHeight)) ||
            !float.IsFinite(value->Height) || value->Height!=(actual ? value->CrouchedHeight : value->StandingHeight) ||
            !float.IsFinite(value->Speed) || value->Speed<=0 || value->Speed>30 || !float.IsFinite(value->CameraDrop) || value->CameraDrop<0 ||
            (configured && (double)value->CameraDrop>(double)value->StandingHeight-value->CrouchedHeight+1e-6) ||
            (!actual && value->CameraDrop!=0) || (requested && !actual) || (blocked!=(!requested && actual)) ||
            (!configured && (value->Flags!=0 || value->CrouchedHeight!=value->StandingHeight || value->CameraDrop!=0)))
            throw new InvalidOperationException("Invalid native committed character stance.");
        return new(entity,requested,actual,blocked,configured,(CharacterGroundState)value->GroundState,
            value->Radius,value->StandingHeight,value->CrouchedHeight,value->Height,value->Speed,value->CameraDrop);
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads committed callback-entry stance during Tick; max256 reads. Live noncontrollers return null.</summary>
    public CharacterStanceState? GetCharacterStance(EntityId entity)
    {
        RequireFeature(GameplayRequiredFeatures.CharacterStance,nameof(ICharacterStanceGame));
        var extended=CharacterStanceServiceAbi.Validate(services);
        NativeCharacterStanceStateV1 value=new(){Version=1,Bytes=64};NativeError error=default;
        Check(extended->CharacterStanceGet(services->Context,&entity,&value,&error),&error);
        return CharacterStanceServiceAbi.Decode(&value,entity);
    }
    /// <summary>Stages persistent desired stance during Tick; max32 distinct writes. Standing retries if blocked.</summary>
    public void SetCharacterStance(EntityId entity,bool crouch)
    {
        RequireFeature(GameplayRequiredFeatures.CharacterStance,nameof(ICharacterStanceGame));
        var extended=CharacterStanceServiceAbi.Validate(services);
        if(entity==default)throw new ArgumentException("Character stance requires a nonempty entity.");
        NativeCharacterStanceCommandV1 value=new(){Version=1,Bytes=32,Entity=entity,Crouch=crouch ? 1u : 0u};NativeError error=default;
        Check(extended->CharacterStanceSet(services->Context,&value,&error),&error);
    }
}
