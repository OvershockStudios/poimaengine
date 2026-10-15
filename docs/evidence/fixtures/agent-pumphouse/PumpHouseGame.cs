// Pump House: a first-person repair adventure for the Poima native runtime.
// All mutable authoritative data lives in PumpState (native-owned, saved, rolled back).
using System.Numerics;
using Poima;

namespace PumpHouse;

[GameplayPersistence(1)]
public struct PumpState
{
    // Inventory (bounded: at most 2 fuses, 1 drive cog).
    [GameplayField("a0000000000000000000000000000001", Default = "0")] public int FuseCount;
    [GameplayField("a0000000000000000000000000000002", Default = "0")] public int CogCount;
    // Pickup availability: 1 = still lying in the world, 0 = taken.
    [GameplayField("a0000000000000000000000000000003", Default = "1")] public int FuseAPresent;
    [GameplayField("a0000000000000000000000000000004", Default = "1")] public int FuseBPresent;
    [GameplayField("a0000000000000000000000000000005", Default = "1")] public int CogPresent;
    // World changes: breaker powered (opens the parts-locker shutter), bridge raised, pump running.
    [GameplayField("a0000000000000000000000000000006", Default = "0")] public int BreakerOn;
    [GameplayField("a0000000000000000000000000000007", Default = "0")] public int BridgeRaised;
    [GameplayField("a0000000000000000000000000000008", Default = "0")] public int PumpOn;
    // Flow/UI.
    [GameplayField("a0000000000000000000000000000009", Default = "0")] public int Started;
    [GameplayField("a000000000000000000000000000000a", Default = "0")] public int Paused;
    [GameplayField("a000000000000000000000000000000b", Default = "0")] public int Feedback;
    [GameplayField("a000000000000000000000000000000c", Default = "0")] public int LookCode;
    [GameplayField("a000000000000000000000000000000d", Default = "1")] public int HudDirty;
    [GameplayField("a000000000000000000000000000000e", Default = "0")] public int ResetPending;
    [GameplayField("a000000000000000000000000000000f", Default = "0")] public int ModalShown;
    [GameplayField("a0000000000000000000000000000010", Default = "0")] public int SaveStatus;
    [GameplayField("a0000000000000000000000000000011", Default = "0")] public int SaveKindPending;
    [GameplayField("a0000000000000000000000000000012", Default = "0")] public long SaveEpochHigh;
    [GameplayField("a0000000000000000000000000000013", Default = "0")] public long SaveEpochLow;
    [GameplayField("a0000000000000000000000000000014", Default = "0")] public long SaveSequence;
    [GameplayField("a0000000000000000000000000000015", Default = "0")] public long SessionEpochHigh;
    [GameplayField("a0000000000000000000000000000016", Default = "0")] public long SessionEpochLow;
    [GameplayField("a0000000000000000000000000000017", Default = "0")] public long SavedTick;
    [GameplayField("a0000000000000000000000000000018", Default = "0")] public int Objective;
    [GameplayField("a0000000000000000000000000000019", Default = "0")] public int Interactions;
}

[GameModule("pumphouse.repair")]
public sealed class PumpHouseGame : Game<PumpState>
{
    // Authored entity IDs (see LOCATOR.json).
    static readonly EntityId Player = Id(0x02), Camera = Id(0x03);
    static readonly EntityId FuseA = Id(0x10), FuseB = Id(0x11), Cog = Id(0x12);
    static readonly EntityId Breaker = Id(0x20), Shutter = Id(0x21), Winch = Id(0x22), Deck = Id(0x23), Console = Id(0x24);

    // Authored kinematic poses.
    static readonly Vector3d FuseAHome = new(-6, 0.95, 5), FuseBHome = new(5, 1.05, 6.5), CogHome = new(7.5, 1.1, 0);
    static readonly Vector3d ShutterClosed = new(6.95, 1.2, 0), ShutterOpen = new(6.95, 3.0, 0);
    static readonly Vector3d DeckLowered = new(0, -0.05, -6.5), DeckRaised = new(0, 1.7, -6.5);

    // UI IDs.
    static readonly UiId HudObjective = Ui(0xa1), HudInventory = Ui(0xa2), HudFeedback = Ui(0xa3), HudPrompt = Ui(0xa4), HudSave = Ui(0xa5);
    static readonly UiId StartPanel = Ui(0xb0), StartStatus = Ui(0xb5), PausePanel = Ui(0xc0), PauseStatus = Ui(0xc6);

