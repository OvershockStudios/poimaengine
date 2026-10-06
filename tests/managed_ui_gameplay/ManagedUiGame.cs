// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using Poima;
namespace Poima.Tests;

[StructLayout(LayoutKind.Sequential)]
public struct ManagedUiState
{
    public int Mode,Ticks,ControlCalls,ReadBefore,ReadAfter,VisibleAfter,EnabledAfter,LastAction;
    public long SeenTick,SeenSequence,SaveHigh,SaveLow,SaveSequence;
}

[GameModule("poima.test.managed-ui")]
public sealed class ManagedUiGame : Game<ManagedUiState>
{
    static readonly UiId Label=UiId.Parse("00000000000000000000000000000002");
    static readonly UiId EditButton=UiId.Parse("00000000000000000000000000000003");
    static readonly UiId Modal=UiId.Parse("00000000000000000000000000000009");
    static void Check(bool condition,string message)
    {
        if(!condition)throw new InvalidOperationException(message);
    }
    public override void Initialize(ref ManagedUiState state) {state=default;}
    public override void Tick(ref ManagedUiState state,GameContext context)
    {
        ++state.Ticks;
        if(state.Mode==1)
        {
            var before=context.GetUi(Label);
            context.SetUi(Label,text:"Tick "+state.Ticks);
            Check(context.GetUi(Label).Text==before.Text,"Queued Tick UI write leaked into committed reads.");
        }
        else if(state.Mode==2)
        {
            if((context.Tick&1)!=0)throw new InvalidOperationException("Later UI Tick rollback.");
            context.SetUi(Label,text:"Tentative Tick");
            var ticket=context.RequestSave("ui-tick",0);
            state.SaveHigh=ticket.EpochHigh;state.SaveLow=ticket.EpochLow;state.SaveSequence=ticket.Sequence;
        }
    }
    public override void Control(ref ManagedUiState state,ControlContext context)
    {
        ++state.ControlCalls;state.SeenTick=checked((long)context.Tick);state.SeenSequence=checked((long)context.Sequence);
        var before=context.GetUi(Label);
        switch(context.Action)
        {
            case "edit":
                state.LastAction=1;
                state.ReadBefore=before.Text=="Original"?1:0;
                context.SetUi(Label,text:"First queued text");
                context.SetUi(Label,text:"Control "+state.ControlCalls);
                state.ReadAfter=context.GetUi(Label).Text==before.Text?1:0;
                context.SetUi(EditButton,visible:true,enabled:false);
                state.VisibleAfter=context.GetUi(EditButton).Visible?1:0;
                state.EnabledAfter=context.GetUi(EditButton).Enabled?1:0;
                Check(state.ReadAfter==1 && state.EnabledAfter==1,"Control queued writes became visible before commit.");
                return;
            case "save":
                state.LastAction=2;context.SetUi(Label,text:"Saved control "+state.ControlCalls);
                var saved=context.RequestSave("ui-control",0);
                state.SaveHigh=saved.EpochHigh;state.SaveLow=saved.EpochLow;state.SaveSequence=saved.Sequence;
                return;
            case "resume":state.LastAction=3;context.RequestResume();return;
            case "pause":state.LastAction=4;context.RequestPause();return;
            case "throw":
                state.LastAction=5;context.SetUi(Label,text:"Must roll back");
                var failed=context.RequestSave("ui-control",0);
                state.SaveHigh=failed.EpochHigh;state.SaveLow=failed.EpochLow;state.SaveSequence=failed.Sequence;
                context.RequestResume();
                throw new InvalidOperationException("Control UI/global/save rollback.");
            case "modal":
                state.LastAction=6;context.SetUi(Modal,visible:true);context.SetModal(Modal);return;
            case "clear_modal":
                state.LastAction=7;context.SetUi(Modal,visible:false);context.SetModal(null);return;
            case "invalid":
                state.LastAction=8;context.SetUi(Label,text:"Partial write");
                context.SetUi(UiId.Parse("ffffffffffffffffffffffffffffffff"),text:"Unknown");return;
            case "load":
                state.LastAction=9;context.SetUi(Label,text:"Transient load source");
                var loaded=context.RequestLoad("ui-control",1);
                state.SaveHigh=loaded.EpochHigh;state.SaveLow=loaded.EpochLow;state.SaveSequence=loaded.Sequence;
                return;
            default:throw new InvalidOperationException("Unknown UI fixture action: "+context.Action);
        }
    }
}

// CoreCLR-only fixture for the default optional-handler behavior. NativeAOT
// publication selects ManagedUiGame above; no runtime type loader is implied.
[GameModule("poima.test.no-ui-handler")]
public sealed class NoUiHandlerGame : Game<ManagedUiState>
{
    public override void Initialize(ref ManagedUiState state) {state=default;}
    public override void Tick(ref ManagedUiState state,GameContext context) {++state.Ticks;}
}
