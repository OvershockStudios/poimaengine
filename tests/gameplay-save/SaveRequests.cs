// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct GameplaySaveState
{
    public int Ticks, Mode, Requests, Rejection, QueuedState, BusyRejection;
    public int LastState, LastError, ExpiredSeen, RestoreSeen, RestoreInitiated, Enabled;
    public long TriggerTick, ThrowTick, ExpectedGeneration;
    public long TicketHigh, TicketLow, TicketSequence;
    public long EpochHigh, EpochLow, LastRequestedTick, LastCommittedTick, LastGeneration, RestoredTick;
}

[GameModule("poima.test.gameplay-save-requests")]
public sealed class GameplaySaveProbe : Game<GameplaySaveState>
{
    public override void Initialize(ref GameplaySaveState state)
    {
        state.ThrowTick=-1;state.ExpectedGeneration=-1;
    }
    public override void Tick(ref GameplaySaveState state,GameContext context)
    {
        ++state.Ticks;
        var capabilities=context.Saves;
        state.Enabled=capabilities.Enabled ? 1 : 0;
        if(capabilities.Epoch.High!=state.EpochHigh || capabilities.Epoch.Low!=state.EpochLow)
        {
            state.EpochHigh=capabilities.Epoch.High;state.EpochLow=capabilities.Epoch.Low;
            if(capabilities.LastRestore is { } restored)
            {
                ++state.RestoreSeen;state.RestoreInitiated=restored.InitiatingTicket.HasValue ? 1 : 0;
                state.RestoredTick=(long)restored.RestoredTick;
            }
        }
        if(state.TicketSequence!=0)
        {
            var previous=context.GetSaveResult(new(state.TicketHigh,state.TicketLow,state.TicketSequence));
            state.LastState=(int)previous.State;state.LastError=previous.ErrorCode;
            state.LastRequestedTick=(long)previous.RequestedTick;state.LastCommittedTick=(long)previous.CommittedTick;
            state.LastGeneration=(long)previous.Generation;
            if(previous.State==SaveOperationState.Expired)++state.ExpiredSeen;
            // A saved pending token is data, never an instruction to enqueue again.
            if(previous.IsTerminal)state.TicketSequence=0;
        }
        if(state.Mode!=0 && (long)context.Tick==state.TriggerTick)
        {
            var mode=state.Mode;state.Mode=0;
            var expected=state.ExpectedGeneration<0 ? (long?)null : state.ExpectedGeneration;
            var request=mode switch {
                1 => context.TryRequestSave("quick",expected),
                2 => context.TryRequestLoad("quick",expected),
                3 => context.TryRequestSave("../outside",expected),
                4 => context.TryRequestLoad("missing",expected),
                _ => throw new InvalidOperationException("Unknown fixture mode")
            };
            state.Rejection=(int)request.Rejection;
            if(request.Accepted)
            {
                ++state.Requests;
                state.TicketHigh=request.Ticket.EpochHigh;state.TicketLow=request.Ticket.EpochLow;state.TicketSequence=request.Ticket.Sequence;
                state.QueuedState=(int)context.GetSaveResult(request.Ticket).State;
                state.BusyRejection=(int)context.TryRequestSave("busy").Rejection;
            }
        }
        if((long)context.Tick==state.ThrowTick)throw new InvalidOperationException("Fixture later-tick failure after staged save request.");
    }
}
