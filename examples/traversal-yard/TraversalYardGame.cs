// SPDX-License-Identifier: Apache-2.0
using Poima;

namespace Poima.Examples;

// Author these values from the inspected cooked asset, not its source clip order.
[GameplayComponent("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb75")]
public partial struct LocomotionVisualConfig
{
    [GameplayField("00000000000000000000000000000001")] public EntityId Visual;
    [GameplayField("00000000000000000000000000000002")] public int IdleClip;
    [GameplayField("00000000000000000000000000000003")] public int RunClip;
    [GameplayField("00000000000000000000000000000004")] public int BlendTicks;
    [GameplayField("00000000000000000000000000000005")] public int IdleLeadTicks;
    [GameplayField("00000000000000000000000000000006")] public double HomeX;
    [GameplayField("00000000000000000000000000000007")] public double HomeZ;
    [GameplayField("00000000000000000000000000000008")] public double DeliveryX;
    [GameplayField("00000000000000000000000000000009")] public double DeliveryZ;
    // Authored nominal reference speed, calibrated separately from source stride.
    [GameplayField("0000000000000000000000000000000a",Default="3",Unit="m/s")] public double NominalRunSpeed;
    [GameplayField("0000000000000000000000000000000b",Default="0.8",Unit="m")] public double SlowdownDistance;
}

[GameplayBuffer(typeof(float),30)]
public partial struct LocomotionCoordinates { }

[GameplayComponent("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb76")]
public partial struct LocomotionRoute
{
    [GameplayField("00000000000000000000000000000001")] public LocomotionCoordinates Coordinates;
    [GameplayField("00000000000000000000000000000002")] public int Cursor;
}

public struct TraversalState
{
    public EntityId Actor, AnimatedVisual;
    public int Ticks, ReadyTicks, Phase, Paused, DispatchRequested, Destination;
    public int RouteValid, Plans, Arrivals, Dispatches, LastPathStatus, LastCornerCount;
    public int PreviousPositionValid, Moving, AnimationMode, AnimationClip, AnimationLoop, AnimationChanges;
    public int AnimationTransitions, RateChanges, DeferredRateUpdates;
    public double GoalX, GoalZ, PreviousX, PreviousZ, LastDisplacement;
    public double LastHorizontalSpeed, RequestedRunRate, AnimationRate;
    public long PreviousSampleTick;
    public int StatusKey, SaveStatusKey, SavePending, SaveNotice, SaveKind, LastSaveState, LastSaveError;
    public long SaveHigh, SaveLow, SaveSequence, LastSaveGeneration, LastRestoreTick, LastRestoreGeneration;
    // Primitive fields are part of the durable game state; no instance or wall-time state.
    public int PlayerTicks, BeaconMask, Collected, CrouchedBeacon, Won;
    public int LiftRequested, LiftDestination, LiftMoves, LiftBusyNotice;
    public int StanceWrites, LensInitialized, Aiming, AimElapsed, AimTransitions;
    public double AimStartFov, AimTargetFov, AimCurrentFov;
}

[GameModule("poima.examples.traversal-yard")]
public sealed class TraversalYardGame : Game<TraversalState>, INavigationGame, ICharacterInputGame, IInertialAnimationGame, IInputActionsGame, ICharacterStanceGame, ICameraLensGame
{
    // Actor300 owns the native capsule; visual400 is its authored child.
    // Observer player100 and its camera remain separate from courier control.
    public static readonly EntityId Actor = new(0,300);
    public static readonly EntityId Player = new(0,100), Camera = new(0,101);
    public static readonly EntityId FirstBeacon = new(0,210), LiftBeacon = new(0,211), LastBeacon = new(0,212), Gate = new(0,213);
    public static readonly EntityId Lift = new(0,220), LiftLower = new(0,221), LiftUpper = new(0,222);
    public const uint LiftTicks=90, AimDurationTicks=12;
    public const double PickupRadius=1.25, LiftUseRadius=2.5, AimFov=45;
    public static readonly UiId StatusLabel = new(0,700);
    public static readonly UiId MotionLabel = new(0,701);
    public static readonly UiId SaveLabel = new(0,702);
    const int Ready=0, Travelling=1, Delivered=2, Blocked=3;
    const int Idle=0, Running=1;
    // Suppress tiny physics rounding changes; every accepted rate remains
    // controller-derived. This is not stride fitting or automatic loop repair.
    const double RateEpsilon=.01;
    const string SaveSlot="traversal-yard";

