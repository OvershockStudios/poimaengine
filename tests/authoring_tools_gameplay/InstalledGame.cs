// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using Poima;

namespace Poima.Tests;

[GameplayComponent("a1620000000000000000000000000001")]
public partial struct InstalledLedger
{
    [GameplayField("a1620000000000000000000000000011", Default="2")]
    public int Count;
    [GameplayField("a1620000000000000000000000000012", Default="9007199254740993")]
    public long Exact;
}

[GameplayPersistence(1)]
[StructLayout(LayoutKind.Sequential)]
public struct InstalledState
{
    [GameplayField("a1620000000000000000000000000021", Default="0")]
    public int Ticks;
    [GameplayField("a1620000000000000000000000000022", Default="0")]
    public int Controls;
    [GameplayField("a1620000000000000000000000000023", Default="0")]
    public int ObservedCount;
    [GameplayField("a1620000000000000000000000000024", Default="0")]
    public long ObservedExact;
}

[GameModule("poima.test.installed-authoring")]
public sealed class InstalledGame : Game<InstalledState>
{
    static readonly EntityId Actor=new(0,1621);
    static readonly UiId Label=new(0,1622);

    public override void Initialize(ref InstalledState state) => state=default;

    public override void Tick(ref InstalledState state, GameContext context)
    {
        Span<EntityId> entities=stackalloc EntityId[2];
        if(context.Query<InstalledLedger>(entities)!=1 || entities[0]!=Actor)
            throw new InvalidOperationException("Installed component query differs.");
        var ledger=context.Get<InstalledLedger>(Actor);
        state.ObservedCount=ledger.Count;state.ObservedExact=ledger.Exact;
        ++state.Ticks;++ledger.Count;ledger.Exact=checked(ledger.Exact+1);
        context.Set(Actor,in ledger);
        if(context.Get<InstalledLedger>(Actor).Count!=state.ObservedCount)
            throw new InvalidOperationException("Staged write leaked before commit.");
    }

    public override void Control(ref InstalledState state, ControlContext context)
    {
        ++state.Controls;
        var ledger=context.Get<InstalledLedger>(Actor);
        state.ObservedCount=ledger.Count;state.ObservedExact=ledger.Exact;
        context.SetUi(Label,text:$"Count {ledger.Count}; controls {state.Controls}");
        if(context.Action=="fail")throw new InvalidOperationException("Installed control rollback.");
        if(context.Action!="inspect")throw new InvalidOperationException("Unexpected installed control.");
    }
}
