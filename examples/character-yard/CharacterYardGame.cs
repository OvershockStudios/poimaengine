// SPDX-License-Identifier: Apache-2.0
using Poima;

namespace Poima.Examples;

// Author these values from the inspected cooked asset, not its source clip order.
[GameplayComponent("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb70")]
public partial struct ActorVisualConfig
{
    [GameplayField("00000000000000000000000000000001")] public EntityId Visual;
    [GameplayField("00000000000000000000000000000002")] public int HoldClip;
    [GameplayField("00000000000000000000000000000003")] public int MotionClip;
    [GameplayField("00000000000000000000000000000004")] public int BlendTicks;
    [GameplayField("00000000000000000000000000000005")] public int IdleLeadTicks;
    [GameplayField("00000000000000000000000000000006")] public double HomeX;
    [GameplayField("00000000000000000000000000000007")] public double HomeZ;
    [GameplayField("00000000000000000000000000000008")] public double DeliveryX;
    [GameplayField("00000000000000000000000000000009")] public double DeliveryZ;
    // Registered scalar components use int flags: zero deliberately forbids
    // looping a source take whose final pose differs from its initial pose.
    [GameplayField("0000000000000000000000000000000a")] public int LoopMotion;
}

[GameplayBuffer(typeof(float),30)]
public partial struct YardCoordinates { }

[GameplayComponent("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb71")]
public partial struct YardRoute
{
    [GameplayField("00000000000000000000000000000001")] public YardCoordinates Coordinates;
    [GameplayField("00000000000000000000000000000002")] public int Cursor;
}

public struct YardState
{
    public EntityId Actor, AnimatedVisual;
    public int Ticks, ReadyTicks, Phase, Paused, DispatchRequested, Destination;
    public int RouteValid, Plans, Arrivals, Dispatches, LastPathStatus, LastCornerCount;
    public int PreviousPositionValid, Moving, AnimationMode, AnimationClip, AnimationLoop, AnimationChanges;
    public int GestureRequested, GestureActive;
    public double GoalX, GoalZ, PreviousX, PreviousZ, LastDisplacement;
    public long PreviousSampleTick;
    public int StatusKey, SaveStatusKey, SavePending, SaveNotice, SaveKind, LastSaveState, LastSaveError;
    public long SaveHigh, SaveLow, SaveSequence, LastSaveGeneration, ObservedEpochHigh, ObservedEpochLow;
}

[GameModule("poima.examples.character-yard")]
public sealed class CharacterYardGame : Game<YardState>, INavigationGame, ICharacterInputGame, IInertialAnimationGame
{
    // The camera/controller used by the player is a different authored entity.
    public static readonly EntityId Actor = new(0,300);
    public static readonly UiId StatusLabel = new(0,700);
    public static readonly UiId MotionLabel = new(0,701);
    public static readonly UiId SaveLabel = new(0,702);
    const int Ready=0, Travelling=1, Delivered=2, Blocked=3;
    const int Hold=0, Advancing=1, Finished=2;
    const string SaveSlot="character-yard";

    public override void Initialize(ref YardState state)
    {
        state=default;
        state.Actor=Actor;
        state.AnimationMode=-1;
        state.AnimationClip=-1;
        state.StatusKey=-1;
        state.SaveStatusKey=-1;
    }

    static void Check(bool condition,string message)
    {
        if(!condition)throw new InvalidOperationException(message);
    }

    static void Validate(in ActorVisualConfig config,in YardRoute route)
    {
        Check(config.Visual!=default && config.HoldClip>=0 && config.MotionClip>=0,"Author a visual rig and inspected hold/motion clip indices.");
        Check(config.BlendTicks is >=0 and <=3600 && config.IdleLeadTicks is >=0 and <=3600,"Invalid character-yard animation timing.");
        Check(config.LoopMotion is 0 or 1,"LoopMotion must be zero or one.");
        Check(double.IsFinite(config.HomeX) && double.IsFinite(config.HomeZ) && double.IsFinite(config.DeliveryX) && double.IsFinite(config.DeliveryZ),"Invalid delivery stations.");
        Check(route.Coordinates.Count%3==0 && route.Cursor>=0 && route.Cursor<=route.Coordinates.Count/3,"Invalid persisted character route.");
        for(int i=0;i<route.Coordinates.Count;++i)Check(float.IsFinite(route.Coordinates[i]),"Nonfinite character route coordinate.");
    }

    static double Distance(double x,double z)=>Math.Sqrt(x*x+z*z);
    static double Wrap(double angle)
    {
        while(angle>180)angle-=360;
        while(angle < -180)angle+=360;
        return angle;
    }

