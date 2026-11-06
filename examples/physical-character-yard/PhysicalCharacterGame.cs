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
    // Requested controls are distinct from last committed native observations.
    // Applied* caches prevent repeating unchanged preset writes every Tick.
    public int HoldRequested, AppliedHold, PosePreset, AppliedPreset, SelectedJoint;
    public int DrivePending, DriveWrites, DriveChanges, ObservedDriveCount, ObservedJoint;
    public long LastDriveTick;
    public double ObservedSelectedStrength;
    public double StrengthSpine, StrengthNeck, StrengthShoulderL, StrengthElbowL, StrengthWristL;
    public double StrengthShoulderR, StrengthElbowR, StrengthWristR;
    public double StrengthHipL, StrengthKneeL, StrengthAnkleL, StrengthHipR, StrengthKneeR, StrengthAnkleR;
}

[GameModule("poima.example.physical-character")]
public sealed class PhysicalCharacterGame : Game<PhysicalCharacterState>,
    IInputActionsGame, ICharacterInputGame, IAnimationRateGame,
    IGroundingControlGame, IHitRegionsGame, IPhysicalRigDriveGame
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
        s.AppliedPreset=-1; s.SelectedJoint=2; s.ObservedJoint=-1; s.LastDriveTick=-1;
        s.StrengthSpine=s.StrengthNeck=s.StrengthShoulderL=s.StrengthElbowL=s.StrengthWristL=.65;
        s.StrengthShoulderR=s.StrengthElbowR=s.StrengthWristR=.65;
        s.StrengthHipL=s.StrengthKneeL=s.StrengthAnkleL=s.StrengthHipR=s.StrengthKneeR=s.StrengthAnkleR=.65;
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
        Require(s.HoldRequested is 0 or 1 && s.AppliedHold is 0 or 1 &&
            s.PosePreset is >=0 and <2 && s.AppliedPreset is >=-1 and <2 &&
            s.SelectedJoint is >=0 and <14 && s.ObservedJoint is >=-1 and <14 &&
            s.DrivePending is 0 or 1 && s.DriveWrites>=0 && s.DriveChanges>=0 &&
            s.ObservedDriveCount is >=0 and <=14 && Within(s.ObservedSelectedStrength,0,1),
            "Invalid saved joint-drive mode, preset, selection or observation.");
        for(int joint=0;joint<14;++joint)
            Require(Within(JointStrength(in s,joint),0,1),"Every requested joint strength must be finite in [0,1].");
    }

    public static string JointName(int index) => index switch
    {
        0=>"spine", 1=>"neck", 2=>"shoulder_l", 3=>"elbow_l", 4=>"wrist_l",
        5=>"shoulder_r", 6=>"elbow_r", 7=>"wrist_r", 8=>"hip_l", 9=>"knee_l",
        10=>"ankle_l", 11=>"hip_r", 12=>"knee_r", 13=>"ankle_r",
        _=>throw new ArgumentOutOfRangeException(nameof(index))
    };

    public static string PresetName(int preset) => preset switch
    {
        0=>"Relaxed", 1=>"Guard",
        _=>throw new ArgumentOutOfRangeException(nameof(preset))
    };

    // Exact authored joint-frame targets from drive-poses.json. The protocol
    // verifier independently joins every requested native target to that data.
    // These are pose holds, not generated gait, balance or recovery animation.
    public static Quaterniond PresetTarget(int preset,int joint) => (preset,joint) switch
    {
        (0,0)=>new(0.0, 0.0, 0.0, 1.0), // Relaxed: spine
        (0,1)=>new(-0.02999550020249566, 0.0, 0.0, 0.9995500337489875), // Relaxed: neck
        (0,2)=>new(0.2793298192436579, 0.16080067394807174, -0.3047319312422322, 0.8962457505732069), // Relaxed: shoulder_l
        (0,3)=>new(0.43496553411123023, 0.0, 0.0, 0.9004471023526769), // Relaxed: elbow_l
        (0,4)=>new(-0.07492970727274234, 0.0, 0.0, 0.9971888181122075), // Relaxed: wrist_l
        (0,5)=>new(-0.20856835722953126, -0.1733770535798174, -0.33248757484195585, 0.9032672086543776), // Relaxed: shoulder_r
        (0,6)=>new(0.45733844717895544, 0.0, 0.0, 0.8892927216231682), // Relaxed: elbow_r
        (0,7)=>new(0.07492970727274234, 0.0, 0.0, 0.9971888181122075), // Relaxed: wrist_r
        (0,8)=>new(0.07494708133507871, 0.009206835314530976, 0.08215474385573462, 0.9937548828784909), // Relaxed: hip_l
        (0,9)=>new(0.08987854919801104, 0.0, 0.0, 0.9959527330119943), // Relaxed: knee_l
        (0,10)=>new(0.0, 0.0, 0.0, 1.0), // Relaxed: ankle_l
        (0,11)=>new(0.031012770338822077, -0.042729821769199164, -0.02853349763418478, 0.9981974804217071), // Relaxed: hip_r
        (0,12)=>new(0.16918234906699603, 0.0, 0.0, 0.9855847669095608), // Relaxed: knee_r
        (0,13)=>new(0.0, 0.0, 0.0, 1.0), // Relaxed: ankle_r
        (1,0)=>new(0.0, 0.0, 0.0, 1.0), // Guard: spine
        (1,1)=>new(-0.02999550020249566, 0.0, 0.0, 0.9995500337489875), // Guard: neck
        (1,2)=>new(-0.45176623617667655, -0.0765235859264746, -0.32588478229752826, 0.826952548405654), // Guard: shoulder_l
        (1,3)=>new(0.5646424733950354, 0.0, 0.0, 0.8253356149096783), // Guard: elbow_l
        (1,4)=>new(-0.07492970727274234, 0.0, 0.0, 0.9971888181122075), // Guard: wrist_l
        (1,5)=>new(0.45176622975642106, 0.07652392122413802, -0.3258847422856243, 0.8269525366534543), // Guard: shoulder_r
        (1,6)=>new(0.5646424733950354, 0.0, 0.0, 0.8253356149096783), // Guard: elbow_r
        (1,7)=>new(0.07492970727274234, 0.0, 0.0, 0.9971888181122075), // Guard: wrist_r
        (1,8)=>new(0.07494708133507871, 0.009206835314530976, 0.08215474385573462, 0.9937548828784909), // Guard: hip_l
        (1,9)=>new(0.08987854919801104, 0.0, 0.0, 0.9959527330119943), // Guard: knee_l
        (1,10)=>new(0.0, 0.0, 0.0, 1.0), // Guard: ankle_l
        (1,11)=>new(0.031012770338822077, -0.042729821769199164, -0.02853349763418478, 0.9981974804217071), // Guard: hip_r
        (1,12)=>new(0.16918234906699603, 0.0, 0.0, 0.9855847669095608), // Guard: knee_r
        (1,13)=>new(0.0, 0.0, 0.0, 1.0), // Guard: ankle_r
        _=>throw new ArgumentOutOfRangeException("preset/joint")
    };

    public static double JointStrength(in PhysicalCharacterState s,int joint) => joint switch
    {
        0=>s.StrengthSpine, 1=>s.StrengthNeck, 2=>s.StrengthShoulderL,
        3=>s.StrengthElbowL, 4=>s.StrengthWristL, 5=>s.StrengthShoulderR,
        6=>s.StrengthElbowR, 7=>s.StrengthWristR, 8=>s.StrengthHipL,
        9=>s.StrengthKneeL, 10=>s.StrengthAnkleL, 11=>s.StrengthHipR,
        12=>s.StrengthKneeR, 13=>s.StrengthAnkleR,
        _=>throw new ArgumentOutOfRangeException(nameof(joint))
    };

    static void SetJointStrength(ref PhysicalCharacterState s,int joint,double strength)
    {
        Require(Within(strength,0,1),"Requested joint strength must be finite in [0,1].");
        switch(joint)
        {
            case 0:s.StrengthSpine=strength;break;
            case 1:s.StrengthNeck=strength;break;
            case 2:s.StrengthShoulderL=strength;break;
            case 3:s.StrengthElbowL=strength;break;
            case 4:s.StrengthWristL=strength;break;
            case 5:s.StrengthShoulderR=strength;break;
            case 6:s.StrengthElbowR=strength;break;
            case 7:s.StrengthWristR=strength;break;
            case 8:s.StrengthHipL=strength;break;
            case 9:s.StrengthKneeL=strength;break;
            case 10:s.StrengthAnkleL=strength;break;
            case 11:s.StrengthHipR=strength;break;
            case 12:s.StrengthKneeR=strength;break;
            case 13:s.StrengthAnkleR=strength;break;
            default:throw new ArgumentOutOfRangeException(nameof(joint));
        }
    }

    static void ObserveDrives(ref PhysicalCharacterState s,GameContext c)
    {
        s.ObservedDriveCount=0; s.ObservedJoint=s.SelectedJoint;
        for(int joint=0;joint<14;++joint)
        {
            var value=c.GetPhysicalRigDrive(E(100),JointName(joint))
                ?? throw new InvalidOperationException("Every sample joint requires its authored drive profile.");
            if(value.Strength>0)++s.ObservedDriveCount;
            if(joint==s.ObservedJoint)s.ObservedSelectedStrength=value.Strength;
        }
    }

    // Fire runs first. Its explicit activation, if any, therefore precedes the
    // drive commands and is never duplicated. No animation/motor writes follow.
    static bool ApplyDrives(ref PhysicalCharacterState s,GameContext c,PhysicalRigMode mode,bool physicalWrite)
    {
        bool physical=mode!=PhysicalRigMode.Animated || physicalWrite;
        if(!physical && s.HoldRequested==0)
        {
            s.AppliedHold=0; s.DrivePending=0;
            return false;
        }
        if(!physical && c.Tick==0)
        {
            // A first completed locomotion/grounding interval is required for
            // real controller takeover; keep the ordinary request queued.
            s.DrivePending=1;
            return false;
        }
        if(!physical)
        {
            c.ActivatePhysicalRig(E(100));
            s.Takeovers=checked(s.Takeovers+1); physicalWrite=true;
        }
        bool presetChanged=s.HoldRequested!=0 && (s.AppliedHold==0 || s.AppliedPreset!=s.PosePreset);
        int writes=0;
        for(int joint=0;joint<14;++joint)
        {
            var current=c.GetPhysicalRigDrive(E(100),JointName(joint))
                ?? throw new InvalidOperationException("Sample drive permission is missing.");
            double desired=s.HoldRequested!=0 ? JointStrength(in s,joint) : 0;
            if(!presetChanged && current.Strength==desired)continue;
            var target=s.HoldRequested!=0 ? PresetTarget(s.PosePreset,joint) : current.TargetRotation;
            c.SetPhysicalRigDrive(E(100),JointName(joint),target,desired);
            ++writes;
        }
        if(writes!=0)
        {
            s.DriveWrites=checked(s.DriveWrites+writes); s.DriveChanges=checked(s.DriveChanges+1);
            s.LastDriveTick=checked((long)c.Tick); physicalWrite=true;
        }
        if(s.HoldRequested!=0)s.AppliedPreset=s.PosePreset;
        s.AppliedHold=s.HoldRequested; s.DrivePending=0;
        s.TargetMoving=0; s.TargetMode=2;
        return physicalWrite;
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
        if(!activate && mode==PhysicalRigMode.Animated)
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
        $"{(s.PhysicalMode==2 ? "DRIVEN" : s.TargetMode==2 ? "PHYSICAL" : s.TargetMode==1 ? "RUNNING" : "IDLE")}  /  HEALTH {s.Health:0}\n"+
        $"SHOTS {s.Shots}  HITS {s.Hits}  MISSES {s.Misses}  /  LAST PART {PartName(s.LastHitPart)}";
    static string DriveStatus(in PhysicalCharacterState s)
    {
        bool pending=s.DrivePending!=0 || s.HoldRequested!=s.AppliedHold ||
            (s.HoldRequested!=0 && s.PosePreset!=s.AppliedPreset);
        string phase=s.PhysicalMode switch {0=>"Animated",1=>"Passive",2=>"Driven",_=>"Unknown"};
        string observed=s.ObservedJoint==s.SelectedJoint
            ? $"{s.ObservedSelectedStrength:P0}" : "awaiting Tick";
        return $"{(pending ? "QUEUED / next playing Tick" : s.HoldRequested!=0 ? "HOLD requested" : "RELEASE requested")}  |  "+
            $"observed {phase}, {s.ObservedDriveCount}/14 powered  |  selected observed {observed}. Gravity still applies.";
    }
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
        c.SetUi(U(514),visible:s.Active!=0,enabled:s.HoldRequested==0);
        c.SetUi(U(515),visible:s.Active!=0,enabled:s.HoldRequested!=0 || s.AppliedHold!=0 || s.ObservedDriveCount>0);
        c.SetUi(U(516),"Pose: "+PresetName(s.PosePreset),visible:s.Active!=0);
        c.SetUi(U(517),"Joint: "+JointName(s.SelectedJoint),visible:s.Active!=0);
        c.SetUi(U(518),$"Request: {JointStrength(in s,s.SelectedJoint):P0}",visible:s.Active!=0);
        c.SetUi(U(519),DriveStatus(in s),visible:s.Active!=0);
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
        c.SetUi(U(514),visible:s.Active!=0,enabled:s.HoldRequested==0);
        c.SetUi(U(515),visible:s.Active!=0,enabled:s.HoldRequested!=0 || s.AppliedHold!=0 || s.ObservedDriveCount>0);
        c.SetUi(U(516),"Pose: "+PresetName(s.PosePreset),visible:s.Active!=0);
        c.SetUi(U(517),"Joint: "+JointName(s.SelectedJoint),visible:s.Active!=0);
        c.SetUi(U(518),$"Request: {JointStrength(in s,s.SelectedJoint):P0}",visible:s.Active!=0);
        c.SetUi(U(519),DriveStatus(in s),visible:s.Active!=0);
    }

    public override void Tick(ref PhysicalCharacterState s,GameContext c)
    {
        Validate(in s); PollSave(ref s,c);
        var physical=c.GetPhysicalRig(E(100))
            ?? throw new InvalidOperationException("The original character requires its authored physical rig.");
        Require(physical.BodyCount==15 && physical.JointCount==14,"Expected the complete 15-part character profile.");
        s.PhysicalMode=(int)physical.Mode;
        ObserveDrives(ref s,c);
        if(physical.Mode!=PhysicalRigMode.Animated){ s.TargetMoving=0; s.TargetMode=2; }
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
        physicalWrite=ApplyDrives(ref s,c,physical.Mode,physicalWrite) || physicalWrite;
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
            case "pose-hold":
                if(s.Active!=0 && s.HoldRequested==0){ s.HoldRequested=1; s.DrivePending=1; }
                break;
            case "pose-release":
                if(s.Active!=0){ s.HoldRequested=0; s.DrivePending=1; }
                break;
            case "pose-preset":
                if(s.Active!=0)
                {
                    s.PosePreset=(s.PosePreset+1)%2;
                    if(s.HoldRequested!=0)s.DrivePending=1;
                }
                break;
            case "pose-joint":
                if(s.Active!=0)s.SelectedJoint=(s.SelectedJoint+1)%14;
                break;
            case "pose-strength":
                if(s.Active!=0)
                {
                    // These are deliberate UI steps. Direct typed fields retain
                    // continuous strength; no runtime clamp or rounding occurs.
                    double value=JointStrength(in s,s.SelectedJoint);
                    SetJointStrength(ref s,s.SelectedJoint,value<.25 ? .25 : value<.5 ? .5 : value<.75 ? .75 : value<1 ? 1 : 0);
                    if(s.HoldRequested!=0)s.DrivePending=1;
                }
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