    const string Slot = "checkpoint";
    const double Reach = 3.0;

    static EntityId Id(ulong low) => new(0, low);
    static UiId Ui(ulong low) => new(0x7000000000000000UL, low);

    public override void Initialize(ref PumpState s)
    {
        s = default;
        s.FuseAPresent = 1; s.FuseBPresent = 1; s.CogPresent = 1;
        s.HudDirty = 1;
    }

    public override void Tick(ref PumpState s, GameContext ctx)
    {
        TrackSession(ref s, ctx);
        PollSave(ref s, ctx);

        if (s.Started == 0 && s.ModalShown == 0)
        {
            ctx.SetModal(StartPanel);
            s.ModalShown = 1;
        }

        if (s.ResetPending != 0)
        {
            // Staged New Game reset: return every moved body to its authored pose (completes within 45 ticks).
            var q = Quaternion.Identity;
            ctx.MoveKinematic(FuseA, FuseAHome, q, 4);
            ctx.MoveKinematic(FuseB, FuseBHome, q, 4);
            ctx.MoveKinematic(Cog, CogHome, q, 4);
            ctx.MoveKinematic(Shutter, ShutterClosed, q, 30);
            ctx.MoveKinematic(Deck, DeckLowered, q, 45);
            s.ResetPending = 0;
            s.HudDirty = 1;
        }
        else if (s.Started != 0 && s.Paused == 0)
        {
            var look = LookAt(ctx);
            int code = CodeOf(look);
            if (code != s.LookCode) { s.LookCode = code; s.HudDirty = 1; }
            if (ctx.Pressed(Player, GameAction.Use) && code != 0) Interact(ref s, ctx, code);
        }

        int objective = ObjectiveOf(s);
        if (objective != s.Objective) { s.Objective = objective; s.HudDirty = 1; }
        if (s.HudDirty != 0)
        {
            ctx.SetUi(HudObjective, ObjectiveText(s));
            ctx.SetUi(HudInventory, InventoryText(s));
            ctx.SetUi(HudFeedback, FeedbackText(s));
            ctx.SetUi(HudPrompt, PromptText(s));
            ctx.SetUi(HudSave, SaveText(s));
            ctx.SetUi(StartStatus, SaveText(s));
            ctx.SetUi(PauseStatus, SaveText(s));
            s.HudDirty = 0;
        }
    }

    public override void Control(ref PumpState s, ControlContext ctx)
    {
        switch (ctx.Action)
        {
            case "start":
                s.Started = 1; s.Paused = 0; s.ModalShown = 1;
                ctx.SetUi(StartPanel, visible: false);
                ctx.SetModal(null);
                ctx.RequestResume();
                break;
            case "pause":
                if (s.Started == 0) break;
                s.Paused = 1;
                ctx.SetUi(PausePanel, visible: true);
                ctx.SetModal(PausePanel);
                ctx.RequestPause();
                break;
            case "resume":
                ClosePause(ref s, ctx);
                break;
            case "save":
            {
                var r = ctx.TryRequestSave(Slot);
                if (r.Accepted) { Remember(ref s, r.Ticket, 1); s.SaveStatus = 1; }
                else s.SaveStatus = 7;
                ClosePause(ref s, ctx);
                break;
            }
            case "load":
            {
                var r = ctx.TryRequestLoad(Slot);
                if (r.Accepted) { Remember(ref s, r.Ticket, 2); s.SaveStatus = 4; }
                else s.SaveStatus = 7;
                if (s.Started != 0) ClosePause(ref s, ctx);
                break;
            }
            case "newgame":
                // Reset inventory, world flags and objective. Kinematic bodies return on the next Tick.
                // The user checkpoint slot is neither loaded nor deleted.
                s.FuseCount = 0; s.CogCount = 0;
                s.FuseAPresent = 1; s.FuseBPresent = 1; s.CogPresent = 1;
                s.BreakerOn = 0; s.BridgeRaised = 0; s.PumpOn = 0;
                s.Objective = 0; s.LookCode = 0; s.Interactions = 0;
                s.ResetPending = 1; s.Feedback = 14;
                s.Started = 1; s.ModalShown = 1;
                ctx.SetUi(StartPanel, visible: false);
                ClosePause(ref s, ctx);
                break;
            default:
                throw new InvalidOperationException("Unhandled UI action: " + ctx.Action);
        }
        s.HudDirty = 1;
        ctx.SetUi(HudSave, SaveText(s));
        ctx.SetUi(StartStatus, SaveText(s));
        ctx.SetUi(PauseStatus, SaveText(s));
    }

