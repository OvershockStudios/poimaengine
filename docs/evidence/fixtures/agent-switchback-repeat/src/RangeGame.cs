using Poima;
namespace Switchback;
[GameplayPersistence(1)]
public struct RangeState
{
    [GameplayField("00000000000000000000000000007001", Default="0")] public int Phase;
    [GameplayField("00000000000000000000000000007002", Default="0")] public int Supply;
    [GameplayField("00000000000000000000000000007003", Default="0")] public int ClearedMask;
    [GameplayField("00000000000000000000000000007004", Default="0")] public int Ammo;
    [GameplayField("00000000000000000000000000007005", Default="0")] public int Shots;
    [GameplayField("00000000000000000000000000007006", Default="0")] public int Rejected;
    [GameplayField("00000000000000000000000000007007", Default="3")] public int Health;
    [GameplayField("00000000000000000000000000007008", Default="0")] public int Timer;
    [GameplayField("00000000000000000000000000007009", Default="0")] public int MenuOpen;
    [GameplayField("0000000000000000000000000000700a", Default="0")] public int ReloadRemaining;
    [GameplayField("0000000000000000000000000000700b", Default="0")] public int AimTarget;
    [GameplayField("0000000000000000000000000000700c", Default="12")] public int AdsElapsed;
    [GameplayField("0000000000000000000000000000700d", Default="70")] public double WorldStart;
    [GameplayField("0000000000000000000000000000700e", Default="70")] public double WorldTarget;
    [GameplayField("0000000000000000000000000000700f", Default="75")] public double ForegroundStart;
    [GameplayField("00000000000000000000000000007010", Default="75")] public double ForegroundTarget;
    [GameplayField("00000000000000000000000000007011", Default="70")] public double WorldFov;
    [GameplayField("00000000000000000000000000007012", Default="75")] public double ForegroundFov;
    [GameplayField("00000000000000000000000000007013", Default="0")] public int HazardInside;
    [GameplayField("00000000000000000000000000007014", Default="1")] public int PatrolDirection;
    [GameplayField("00000000000000000000000000007015", Default="0")] public int AnimationClip;
    [GameplayField("00000000000000000000000000007016", Default="3")] public double PreviousBX;
    [GameplayField("00000000000000000000000000007017", Default="-6")] public double PreviousBZ;
    [GameplayField("00000000000000000000000000007018", Default="0")] public double AnimationTime;
    [GameplayField("00000000000000000000000000007019", Default="0")] public long TicketHigh;
    [GameplayField("0000000000000000000000000000701a", Default="0")] public long TicketLow;
    [GameplayField("0000000000000000000000000000701b", Default="0")] public long TicketSequence;
    [GameplayField("0000000000000000000000000000701c", Default="0")] public long SaveGeneration;
    [GameplayField("0000000000000000000000000000701d", Default="0")] public int SaveState;
    [GameplayField("0000000000000000000000000000701e", Default="0")] public int SaveKind;
}
[GameModule("switchback.range.original.v1")]
public sealed class RangeGame : Game<RangeState>, ICharacterInputGame, IInputActionsGame, IHitRegionsGame, ICameraLensGame, ICharacterStanceGame
{
    private static EntityId E(ulong n)=>new(0,n);
    private static UiId U(ulong n)=>new(0,1000+n);
    public override void Initialize(ref RangeState s)
    {
        s=default; s.Health=3;s.PatrolDirection=1;s.PreviousBX=3;s.PreviousBZ=-6;
        s.WorldFov=s.WorldStart=s.WorldTarget=70;
        s.ForegroundFov=s.ForegroundStart=s.ForegroundTarget=75;
        s.AdsElapsed=12;
    }
    private static bool Inside(Vector3d p,double x,double z,double r)=>Math.Abs(p.Y)<=.05 && (p.X-x)*(p.X-x)+(p.Z-z)*(p.Z-z)<=r*r;
    private static SaveTicket Ticket(in RangeState s)=>new(s.TicketHigh,s.TicketLow,s.TicketSequence);
    private static void Accept(ref RangeState s,SaveRequestResult r,int kind)
    {
        if(!r.Accepted){s.SaveState=4;return;}
        s.TicketHigh=r.Ticket.EpochHigh;s.TicketLow=r.Ticket.EpochLow;s.TicketSequence=r.Ticket.Sequence;s.SaveState=1;s.SaveKind=kind;
    }
    private static void Reconcile(ref RangeState s,GameContext c)
    {
        if(s.SaveState is not (1 or 2))return;
        var r=c.GetSaveResult(Ticket(s));
        if(r.State==SaveOperationState.Expired && c.Saves.LastRestore is {} restored)
        {s.SaveState=3;s.SaveGeneration=(long)restored.Generation;}
        else {s.SaveState=(int)r.State;if(r.State==SaveOperationState.Succeeded)s.SaveGeneration=(long)r.Generation;}
    }
    private static void Reconcile(ref RangeState s,ControlContext c)
    {
        if(s.SaveState is not (1 or 2))return;
        var r=c.GetSaveResult(Ticket(s));
        if(r.State==SaveOperationState.Expired && c.Saves.LastRestore is {} restored)
        {s.SaveState=3;s.SaveGeneration=(long)restored.Generation;}
        else {s.SaveState=(int)r.State;if(r.State==SaveOperationState.Succeeded)s.SaveGeneration=(long)r.Generation;}
    }
    private static string SaveText(in RangeState s)=>s.SaveState switch {1=>"Checkpoint: queued",2=>"Checkpoint: resolving",3=>$"Checkpoint: completed / generation {s.SaveGeneration}",4=>"Checkpoint: failed",_=>"Checkpoint: no completed save"};
    private static string Hud(in RangeState s,string stance)=>$"Ammo {s.Ammo}/4 | Targets {((s.ClearedMask&1)!=0?1:0)+((s.ClearedMask&2)!=0?1:0)}/2 | Health {s.Health} | Reload {s.ReloadRemaining}\nSupply {(s.Supply==1?"collected":"needed")} | {stance} | Time {s.Timer/60.0:F1}s";
    private static void Panels(in RangeState s,GameContext c)
    {
        c.SetUi(U(0),visible:s.Phase==0);c.SetUi(U(5),visible:s.Phase==1 && s.MenuOpen==0);
        c.SetUi(U(8),visible:s.Phase==1 && s.MenuOpen==1);c.SetUi(U(13),visible:s.Phase==2);c.SetUi(U(17),visible:s.Phase==3);
        c.SetUi(U(20),visible:s.Phase==1 && s.MenuOpen==0);c.SetUi(U(19),SaveText(s));
        c.SetModal(s.Phase==0?U(0):s.Phase==2?U(13):s.Phase==3?U(17):s.MenuOpen==1?U(8):null);
    }
    private static void Panels(in RangeState s,ControlContext c)
    {
        c.SetUi(U(0),visible:s.Phase==0);c.SetUi(U(5),visible:s.Phase==1 && s.MenuOpen==0);
        c.SetUi(U(8),visible:s.Phase==1 && s.MenuOpen==1);c.SetUi(U(13),visible:s.Phase==2);c.SetUi(U(17),visible:s.Phase==3);
        c.SetUi(U(20),visible:s.Phase==1 && s.MenuOpen==0);c.SetUi(U(19),SaveText(s));
        c.SetModal(s.Phase==0?U(0):s.Phase==2?U(13):s.Phase==3?U(17):s.MenuOpen==1?U(8):null);
    }
    public override void Control(ref RangeState s,ControlContext c)
    {
        Reconcile(ref s,c);
        switch(c.Action)
        {
            case "begin" when s.Phase==0:
                s.Phase=1;s.MenuOpen=0;c.RequestResume();Accept(ref s,c.TryRequestSave("switchback-checkpoint"),1);break;
            case "menu" when s.Phase==1 && s.MenuOpen==0:s.MenuOpen=1;c.RequestPause();break;
            case "resume" when s.Phase==1 && s.MenuOpen==1:s.MenuOpen=0;c.RequestResume();break;
            case "save" when s.Phase==1 && s.MenuOpen==1:
                if(s.SaveState is not (1 or 2))Accept(ref s,c.TryRequestSave("switchback-checkpoint"),1);break;
            case "load" when s.Phase!=3:
                if(s.SaveState is not (1 or 2))Accept(ref s,c.TryRequestLoad("switchback-checkpoint"),2);break;
        }
        Panels(s,c);
    }
    private static void Ads(ref RangeState s,GameContext c,bool aim)
    {
        int target=aim?1:0;
        if(target!=s.AimTarget)
        {
            s.AimTarget=target;s.WorldStart=s.WorldFov;s.ForegroundStart=s.ForegroundFov;
            s.WorldTarget=aim?35:70;s.ForegroundTarget=aim?55:75;s.AdsElapsed=0;
        }
        else if(s.AdsElapsed<12)++s.AdsElapsed;
        s.WorldFov=CameraFovTransition.Evaluate(s.WorldStart,s.WorldTarget,(uint)s.AdsElapsed,12);
        s.ForegroundFov=CameraFovTransition.Evaluate(s.ForegroundStart,s.ForegroundTarget,(uint)s.AdsElapsed,12);
        var lens=c.GetCameraLens(E(3))!.Value;
        c.SetCameraLens(E(3),lens.World with {VerticalFov=s.WorldFov},lens.Viewmodel!.Value with {VerticalFov=s.ForegroundFov});
    }
    public override void Tick(ref RangeState s,GameContext c)
    {
        bool active=s.Phase==1 && s.MenuOpen==0;
        Reconcile(ref s,c);
        if(active)
        {
            ++s.Timer;
            c.SetCharacterStance(E(2),c.GetInputAction(E(2),"crouch").Held);
            Ads(ref s,c,c.GetInputAction(E(2),"aim").Held);
            if(s.ReloadRemaining>0 && --s.ReloadRemaining==0)s.Ammo=4;
            if(c.GetInputAction(E(2),"reload").Pressed)
            {
                if(s.Supply==1 && s.Ammo<4 && s.ReloadRemaining==0)s.ReloadRemaining=12;else ++s.Rejected;
            }
            var p=c.Get(E(2)).Transform.Position;
            bool hazard=Inside(p,0,-4,.4);
            if(hazard && s.HazardInside==0 && --s.Health==0)s.Phase=2;
            s.HazardInside=hazard?1:0;
            if(s.Phase==1 && c.Pressed(E(2),GameAction.Use))
            {
                if(s.Supply==0 && Inside(p,-3,0,.5) && c.GetCharacterStance(E(2))!.Value.Crouched)
                {s.Supply=1;s.Ammo=4;c.PlaySound(E(170));}
                else if(Inside(p,0,5,.6) && s.Supply==1 && s.ClearedMask==3)
                {s.Phase=3;c.PlaySound(E(172));}
                else ++s.Rejected;
            }
            if(s.Phase==1 && c.GetInputAction(E(2),"fire").Pressed)
            {
                if(s.Ammo==0 || s.ReloadRemaining>0)++s.Rejected;
                else
                {
                    --s.Ammo;++s.Shots;
                    var camera=c.Get(E(3)).Transform;
                    var hit=c.HitTest(camera.Position,camera.Forward,20,new EntityId[]{E(2)});
                    int bit=hit is {} h && h.Region=="target" ? h.Entity==E(110)?1:h.Entity==E(120)?2:0 :0;
                    if(s.Supply==1 && bit!=0 && (s.ClearedMask&bit)==0){s.ClearedMask|=bit;c.PlaySound(E(171));}
                    else ++s.Rejected;
                }
            }
            if(s.Phase==1)
            {
                var b=c.Get(E(120)).Transform.Position;
                if(b.X>=3.5)s.PatrolDirection=-1;else if(b.X<=2.5)s.PatrolDirection=1;
                bool cleared=(s.ClearedMask&2)!=0;
                bool moved=Math.Abs(b.X-s.PreviousBX)+Math.Abs(b.Z-s.PreviousBZ)>.00001;
                int clip=!cleared && moved?1:0;
                if(clip!=s.AnimationClip){s.AnimationClip=clip;s.AnimationTime=0;}else s.AnimationTime+=c.DeltaTime;
                c.SetCharacterInput(E(120),cleared?0:s.PatrolDirection,0);
                // Explicit sample clock advances only in active ticks. Speed zero prevents
                // native explicit steps from advancing the rig while game gating is closed.
                c.SetAnimation(E(121),s.AnimationClip,s.AnimationTime,0,true,true);
                s.PreviousBX=b.X;s.PreviousBZ=b.Z;
            }
        }
        var stance=c.GetCharacterStance(E(2))!.Value;
        c.SetUi(U(6),Hud(s,stance.Crouched?(stance.StandBlocked?"Crouched / stand blocked":"Crouched"):"Standing"));
        Panels(s,c);
    }
}
