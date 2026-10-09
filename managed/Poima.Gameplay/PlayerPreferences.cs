// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into the native player's existing preference owner, independently of other service extensions.</summary>
public interface IPlayerPreferencesGame { }

[Flags]
public enum PlayerPreferenceFields : uint
{
    None=0,VerticalFov=1,SensitivityX=2,SensitivityY=4,InvertX=8,InvertY=16,
    UiScale=32,Samples=64,FramesInFlight=128,MasterGain=256,All=511
}
public enum PlayerPreferenceSource : uint
{
    None=0,AuthoredCamera=1,InputProfile=2,WindowDensity=3,EngineDefault=4,
    SettingsProfile=5,SessionOverride=6,ExplicitOption=7,LiveOverride=8
}
public enum PlayerPreferenceAudioOutcome : uint
{ Disabled=0,NotInitialized=1,SinkGainVerified=2,ApplyFailed=3,InitializationFailed=4 }
public enum PlayerPreferenceRejection : uint
{ None=0,Unavailable=1,StaleOwner=2,StaleRevision=3,Replay=4,Busy=5,Invalid=6,Capacity=7,UnknownTicket=8 }
public enum PlayerPreferenceResultState : uint { Unknown=0,Staged=1,Accepted=2 }

/// <summary>One player lifetime, distinct from a runtime entity or save epoch.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct PlayerPreferenceOwner(ulong High,ulong Low);
/// <summary>A bounded process-local operation identity. It does not identify saved gameplay state.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct PlayerPreferenceTicket(ulong High,ulong Low,ulong Sequence);

public readonly record struct PlayerPreferenceValues(double? VerticalFov,double SensitivityX,double SensitivityY,
    bool InvertX,bool InvertY,double? UiScale,uint Samples,uint FramesInFlight,double MasterGain);
public readonly record struct PlayerPreferenceSources(PlayerPreferenceSource VerticalFov,PlayerPreferenceSource SensitivityX,
    PlayerPreferenceSource SensitivityY,PlayerPreferenceSource InvertX,PlayerPreferenceSource InvertY,
    PlayerPreferenceSource UiScale,PlayerPreferenceSource Samples,PlayerPreferenceSource FramesInFlight,PlayerPreferenceSource MasterGain);
/// <summary>Cached native observations; a missing revision/value is unavailable, not zero or a fallback.</summary>
public readonly record struct PlayerPreferenceObservation(ulong? ObservedRevision,ulong? AppliedRevision,ulong? PresentedRevision,
    double? EffectiveVerticalFov,double? EffectiveUiScale,double RequestedMasterGain,double? SinkGain,PlayerPreferenceAudioOutcome AudioOutcome);
/// <summary>Resolved committed configuration. Inherited FOV/UI remain absent until observed; effective device values are reported separately. Graphics Values describe the existing window and Next values are next-launch intent.</summary>
public readonly record struct PlayerPreferenceSnapshot(bool Available,bool Replay,PlayerPreferenceOwner Owner,ulong? Revision,
    PlayerPreferenceFields Overrides,PlayerPreferenceValues Values,PlayerPreferenceSources Sources,
    uint NextSamples,uint NextFramesInFlight,PlayerPreferenceObservation Observation);
/// <summary>Nullable members omit that key. Reset removes an override and restores the original launch inheritance.</summary>
public readonly record struct PlayerPreferenceChanges(double? VerticalFov=null,double? SensitivityX=null,double? SensitivityY=null,
    bool? InvertX=null,bool? InvertY=null,double? UiScale=null,uint? Samples=null,uint? FramesInFlight=null,double? MasterGain=null);
public readonly record struct PlayerPreferencePatch(PlayerPreferenceOwner Owner,ulong ExpectedRevision,
    PlayerPreferenceChanges Set,PlayerPreferenceFields Reset=PlayerPreferenceFields.None);
/// <summary>Staging reserves intent only. Query the ticket after the complete native boundary succeeds.</summary>
public readonly record struct PlayerPreferenceStageResult(PlayerPreferenceTicket? Ticket,PlayerPreferenceRejection Rejection)
{ public bool Staged=>Rejection==PlayerPreferenceRejection.None && Ticket.HasValue; }
/// <summary>Acceptance is configuration publication, not hardware application or completed presentation.</summary>
public readonly record struct PlayerPreferenceOperationResult(PlayerPreferenceTicket Ticket,PlayerPreferenceResultState State,
    PlayerPreferenceRejection Rejection,ulong? AcceptedRevision,int ErrorCode);

