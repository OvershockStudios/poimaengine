// SPDX-License-Identifier: Apache-2.0
using Poima;
using System.Numerics;
namespace Poima.Verification;
public struct ProbeState
{
    public EntityId Rig, Plain, Body, Emitter;
    public int InertialMode, QActiveMode, QExtendedMatches, StagingChecks; public double QProgress;
    public int Mode, Serial, AppliedSerial, Clip, Loop, Playing, BlendTicks, Count, Ticks, NullProbe;
    public double Time, Speed;
    public long ThrowAt, LastVoice, QueryTick, DelayUntil;
    public int QClip, QLoop, QPlaying, QHasTransition, QSourceFrozen, QSourceClip, QSourceLoop, QSourcePlaying;
    public double QTime, QSpeed, QDuration, QWeight, QSourceTime, QSourceSpeed;
    public long QStartTick;
    public int QDurationTicks, QElapsedTicks;
}
[GameModule("poima.verification.animation-services")]
public sealed class ProbeGame : Game<ProbeState>, IInertialAnimationGame
{
    private static void Stamp(string phase)
    {
        string? path = Environment.GetEnvironmentVariable("POIMA_INERTIAL_SENTINEL");
        if (path is not null) File.AppendAllText(path, phase + "\n");
    }
    public ProbeGame() { Stamp("constructor"); }
    // Compiler proof: these old-source forms must retain legacy resolution.
    // The helper is not invoked, so it cannot queue duplicate rig writes.
    private static void CompileOverloads(GameContext context, EntityId rig)
    {
        context.SetAnimation(rig, 0);
        context.SetAnimation(rig, 0, 0);
        context.SetAnimation(rig, 0, default);
        context.SetAnimation(rig, 0, time: .25, blendTicks: 30);
        context.SetAnimation(rig, 0, AnimationTransitionMode.Crossfade);
        context.SetAnimation(rig, 0, (AnimationTransitionMode)0);
        context.SetAnimation(rig, 0, transitionMode: AnimationTransitionMode.Inertial, blendTicks: 30);
    }
    public override void Initialize(ref ProbeState state)
    {
        Stamp("initialize"); state.InertialMode = 1;
        state.Rig = new(0, 1000); state.Plain = new(0, 3);
        state.Body = new(0, 2); state.Emitter = new(0, 20);
        state.Clip = 0; state.Speed = 1; state.Loop = state.Playing = 1;
        state.ThrowAt = -1;
    }
    private static void Observe(ref ProbeState s, AnimationState? query, ulong tick)
    {
        s.QueryTick = checked((long)tick);
        if (query is not { } a) { s.QClip = -2; return; }
        s.QClip = a.Clip ?? -1; s.QTime = a.Time; s.QSpeed = a.Speed;
        s.QLoop = a.Loop ? 1 : 0; s.QPlaying = a.Playing ? 1 : 0; s.QDuration = a.Duration;
        s.QHasTransition = a.Transition.HasValue ? 1 : 0;
        if (a.Transition is not { } t) return;
        s.QStartTick = checked((long)t.StartTick); s.QDurationTicks = checked((int)t.DurationTicks);
        s.QElapsedTicks = checked((int)t.ElapsedTicks); s.QWeight = t.Weight;
        s.QSourceFrozen = t.SourceFrozen ? 1 : 0; s.QSourceClip = t.SourceClip ?? -1;
        s.QSourceTime = t.SourceTime ?? -1; s.QSourceSpeed = t.SourceSpeed ?? -1;
        s.QSourceLoop = t.SourceLoop.HasValue ? (t.SourceLoop.Value ? 1 : 0) : -1;
        s.QSourcePlaying = t.SourcePlaying.HasValue ? (t.SourcePlaying.Value ? 1 : 0) : -1;
    }
    private static void Set(ref ProbeState s, GameContext c, EntityId id)
        => c.SetAnimation(id, s.Clip < 0 ? null : s.Clip, (AnimationTransitionMode)s.InertialMode, s.Time, s.Speed,
                          s.Loop != 0, s.Playing != 0, checked((uint)s.BlendTicks));
    public override void Tick(ref ProbeState state, GameContext context)
    {
        Stamp("tick"); ++state.Ticks;
        var before = context.GetAnimation(state.Rig);
        var extendedBefore = context.GetAnimationExtended(state.Rig);
        if (!Nullable.Equals(before, extendedBefore?.State))
            throw new InvalidOperationException("Extended state differs from legacy projection.");
        state.QExtendedMatches = 1;
        state.QActiveMode = extendedBefore?.Mode is { } mode ? (int)mode : -1;
        state.QProgress = extendedBefore?.Progress ?? -1;
        Observe(ref state, before, context.Tick);
        if (state.Serial != state.AppliedSerial && (state.Mode != 6 || context.Tick >= (ulong)state.DelayUntil))
        {
            switch (state.Mode)
            {
                case 1: case 6: Set(ref state, context, state.Rig); break;
                case 2: Set(ref state, context, state.Rig); Set(ref state, context, state.Rig); break;
                case 3:
                    if (context.GetAnimation(state.Plain) is not null || context.GetAnimationExtended(state.Plain) is not null) throw new InvalidOperationException("Non-rig had animation.");
                    state.NullProbe = 1; break;
                case 4: context.GetAnimation(new(0, 999999)); break;
                case 5:
                    for (int i = 0; i < state.Count; ++i) Set(ref state, context, new(state.Rig.High, state.Rig.Low + (ulong)i));
                    break;
                case 7:
                    Set(ref state, context, state.Rig);
                    context.MoveKinematic(state.Body, new(7, 1, 0), Quaternion.Identity, 60);
                    state.LastVoice = context.PlaySound(state.Emitter);
                    break;
            }
            if (!Nullable.Equals(before, context.GetAnimation(state.Rig)))
                throw new InvalidOperationException("Queued animation became visible before Tick returned.");
            if (!Nullable.Equals(extendedBefore, context.GetAnimationExtended(state.Rig)))
                throw new InvalidOperationException("Queued extended animation became visible before Tick returned.");
            ++state.StagingChecks;
            state.AppliedSerial = state.Serial;
        }
        if (state.ThrowAt >= 0 && context.Tick >= (ulong)state.ThrowAt)
            throw new InvalidOperationException("Intentional later managed tick failure.");
    }
}
