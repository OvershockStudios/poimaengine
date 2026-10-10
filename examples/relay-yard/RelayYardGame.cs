// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Runtime.InteropServices;
using Poima;

namespace Poima.Examples;

// Flat native-owned state. The locomotion config/route declarations are linked
// from LocomotionYardGame.cs; no second component layout or movement authority.
[StructLayout(LayoutKind.Sequential)]
public struct RelayState
{
    public EntityId Actor, AnimatedVisual, CellOne, CellTwo, CellThree;
    public int Initialized, Started, Collected, Won, LockedUses;
    public int Ticks, Phase, RouteValid, Plans, Arrivals, LastPathStatus, LastCornerCount;
    public int PreviousPositionValid, Moving, AnimationMode, AnimationClip, AnimationLoop, AnimationChanges;
    public int AnimationTransitions, RateChanges, DeferredRateUpdates;
    public double GoalX, GoalZ, PreviousX, PreviousZ, LastDisplacement;
    public double LastHorizontalSpeed, RequestedRunRate, AnimationRate;
    public long PreviousSampleTick;
    public int StatusKey, SaveStatusKey, SavePending, SaveNotice, SaveKind, LastSaveState, LastSaveError;
    public long SaveHigh, SaveLow, SaveSequence, LastSaveGeneration, ObservedEpochHigh, ObservedEpochLow;
    public int PreferenceNotice;
}

