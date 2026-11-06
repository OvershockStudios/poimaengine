// SPDX-License-Identifier: Apache-2.0
using Poima;

namespace Poima.Examples;

// Every mutable gameplay value is saved, reloadable and inspectable. Native
// character, animation, grounding and physical state remain native-owned.
public struct PhysicalCharacterState
{
    // Active: 0 welcome, 1 playing, 2 menu. TargetMode: 0 idle, 1 moving,
    // 2 physical. PhysicalMode is the last committed native observation.
    public int Active, TargetMoving, TargetMode, PhysicalMode, PatrolDirection;
    public int IdleClip, RunClip, BlendTicks, AnimationClip, AnimationChanges;
    public int GroundingEnabled, GroundingChanges, HasPreviousTarget;
    public double PreviousTargetX, PreviousTargetZ, HorizontalSpeed, AnimationRate;
    public long PreviousTargetTick, TickCount;
    public double Health, Damage, ShotImpulse, Range, PatrolExtent, RunInput;
    public int Shots, Hits, Misses, Takeovers, Cooldown, FireQueued, RefusedShots;
    // LastShotKind: 0 none, 1 miss/geometry, 2 nonlethal, 3 takeover,
    // 4 already physical. LastHitPart indexes the authored 15-part profile.
    public int LastShotKind, LastHitPart, LastPhysicalCommand;
    public EntityId LastHitEntity, LastHitRegion;
    public long LastShotTick;
    public double LastOriginX, LastOriginY, LastOriginZ;
    public double LastAimX, LastAimY, LastAimZ, LastHitX, LastHitY, LastHitZ;
    public double LastImpulseX, LastImpulseY, LastImpulseZ;
    // SavePurpose: 1 initial retry checkpoint, 2 checkpoint save,
    // 3 checkpoint load, 4 retry. Ticket words are opaque signed values.
    public int StartReady, SavePending, SavePurpose, SaveNotice, LastSaveState, LastSaveError;
    public long SaveEpochHigh, SaveEpochLow, SaveSequence, LastSaveGeneration, LastRestoreTick;
}