    static void Animate(ref YardState state,GameContext context,in ActorVisualConfig config)
    {
        var current=context.GetAnimationExtended(config.Visual)
            ?? throw new InvalidOperationException("ActorVisualConfig must reference a live AnimationRig.");
        bool restart=state.GestureRequested!=0;
        if(restart) {state.GestureRequested=0;state.GestureActive=1;}
        if(!restart && state.GestureActive!=0 && current.State.Clip==config.MotionClip &&
            current.State.Time>=current.State.Duration)state.GestureActive=0;
        bool motion=state.Moving!=0 || state.GestureActive!=0;
        int clip=motion ? config.MotionClip : config.HoldClip;
        bool loop=motion && state.GestureActive==0 && config.LoopMotion!=0;
        bool sameClip=current.State.Clip==clip;
        double time=!restart && sameClip ? current.State.Time : 0;
        bool ended=sameClip && !restart && time>=current.State.Duration;
        int mode=motion ? ended && !loop ? Finished : Advancing : Hold;
        if(!restart && state.AnimationMode==mode && state.AnimationClip==clip &&
            state.AnimationLoop==(loop ? 1 : 0) && state.AnimatedVisual==config.Visual)return;
        // Holding does not rewind. A completed nonloop take remains stopped;
        // only the explicit gesture action restarts it from time zero.
        if(loop && ended)time=0;
        context.SetAnimation(config.Visual,clip,AnimationTransitionMode.Inertial,time:time,
            speed:mode==Advancing ? 1 : 0,loop:loop,playing:mode==Advancing,blendTicks:(uint)config.BlendTicks);
        state.AnimationMode=mode;
        state.AnimationClip=clip;
        state.AnimationLoop=loop ? 1 : 0;
        state.AnimatedVisual=config.Visual;
        ++state.AnimationChanges;
        context.SetUi(MotionLabel,mode switch
        {
            Advancing=>loop ? "Source motion: advancing (loop enabled)" : "Source motion: advancing once",
            Finished=>"Source motion: finished — Gesture replays it",
            _=>"Source motion: holding pose — Gesture replays it"
        });
    }

    static string Status(in YardState state)
    {
        if(state.Paused!=0)return "Paused — resume to continue the delivery.";
        return state.Phase switch
        {
            Ready=>"Courier ready — delivery starts shortly.",
            Travelling=>state.Destination==0 ? "Courier returning to the home station." : "Courier delivering around the cover.",
            Delivered=>"Delivery complete — dispatch for another trip.",
            _=>"No complete route — dispatch to try again."
        };
    }

    static int StatusKey(in YardState state)=>state.Phase+state.Paused*4+state.Destination*8;
    static void PublishStatus(ref YardState state,GameContext context)
    {
        int key=StatusKey(in state);
        if(key==state.StatusKey)return;
        context.SetUi(StatusLabel,Status(in state));
        state.StatusKey=key;
    }

    static SaveTicket Ticket(in YardState state)=>new(state.SaveHigh,state.SaveLow,state.SaveSequence);
    static void Restored(ref YardState state,SaveCapabilities saves)
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

    static void Result(ref YardState state,SaveOperationResult result)
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

    static string SaveStatus(in YardState state)=>state.SaveNotice switch
    {
        1=>"Saving checkpoint — Status refreshes while paused.",
        2=>"Loading checkpoint — Status refreshes while paused.",
        3=>"Checkpoint storage is not configured.",
        4=>"Checkpoint request unavailable; another operation may be pending.",
        5=>"Checkpoint saved.", 6=>"Checkpoint loaded.",
        7=>"Checkpoint operation failed; inspect the native save result.",
        8=>"Previous checkpoint request is no longer tracked.",
        _=>"Save a checkpoint or load your last delivery."
    };

    static void PollSave(ref YardState state,GameContext context)
    {
        Restored(ref state,context.Saves);
        if(state.SavePending!=0)Result(ref state,context.GetSaveResult(Ticket(in state)));
        if(state.SaveStatusKey==state.SaveNotice)return;
        context.SetUi(SaveLabel,SaveStatus(in state));
        state.SaveStatusKey=state.SaveNotice;
    }

    static void PollSave(ref YardState state,ControlContext context)
    {
        Restored(ref state,context.Saves);
        if(state.SavePending!=0)Result(ref state,context.GetSaveResult(Ticket(in state)));
    }