    public override void Initialize(ref TraversalState state)
    {
        Check(System.Runtime.CompilerServices.Unsafe.SizeOf<TraversalState>()<=65536,"Locomotion state exceeds the native state byte limit.");
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

    static void Animate(ref TraversalState state,GameContext context,in LocomotionVisualConfig config)
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

    static string Status(in TraversalState state)
    {
        if(state.Paused!=0)return $"Paused — {state.Collected}/3 beacons. Resume to continue.";
        if(state.Won!=0)return "Traversal complete — three beacons returned to the gate.";
        if(state.Collected==3)return "Three beacons collected — return to the starting gate.";
        return $"Beacons {state.Collected}/3 — E picks up; C crouches; right mouse aims. E near lift or Lift calls it.";
    }

    static int StatusKey(in TraversalState state)=>state.BeaconMask+state.Won*8+state.Paused*16;
    static void PublishStatus(ref TraversalState state,GameContext context)
    {
        int key=StatusKey(in state);
        if(key==state.StatusKey)return;
        context.SetUi(StatusLabel,Status(in state));
        state.StatusKey=key;
    }

    static SaveTicket Ticket(in TraversalState state)=>new(state.SaveHigh,state.SaveLow,state.SaveSequence);
    static void Restored(ref TraversalState state,SaveCapabilities saves)
    {
        if(saves.LastRestore is { } restore && restore.DestinationEpoch==saves.Epoch)
        {
            // Host epochs are process identities, never newly copied into the
            // durable game state. A request issued in this owner remains active;
            // a restored source-owner ticket cannot describe a current request.
            if(state.SavePending!=0 && state.SaveHigh==saves.Epoch.High && state.SaveLow==saves.Epoch.Low)return;
            long restoredTick=checked((long)restore.RestoredTick),generation=checked((long)restore.Generation);
            if(state.SavePending==0 && state.LastRestoreTick==restoredTick && state.LastRestoreGeneration==generation)return;
            state.LastRestoreTick=restoredTick;
            state.LastRestoreGeneration=generation;
            state.SavePending=0;
            state.SaveHigh=state.SaveLow=state.SaveSequence=0;
            state.SaveNotice=6;
            state.LastSaveState=(int)SaveOperationState.Succeeded;
            state.LastSaveError=0;
            state.LastSaveGeneration=generation;
        }
    }

    static void Result(ref TraversalState state,SaveOperationResult result)
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

    static string SaveStatus(in TraversalState state)=>state.SaveNotice switch
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

    static void PollSave(ref TraversalState state,GameContext context)
    {
        Restored(ref state,context.Saves);
        if(state.SavePending!=0)Result(ref state,context.GetSaveResult(Ticket(in state)));
        if(state.SaveStatusKey==state.SaveNotice)return;
        context.SetUi(SaveLabel,SaveStatus(in state));
        state.SaveStatusKey=state.SaveNotice;
    }

    static void PollSave(ref TraversalState state,ControlContext context)
    {
        Restored(ref state,context.Saves);
        if(state.SavePending!=0)Result(ref state,context.GetSaveResult(Ticket(in state)));
    }

    static double Separation(Vector3d a,Vector3d b)
    {
        double x=a.X-b.X,y=a.Y-b.Y,z=a.Z-b.Z;
        return Math.Sqrt(x*x+y*y+z*z);
    }

    static void UpdatePlayer(ref TraversalState state,GameContext context)
    {
        ++state.PlayerTicks;
        var player=context.Get(Player);
        Check(player.Motion==BodyMotion.Character,"Traversal player needs a native CharacterController.");
        var stance=context.GetCharacterStance(Player)
            ?? throw new InvalidOperationException("Traversal player has no native stance.");
        Check(stance.Configured,"Traversal player needs the frozen crouch profile.");
        bool crouch=context.GetInputAction(Player,"crouch").Held;
        if(stance.RequestedCrouch!=crouch)
        {
            context.SetCharacterStance(Player,crouch);
            ++state.StanceWrites;
        }

        var lenses=context.GetCameraLens(Camera)
            ?? throw new InvalidOperationException("Traversal camera has no native lens.");
        bool aim=context.GetInputAction(Player,"aim").Held;
        if(state.LensInitialized==0)
        {
            state.LensInitialized=1;
            state.AimStartFov=state.AimCurrentFov=lenses.World.VerticalFov;
            state.AimTargetFov=lenses.AuthoredWorld.VerticalFov;
            state.AimElapsed=(int)AimDurationTicks;
        }
        if(state.Aiming!=(aim ? 1 : 0))
        {
            // Retarget from the exact committed lens. Position is continuous;
            // the game deliberately makes no velocity-continuity promise.
            state.Aiming=aim ? 1 : 0;
            state.AimStartFov=lenses.World.VerticalFov;
            state.AimTargetFov=aim ? Math.Min(AimFov,lenses.AuthoredWorld.VerticalFov) : lenses.AuthoredWorld.VerticalFov;
            state.AimElapsed=0;
            ++state.AimTransitions;
        }
        if(state.AimElapsed<(int)AimDurationTicks)++state.AimElapsed;
        state.AimCurrentFov=CameraFovTransition.Evaluate(state.AimStartFov,state.AimTargetFov,
            checked((uint)state.AimElapsed),AimDurationTicks);
        if(lenses.World.VerticalFov!=state.AimCurrentFov)
            context.SetCameraLens(Camera,world:new(state.AimCurrentFov,lenses.World.Near,lenses.World.Far));

        bool use=context.Pressed(Player,GameAction.Use);
        var position=player.Transform.Position;
        var lift=context.Get(Lift);
        Check(lift.Motion==BodyMotion.Kinematic,"Traversal lift needs a kinematic native body.");
        var liftPosition=lift.Transform.Position;
        if(use && Distance(position.X-liftPosition.X,position.Z-liftPosition.Z)<=LiftUseRadius)
            state.LiftRequested=1;
        if(state.LiftRequested!=0)
        {
            state.LiftRequested=0;
            state.LiftBusyNotice=lift.MotionRemainingTicks!=0 ? 1 : 0;
            if(lift.MotionRemainingTicks==0)
            {
                state.LiftDestination=state.LiftDestination==0 ? 1 : 0;
                var endpoint=context.Get(state.LiftDestination==0 ? LiftLower : LiftUpper).Transform.Position;
                context.MoveKinematic(Lift,endpoint,System.Numerics.Quaternion.Identity,LiftTicks);
                ++state.LiftMoves;
            }
        }

        if(use && state.Won==0)
        {
            for(int index=0;index<3;++index)
            {
                int mask=1<<index;
                if((state.BeaconMask&mask)!=0)continue;
                EntityId beacon=index==0 ? FirstBeacon : index==1 ? LiftBeacon : LastBeacon;
                var target=context.Get(beacon).Transform.Position;
                if(Separation(position,target)>PickupRadius)continue;
                if(index==0 && (!stance.Crouched || stance.GroundState!=CharacterGroundState.OnGround ||
                    position.X<=-3.2 || position.X>=-2.2 || position.Z<=5 || position.Z>=9 || Math.Abs(position.Y-.6)>.03))continue;
                if(index==1 && (stance.GroundState!=CharacterGroundState.OnGround || position.Y<=1.2 ||
                    Math.Abs(position.X-liftPosition.X)>.65 || Math.Abs(position.Z-liftPosition.Z)>.65))continue;
                state.BeaconMask|=mask;
                ++state.Collected;
                if(index==0)state.CrouchedBeacon=1;
            }
        }
        if(state.Collected==3 && state.LiftMoves>0 && Separation(position,context.Get(Gate).Transform.Position)<=PickupRadius)
            state.Won=1;
    }

    public override void Tick(ref TraversalState state,GameContext context)
    {
        PollSave(ref state,context);
        if(state.Paused!=0)return;
        ++state.Ticks;
        UpdatePlayer(ref state,context);
        var config=context.Get<LocomotionVisualConfig>(state.Actor);
        var route=context.Get<LocomotionRoute>(state.Actor);
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
        Check(double.IsFinite(context.DeltaTime) && context.DeltaTime>0,"Locomotion requires a finite positive fixed tick interval.");
        state.LastHorizontalSpeed=state.LastDisplacement/context.DeltaTime;
        Check(double.IsFinite(state.LastHorizontalSpeed),"Committed controller displacement must remain finite.");
        state.RequestedRunRate=Math.Clamp(state.LastHorizontalSpeed/config.NominalRunSpeed,0,8);
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
                    Check(route.Coordinates.TryAdd(corners[i].X) && route.Coordinates.TryAdd(corners[i].Y) && route.Coordinates.TryAdd(corners[i].Z),"Locomotion route exceeded its persisted capacity.");
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
                // Slow the actual capsule before reaching a corner. Playback
                // observes this committed displacement on the following tick.
                float movement=Math.Abs(turn)>4 ? 0 : (float)(.75*Math.Min(1,Distance(dx,dz)/config.SlowdownDistance));
                context.SetCharacterInput(state.Actor,0,movement,(float)Math.Clamp(turn,-8,8));
            }
        }
        if(routeChanged)context.Set(state.Actor,in route);
        PublishStatus(ref state,context);
    }

    public override void Control(ref TraversalState state,ControlContext context)
    {
        PollSave(ref state,context);
        switch(context.Action)
        {
            case "lift":state.LiftRequested=1;break;
            case "dispatch":state.DispatchRequested=1;break; // Courier only; not a course shortcut.
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
            default:throw new InvalidOperationException("Unknown traversal-yard control action.");
        }
        context.SetUi(StatusLabel,Status(in state));
        state.StatusKey=StatusKey(in state);
        context.SetUi(SaveLabel,SaveStatus(in state));
        state.SaveStatusKey=state.SaveNotice;
    }
}
