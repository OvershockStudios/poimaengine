// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Declares compiled character intent independently of animation features before game construction.</summary>
public interface ICharacterInputGame { }

[StructLayout(LayoutKind.Sequential)] internal struct NativeCharacterInputV1
{
    public uint Version,Bytes;public EntityId Entity;
    public float MoveRight,MoveForward,LookYaw,LookPitch;public uint Flags,Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeCharacterServicesV1
{
    public NativeAnimationLayerServicesV1 Animation;
    public delegate* unmanaged[Cdecl]<void*,NativeCharacterInputV1*,NativeError*,int> CharacterInput;
}
internal static unsafe class CharacterInputServiceAbi
{
    internal const uint RequiredBytes=216;
    internal static bool LayoutValid()
    {
        NativeCharacterServicesV1 services=default;NativeCharacterInputV1 input=default;
        return AnimationLayerServiceAbi.LayoutValid() && sizeof(NativeCharacterServicesV1)==216 && sizeof(NativeCharacterInputV1)==48 &&
            (byte*)&services.Animation-(byte*)&services==0 && (byte*)&services.CharacterInput-(byte*)&services==208 &&
            (byte*)&input.Entity-(byte*)&input==8 && (byte*)&input.MoveRight-(byte*)&input==24 &&
            (byte*)&input.MoveForward-(byte*)&input==28 && (byte*)&input.LookYaw-(byte*)&input==32 &&
            (byte*)&input.LookPitch-(byte*)&input==36 && (byte*)&input.Flags-(byte*)&input==40 &&
            (byte*)&input.Reserved-(byte*)&input==44;
    }
    internal static NativeCharacterServicesV1* Validate(NativeServices* services)
    {
        // Character intent is independent of animation. Inspect only its own
        // tail callback after admitting the complete named prefix.
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Character input service ABI mismatch: character_input_v1 requires epoch 7 with at least 216 bytes. Declare ICharacterInputGame.");
        var extended=(NativeCharacterServicesV1*)services;
        if(extended->CharacterInput==null)throw new ArgumentException("Versioned character input service callback is absent.");
        return extended;
    }
}
public readonly unsafe ref partial struct GameContext
{
    // The original constructor is retained, with a default baseline feature
    // field. Only matched feature-aware bridges use this additive constructor.
    internal GameContext(NativeServices* services,GameInput* inputs,int count,ulong tick,GameplayRequiredFeatures required)
        : this(services,inputs,count,tick) { requiredFeatures=required; }
    private void RequireFeature(GameplayRequiredFeatures feature,string marker)
    {
        if((requiredFeatures & feature)==0)throw new ArgumentException("Declare "+marker+" before using this gameplay service.");
    }
    /// <summary>Stages this Tick's movement/look/jump for a live CharacterController. Omission next Tick is neutral; staged intent is not committed state or a caller Input.</summary>
    public void SetCharacterInput(EntityId entity,float moveRight,float moveForward,float lookYaw=0,float lookPitch=0,bool jump=false)
    {
        RequireFeature(GameplayRequiredFeatures.CharacterInput,nameof(ICharacterInputGame));
        if(!float.IsFinite(moveRight) || moveRight is < -1 or >1)throw new ArgumentOutOfRangeException(nameof(moveRight));
        if(!float.IsFinite(moveForward) || moveForward is < -1 or >1)throw new ArgumentOutOfRangeException(nameof(moveForward));
        if(!float.IsFinite(lookYaw) || lookYaw is < -180 or >180)throw new ArgumentOutOfRangeException(nameof(lookYaw));
        if(!float.IsFinite(lookPitch) || lookPitch is < -180 or >180)throw new ArgumentOutOfRangeException(nameof(lookPitch));
        var extended=CharacterInputServiceAbi.Validate(services);
        NativeCharacterInputV1 command=new(){Version=1,Bytes=48,Entity=entity,MoveRight=moveRight,MoveForward=moveForward,
            LookYaw=lookYaw,LookPitch=lookPitch,Flags=jump ? 1u : 0u,Reserved=0};
        NativeError error=default;Check(extended->CharacterInput(services->Context,&command,&error),&error);
    }
}