[StructLayout(LayoutKind.Sequential)] internal struct NativePreferenceValuesV1
{
    public double VerticalFov,SensitivityX,SensitivityY,UiScale,MasterGain;
    public uint InvertX,InvertY,Samples,FramesInFlight;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePreferenceSnapshotV1
{
    public uint Version,Bytes,Available,Replay;public PlayerPreferenceOwner Owner;public ulong Revision;
    public uint OverrideMask,ValueMask;public NativePreferenceValuesV1 Values;public fixed uint Sources[9];
    public uint NextSamples,NextFrames,ObservationMask,AudioOutcome,Reserved;
    public ulong ObservedRevision,AppliedRevision,PresentedRevision;
    public double EffectiveFov,EffectiveUiScale,RequestedGain,SinkGain;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativePreferencePatchV1
{
    public uint Version,Bytes;public PlayerPreferenceOwner Owner;public ulong ExpectedRevision;
    public uint SetMask,ResetMask;public NativePreferenceValuesV1 Values;public ulong Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativePreferenceEnqueueV1
{ public uint Version,Bytes;public PlayerPreferenceTicket Ticket;public uint Rejection,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal struct NativePreferenceResultV1
{
    public uint Version,Bytes;public PlayerPreferenceTicket Ticket;public uint State,Rejection;
    public ulong AcceptedRevision;public int ErrorCode;public uint Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePlayerPreferenceServicesV1
{
    public NativeInstanceServicesV1 Instances;
    public delegate* unmanaged[Cdecl]<void*,NativePreferenceSnapshotV1*,NativeError*,int> Snapshot;
    public delegate* unmanaged[Cdecl]<void*,NativePreferencePatchV1*,NativePreferenceEnqueueV1*,NativeError*,int> Patch;
    public delegate* unmanaged[Cdecl]<void*,PlayerPreferenceTicket*,NativePreferenceResultV1*,NativeError*,int> Result;
}
internal static unsafe class PlayerPreferenceServiceAbi
{
    internal const uint RequiredBytes=256,KnownMask=511;
    internal const ulong MaxRevision=9007199254740991UL;
    internal static bool LayoutValid()
    {
        NativePlayerPreferenceServicesV1 s=default;NativePreferenceValuesV1 v=default;
        NativePreferenceSnapshotV1 o=default;NativePreferencePatchV1 p=default;
        NativePreferenceEnqueueV1 e=default;NativePreferenceResultV1 r=default;
        return InstanceServiceAbi.LayoutValid() && sizeof(NativePlayerPreferenceServicesV1)==256 &&
            sizeof(NativePreferenceValuesV1)==56 && sizeof(NativePreferenceSnapshotV1)==216 && sizeof(NativePreferencePatchV1)==104 &&
            sizeof(PlayerPreferenceOwner)==16 && sizeof(PlayerPreferenceTicket)==24 && sizeof(NativePreferenceEnqueueV1)==40 && sizeof(NativePreferenceResultV1)==56 &&
            (byte*)&s.Instances-(byte*)&s==0 && (byte*)&s.Snapshot-(byte*)&s==232 && (byte*)&s.Patch-(byte*)&s==240 && (byte*)&s.Result-(byte*)&s==248 &&
            (byte*)&v.InvertX-(byte*)&v==40 && (byte*)&v.FramesInFlight-(byte*)&v==52 &&
            (byte*)&o.Owner-(byte*)&o==16 && (byte*)&o.Revision-(byte*)&o==32 && (byte*)&o.Values-(byte*)&o==48 &&
            (byte*)o.Sources-(byte*)&o==104 && (byte*)&o.NextSamples-(byte*)&o==140 && (byte*)&o.ObservationMask-(byte*)&o==148 &&
            (byte*)&o.Reserved-(byte*)&o==156 && (byte*)&o.ObservedRevision-(byte*)&o==160 && (byte*)&o.EffectiveFov-(byte*)&o==184 && (byte*)&o.SinkGain-(byte*)&o==208 &&
            (byte*)&p.Owner-(byte*)&p==8 && (byte*)&p.ExpectedRevision-(byte*)&p==24 && (byte*)&p.SetMask-(byte*)&p==32 &&
            (byte*)&p.Values-(byte*)&p==40 && (byte*)&p.Reserved-(byte*)&p==96 && (byte*)&e.Ticket-(byte*)&e==8 && (byte*)&e.Rejection-(byte*)&e==32 &&
            (byte*)&r.Ticket-(byte*)&r==8 && (byte*)&r.State-(byte*)&r==32 && (byte*)&r.AcceptedRevision-(byte*)&r==40 && (byte*)&r.ErrorCode-(byte*)&r==48;
    }
    internal static NativePlayerPreferenceServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Player preference ABI mismatch: player_preferences_v1 requires epoch 7 with at least 256 bytes. Declare IPlayerPreferencesGame.");
        var extended=(NativePlayerPreferenceServicesV1*)services;
        // This opt-in reads only its own three callbacks, not poisoned/unrequested intervening services.
        if(extended->Snapshot==null || extended->Patch==null || extended->Result==null)
            throw new ArgumentException("Versioned player preference callback is absent.");
        return extended;
    }
    internal static bool Number(double value,double minimum,double maximum)=>double.IsFinite(value) && value>=minimum && value<=maximum;
    internal static bool Samples(uint value)=>value is 1 or 4;
    internal static bool Frames(uint value)=>value is 1 or 2;
    internal static bool Ticket(PlayerPreferenceTicket value)=>value.Sequence is >=1 and <=MaxRevision && (value.High!=0 || value.Low!=0);
    internal static double? OptionalNumber(uint mask,uint bit,double value,double minimum,double maximum)
    {
        if((mask&bit)!=0) {if(!Number(value,minimum,maximum))throw new InvalidOperationException("Invalid native preference numeric value.");return value;}
        if(value!=0)throw new InvalidOperationException("Absent native preference numeric value is nonzero.");return null;
    }
    internal static double? OptionalScale(uint mask,uint bit,double value,bool bounded)
    {
        if((mask&bit)!=0)
        {
            if(!double.IsFinite(value) || value<=0 || (bounded && !Number(value,.25,8)))
                throw new InvalidOperationException("Invalid native preference UI scale.");
            return value;
        }
        if(value!=0)throw new InvalidOperationException("Absent native preference UI scale is nonzero.");
        return null;
    }
    internal static ulong? OptionalRevision(uint mask,uint bit,ulong value)
    {
        if((mask&bit)!=0) {if(value>MaxRevision)throw new InvalidOperationException("Invalid native preference observation revision.");return value;}
        if(value!=0)throw new InvalidOperationException("Absent native preference observation revision is nonzero.");return null;
    }
    internal static NativePreferencePatchV1 Encode(PlayerPreferencePatch patch)
    {
        if(patch.Owner==default || patch.ExpectedRevision>MaxRevision)throw new ArgumentException("Preference patch requires a current owner and safe revision.");
        if(((uint)patch.Reset&~KnownMask)!=0)throw new ArgumentOutOfRangeException(nameof(patch),"Unknown reset field.");
        NativePreferencePatchV1 result=new(){Version=1,Bytes=104,Owner=patch.Owner,ExpectedRevision=patch.ExpectedRevision,ResetMask=(uint)patch.Reset};
        var set=patch.Set;
        void Numeric(double? value,uint bit,double minimum,double maximum,ref double destination)
        {
            if(!value.HasValue)return;
            if(!Number(value.Value,minimum,maximum))throw new ArgumentOutOfRangeException(nameof(patch),"Preference value is nonfinite or outside its bounds.");
            result.SetMask|=bit;destination=value.Value;
        }
        Numeric(set.VerticalFov,1,5,150,ref result.Values.VerticalFov);
        Numeric(set.SensitivityX,2,0,10,ref result.Values.SensitivityX);
        Numeric(set.SensitivityY,4,0,10,ref result.Values.SensitivityY);
        Numeric(set.UiScale,32,.25,8,ref result.Values.UiScale);
        Numeric(set.MasterGain,256,0,1,ref result.Values.MasterGain);
        if(set.InvertX.HasValue){result.SetMask|=8;result.Values.InvertX=set.InvertX.Value ? 1u : 0u;}
        if(set.InvertY.HasValue){result.SetMask|=16;result.Values.InvertY=set.InvertY.Value ? 1u : 0u;}
        if(set.Samples.HasValue){if(!Samples(set.Samples.Value))throw new ArgumentOutOfRangeException(nameof(patch));result.SetMask|=64;result.Values.Samples=set.Samples.Value;}
        if(set.FramesInFlight.HasValue){if(!Frames(set.FramesInFlight.Value))throw new ArgumentOutOfRangeException(nameof(patch));result.SetMask|=128;result.Values.FramesInFlight=set.FramesInFlight.Value;}
        if((result.SetMask&result.ResetMask)!=0)throw new ArgumentException("Preference set and reset must be disjoint.");
        return result;
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads committed configuration and cached native observations during Tick or Control. No SDL or storage work occurs.</summary>
    public PlayerPreferenceSnapshot GetPlayerPreferences()
    {
        RequireFeature(GameplayRequiredFeatures.PlayerPreferences,nameof(IPlayerPreferencesGame));
        var extended=PlayerPreferenceServiceAbi.Validate(services);
        NativePreferenceSnapshotV1 result=new(){Version=1,Bytes=216};NativeError error=default;
        Check(extended->Snapshot(services->Context,&result,&error),&error);
        if(result.Version!=1 || result.Bytes!=216 || result.Available>1 || result.Replay>1 || result.Reserved!=0 ||
            (result.OverrideMask&~511u)!=0 || (result.ValueMask&~511u)!=0 || (result.ObservationMask&~63u)!=0 ||
            result.Revision>PlayerPreferenceServiceAbi.MaxRevision || result.AudioOutcome>4)
            throw new InvalidOperationException("Invalid native preference snapshot encoding.");
        for(int i=0;i<9;++i)if(result.Sources[i]>8)throw new InvalidOperationException("Unknown native preference source.");
        if(result.Available==0)
        {
            for(int i=8;i<216;++i)if(((byte*)&result)[i]!=0)
                throw new InvalidOperationException("Unavailable preference snapshot contains nonzero payload.");
            return new(false,false,default,null,PlayerPreferenceFields.None,default,default,0,0,default);
        }
        if(result.Owner==default || (result.ValueMask&478u)!=478u || (result.OverrideMask&~result.ValueMask)!=0 ||
            !PlayerPreferenceServiceAbi.Number(result.Values.SensitivityX,0,10) || !PlayerPreferenceServiceAbi.Number(result.Values.SensitivityY,0,10) ||
            !PlayerPreferenceServiceAbi.Number(result.Values.MasterGain,0,1) || result.Values.InvertX>1 || result.Values.InvertY>1 ||
            !PlayerPreferenceServiceAbi.Samples(result.Values.Samples) || !PlayerPreferenceServiceAbi.Frames(result.Values.FramesInFlight) ||
            !PlayerPreferenceServiceAbi.Samples(result.NextSamples) || !PlayerPreferenceServiceAbi.Frames(result.NextFrames) ||
            !PlayerPreferenceServiceAbi.Number(result.RequestedGain,0,1))
            throw new InvalidOperationException("Invalid native preference snapshot values.");
        var observation=new PlayerPreferenceObservation(
            PlayerPreferenceServiceAbi.OptionalRevision(result.ObservationMask,1,result.ObservedRevision),
            PlayerPreferenceServiceAbi.OptionalRevision(result.ObservationMask,2,result.AppliedRevision),
            PlayerPreferenceServiceAbi.OptionalRevision(result.ObservationMask,4,result.PresentedRevision),
            PlayerPreferenceServiceAbi.OptionalNumber(result.ObservationMask,8,result.EffectiveFov,5,150),
            PlayerPreferenceServiceAbi.OptionalScale(result.ObservationMask,16,result.EffectiveUiScale,false),
            result.RequestedGain,PlayerPreferenceServiceAbi.OptionalNumber(result.ObservationMask,32,result.SinkGain,0,1),
            (PlayerPreferenceAudioOutcome)result.AudioOutcome);
        return new(true,result.Replay!=0,result.Owner,result.Revision,(PlayerPreferenceFields)result.OverrideMask,
            new(PlayerPreferenceServiceAbi.OptionalNumber(result.ValueMask,1,result.Values.VerticalFov,5,150),result.Values.SensitivityX,result.Values.SensitivityY,
                result.Values.InvertX!=0,result.Values.InvertY!=0,PlayerPreferenceServiceAbi.OptionalScale(result.ValueMask,32,result.Values.UiScale,(result.OverrideMask&32u)!=0),
                result.Values.Samples,result.Values.FramesInFlight,result.Values.MasterGain),
            new((PlayerPreferenceSource)result.Sources[0],(PlayerPreferenceSource)result.Sources[1],(PlayerPreferenceSource)result.Sources[2],
                (PlayerPreferenceSource)result.Sources[3],(PlayerPreferenceSource)result.Sources[4],(PlayerPreferenceSource)result.Sources[5],
                (PlayerPreferenceSource)result.Sources[6],(PlayerPreferenceSource)result.Sources[7],(PlayerPreferenceSource)result.Sources[8]),
            result.NextSamples,result.NextFrames,observation);
    }
    /// <summary>Stages one complete guarded patch per native Control/step boundary. Reads stay committed until that entire boundary succeeds.</summary>
    public PlayerPreferenceStageResult TryStagePlayerPreferences(PlayerPreferencePatch patch)
    {
        RequireFeature(GameplayRequiredFeatures.PlayerPreferences,nameof(IPlayerPreferencesGame));
        var extended=PlayerPreferenceServiceAbi.Validate(services);var request=PlayerPreferenceServiceAbi.Encode(patch);
        NativePreferenceEnqueueV1 result=new(){Version=1,Bytes=40};NativeError error=default;
        Check(extended->Patch(services->Context,&request,&result,&error),&error);
        if(result.Version!=1 || result.Bytes!=40 || result.Reserved!=0 || result.Rejection>7 ||
            (result.Rejection==0 ? !PlayerPreferenceServiceAbi.Ticket(result.Ticket) || result.Ticket.High!=patch.Owner.High || result.Ticket.Low!=patch.Owner.Low : result.Ticket!=default))
            throw new InvalidOperationException("Invalid native preference staging result.");
        return new(result.Rejection==0 ? result.Ticket : null,(PlayerPreferenceRejection)result.Rejection);
    }
    /// <summary>Queries staged or retained accepted configuration intent. It neither applies a patch nor proves device application.</summary>
    public PlayerPreferenceOperationResult GetPlayerPreferenceResult(PlayerPreferenceTicket ticket)
    {
        RequireFeature(GameplayRequiredFeatures.PlayerPreferences,nameof(IPlayerPreferencesGame));
        if(!PlayerPreferenceServiceAbi.Ticket(ticket))throw new ArgumentException("Preference ticket requires a nonzero owner and bounded sequence.",nameof(ticket));
        var extended=PlayerPreferenceServiceAbi.Validate(services);
        NativePreferenceResultV1 result=new(){Version=1,Bytes=56};NativeError error=default;
        Check(extended->Result(services->Context,&ticket,&result,&error),&error);
        if(result.Version!=1 || result.Bytes!=56 || result.Reserved!=0 || result.State>2 || result.Rejection>8 || result.ErrorCode!=0 || result.Ticket!=ticket ||
            result.AcceptedRevision>PlayerPreferenceServiceAbi.MaxRevision ||
            (result.State==2 ? result.Rejection!=0 || result.AcceptedRevision==0 : result.AcceptedRevision!=0) ||
            (result.State==1 && result.Rejection!=0) || (result.State==0 && result.Rejection==0))
            throw new InvalidOperationException("Invalid native preference operation result.");
        return new(result.Ticket,(PlayerPreferenceResultState)result.State,(PlayerPreferenceRejection)result.Rejection,
            result.State==2 ? result.AcceptedRevision : null,result.ErrorCode);
    }
}
public readonly unsafe ref partial struct ControlContext
{
    public PlayerPreferenceSnapshot GetPlayerPreferences()=>context.GetPlayerPreferences();
    public PlayerPreferenceStageResult TryStagePlayerPreferences(PlayerPreferencePatch patch)=>context.TryStagePlayerPreferences(patch);
    public PlayerPreferenceOperationResult GetPlayerPreferenceResult(PlayerPreferenceTicket ticket)=>context.GetPlayerPreferenceResult(ticket);
}
