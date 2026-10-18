using Poima;
using System.Runtime.InteropServices;
namespace KnightRoute;

[StructLayout(LayoutKind.Sequential)]
public struct RouteState
{
    public int Phase, Progress, Refusals, ActiveTicks;
    public int HavePrevious;
    public double PreviousX, PreviousZ;
    public int SelectedClip;
    public long EpochHigh, EpochLow, TicketHigh, TicketLow, TicketSequence;
    public int TicketKind, Notice;
}

[GameModule("knight.two-signals.original.v1")]
public sealed class RouteGame : Game<RouteState>, IInputActionsGame, IInertialAnimationGame
{
    // Constants identify authored native entities; there is no mutable static state.
    private static EntityId Player => new(0,2);
    private static EntityId Rig => new(0,16);
    private static UiId Ui(ulong low) => new(0,low);
    private static string Status(int n) => n switch {
        1 => "Checkpoint pending", 2 => "Checkpoint completed",
        3 => "Checkpoint loaded", 4 => "Checkpoint failed", _ => "No checkpoint requested" };
    public override void Initialize(ref RouteState s)
    {
        s=default;
        s.SelectedClip=36;
    }
    private static bool Restored(ref RouteState s, SaveCapabilities caps)
    {
        bool changed=s.EpochHigh!=caps.Epoch.High || s.EpochLow!=caps.Epoch.Low;
        if(changed && caps.LastRestore is { } r && r.DestinationEpoch==caps.Epoch)
        {
            s.Notice=3; s.TicketSequence=0; s.TicketKind=0;
        }
        s.EpochHigh=caps.Epoch.High;s.EpochLow=caps.Epoch.Low;
        return changed && caps.LastRestore is not null;
    }
    private static void Result(ref RouteState s, SaveOperationResult result)
    {
        if(!result.IsTerminal)return;
        s.Notice=result.State==SaveOperationState.Succeeded ? (s.TicketKind==2?3:2) : 4;
        s.TicketSequence=0;s.TicketKind=0;
    }
    private static SaveTicket Ticket(in RouteState s)=>new(s.TicketHigh,s.TicketLow,s.TicketSequence);
    private static void Reconcile(ref RouteState s, GameContext c)
    {
        if(!Restored(ref s,c.Saves) && s.TicketSequence>0)Result(ref s,c.GetSaveResult(Ticket(s)));
    }
    private static void Reconcile(ref RouteState s, ControlContext c)
    {
        if(!Restored(ref s,c.Saves) && s.TicketSequence>0)Result(ref s,c.GetSaveResult(Ticket(s)));
    }
    private static bool OnPad(Vector3d p,double x,double z)
    {double dx=p.X-x,dz=p.Z-z;return dx*dx+dz*dz<=0.36 && Math.Abs(p.Y)<=0.05;}
    private static string PhaseText(int p)=>p switch {1=>"Phase: Running",2=>"Phase: Complete",_=>"Phase: Ready"};
    private static void Hud(in RouteState s,GameContext c)
    {
        c.SetUi(Ui(262),PhaseText(s.Phase));c.SetUi(Ui(263),$"Progress: {s.Progress} / 2");
        c.SetUi(Ui(264),$"Refusals: {s.Refusals}");c.SetUi(Ui(265),Status(s.Notice));
    }
    private static void Hud(in RouteState s,ControlContext c)
    {
        c.SetUi(Ui(262),PhaseText(s.Phase));c.SetUi(Ui(263),$"Progress: {s.Progress} / 2");
        c.SetUi(Ui(264),$"Refusals: {s.Refusals}");c.SetUi(Ui(265),Status(s.Notice));
    }
    public override void Tick(ref RouteState s,GameContext c)
    {
        Reconcile(ref s,c);
        var p=c.Get(Player).Transform.Position;
        double dx=p.X-s.PreviousX,dz=p.Z-s.PreviousZ;
        int selected=s.HavePrevious==1 && dx*dx+dz*dz>1e-8 ? 48 : 36;
        s.PreviousX=p.X;s.PreviousZ=p.Z;s.HavePrevious=1;
        if(selected!=s.SelectedClip)
        {
            c.SetAnimation(Rig,selected,AnimationTransitionMode.Inertial,time:0,speed:1,loop:true,playing:true,blendTicks:12);
            s.SelectedClip=selected;
        }
        if(s.Phase==1)
        {
            ++s.ActiveTicks;
            if(c.GetInputAction(Player,"route.use").Pressed)
            {
                if((s.Progress==0 && OnPad(p,-2,0)) || (s.Progress==1 && OnPad(p,2,-5)))
                {++s.Progress;if(s.Progress==2)s.Phase=2;}
                else ++s.Refusals;
            }
        }
        Hud(s,c);
    }
    public override void Control(ref RouteState s,ControlContext c)
    {
        if(c.Action=="begin" && s.Phase==0)
        {s.Phase=1;Hud(s,c);return;}
        Reconcile(ref s,c);
        if(c.Action is "save" or "load")
        {
            var r=c.Action=="save"?c.TryRequestSave("route"):c.TryRequestLoad("route");
            if(r.Accepted)
            {
                s.TicketHigh=r.Ticket.EpochHigh;s.TicketLow=r.Ticket.EpochLow;
                s.TicketSequence=r.Ticket.Sequence;s.TicketKind=c.Action=="save"?1:2;s.Notice=1;
            }
            else s.Notice=4;
        }
        Hud(s,c);
    }
}
