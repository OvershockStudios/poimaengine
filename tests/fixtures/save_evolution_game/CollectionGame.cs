// SPDX-License-Identifier: Apache-2.0
using Poima;
using System.Numerics;
using System.Runtime.InteropServices;
namespace Poima.Examples;

[GameplayPersistence(1)]
[StructLayout(LayoutKind.Sequential)]
public struct CollectionState
{
    [GameplayField("598c02252e61ba4bf2e9b33fdfef3f61",Default="731")]
    public int UpgradeSteps;
    [GameplayField("5e1b007e3411ac97be491647b33aee1f",Default="0")]
    public long SaveSequence;
    [GameplayField("93e955df13abe412393f3b1c1a4059fe",Default="0")]
    public int Status;
    [GameplayField("1e172aaa01dab1a35ec7788f3ab63182",Default="00000000000000000000000000000000")]
    public EntityId Third;
    [GameplayField("774f695ea57dc29e1c5394a0332156aa",Default="0")]
    public long SaveLow;
    [GameplayField("7e6e97769a026dbb667fce2f360b30b9",Default="0")]
    public int RecoveredCount;
    [GameplayField("99c785bf684623e942724aca2b10dbe2",Default="00000000000000000000000000000000")]
    public EntityId Second;
    [GameplayField("a72612b6a90202860324135e796aec0b",Default="0")]
    public int SavePending;
    [GameplayField("734786b151d812e9f00bd3b33042edff",Default="00000000000000000000000000000000")]
    public EntityId First;
    [GameplayField("ee1e4bc818453f8020b51aec85078ba0",Default="0")]
    public long SaveHigh;
    [GameplayField("0301b60f43cd80d8838b92ea546ad250",Default="0")]
    public int Started;
    [GameplayField("4c5cb3a009563eb21f19541f53f21d6d",Default="00000000000000000000000000000000")]
    public EntityId Camera;
    [GameplayField("3724b91c3375792fc360d716a35de67d",Default="00000000000000000000000000000000")]
    public EntityId Player;
}

[GameModule("poima.example.collection-room")]
public sealed class CollectionGame:Game<CollectionState>
{
    static readonly TemplateId Cell=new(0,200);
    static readonly UiId Progress=new(0,2),Status=new(0,3);
    const string Slot="collection-room";
    public override void Initialize(ref CollectionState state)
    {
        if(Environment.GetEnvironmentVariable("POIMA_SAVE_UPGRADE_FORBID_INITIALIZE")=="1")
            throw new InvalidOperationException("Initialize forbidden during old-save upgrade.");
        state=default;state.Player=new(0,100);state.Camera=new(0,101);state.UpgradeSteps=731;
    }
    static string ProgressText(int count)=>count==3 ? "All 3 cells collected. Room complete!" : $"Cells collected: {count} / 3";
    static string StatusText(int status)=>status switch
    {
        1=>"Checkpoint save requested.",2=>"Checkpoint saved.",3=>"Checkpoint loaded.",
        4=>"Save/load failed. Check the engine save result.",5=>"Save storage is not configured.",
        6=>"A save/load request is already pending.",7=>"Checkpoint load requested.",
        8=>"Previous request is no longer tracked.",_=>"WASD move | E collect | Tab releases cursor\nClick outside this panel to return to the game."
    };
    static SaveTicket Ticket(in CollectionState state)=>new(state.SaveHigh,state.SaveLow,state.SaveSequence);
    static bool Restored(ref CollectionState state,SaveCapabilities saves)
    {
        // A saved pending ticket describes the source timeline's Save. It may
        // still report success after a same-process Load, or expire in a new
        // process. Current owner restore metadata identifies the new timeline.
        var ticketEpoch=new SaveEpoch(state.SaveHigh,state.SaveLow);
        if(state.SavePending==0 || ticketEpoch==saves.Epoch ||
            saves.LastRestore is not { } restore || restore.DestinationEpoch!=saves.Epoch)return false;
        state.SavePending=0;state.Status=3;return true;
    }
    static void Result(ref CollectionState state,SaveOperationResult result)
    {
        if(!result.IsTerminal)return;
        state.SavePending=0;
        state.Status=result.State==SaveOperationState.Succeeded ? 2 : result.State==SaveOperationState.Expired ? 8 : 4;
    }
    public override void Tick(ref CollectionState state,GameContext context)
    {
        if(state.UpgradeSteps<731)throw new InvalidOperationException("Explicit added-field default was not restored.");
        state.UpgradeSteps=checked(state.UpgradeSteps+1);
        if(state.SavePending!=0)
        {
            if(!Restored(ref state,context.Saves))Result(ref state,context.GetSaveResult(Ticket(in state)));
            if(state.SavePending==0)context.SetUi(Status,text:StatusText(state.Status));
        }
        if(state.Started==0)
        {
            state.First=context.Spawn(Cell,new SpawnTransform(new(0,1.5,0),Quaternion.Identity,new(.7,1,.7)));
            state.Second=context.Spawn(Cell,new SpawnTransform(new(0,1.5,-4),Quaternion.Identity,new(.7,1,.7)));
            state.Third=context.Spawn(Cell,new SpawnTransform(new(0,1.5,-8),Quaternion.Identity,new(.7,1,.7)));
            state.Started=1;context.SetUi(Progress,text:ProgressText(state.RecoveredCount));return;
        }
        if(!context.Pressed(state.Player,GameAction.Use))return;
        var camera=context.Get(state.Camera).Transform;
        var hit=context.Raycast(camera.Position,camera.Forward,3.2,[state.Player]);
        if(hit is null)return;
        var entity=hit.Value.Entity;
        if(entity==state.First)state.First=default;
        else if(entity==state.Second)state.Second=default;
        else if(entity==state.Third)state.Third=default;
        else return;
        // Clear the persisted reference in the same atomic Tick as removal.
        context.Despawn(entity);++state.RecoveredCount;
        context.SetUi(Progress,text:ProgressText(state.RecoveredCount));
    }
    public override void Control(ref CollectionState state,ControlContext context)
    {
        if(state.UpgradeSteps<731)throw new InvalidOperationException("Explicit added-field default was not restored.");
        state.UpgradeSteps=checked(state.UpgradeSteps+1);
        if(state.SavePending!=0 && !Restored(ref state,context.Saves))Result(ref state,context.GetSaveResult(Ticket(in state)));
        switch(context.Action)
        {
            case "pause":context.RequestPause();break;
            case "resume":context.RequestResume();break;
            case "save":
            case "load":
                if(!context.Saves.Enabled) {state.Status=5;break;}
                var request=context.Action=="save" ? context.TryRequestSave(Slot) : context.TryRequestLoad(Slot);
                if(!request.Accepted) {state.Status=request.Rejection==SaveRequestRejection.Busy ? 6 : 4;break;}
                state.SaveHigh=request.Ticket.EpochHigh;state.SaveLow=request.Ticket.EpochLow;state.SaveSequence=request.Ticket.Sequence;
                state.SavePending=1;state.Status=context.Action=="save" ? 1 : 7;
                break;
            default:throw new InvalidOperationException("Unknown collection-room UI action.");
        }
        context.SetUi(Status,text:StatusText(state.Status));
    }
}