    public override void Tick(ref YardState state,GameContext context)
    {
        PollSave(ref state,context);
        if(state.Paused!=0)return;
        ++state.Ticks;
        var config=context.Get<ActorVisualConfig>(state.Actor);
        var route=context.Get<YardRoute>(state.Actor);
        Validate(in config,in route);
        var actor=context.Get(state.Actor);
        Check(actor.Motion==BodyMotion.Character,"The courier needs an authored CharacterController root.");
        var position=actor.Transform.Position;
        // Get().Velocity has already received neutral input preparation. Sample
        // prior committed positions instead; both this history and the route save.
        long tick=checked((long)context.Tick);
        state.LastDisplacement=state.PreviousPositionValid!=0 && state.PreviousSampleTick+1==tick
            ? Distance(position.X-state.PreviousX,position.Z-state.PreviousZ) : 0;
        state.PreviousX=position.X;
        state.PreviousZ=position.Z;
        state.PreviousSampleTick=tick;
        state.PreviousPositionValid=1;
        state.Moving=state.LastDisplacement>.001 ? 1 : 0;
        Animate(ref state,context,in config);

        if(state.Phase==Ready && state.DispatchRequested==0)
        {
            if(++state.ReadyTicks>config.IdleLeadTicks)state.DispatchRequested=1;
        }
        if(state.DispatchRequested!=0)
        {
            state.DispatchRequested=0;
            state.Destination=state.Destination==0 ? 1 : 0;
            state.GoalX=state.Destination==0 ? config.HomeX : config.DeliveryX;
            state.GoalZ=state.Destination==0 ? config.HomeZ : config.DeliveryZ;
            state.RouteValid=0;
            state.Phase=Travelling;
            ++state.Dispatches;
        }

        bool routeChanged=false;
        if(state.Phase==Travelling && state.RouteValid==0)
        {
            Span<NavigationPoint> corners=stackalloc NavigationPoint[10];
            var answer=context.FindNavigationPath(state.Actor,new(state.GoalX,0,state.GoalZ),corners,new(.5,2,.5),256,4096);
            state.LastPathStatus=(int)answer.Status;
            state.LastCornerCount=answer.CornerCount;
            ++state.Plans;
            route.Coordinates.Clear();
            route.Cursor=0;
            routeChanged=true;
            if(!answer.Complete || answer.CornerCount<1)state.Phase=Blocked;
            else
            {
                for(int i=0;i<answer.CornerCount;++i)
                    Check(route.Coordinates.TryAdd(corners[i].X) && route.Coordinates.TryAdd(corners[i].Y) && route.Coordinates.TryAdd(corners[i].Z),"Character route exceeded its persisted capacity.");
                state.RouteValid=1;
            }
        }

        if(state.Phase==Travelling)
        {
            int priorCursor=route.Cursor;
            while(route.Cursor<route.Coordinates.Count/3 && Distance(route.Coordinates[route.Cursor*3]-position.X,route.Coordinates[route.Cursor*3+2]-position.Z)<.08)
                ++route.Cursor;
            routeChanged|=route.Cursor!=priorCursor;
            if(route.Cursor==route.Coordinates.Count/3)
            {
                state.Phase=Delivered;
                ++state.Arrivals;
            }
            else
            {
                double dx=route.Coordinates[route.Cursor*3]-position.X,dz=route.Coordinates[route.Cursor*3+2]-position.Z;
                double desired=Math.Atan2(-dx,-dz)*180/Math.PI;
                var forward=actor.Transform.Forward;
                double turn=Wrap(desired-Math.Atan2(-forward.X,-forward.Z)*180/Math.PI);
                float movement=Math.Abs(turn)>4 ? 0 : (float)Math.Min(.75,Distance(dx,dz)/.08);
                context.SetCharacterInput(state.Actor,0,movement,(float)Math.Clamp(turn,-8,8));
            }
        }
        if(routeChanged)context.Set(state.Actor,in route);
        PublishStatus(ref state,context);
    }

    public override void Control(ref YardState state,ControlContext context)
    {
        PollSave(ref state,context);
        switch(context.Action)
        {
            case "dispatch":state.DispatchRequested=1;break;
            case "gesture":state.GestureRequested=1;break;
            case "pause":state.Paused=1;context.RequestPause();break;
            case "resume":state.Paused=0;context.RequestResume();break;
            case "status":break; // Memory-only receipt refresh, including paused playback.
            case "save":
            case "load":
                if(!context.Saves.Enabled) {state.SaveNotice=3;break;}
                var request=context.Action=="save" ? context.TryRequestSave(SaveSlot) : context.TryRequestLoad(SaveSlot);
                if(!request.Accepted) {state.SaveNotice=4;break;}
                state.SaveHigh=request.Ticket.EpochHigh;
                state.SaveLow=request.Ticket.EpochLow;
                state.SaveSequence=request.Ticket.Sequence;
                state.SaveKind=context.Action=="save" ? (int)Poima.SaveKind.Save : (int)Poima.SaveKind.Load;
                state.SavePending=1;
                state.SaveNotice=context.Action=="save" ? 1 : 2;
                break;
            default:throw new InvalidOperationException("Unknown character-yard control action.");
        }
        context.SetUi(StatusLabel,Status(in state));
        state.StatusKey=StatusKey(in state);
        context.SetUi(SaveLabel,SaveStatus(in state));
        state.SaveStatusKey=state.SaveNotice;
    }
}