[GameModule("poima.examples.relay-yard")]
public sealed class RelayYardGame : Game<RelayState>, INavigationGame,
    ICharacterInputGame, IInertialAnimationGame, IPlayerPreferencesGame
{
    public static readonly EntityId Actor = new(0,300), Player = new(0,100), Camera = new(0,101), Terminal = new(0,600);
    public static readonly UiId StatusLabel = new(0,700), MotionLabel = new(0,701), SaveLabel = new(0,702);
    public static readonly UiId WelcomeSaveLabel = new(0,914), MenuSaveLabel = new(0,922);
    public static readonly UiId Welcome = new(0,910), Menu = new(0,920), PreferenceLabel = new(0,921);
    const int Ready=0, Travelling=1, Delivered=2, Blocked=3;
    const int Idle=0, Running=1;
    const double RateEpsilon=.01, UseDistance=3;
    const string SaveSlot="relay-yard";

    public override void Initialize(ref RelayState state)
    {
        state=default;
        state.Actor=Actor;
        state.AnimationMode=state.AnimationClip=state.StatusKey=state.SaveStatusKey=-1;
    }

    // Exact reviewed source-motion, navigation-config and save-ticket helpers
    // follow below. They are copied from the existing licensed-source sample,
    // with only the flat state type renamed and sample labels adjusted.
    static void Check(bool condition,string message)
    {
        if(!condition)throw new InvalidOperationException(message);
    }

    static void Validate(in LocomotionVisualConfig config,in LocomotionRoute route)
    {
        Check(config.Visual!=default && config.IdleClip>=0 && config.RunClip>=0 && config.IdleClip!=config.RunClip,"Author a visual rig and distinct inspected idle/run clip indices.");
        Check(config.BlendTicks is >=0 and <=3600 && config.IdleLeadTicks is >=0 and <=3600,"Invalid locomotion-yard animation timing.");
        Check(double.IsFinite(config.NominalRunSpeed) && config.NominalRunSpeed>0 && config.NominalRunSpeed<=100,"NominalRunSpeed must be finite and within (0,100] meters per second.");
        Check(double.IsFinite(config.SlowdownDistance) && config.SlowdownDistance>=.1 && config.SlowdownDistance<=100,"SlowdownDistance must be finite and within [0.1,100] meters, above the arrival tolerance.");
        Check(double.IsFinite(config.HomeX) && double.IsFinite(config.HomeZ) && double.IsFinite(config.DeliveryX) && double.IsFinite(config.DeliveryZ),"Invalid delivery stations.");
        Check(route.Coordinates.Count is >=0 and <=30,"Locomotion coordinates exceed their fixed-capacity native buffer.");
        Check(route.Coordinates.Count%3==0 && route.Cursor>=0 && route.Cursor<=route.Coordinates.Count/3,"Invalid persisted locomotion route.");
        for(int i=0;i<route.Coordinates.Count;++i)Check(float.IsFinite(route.Coordinates[i]),"Nonfinite locomotion route coordinate.");
    }

    static double Distance(double x,double z)=>Math.Sqrt(x*x+z*z);
    static double Wrap(double angle)
    {
        while(angle>180)angle-=360;
        while(angle < -180)angle+=360;
        return angle;
    }

    static void Animate(ref RelayState state,GameContext context,in LocomotionVisualConfig config)
    {
        var observed=context.GetAnimationExtended(config.Visual)
            ?? throw new InvalidOperationException("LocomotionVisualConfig must reference a live AnimationRig.");
        var current=observed.State;
        int mode=state.Moving!=0 ? Running : Idle;
        int clip=mode==Running ? config.RunClip : config.IdleClip;
        double rate=mode==Running ? state.RequestedRunRate : 1;
        bool bindingChanged=state.AnimatedVisual!=config.Visual;
        bool clipChanged=current.Clip!=clip;
        bool statusChanged=bindingChanged || state.AnimationMode!=mode;
        state.AnimationRate=current.Speed;

        if(bindingChanged || clipChanged)
        {
            // A clip change starts its authored loop at zero. Binding to an
            // already matching clip preserves its committed clock. Changes in
            // movement can interrupt a fade, but rate-only changes never do.
            context.SetAnimation(config.Visual,clip,AnimationTransitionMode.Inertial,
                time:clipChanged ? 0 : current.Time,speed:rate,loop:true,playing:true,
                blendTicks:(uint)config.BlendTicks);
            state.AnimationRate=rate;
            ++state.AnimationChanges;
            ++state.AnimationTransitions;
        }
        else
        {
            bool rateChanged=Math.Abs(current.Speed-rate)>=RateEpsilon;
            bool playbackChanged=!current.Loop || !current.Playing;
            if(rateChanged || playbackChanged)
            {
                // SetAnimation replaces the entire native playback, including
                // its fade. Defer rate/flag corrections until the finite active
                // transition ends; then retain the observed time with blend0.
                // No rate correction extends or restarts an active transition.
                if(current.Transition is not null)++state.DeferredRateUpdates;
                else
                {
                    double time=current.Time;
                    // Only recovering an externally stopped nonloop endpoint
                    // wraps its invalid loop endpoint; rate-only updates never
                    // change the committed time. Normal sample clocks loop.
                    if(!current.Loop && current.Duration>0 && time>=current.Duration)time=0;
                    context.SetAnimation(config.Visual,clip,AnimationTransitionMode.Inertial,
                        time:time,speed:rate,loop:true,playing:true,blendTicks:0);
                    state.AnimationRate=rate;
                    ++state.AnimationChanges;
                    if(rateChanged)++state.RateChanges;
                }
            }
        }
        state.AnimationMode=mode;
        state.AnimationClip=clip;
        state.AnimationLoop=1;
        state.AnimatedVisual=config.Visual;
        if(statusChanged)context.SetUi(MotionLabel,mode==Running
            ? "Run loop — rate follows committed controller movement."
            : "Idle loop — breathing continues while stationary.");
    }

    static SaveTicket Ticket(in RelayState state)=>new(state.SaveHigh,state.SaveLow,state.SaveSequence);
    static void Restored(ref RelayState state,SaveCapabilities saves)
    {
        if(state.ObservedEpochHigh==saves.Epoch.High && state.ObservedEpochLow==saves.Epoch.Low)return;
        state.ObservedEpochHigh=saves.Epoch.High;
        state.ObservedEpochLow=saves.Epoch.Low;
        if(saves.LastRestore is { } restore && restore.DestinationEpoch==saves.Epoch)
        {
            state.SavePending=0;
            state.SaveNotice=6;
            state.LastSaveState=(int)SaveOperationState.Succeeded;
            state.LastSaveError=0;
            state.LastSaveGeneration=checked((long)restore.Generation);
        }
    }

    static void Result(ref RelayState state,SaveOperationResult result)
    {
        state.LastSaveState=(int)result.State;
        state.LastSaveError=result.ErrorCode;
        state.LastSaveGeneration=checked((long)result.Generation);
        if(!result.IsTerminal)return;
        state.SavePending=0;
        state.SaveNotice=result.State switch
        {
            SaveOperationState.Succeeded=>result.Kind==Poima.SaveKind.Load ? 6 : 5,
            SaveOperationState.Expired=>8,
            _=>7
        };
    }

    static string SaveStatus(in RelayState state)=>state.SaveNotice switch
    {
        1=>"Saving checkpoint — Status refreshes while paused.",
        2=>"Loading checkpoint — Status refreshes while paused.",
        3=>"Checkpoint storage is not configured.",
        4=>"Checkpoint request unavailable; another operation may be pending.",
        5=>"Checkpoint saved.", 6=>"Checkpoint loaded.",
        7=>"Checkpoint operation failed; inspect the native save result.",
        8=>"Previous checkpoint request is no longer tracked.",
        _=>"Save progress or load your last relay checkpoint."
    };

    static void PollSave(ref RelayState state,GameContext context)
    {
        Restored(ref state,context.Saves);
        if(state.SavePending!=0)Result(ref state,context.GetSaveResult(Ticket(in state)));
        if(state.SaveStatusKey==state.SaveNotice)return;
        context.SetUi(SaveLabel,SaveStatus(in state));
        context.SetUi(WelcomeSaveLabel,SaveStatus(in state));
        context.SetUi(MenuSaveLabel,SaveStatus(in state));
        state.SaveStatusKey=state.SaveNotice;
    }

    static void PollSave(ref RelayState state,ControlContext context)
    {
        Restored(ref state,context.Saves);
        if(state.SavePending!=0)Result(ref state,context.GetSaveResult(Ticket(in state)));
    }



    static string Status(in RelayState state)=>state.Won!=0
        ? "Relay restored. Delivery complete!"
        : state.Started==0 ? "Begin your shift, or load a checkpoint."
        : state.Collected<3 ? $"Power cells {state.Collected}/3 — look at a cell and press E within 3 m."
        : state.Phase==Delivered ? "Courier arrived — look at the relay terminal and press E within 3 m."
        : state.Phase==Blocked ? "Courier route unavailable — Menu > Retry courier queries navigation again."
        : "All cells recovered. Follow the courier around the cover to the relay.";

    static void Publish(GameContext context,in RelayState state)
    {
        string text=Status(in state);
        if(context.GetUi(StatusLabel).Text!=text)context.SetUi(StatusLabel,text);
    }

    static void Collect(ref EntityId cell,ref RelayState state,GameContext context)
    {
        context.Despawn(cell);
        cell=default;
        ++state.Collected;
    }

    static void Use(ref RelayState state,GameContext context)
    {
        if(!context.Pressed(Player,GameAction.Use) || state.Won!=0)return;
        var camera=context.Get(Camera).Transform;
        Span<EntityId> ignore=stackalloc EntityId[1];ignore[0]=Player;
        var hit=context.Raycast(camera.Position,camera.Forward,UseDistance,ignore);
        if(hit is not { } target)return;
        if(state.CellOne!=default && target.Entity==state.CellOne)Collect(ref state.CellOne,ref state,context);
        else if(state.CellTwo!=default && target.Entity==state.CellTwo)Collect(ref state.CellTwo,ref state,context);
        else if(state.CellThree!=default && target.Entity==state.CellThree)Collect(ref state.CellThree,ref state,context);
        else if(target.Entity==Terminal)
        {
            // A UI action, a cell count or touching the relay cannot win.
            // Completion requires this real in-range native ray after arrival.
            if(state.Collected==3 && state.Phase==Delivered)state.Won=1;
            else ++state.LockedUses;
        }
    }

    static void Courier(ref RelayState state,GameContext context)
    {
        var config=context.Get<LocomotionVisualConfig>(Actor);
        var route=context.Get<LocomotionRoute>(Actor);
        Validate(in config,in route);
        var actor=context.Get(Actor);
        Check(actor.Motion==BodyMotion.Character,"The courier needs its native CharacterController root.");
        var position=actor.Transform.Position;
        long tick=checked((long)context.Tick);
        state.LastDisplacement=state.PreviousPositionValid!=0 && state.PreviousSampleTick+1==tick
            ? Distance(position.X-state.PreviousX,position.Z-state.PreviousZ) : 0;
        state.PreviousX=position.X;state.PreviousZ=position.Z;state.PreviousSampleTick=tick;state.PreviousPositionValid=1;
        Check(double.IsFinite(context.DeltaTime) && context.DeltaTime>0,"A positive fixed interval is required.");
        state.LastHorizontalSpeed=state.LastDisplacement/context.DeltaTime;
        Check(double.IsFinite(state.LastHorizontalSpeed),"Controller displacement must remain finite.");
        state.RequestedRunRate=Math.Clamp(state.LastHorizontalSpeed/config.NominalRunSpeed,0,8);
        state.Moving=state.LastDisplacement>.001 ? 1 : 0;
        Animate(ref state,context,in config);

        if(state.Collected==3 && state.Phase==Ready)
        {
            state.Phase=Travelling;state.GoalX=config.DeliveryX;state.GoalZ=config.DeliveryZ;state.RouteValid=0;
        }
        bool routeChanged=false;
        if(state.Phase==Travelling && state.RouteValid==0)
        {
            Span<NavigationPoint> corners=stackalloc NavigationPoint[10];
            var answer=context.FindNavigationPath(Actor,new(state.GoalX,0,state.GoalZ),corners,new(.5,2,.5),256,4096);
            state.LastPathStatus=(int)answer.Status;state.LastCornerCount=answer.CornerCount;++state.Plans;
            route.Coordinates.Clear();route.Cursor=0;routeChanged=true;
            if(!answer.Complete || answer.CornerCount<1)state.Phase=Blocked;
            else
            {
                for(int i=0;i<answer.CornerCount;++i)
                    Check(route.Coordinates.TryAdd(corners[i].X) && route.Coordinates.TryAdd(corners[i].Y) &&
                          route.Coordinates.TryAdd(corners[i].Z),"Courier path exceeded native buffer capacity.");
                state.RouteValid=1;
            }
        }
        if(state.Phase==Travelling)
        {
            int priorCursor=route.Cursor;
            while(route.Cursor<route.Coordinates.Count/3 &&
                  Distance(route.Coordinates[route.Cursor*3]-position.X,route.Coordinates[route.Cursor*3+2]-position.Z)<.08)
                ++route.Cursor;
            routeChanged|=route.Cursor!=priorCursor;
            if(route.Cursor==route.Coordinates.Count/3)
            {state.Phase=Delivered;++state.Arrivals;}
            else
            {
                double dx=route.Coordinates[route.Cursor*3]-position.X,dz=route.Coordinates[route.Cursor*3+2]-position.Z;
                double desired=Math.Atan2(-dx,-dz)*180/Math.PI;
                var forward=actor.Transform.Forward;
                double turn=Wrap(desired-Math.Atan2(-forward.X,-forward.Z)*180/Math.PI);
                float move=Math.Abs(turn)>4 ? 0 : (float)(.75*Math.Min(1,Distance(dx,dz)/config.SlowdownDistance));
                context.SetCharacterInput(Actor,0,move,(float)Math.Clamp(turn,-8,8));
            }
        }
        if(routeChanged)context.Set(Actor,in route);
    }

    public override void Tick(ref RelayState state,GameContext context)
    {
        PollSave(ref state,context);
        if(state.Initialized==0)
        {
            state.CellOne=context.Spawn(new(0,1101));state.CellTwo=context.Spawn(new(0,1102));
            state.CellThree=context.Spawn(new(0,1103));state.Initialized=1;
            // Standalone game.run cannot issue pause from Tick. The initial
            // welcome gates game logic and blocks input, without pretending
            // the engine clock is paused. Begin's Control requests Resume.
            if(state.Started==0)context.SetModal(Welcome);
        }
        if(state.Started==0) {Publish(context,in state);return;}
        ++state.Ticks;
        Use(ref state,context);
        Courier(ref state,context);
        Publish(context,in state);
    }

    static string Preferences(PlayerPreferenceSnapshot value,int notice)
    {
        if(!value.Available)return "Preferences unavailable until a native player is attached.";
        static string Number(double? n)=>n?.ToString("0.##",CultureInfo.InvariantCulture)??"inherited";
        string result=notice switch {1=>"Intent staged; Refresh reads acceptance.",2=>"Intent rejected; Refresh before retry.",_=>"Current player preferences."};
        return $"{result}\nFOV {Number(value.Values.VerticalFov??value.Observation.EffectiveVerticalFov)} | " +
            $"UI {Number(value.Values.UiScale??value.Observation.EffectiveUiScale)} | gain {Number(value.Values.MasterGain)}\n" +
            $"Pointer {Number(value.Values.SensitivityX)}/{Number(value.Values.SensitivityY)} | config r{value.Revision}";
    }

    static void Preference(ref RelayState state,ControlContext context)
    {
        var before=context.GetPlayerPreferences();
        if(context.Action=="refresh") {state.PreferenceNotice=0;return;}
        if(!before.Available || before.Revision is null) {state.PreferenceNotice=2;return;}
        PlayerPreferenceChanges changes=default;PlayerPreferenceFields reset=PlayerPreferenceFields.None;
        switch(context.Action)
        {
            case "fov.minus":case "fov.plus":
                if((before.Values.VerticalFov??before.Observation.EffectiveVerticalFov) is not { } fov)
                {state.PreferenceNotice=2;return;}
                changes=new(VerticalFov:Math.Clamp(fov+(context.Action=="fov.plus"?5:-5),5,150));break;
            case "ui.minus":case "ui.plus":
                if((before.Values.UiScale??before.Observation.EffectiveUiScale) is not { } scale)
                {state.PreferenceNotice=2;return;}
                changes=new(UiScale:Math.Clamp(scale+(context.Action=="ui.plus" ? .25 : -.25),.25,8));break;
            case "sensitivity.minus":case "sensitivity.plus":
                double delta=context.Action=="sensitivity.plus" ? .05 : -.05;
                changes=new(SensitivityX:Math.Clamp(before.Values.SensitivityX+delta,0,10),
                            SensitivityY:Math.Clamp(before.Values.SensitivityY+delta,0,10));break;
            case "gain.minus":case "gain.plus":
                changes=new(MasterGain:Math.Clamp(before.Values.MasterGain+(context.Action=="gain.plus" ? .1 : -.1),0,1));break;
            case "preferences.reset":reset=PlayerPreferenceFields.VerticalFov|PlayerPreferenceFields.UiScale|
                PlayerPreferenceFields.SensitivityX|PlayerPreferenceFields.SensitivityY|PlayerPreferenceFields.MasterGain;break;
            default:throw new InvalidOperationException("Unknown preference action.");
        }
        // Always guard against the fresh native snapshot. No saved owner,
        // revision, preference value or ticket is ever reapplied on restore.
        var result=context.TryStagePlayerPreferences(new(before.Owner,before.Revision.Value,changes,reset));
        state.PreferenceNotice=result.Staged?1:2;
    }

    public override void Control(ref RelayState state,ControlContext context)
    {
        PollSave(ref state,context);
        switch(context.Action)
        {
            case "begin":state.Started=1;context.SetUi(Welcome,visible:false);context.SetModal(null);context.RequestResume();break;
            case "menu":context.SetUi(Menu,visible:true);context.SetModal(Menu);context.RequestPause();break;
            case "close":context.SetUi(Menu,visible:false);context.SetModal(null);context.RequestResume();break;
            case "retry":if(state.Phase==Blocked) {state.Phase=Travelling;state.RouteValid=0;}break;
            case "save":case "load":
                if(!context.Saves.Enabled) {state.SaveNotice=3;break;}
                var request=context.Action=="save"?context.TryRequestSave(SaveSlot):context.TryRequestLoad(SaveSlot);
                if(!request.Accepted) {state.SaveNotice=4;break;}
                state.SaveHigh=request.Ticket.EpochHigh;state.SaveLow=request.Ticket.EpochLow;
                state.SaveSequence=request.Ticket.Sequence;state.SavePending=1;
                state.SaveKind=context.Action=="save"?(int)Poima.SaveKind.Save:(int)Poima.SaveKind.Load;
                state.SaveNotice=context.Action=="save"?1:2;break;
            default:Preference(ref state,context);break;
        }
        context.SetUi(StatusLabel,Status(in state));
        context.SetUi(SaveLabel,SaveStatus(in state));
        context.SetUi(WelcomeSaveLabel,SaveStatus(in state));
        context.SetUi(MenuSaveLabel,SaveStatus(in state));state.SaveStatusKey=state.SaveNotice;
        context.SetUi(PreferenceLabel,Preferences(context.GetPlayerPreferences(),state.PreferenceNotice));
    }
}