    static void ClosePause(ref PumpState s, ControlContext ctx)
    {
        s.Paused = 0;
        ctx.SetUi(PausePanel, visible: false);
        ctx.SetModal(null);
        ctx.RequestResume();
    }

    static void Remember(ref PumpState s, SaveTicket t, int kind)
    {
        s.SaveEpochHigh = t.EpochHigh; s.SaveEpochLow = t.EpochLow; s.SaveSequence = t.Sequence; s.SaveKindPending = kind;
    }

    // A new runtime epoch after a successful load: the saved pending ticket is stale data, not a command.
    static void TrackSession(ref PumpState s, GameContext ctx)
    {
        var caps = ctx.Saves;
        if (caps.Epoch.High == s.SessionEpochHigh && caps.Epoch.Low == s.SessionEpochLow) return;
        bool first = s.SessionEpochHigh == 0 && s.SessionEpochLow == 0;
        s.SessionEpochHigh = caps.Epoch.High; s.SessionEpochLow = caps.Epoch.Low;
        if (!first && caps.LastRestore.HasValue)
        {
            s.SaveSequence = 0; s.SaveKindPending = 0; s.SaveStatus = 6;
            s.Paused = 0; s.ResetPending = 0;
        }
        s.HudDirty = 1;
    }

    static void PollSave(ref PumpState s, GameContext ctx)
    {
        if (s.SaveSequence == 0) return;
        var result = ctx.GetSaveResult(new SaveTicket(s.SaveEpochHigh, s.SaveEpochLow, s.SaveSequence));
        if (!result.IsTerminal) return;
        bool ok = result.State == SaveOperationState.Succeeded;
        if (s.SaveKindPending == 1) { s.SaveStatus = ok ? 2 : 3; if (ok) s.SavedTick = (long)result.CommittedTick; }
        else s.SaveStatus = ok ? 6 : 5;
        s.SaveSequence = 0; s.SaveKindPending = 0; s.HudDirty = 1;
    }

    static RayHit? LookAt(GameContext ctx)
    {
        var eye = ctx.Get(Camera).Transform;
        ReadOnlySpan<EntityId> ignore = [Player];
        return ctx.Raycast(eye.Position, eye.Forward, Reach, ignore);
    }

    static int CodeOf(RayHit? hit)
    {
        if (!hit.HasValue) return 0;
        var e = hit.Value.Entity;
        if (e == FuseA) return 1;
        if (e == FuseB) return 2;
        if (e == Cog) return 3;
        if (e == Breaker) return 4;
        if (e == Shutter) return 5;
        if (e == Winch) return 6;
        if (e == Deck) return 7;
        if (e == Console) return 8;
        return 0;
    }

    static void Interact(ref PumpState s, GameContext ctx, int code)
    {
        var q = Quaternion.Identity;
        s.Interactions++;
        s.HudDirty = 1;
        switch (code)
        {
            case 1 or 2:
            {
                ref int present = ref (code == 1 ? ref s.FuseAPresent : ref s.FuseBPresent);
                if (present == 0) return;
                if (s.FuseCount >= 2) { s.Feedback = 2; return; }
                present = 0; s.FuseCount++;
                var home = code == 1 ? FuseAHome : FuseBHome;
                ctx.MoveKinematic(code == 1 ? FuseA : FuseB, new Vector3d(home.X, -3, home.Z), q, 4);
                s.Feedback = 1;
                return;
            }
            case 3:
                if (s.CogPresent == 0 || s.BreakerOn == 0) return;
                if (s.CogCount >= 1) { s.Feedback = 2; return; }
                s.CogPresent = 0; s.CogCount = 1;
                ctx.MoveKinematic(Cog, new Vector3d(CogHome.X, -3, CogHome.Z), q, 4);
                s.Feedback = 7;
                return;
            case 4:
                if (s.BreakerOn != 0) { s.Feedback = 5; return; }
                if (s.FuseCount < 2) { s.Feedback = 3; return; }
                s.FuseCount -= 2; s.BreakerOn = 1;
                ctx.MoveKinematic(Shutter, ShutterOpen, q, 60);
                s.Feedback = 4;
                return;
            case 5:
                s.Feedback = s.BreakerOn == 0 ? 6 : 16;
                return;
            case 6:
                if (s.BridgeRaised != 0) { s.Feedback = 10; return; }
                if (s.CogCount < 1) { s.Feedback = 8; return; }
                s.CogCount = 0; s.BridgeRaised = 1;
                ctx.MoveKinematic(Deck, DeckRaised, q, 90);
                s.Feedback = 9;
                return;
            case 7:
                s.Feedback = s.BridgeRaised == 0 ? 15 : 17;
                return;
            case 8:
                if (s.PumpOn != 0) { s.Feedback = 13; return; }
                if (s.BridgeRaised == 0) { s.Feedback = 11; return; }
                s.PumpOn = 1;
                s.Feedback = 12;
                return;
        }
    }

