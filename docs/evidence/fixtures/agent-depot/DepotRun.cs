using Poima;
using System.Runtime.InteropServices;

namespace Depot;

[StructLayout(LayoutKind.Sequential)]
public struct DepotState
{
    public int Phase, Stage, Cargo, RunningTicks, Rejected, PreviousUse, Menu;
    public long TicketHigh, TicketLow, TicketSequence;
    public int TicketKind, StorageStatus, StorageError;
    public long CompletionVoice;
}

[GameModule("depot-run-v1")]
public sealed class DepotGame : Game<DepotState>
{
    private static readonly EntityId Player = EntityId.Parse("de907000000000000000000000000001");
    private static readonly EntityId Emitter = EntityId.Parse("de90700000000000000000000000000d");
    private static readonly UiId Welcome = UiId.Parse("de90700000000000000000000000000e");
    private static readonly UiId Hud = UiId.Parse("de907000000000000000000000000011");
    private static readonly UiId MenuPanel = UiId.Parse("de907000000000000000000000000013");
    private static readonly UiId SaveStatus = UiId.Parse("de907000000000000000000000000017");
    private static readonly UiId HudStatus = UiId.Parse("de907000000000000000000000000018");
    private static readonly UiId Terminal = UiId.Parse("de907000000000000000000000000019");
    private static readonly UiId TerminalText = UiId.Parse("de90700000000000000000000000001a");
    private static readonly UiId WelcomeStatus = UiId.Parse("de90700000000000000000000000001b");

    public override void Initialize(ref DepotState s) => s = default;

    // Authority is the committed controller foot, never the camera or a visual child.
    private static bool Near(Vector3d foot, double x, double z) =>
        foot.Y >= -.6 && foot.Y <= .6 &&
        (foot.X-x)*(foot.X-x) + (foot.Z-z)*(foot.Z-z) <= .8*.8;

    public override void Tick(ref DepotState s, GameContext c)
    {
        Poll(ref s, c);
        bool use = c.Pressed(Player, GameAction.Use);
        bool rising = use && s.PreviousUse == 0;
        s.PreviousUse = use ? 1 : 0;
        if (s.Phase == 1 && s.Menu == 0)
        {
            ++s.RunningTicks;
            if (rising)
            {
                Vector3d p = c.Get(Player).Transform.Position;
                if (s.Stage == 0 && s.Cargo == 0 && Near(p,-3,0)) { s.Stage=1; s.Cargo=1; }
                else if (s.Stage == 1 && s.Cargo == 1 && Near(p,3,0)) { s.Stage=2; s.Cargo=0; }
                else if (s.Stage == 2 && s.Cargo == 0 && Near(p,-3,-5)) { s.Stage=3; s.Cargo=2; }
                else if (s.Stage == 3 && s.Cargo == 2 && Near(p,3,-5)) { s.Stage=4; s.Cargo=0; }
                else if (s.Stage == 4 && s.Cargo == 0 && Near(p,0,-8))
                { s.Phase=2; s.CompletionVoice=c.PlaySound(Emitter); }
                else ++s.Rejected;
            }
        }
        Paint(s,c);
    }

    public override void Control(ref DepotState s, ControlContext c)
    {
        Poll(ref s,c);
        switch (c.Action)
        {
            case "begin" when s.Phase == 0:
                s.Phase=1; s.Menu=0;
                c.RequestResume();
                break;
            case "menu" when s.Phase == 1 && s.Menu == 0:
                s.Menu=1;
                c.RequestPause();
                break;
            case "resume" when s.Phase == 1 && s.Menu != 0:
                s.Menu=0;
                c.RequestResume();
                break;
            case "save" when s.Phase == 1 && s.Menu != 0:
                if (s.TicketSequence == 0) Remember(ref s,c.TryRequestSave("depot-checkpoint"),1);
                break;
            case "load" when s.Phase == 0 || (s.Phase == 1 && s.Menu != 0):
                if (s.TicketSequence == 0) Remember(ref s,c.TryRequestLoad("depot-checkpoint"),2);
                break;
        }
        Paint(s,c);
    }