[GameModule("poima.example.physical-character")]
public sealed class PhysicalCharacterGame : Game<PhysicalCharacterState>,
    IInputActionsGame, ICharacterInputGame, IAnimationRateGame,
    IGroundingControlGame, IHitRegionsGame, IPhysicalRigGame
{
    public const string CheckpointSlot = "physical-character";
    public const string StartSlot = "physical-character-start";
    static EntityId E(ulong id) => new(0,id);
    static UiId U(ulong id) => new(0,id);

    public override void Initialize(ref PhysicalCharacterState s)
    {
        s=default;
        // prepare.py inspects the complete cooked library before accepting these
        // original Idle/Running_A indices; no clip remapping or replacement art.
        s.IdleClip=36; s.RunClip=48; s.BlendTicks=6;
        s.AnimationClip=-1; s.LastHitPart=-1; s.PatrolDirection=1;
        s.Health=100; s.Damage=100; s.ShotImpulse=18; s.Range=50;
        s.PatrolExtent=1.8; s.RunInput=.6; s.AnimationRate=1;
    }

    static void Require(bool condition,string message)
    {
        if(!condition)throw new InvalidOperationException(message);
    }

    static bool Within(double value,double minimum,double maximum) =>
        double.IsFinite(value) && value>=minimum && value<=maximum;

    static void Validate(in PhysicalCharacterState s)
    {
        Require(s.Active is >=0 and <=2 && s.TargetMoving is 0 or 1 &&
            s.PatrolDirection is -1 or 1,"Invalid saved character-yard mode.");
        Require(s.IdleClip is >=0 and <76 && s.RunClip is >=0 and <76 &&
            s.IdleClip!=s.RunClip && s.BlendTicks is >=0 and <=120,
            "Use distinct inspected clips and a blend of 0..120 ticks.");
        Require(Within(s.Health,0,1000) && Within(s.Damage,.01,1000) &&
            Within(s.ShotImpulse,0,250) && Within(s.Range,.1,100),
            "Health, damage, shot impulse or range exceeds the sample's finite bounds.");
        Require(Within(s.PatrolExtent,.25,6) && Within(s.RunInput,.05,1),
            "Patrol extent must be 0.25..6m and movement input 0.05..1.");
        Require(s.Cooldown is >=0 and <=6 && s.FireQueued is 0 or 1 &&
            s.SavePending is 0 or 1,"Invalid saved shot or save state.");
    }

    public static string PartName(int index) => index switch
    {
        0=>"pelvis", 1=>"chest", 2=>"head",
        3=>"upper_arm_l", 4=>"forearm_l", 5=>"hand_l",
        6=>"upper_arm_r", 7=>"forearm_r", 8=>"hand_r",
        9=>"thigh_l", 10=>"shin_l", 11=>"foot_l",
        12=>"thigh_r", 13=>"shin_r", 14=>"foot_r",
        _=>"none"
    };

    static int PartIndex(string? name)
    {
        for(int i=0;i<15;++i)if(name==PartName(i))return i;
        return -1;
    }

    static double Wrap(double degrees)
    {
        if(degrees>180)degrees-=360;
        if(degrees< -180)degrees+=360;
        return degrees;
    }

    static void ObserveMovement(ref PhysicalCharacterState s,GameContext c)
    {
        var position=c.Get(E(1)).Transform.Position;
        long tick=checked((long)c.Tick);
        double dx=position.X-s.PreviousTargetX,dz=position.Z-s.PreviousTargetZ;
        s.HorizontalSpeed=s.HasPreviousTarget!=0 && s.PreviousTargetTick+1==tick
            ? Math.Sqrt(dx*dx+dz*dz)/c.DeltaTime : 0;
        s.PreviousTargetX=position.X; s.PreviousTargetZ=position.Z;
        s.PreviousTargetTick=tick; s.HasPreviousTarget=1;
    }

    static void AnimateAndMove(ref PhysicalCharacterState s,GameContext c)
    {
        var actor=c.Get(E(1));
        Require(actor.Motion==BodyMotion.Character,"Target actor1 requires its linked character controller.");
        var animation=c.GetAnimation(E(100))
            ?? throw new InvalidOperationException("Target rig100 requires the imported animation library.");
        bool moving=s.TargetMoving!=0;
        int clip=moving ? s.RunClip : s.IdleClip;
        // A stopped target blends back to Idle before foot contact takes over;
        // the run uses the original gait without locking its feet to the floor.
        bool grounding=!moving && animation.Clip==s.IdleClip && animation.Transition is null;
        for(uint slot=1;slot<=2;++slot)
        {
            var requested=c.GetGroundingControl(E(100),slot)
                ?? throw new InvalidOperationException("Both authored automatic foot slots are required.");
            if(requested.Enabled!=grounding || requested.Weight!=1)
            {
                c.SetGroundingControl(new(E(100),slot,grounding,1));
                s.GroundingChanges=checked(s.GroundingChanges+1);
            }
        }
        s.GroundingEnabled=grounding ? 1 : 0;

        // The target owns only actor1's intent. Player2's native mapped input is
        // already applied by the runtime; restaging it would conflict or double look.
        if(moving)
        {
            if(actor.Transform.Position.X>=s.PatrolExtent)s.PatrolDirection=-1;
            else if(actor.Transform.Position.X<=-s.PatrolExtent)s.PatrolDirection=1;
            double desired=s.PatrolDirection>0 ? -90 : 90;
            var forward=actor.Transform.Forward;
            double yaw=Math.Atan2(-forward.X,-forward.Z)*180/Math.PI;
            double turn=Wrap(desired-yaw);
            c.SetCharacterInput(E(1),0,Math.Abs(turn)<12 ? (float)s.RunInput : 0,
                (float)Math.Clamp(turn,-8,8));
        }
        else c.SetCharacterInput(E(1),0,0);

        double rate=moving ? Math.Clamp(s.HorizontalSpeed/3,.25,2) : 1;
        bool replacement=animation.Clip!=clip || !animation.Loop || !animation.Playing;
        if(replacement)
        {
            c.SetAnimation(E(100),clip,AnimationTransitionMode.Inertial,
                time:animation.Clip==clip && animation.Time<animation.Duration ? animation.Time : 0,
                speed:rate,loop:true,playing:true,blendTicks:(uint)s.BlendTicks);
            s.AnimationChanges=checked(s.AnimationChanges+1);
        }
        else if(Math.Abs(animation.Speed-rate)>=.01)
        {
            c.SetAnimationSpeed(E(100),rate);
            s.AnimationChanges=checked(s.AnimationChanges+1);
        }
        else rate=animation.Speed;
        s.AnimationRate=rate; s.AnimationClip=clip; s.TargetMode=moving ? 1 : 0;
    }

    // Returns whether a physical command owns the target this tick. Call this
    // before animation, rate, grounding or target motor writes, never after them.
    static bool Fire(ref PhysicalCharacterState s,GameContext c,PhysicalRigMode mode)
    {
        var camera=c.Get(E(3)).Transform;
        var forward=camera.Forward;
        double length=Math.Sqrt(forward.X*forward.X+forward.Y*forward.Y+forward.Z*forward.Z);
        Require(double.IsFinite(length) && length>0,"Player camera must have a finite aiming direction.");
        var direction=new Vector3d(forward.X/length,forward.Y/length,forward.Z/length);
        s.LastOriginX=camera.Position.X; s.LastOriginY=camera.Position.Y; s.LastOriginZ=camera.Position.Z;
        s.LastAimX=direction.X; s.LastAimY=direction.Y; s.LastAimZ=direction.Z;
        s.Shots=checked(s.Shots+1); s.LastShotTick=checked((long)c.Tick); s.Cooldown=6;
        s.LastHitEntity=default; s.LastHitRegion=default; s.LastHitPart=-1;
        s.LastHitX=s.LastHitY=s.LastHitZ=0;
        s.LastImpulseX=s.LastImpulseY=s.LastImpulseZ=0; s.LastPhysicalCommand=0;
        var hit=c.HitTest(camera.Position,direction,s.Range,[E(2)]);
        if(hit is {} observed)
        {
            s.LastHitEntity=observed.Entity; s.LastHitRegion=observed.RegionEntity??default;
            s.LastHitX=observed.Position.X; s.LastHitY=observed.Position.Y; s.LastHitZ=observed.Position.Z;
        }
        int index=hit is {} candidate && candidate.Entity==E(1) ? PartIndex(candidate.Region) : -1;
        if(index<0)
        {
            s.Misses=checked(s.Misses+1); s.LastShotKind=1;
            return false;
        }
        var impact=hit!.Value;
        // Membership comes from the exact returned region name, then native
        // part lookup. Neither target position nor a canned body is the aim ray.
        Require(impact.RegionEntity==E((ulong)(2000+index)),"Hit-region identity does not match the authored physical part.");
        var part=c.GetPhysicalRigPart(E(100),PartName(index))
            ?? throw new InvalidOperationException("A hit region names a missing physical part.");
        Require(part.Entity==E(100),"Physical part belongs to a different rig.");
        s.LastHitPart=index; s.Hits=checked(s.Hits+1);
        s.Health=Math.Max(0,s.Health-s.Damage);
        bool activate=mode==PhysicalRigMode.Animated && s.Health<=0;
        if(!activate && mode!=PhysicalRigMode.Passive)
        {
            s.LastShotKind=2;
            return false;
        }
        var impulse=new Vector3d(direction.X*s.ShotImpulse,direction.Y*s.ShotImpulse,direction.Z*s.ShotImpulse);
        if(s.ShotImpulse>0)
        {
            c.ImpulsePhysicalRig(E(100),PartName(index),impact.Position,impulse,activate);
            s.LastPhysicalCommand=activate ? 3 : 2; // activation+impulse / impulse
        }
        else if(activate)
        {
            c.ActivatePhysicalRig(E(100));
            s.LastPhysicalCommand=1; // zero-impulse authored takeover
        }
        s.LastImpulseX=impulse.X; s.LastImpulseY=impulse.Y; s.LastImpulseZ=impulse.Z;
        s.LastShotKind=activate ? 3 : 4;
        if(activate)s.Takeovers=checked(s.Takeovers+1);
        s.TargetMoving=0; s.TargetMode=2;
        return true;
    }

    static SaveTicket Ticket(in PhysicalCharacterState s) => new(s.SaveEpochHigh,s.SaveEpochLow,s.SaveSequence);
    static void ClearTicket(ref PhysicalCharacterState s)
    {
        s.SavePending=0; s.SaveEpochHigh=0; s.SaveEpochLow=0; s.SaveSequence=0;
    }
    static void Remember(ref PhysicalCharacterState s,SaveRequestResult request,int purpose)
    {
        if(!request.Accepted)
        {
            s.SaveNotice=request.Rejection==SaveRequestRejection.Disabled ? 3 : 4;
            return;
        }
        s.SaveEpochHigh=request.Ticket.EpochHigh; s.SaveEpochLow=request.Ticket.EpochLow;
        s.SaveSequence=request.Ticket.Sequence; s.SavePending=1; s.SavePurpose=purpose;
        s.SaveNotice=purpose is 1 or 2 ? 1 : 2; s.LastSaveError=0;
        s.LastSaveState=(int)SaveOperationState.Queued;
    }
    static bool RestoredPending(ref PhysicalCharacterState s,SaveCapabilities saves)
    {
        if(s.SavePending==0)return true;
        if(s.SaveEpochHigh==saves.Epoch.High && s.SaveEpochLow==saves.Epoch.Low)return false;
        if(saves.LastRestore is {} restored && restored.DestinationEpoch==saves.Epoch)
        {
            // A restored start snapshot can contain its own then-pending save
            // ticket. Its destination epoch proves that the archive was stored.
            if(s.SavePurpose==1)s.StartReady=1;
            s.LastSaveGeneration=checked((long)restored.Generation);
            s.LastRestoreTick=checked((long)restored.RestoredTick);
            s.LastSaveState=(int)SaveOperationState.Succeeded; s.LastSaveError=0; s.SaveNotice=6;
        }
        else { s.LastSaveState=(int)SaveOperationState.Expired; s.SaveNotice=8; }
        ClearTicket(ref s);
        return true;
    }
    static void SetResult(ref PhysicalCharacterState s,SaveOperationResult result)
    {
        s.LastSaveState=(int)result.State; s.LastSaveError=result.ErrorCode;
        s.LastSaveGeneration=checked((long)result.Generation);
        if(!result.IsTerminal)return;
        if(result.State==SaveOperationState.Succeeded)
        {
            if(s.SavePurpose==1)s.StartReady=1;
            s.SaveNotice=result.Kind==SaveKind.Load ? 6 : 5;
        }
        else s.SaveNotice=result.State==SaveOperationState.Expired ? 8 : 7;
        ClearTicket(ref s);
    }
    static void PollSave(ref PhysicalCharacterState s,GameContext c)
    {
        if(!RestoredPending(ref s,c.Saves))SetResult(ref s,c.GetSaveResult(Ticket(in s)));
    }
    static void PollSave(ref PhysicalCharacterState s,ControlContext c)
    {
        if(!RestoredPending(ref s,c.Saves))SetResult(ref s,c.GetSaveResult(Ticket(in s)));
    }

    static string Title(in PhysicalCharacterState s) => s.Active switch
    {
        0=>"PHYSICAL CHARACTER YARD / BEGIN",
        2=>"PHYSICAL CHARACTER YARD / MENU",
        _=>"PHYSICAL CHARACTER YARD / LIVE"
    };
    static string Stats(in PhysicalCharacterState s) =>
        $"{(s.TargetMode==2 ? "PHYSICAL" : s.TargetMode==1 ? "RUNNING" : "IDLE")}  /  HEALTH {s.Health:0}\n"+
        $"SHOTS {s.Shots}  HITS {s.Hits}  MISSES {s.Misses}  /  LAST PART {PartName(s.LastHitPart)}";
    static string Notice(in PhysicalCharacterState s) => s.SaveNotice switch
    {
        1=>"Saving checkpoint...", 2=>"Loading checkpoint...",
        3=>"Checkpoint storage is unavailable. Configure saves to enable Save and Retry.",
        4=>"Checkpoint request unavailable; wait for the current operation.",
        5=>s.SavePurpose==1 ? "Starting checkpoint ready. Move, aim and fire; Retry restores this start." : "Checkpoint saved.",
        6=>"Checkpoint restored. Loaded checkpoints open paused; Resume continues.",
        7=>$"Checkpoint operation failed ({s.LastSaveError}). Your live session remains available.",
        8=>"Checkpoint ticket expired. No successful save is assumed.",
        _=>"Move and look with the bound controls. Fire at the character. Toggle movement to run or stop."
    };
    static void Publish(in PhysicalCharacterState s,GameContext c)
    {
        c.SetUi(U(501),Title(in s)); c.SetUi(U(502),Stats(in s)); c.SetUi(U(509),Notice(in s));
        c.SetUi(U(503),visible:s.Active==0); c.SetUi(U(504),visible:s.Active==1);
        c.SetUi(U(505),visible:s.Active!=0,enabled:s.SavePending==0 && c.Saves.Enabled);
        c.SetUi(U(506),visible:true,enabled:s.SavePending==0 && c.Saves.Enabled);
        // A loaded live checkpoint opens in a paused native player. Its saved
        // Active value is deliberately retained, so Resume also stays available
        // for Active=1 and needs no unsaved host-pause flag in gameplay.
        c.SetUi(U(507),visible:s.Active!=0);
        c.SetUi(U(508),visible:s.Active!=0,enabled:s.StartReady!=0 && s.SavePending==0);
        c.SetUi(U(510),s.TargetMoving!=0 ? "Stop target" : "Move target",visible:s.Active!=0,
            enabled:s.Active==1 && s.TargetMode!=2);
    }
    static void Publish(in PhysicalCharacterState s,ControlContext c)
    {
        c.SetUi(U(501),Title(in s)); c.SetUi(U(502),Stats(in s)); c.SetUi(U(509),Notice(in s));
        c.SetUi(U(503),visible:s.Active==0); c.SetUi(U(504),visible:s.Active==1);
        c.SetUi(U(505),visible:s.Active!=0,enabled:s.SavePending==0 && c.Saves.Enabled);
        c.SetUi(U(506),visible:true,enabled:s.SavePending==0 && c.Saves.Enabled);
        c.SetUi(U(507),visible:s.Active!=0);
        c.SetUi(U(508),visible:s.Active!=0,enabled:s.StartReady!=0 && s.SavePending==0);
        c.SetUi(U(510),s.TargetMoving!=0 ? "Stop target" : "Move target",visible:s.Active!=0,
            enabled:s.Active==1 && s.TargetMode!=2);
    }

    public override void Tick(ref PhysicalCharacterState s,GameContext c)
    {
        Validate(in s); PollSave(ref s,c);
        var physical=c.GetPhysicalRig(E(100))
            ?? throw new InvalidOperationException("The original character requires its authored physical rig.");
        Require(physical.BodyCount==15 && physical.JointCount==14,"Expected the complete 15-part character profile.");
        s.PhysicalMode=(int)physical.Mode;
        if(physical.Mode==PhysicalRigMode.Passive){ s.TargetMoving=0; s.TargetMode=2; }
        if(s.Active!=1)
        {
            // Other hosts can drive a first Tick before presenting the menu.
            // Keep native pointer/gamepad routing inside that menu as well.
            c.SetModal(U(500));
            Publish(in s,c);
            return;
        }
        s.TickCount=checked(s.TickCount+1); ObserveMovement(ref s,c);
        if(s.Cooldown>0)--s.Cooldown;
        bool fire=c.GetInputAction(E(2),"fire").Pressed || s.FireQueued!=0;
        s.FireQueued=0;
        // Initial controller takeover needs one committed locomotion/grounding
        // interval. Keep an early trigger edge, then query its real camera on the
        // next tick rather than inventing a hit or a prepared history at birth.
        if(fire && c.Tick==0){ s.FireQueued=1; fire=false; }
        bool physicalWrite=false;
        if(fire)
        {
            if(s.Cooldown==0)physicalWrite=Fire(ref s,c,physical.Mode);
            else s.RefusedShots=checked(s.RefusedShots+1);
        }
        if(physical.Mode==PhysicalRigMode.Animated && !physicalWrite)AnimateAndMove(ref s,c);
        Publish(in s,c);
    }

    public override void Control(ref PhysicalCharacterState s,ControlContext c)
    {
        Validate(in s); PollSave(ref s,c);
        switch(c.Action)
        {
            case "begin":
                if(s.Active==0)
                {
                    s.Active=1;
                    c.SetModal(null);
                    Remember(ref s,c.TryRequestSave(StartSlot),1);
                    c.RequestResume();
                }
                break;
            case "menu":
                if(s.Active==1){ s.Active=2; c.SetModal(U(500)); c.RequestPause(); }
                break;
            case "resume":
                if(s.Active!=0){ s.Active=1; c.SetModal(null); c.RequestResume(); }
                break;
            case "target-motion":
                if(s.Active==1 && s.TargetMode!=2)s.TargetMoving=1-s.TargetMoving;
                break;
            case "save":
                if(s.SavePending==0)Remember(ref s,c.TryRequestSave(CheckpointSlot),2);
                break;
            case "load":
                if(s.SavePending==0)Remember(ref s,c.TryRequestLoad(CheckpointSlot),3);
                break;
            case "retry":
                if(s.StartReady!=0 && s.SavePending==0)
                    Remember(ref s,c.TryRequestLoad(StartSlot),4);
                break;
            default:throw new InvalidOperationException("Unknown physical-character UI action.");
        }
        Publish(in s,c);
    }
}
