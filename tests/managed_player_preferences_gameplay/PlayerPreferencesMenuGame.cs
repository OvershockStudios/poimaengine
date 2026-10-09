// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Runtime.InteropServices;
using Poima;
namespace Poima.Tests;

// These are callback observations and process-local diagnostic guards/tickets,
// never a second preference authority. No saved value is reapplied on restore.
[StructLayout(LayoutKind.Sequential)]
public struct PlayerPreferencesMenuState
{
    public int Mode,Ticks,Controls,Available,Replay,FovPresent,UiPresent,EffectiveFovPresent,EffectiveUiPresent;
    public int StageRejection,ResultState,ResultRejection,SourceFov,SourceUi,Overrides,NextSamples,NextFrames,ReadUnchanged;
    public long Revision,ReadAfterRevision,OwnerHigh,OwnerLow,TicketHigh,TicketLow,TicketSequence,AcceptedRevision;
    public long GuardHigh,GuardLow,GuardRevision,ObservedRevision,AppliedRevision,PresentedRevision;
    public long SaveHigh,SaveLow,SaveSequence;
    public double Fov,UiScale,Gain,EffectiveFov,EffectiveUi,SensitivityX,SensitivityY,SinkGain;
    public int InvertX,InvertY,AudioOutcome,SinkGainPresent;
    public EntityId Target;
}