    private static void Remember(ref DepotState s, SaveRequestResult r, int kind)
    {
        if (!r.Accepted) { s.StorageStatus=5; s.StorageError=(int)r.Rejection; return; }
        s.TicketHigh=r.Ticket.EpochHigh; s.TicketLow=r.Ticket.EpochLow;
        s.TicketSequence=r.Ticket.Sequence; s.TicketKind=kind;
        s.StorageStatus=1; s.StorageError=0;
    }
    private static void Result(ref DepotState s, SaveOperationResult r)
    {
        s.StorageStatus=(int)r.State; s.StorageError=r.ErrorCode;
        // Restored/expired tokens are observations, never instructions to enqueue again.
        if (r.IsTerminal) { s.TicketSequence=0; s.TicketHigh=0; s.TicketLow=0; s.TicketKind=0; }
    }
    private static void Poll(ref DepotState s, GameContext c)
    { if (s.TicketSequence != 0) Result(ref s,c.GetSaveResult(new(s.TicketHigh,s.TicketLow,s.TicketSequence))); }
    private static void Poll(ref DepotState s, ControlContext c)
    { if (s.TicketSequence != 0) Result(ref s,c.GetSaveResult(new(s.TicketHigh,s.TicketLow,s.TicketSequence))); }

    private static string Status(DepotState s) => s.StorageStatus switch
    {
        1 => "Checkpoint request queued",
        2 => "Storage resolving - awaiting native result",
        3 => "Checkpoint operation succeeded",
        4 => $"Storage failed (code {s.StorageError}) - retry available",
        5 => $"Request rejected (reason {s.StorageError})",
        _ => "Checkpoint slot: depot-checkpoint"
    };
    private static string HudText(DepotState s)
    {
        string cargo=s.Cargo switch { 1=>"Parcel A",2=>"Parcel B",_=>"empty" };
        string next=s.Stage switch { 0=>"Parcel A (-3, 0)",1=>"Bay A (3, 0)",2=>"Parcel B (-3, -5)",3=>"Bay B (3, -5)",_=>"Exit (0, -8)" };
        return $"Cargo: {cargo} · Delivered: {s.Stage/2}/2 · Time: {s.RunningTicks} ticks\nNext: {next} · Rejected: {s.Rejected}";
    }
    private static void Paint(DepotState s, GameContext c)
    {
        c.SetModal(s.Phase==0 ? Welcome : s.Phase==2 ? Terminal : s.Menu!=0 ? MenuPanel : null);
        c.SetUi(Welcome,visible:s.Phase==0); c.SetUi(Hud,visible:s.Phase==1);
        c.SetUi(MenuPanel,visible:s.Phase==1 && s.Menu!=0); c.SetUi(Terminal,visible:s.Phase==2);
        c.SetUi(HudStatus,HudText(s)); c.SetUi(SaveStatus,Status(s)); c.SetUi(WelcomeStatus,Status(s));
        c.SetUi(TerminalText,$"SHIFT COMPLETE\n2 parcels delivered\nElapsed: {s.RunningTicks} ticks ({s.RunningTicks/60.0:F1}s)");
    }
    private static void Paint(DepotState s, ControlContext c)
    {
        c.SetModal(s.Phase==0 ? Welcome : s.Phase==2 ? Terminal : s.Menu!=0 ? MenuPanel : null);
        c.SetUi(Welcome,visible:s.Phase==0); c.SetUi(Hud,visible:s.Phase==1);
        c.SetUi(MenuPanel,visible:s.Phase==1 && s.Menu!=0); c.SetUi(Terminal,visible:s.Phase==2);
        c.SetUi(HudStatus,HudText(s)); c.SetUi(SaveStatus,Status(s)); c.SetUi(WelcomeStatus,Status(s));
    }
}