    static int ObjectiveOf(in PumpState s) =>
        s.PumpOn != 0 ? 4 : s.BridgeRaised != 0 ? 3 : s.CogCount > 0 ? 2 : s.BreakerOn != 0 ? 1 : 0;

    static string ObjectiveText(in PumpState s) => ObjectiveOf(s) switch
    {
        0 => "OBJECTIVE 1/4: Restore power. Find 2 fuses and install them in the breaker panel (west wall).",
        1 => "OBJECTIVE 2/4: The parts locker (east wall) is open. Take the drive cog.",
        2 => "OBJECTIVE 3/4: Climb the ramp to the west catwalk and fit the drive cog into the bridge winch.",
        3 => "OBJECTIVE 4/4: Cross the raised bridge and start the pump at the control console.",
        _ => "COMPLETE: The pump is running - Pump House restored! (Tab, Menu > New Game to replay)",
    };

    static string InventoryText(in PumpState s) =>
        $"INVENTORY  Fuse x{s.FuseCount} (max 2)   |   Drive cog x{s.CogCount} (max 1)";

    static string FeedbackText(in PumpState s) => s.Feedback switch
    {
        1 => $"Picked up a fuse ({s.FuseCount}/2).",
        2 => "You cannot carry any more of that item.",
        3 => $"REJECTED: The breaker needs 2 fuses - you have {s.FuseCount}. Nothing was installed.",
        4 => "2 fuses installed. Power restored - the parts locker shutter is rising.",
        5 => "The breaker is already powered. No fuses were used.",
        6 => "The locker shutter is locked shut. Restore power at the breaker first.",
        7 => "Picked up the drive cog (1/1).",
        8 => "REJECTED: The winch is missing its drive cog. The bridge stays down.",
        9 => "Drive cog installed. The bridge deck is rising!",
        10 => "The winch already has its cog. Nothing more to install.",
        11 => "The pump console is out of reach until the bridge is raised.",
        12 => "Pump started! Water is moving again.",
        13 => "The pump is already running.",
        14 => "New game: station reset. Your saved checkpoint was kept (use Load to return to it).",
        15 => "The bridge deck lies collapsed in the channel. The winch on the west catwalk can raise it.",
        16 => "The locker shutter is open.",
        17 => "The bridge deck is raised and locked.",
        _ => "Look at objects and press E to interact. Tab releases the mouse; click Menu to pause.",
    };

    static string PromptText(in PumpState s) => s.LookCode switch
    {
        1 or 2 => "[E] Take fuse",
        3 => "[E] Take drive cog",
        4 => s.BreakerOn != 0 ? "Breaker panel (powered)" : "[E] Breaker panel - install 2 fuses",
        5 => s.BreakerOn != 0 ? "Locker shutter (open)" : "[E] Parts locker shutter (no power)",
        6 => s.BridgeRaised != 0 ? "Bridge winch (running)" : "[E] Bridge winch - fit drive cog",
        7 => "[E] Bridge deck",
        8 => s.PumpOn != 0 ? "Pump console (running)" : "[E] Pump console - start pump",
        _ => "",
    };

    static string SaveText(in PumpState s) => s.SaveStatus switch
    {
        1 => "Saving checkpoint...",
        2 => $"Checkpoint saved (tick {s.SavedTick}).",
        3 => "Save failed. Your previous checkpoint is unchanged.",
        4 => "Loading checkpoint...",
        5 => "Load failed: no usable checkpoint (save first).",
        6 => "Checkpoint loaded. Click the game view to resume.",
        7 => "Save system busy or unavailable (launch with --save-root).",
        _ => "No save activity yet.",
    };
}