[GameModule("poima.test.player-preferences")]
public sealed class ManagedPlayerPreferencesGame : Game<PlayerPreferencesMenuState>,IPlayerPreferencesGame
{
    // Native scene definitions use these stable IDs. Game code creates no UI.
    public static readonly UiId Status=new(0,501),Live=new(0,502);
    public static readonly UiId MenuPanel=new(0,700),MenuStatus=new(0,701),Opener=new(0,699);
    public override void Initialize(ref PlayerPreferencesMenuState state)
    {
        state=default;state.Target=new(0,100);state.Revision=state.AcceptedRevision=-1;
        state.ObservedRevision=state.AppliedRevision=state.PresentedRevision=-1;
    }
    static long Bits(ulong value)=>unchecked((long)value);
    static ulong Bits(long value)=>unchecked((ulong)value);
    static void Check(bool condition,string message)
    { if(!condition)throw new InvalidOperationException(message); }
    static void Observe(ref PlayerPreferencesMenuState state,PlayerPreferenceSnapshot value)
    {
        state.Available=value.Available?1:0;state.Replay=value.Replay?1:0;
        state.OwnerHigh=Bits(value.Owner.High);state.OwnerLow=Bits(value.Owner.Low);state.Revision=value.Revision.HasValue?checked((long)value.Revision.Value):-1;
        state.Overrides=(int)value.Overrides;state.FovPresent=value.Values.VerticalFov.HasValue?1:0;state.Fov=value.Values.VerticalFov??0;
        state.UiPresent=value.Values.UiScale.HasValue?1:0;state.UiScale=value.Values.UiScale??0;state.Gain=value.Values.MasterGain;
        state.SensitivityX=value.Values.SensitivityX;state.SensitivityY=value.Values.SensitivityY;
        state.InvertX=value.Values.InvertX?1:0;state.InvertY=value.Values.InvertY?1:0;
        state.SourceFov=(int)value.Sources.VerticalFov;state.SourceUi=(int)value.Sources.UiScale;
        state.NextSamples=(int)value.NextSamples;state.NextFrames=(int)value.NextFramesInFlight;
        var observation=value.Observation;
        state.EffectiveFovPresent=observation.EffectiveVerticalFov.HasValue?1:0;state.EffectiveFov=observation.EffectiveVerticalFov??0;
        state.EffectiveUiPresent=observation.EffectiveUiScale.HasValue?1:0;state.EffectiveUi=observation.EffectiveUiScale??0;
        state.ObservedRevision=observation.ObservedRevision.HasValue?checked((long)observation.ObservedRevision.Value):-1;
        state.AppliedRevision=observation.AppliedRevision.HasValue?checked((long)observation.AppliedRevision.Value):-1;
        state.PresentedRevision=observation.PresentedRevision.HasValue?checked((long)observation.PresentedRevision.Value):-1;
        state.AudioOutcome=(int)observation.AudioOutcome;state.SinkGainPresent=observation.SinkGain.HasValue?1:0;state.SinkGain=observation.SinkGain??0;
    }
    static PlayerPreferencePatch Patch(PlayerPreferenceSnapshot snapshot,PlayerPreferenceChanges set,PlayerPreferenceFields reset=PlayerPreferenceFields.None)
        =>new(snapshot.Available?snapshot.Owner:new(0,1),snapshot.Revision??0,set,reset);
    static PlayerPreferenceChanges LiveChanges(PlayerPreferenceSnapshot value)=>new(
        VerticalFov:Math.Clamp((value.Values.VerticalFov??value.Observation.EffectiveVerticalFov??60)+5,5,150),
        UiScale:Math.Clamp((value.Values.UiScale??value.Observation.EffectiveUiScale??1)+.25,.25,8),
        MasterGain:Math.Clamp(value.Values.MasterGain-.1,0,1));
    static void Staged(ref PlayerPreferencesMenuState state,PlayerPreferenceStageResult staged)
    {
        state.StageRejection=(int)staged.Rejection;
        if(staged.Ticket is { } ticket)
        {
            state.TicketHigh=Bits(ticket.High);state.TicketLow=Bits(ticket.Low);state.TicketSequence=checked((long)ticket.Sequence);
        }
    }
    static PlayerPreferenceTicket Ticket(in PlayerPreferencesMenuState state)=>new(Bits(state.TicketHigh),Bits(state.TicketLow),checked((ulong)state.TicketSequence));
    static void Result(ref PlayerPreferencesMenuState state,PlayerPreferenceOperationResult result)
    {
        state.ResultState=(int)result.State;state.ResultRejection=(int)result.Rejection;
        state.AcceptedRevision=result.AcceptedRevision.HasValue?checked((long)result.AcceptedRevision.Value):-1;
    }
    static void ReadAfter(ref PlayerPreferencesMenuState state,PlayerPreferenceSnapshot before,PlayerPreferenceSnapshot after)
    {
        state.ReadAfterRevision=after.Revision.HasValue?checked((long)after.Revision.Value):-1;
        state.ReadUnchanged=before==after?1:0;
        Check(state.ReadUnchanged==1,"Tentative preference patch leaked into committed callback reads.");
    }
    public override void Tick(ref PlayerPreferencesMenuState state,GameContext context)
    {
        ++state.Ticks;var before=context.GetPlayerPreferences();Observe(ref state,before);
        if(state.TicketSequence>0)Result(ref state,context.GetPlayerPreferenceResult(Ticket(in state)));
        if(state.Mode==0)return;
        if(state.Mode==2 && (context.Tick&1)!=0)throw new InvalidOperationException("Later preference Tick rollback.");
        var staged=context.TryStagePlayerPreferences(Patch(before,LiveChanges(before)));Staged(ref state,staged);
        if(staged.Staged)Result(ref state,context.GetPlayerPreferenceResult(staged.Ticket!.Value));
        ReadAfter(ref state,before,context.GetPlayerPreferences());
        context.SetUi(Status,"Staged Tick preference intent");
        switch(state.Mode)
        {
            case 1:return; // A second Tick in the same step must report Busy.
            case 2:return; // The next odd Tick fails the whole batch.
            case 3:state.Target=new(0,99999);return; // Native entity-state validation fails after callback.
            case 4:throw new InvalidOperationException("Immediate preference Tick rollback.");
            default:throw new InvalidOperationException("Unknown preference Tick fixture mode.");
        }
    }
    static string Number(double? value)=>value?.ToString("0.###",CultureInfo.InvariantCulture)??"inherited (unobserved)";
    static string Revision(ulong? value)=>value?.ToString(CultureInfo.InvariantCulture)??"unavailable";
    static void Refresh(ControlContext context,PlayerPreferenceSnapshot snapshot,string status)
    {
        string text=snapshot.Available
            ? $"{status}\nConfig r{snapshot.Revision}: FOV {Number(snapshot.Values.VerticalFov)} ({snapshot.Sources.VerticalFov}), UI {Number(snapshot.Values.UiScale)} ({snapshot.Sources.UiScale}), gain {Number(snapshot.Values.MasterGain)}\n"+
              $"Input X/Y {Number(snapshot.Values.SensitivityX)}/{Number(snapshot.Values.SensitivityY)}, invert {snapshot.Values.InvertX}/{snapshot.Values.InvertY}\n"+
              $"Graphics current {snapshot.Values.Samples} samples/{snapshot.Values.FramesInFlight} frames; next launch {snapshot.NextSamples}/{snapshot.NextFramesInFlight}\n"+
              $"Actual FOV/UI {Number(snapshot.Observation.EffectiveVerticalFov)}/{Number(snapshot.Observation.EffectiveUiScale)}; observed/applied/presented {Revision(snapshot.Observation.ObservedRevision)}/{Revision(snapshot.Observation.AppliedRevision)}/{Revision(snapshot.Observation.PresentedRevision)}; audio {snapshot.Observation.AudioOutcome}, sink {Number(snapshot.Observation.SinkGain)}"
            : "Player preferences unavailable: attach a native player and refresh.";
        context.SetUi(Status,text:text);
        context.SetUi(MenuStatus,text:text);
    }
    public override void Control(ref PlayerPreferencesMenuState state,ControlContext context)
    {
        ++state.Controls;var before=context.GetPlayerPreferences();Observe(ref state,before);
        if(state.TicketSequence>0)Result(ref state,context.GetPlayerPreferenceResult(Ticket(in state)));
        PlayerPreferenceChanges changes=default;PlayerPreferenceFields reset=PlayerPreferenceFields.None;
        bool stage=true;string status="Refreshed committed preferences";
        switch(context.Action)
        {
            case "pref.open":context.SetUi(MenuPanel,visible:true);context.SetModal(MenuPanel);context.RequestPause();stage=false;break;
            case "pref.close":context.SetUi(MenuPanel,visible:false);context.SetModal(null);context.RequestResume();stage=false;break;
            case "pref.refresh":case "pref.query":stage=false;break;
            case "pref.remember":state.GuardHigh=Bits(before.Owner.High);state.GuardLow=Bits(before.Owner.Low);state.GuardRevision=checked((long)(before.Revision??0));stage=false;status="Guard remembered for conflict probe";break;
            case "live":case "pref.stage":case "pref.stage.throw":case "pref.stage.invalid":case "pref.stage.save":case "pref.stage.load":case "pref.stale":changes=LiveChanges(before);break;
            case "pref.fov.minus":changes=new(VerticalFov:Math.Clamp((before.Values.VerticalFov??before.Observation.EffectiveVerticalFov??60)-5,5,150));break;
            case "pref.fov.plus":changes=new(VerticalFov:Math.Clamp((before.Values.VerticalFov??before.Observation.EffectiveVerticalFov??60)+5,5,150));break;
            case "pref.sensitivity.minus":changes=new(SensitivityX:Math.Clamp(before.Values.SensitivityX-.05,0,10),SensitivityY:Math.Clamp(before.Values.SensitivityY-.05,0,10));break;
            case "pref.sensitivity.plus":changes=new(SensitivityX:Math.Clamp(before.Values.SensitivityX+.05,0,10),SensitivityY:Math.Clamp(before.Values.SensitivityY+.05,0,10));break;
            case "pref.invert":changes=new(InvertY:!before.Values.InvertY);break;
            case "pref.ui.minus":changes=new(UiScale:Math.Clamp((before.Values.UiScale??before.Observation.EffectiveUiScale??1)-.25,.25,8));break;
            case "pref.ui.plus":changes=new(UiScale:Math.Clamp((before.Values.UiScale??before.Observation.EffectiveUiScale??1)+.25,.25,8));break;
            case "pref.gain.minus":changes=new(MasterGain:Math.Clamp(before.Values.MasterGain-.1,0,1));break;
            case "pref.gain.plus":changes=new(MasterGain:Math.Clamp(before.Values.MasterGain+.1,0,1));break;
            case "pref.reset":reset=PlayerPreferenceFields.VerticalFov|PlayerPreferenceFields.UiScale|PlayerPreferenceFields.MasterGain;break;
            case "pref.reset.all":reset=PlayerPreferenceFields.All;break;
            case "pref.graphics.low":changes=new(Samples:1,FramesInFlight:1);break;
            case "pref.graphics.high":changes=new(Samples:4,FramesInFlight:2);break;
            case "pref.save":
                var saved=context.RequestSave("player-preferences",0);state.SaveHigh=saved.EpochHigh;state.SaveLow=saved.EpochLow;state.SaveSequence=saved.Sequence;stage=false;break;
            case "pref.load":
                var loaded=context.RequestLoad("player-preferences",1);state.SaveHigh=loaded.EpochHigh;state.SaveLow=loaded.EpochLow;state.SaveSequence=loaded.Sequence;stage=false;break;
            default:throw new InvalidOperationException("Unknown preference menu action: "+context.Action);
        }
        if(stage)
        {
            var patch=Patch(before,changes,reset);
            if(context.Action=="pref.stale")patch=patch with {Owner=new(Bits(state.GuardHigh),Bits(state.GuardLow)),ExpectedRevision=checked((ulong)state.GuardRevision)};
            var staged=context.TryStagePlayerPreferences(patch);Staged(ref state,staged);
            if(staged.Staged)Result(ref state,context.GetPlayerPreferenceResult(staged.Ticket!.Value));
            ReadAfter(ref state,before,context.GetPlayerPreferences());
            status=staged.Staged?"Staged intent: refresh to read acceptance/application":"Rejected: "+staged.Rejection+"; refresh before retry";
            if(context.Action=="pref.stage.throw")
            { context.SetUi(Status,"Tentative Control must roll back");context.RequestResume();throw new InvalidOperationException("Preference Control rollback."); }
            if(context.Action=="pref.stage.invalid")state.Target=new(0,99999);
            if(context.Action=="pref.stage.save")
            { var saved=context.RequestSave("player-preferences",0);state.SaveHigh=saved.EpochHigh;state.SaveLow=saved.EpochLow;state.SaveSequence=saved.Sequence; }
            if(context.Action=="pref.stage.load")
            { var loaded=context.RequestLoad("player-preferences",1);state.SaveHigh=loaded.EpochHigh;state.SaveLow=loaded.EpochLow;state.SaveSequence=loaded.Sequence; }
        }
        Refresh(context,before,status);
    }
}
