// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)]
unsafe struct TimedSurfaceMarkWire
{
    public TexturedSurfaceMarkWire Textured;
    public uint Lifetime, Overflow;
}

[StructLayout(LayoutKind.Sequential)]
unsafe struct TimedSurfaceMarkServicesWire
{
    public TexturedSurfaceMarkServicesWire Textured;
    public delegate* unmanaged[Cdecl]<void*, TimedSurfaceMarkWire*, void*, int> Place;
}

[GameModule("poima-test-timed-surface-mark")]
public sealed class TimedSurfaceMarkProbe : Game<ProbeState>, ITimedSurfaceMarkGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 8200;

    public override void Tick(ref ProbeState state, GameContext context)
    {
        foreach (uint lifetime in new[] { 0U, 216001U, uint.MaxValue })
        {
            try { PlaceWorld(context, lifetime); }
            catch (ArgumentOutOfRangeException error) when (error.ParamName == "lifetimeTicks") { continue; }
            throw new Exception("An out-of-range lifetime reached the timed mark callback.");
        }

        try
        {
            PlaceWorld(context, 1, (SurfaceMarkOverflowPolicy)2);
            throw new Exception("An unknown overflow policy reached the timed mark callback.");
        }
        catch (ArgumentOutOfRangeException error) when (error.ParamName == "overflow") { }

        try
        {
            context.PlaceTimedSurfaceMark(new(23, 71), new(10, 20, -30), new(0, -1, 0), 3,
                SurfaceMarkProbe.Style, default, 1);
            throw new Exception("A zero travel direction reached the timed mark callback.");
        }
        catch (ArgumentOutOfRangeException error) when (error.ParamName == "travelDirection") { }

        try
        {
            TexturedSurfaceMarkProbe.PlaceWorld(context, new(43, 81), new(0, 1e-8, 0));
            throw new Exception("The timed marker granted textured placement rights.");
        }
        catch (ArgumentException error) when (error.Message.Contains("Declare ITexturedSurfaceMarkGame")) { }

        context.PlaceTimedSurfaceMark(new(23, 71), new(10, 20, -30), new(0, -1, 0), 3,
            SurfaceMarkProbe.Style, new(0, 1e-8, 0), 1);
        context.PlaceTimedSurfaceMark(new(0, 72), new(1e6, -1e6, .5), new(0, 3, 4), 10000,
            new(new(.005f, 4), .25f, 0, 1, Vector3.One, 1, .045f), new(-1e6, .125, 1e6), 216000,
            SurfaceMarkOverflowPolicy.RecycleOldestTimed, new(24, 75), new(43, 81));
        ++state.Count;
    }

    internal static void PlaceWorld(GameContext context, uint lifetime = 1,
        SurfaceMarkOverflowPolicy overflow = SurfaceMarkOverflowPolicy.Reject) =>
        context.PlaceTimedSurfaceMark(new(23, 71), new(10, 20, -30), new(0, -1, 0), 3,
            SurfaceMarkProbe.Style, new(0, 1e-8, 0), lifetime, overflow);
}

[GameModule("poima-test-timed-surface-mark-constructor")]
public sealed class TimedSurfaceMarkConstructorProbe : Game<ProbeState>, ITimedSurfaceMarkGame
{
    public TimedSurfaceMarkConstructorProbe() => throw new Exception("TIMED MARK CONSTRUCTOR REACHED");
    public override void Initialize(ref ProbeState state) { }
    public override void Tick(ref ProbeState state, GameContext context) { }
}

[GameModule("poima-test-timed-surface-mark-combined")]
public sealed class TimedSurfaceMarkCombinedProbe : Game<ProbeState>,
    ITimedSurfaceMarkGame, ITexturedSurfaceMarkGame, IDirectionalSurfaceMarkGame, ISurfaceMarkGame, IParticleBurstGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 8300;

    public override void Tick(ref ProbeState state, GameContext context)
    {
        context.BurstParticles(new(21, 74), 32);
        SurfaceMarkProbe.PlaceLocal(context);
        DirectionalSurfaceMarkProbe.PlaceWorld(context, new(0, 1e-8, 0));
        TexturedSurfaceMarkProbe.PlaceWorld(context, new(43, 81), new(0, 1e-8, 0));
        TimedSurfaceMarkProbe.PlaceWorld(context);
        ++state.Count;
    }
}

[GameModule("poima-test-timed-surface-mark-v3")]
public sealed class TexturedMarkCallingTimedProbe : Game<ProbeState>, ITexturedSurfaceMarkGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 8400;

    public override void Tick(ref ProbeState state, GameContext context) => TimedSurfaceMarkProbe.PlaceWorld(context);
}

static unsafe partial class Program
{
    const string TimedMarkFeature = "surface_mark_timed_v4";
    static int timedMarkCalls;
    static bool badTimedMarkPayload, refuseTimedMark;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int PlaceTimedSurfaceMark(void* context, TimedSurfaceMarkWire* command, void* error)
    {
        ++timedMarkCalls;
        var directional = &command->Textured.Directional;
        bool world = directional->Mark.Receiver.High == 23;
        var expectedTravel = world ? (0, 1e-8, 0) : (-1e6, .125, 1e6);
        var expectedProfile = world ? (0UL, 0UL) : (43UL, 81UL);
        var expectedLifetime = world ? (1U, 0U) : (216000U, 1U);
        badTimedMarkPayload |= !SurfaceMarkPayloadMatches(&directional->Mark) ||
            (directional->Travel[0], directional->Travel[1], directional->Travel[2]) != expectedTravel ||
            (command->Textured.Profile.High, command->Textured.Profile.Low) != expectedProfile ||
            (command->Lifetime, command->Overflow) != expectedLifetime || (nint)context != 0x1234 ||
            directional->Mark.Version != 4 || directional->Mark.Bytes != 184;
        return refuseTimedMark ? LifecycleError(error) : 0;
    }

    static void TimedSurfaceMarkContract(byte* output, byte* state, Services good, List<string> checks)
    {
        Check(sizeof(TimedSurfaceMarkWire) == 184 && sizeof(TimedSurfaceMarkServicesWire) == 568 &&
            Marshal.OffsetOf<TimedSurfaceMarkWire>(nameof(TimedSurfaceMarkWire.Textured)).ToInt64() == 0 &&
            Marshal.OffsetOf<TimedSurfaceMarkWire>(nameof(TimedSurfaceMarkWire.Lifetime)).ToInt64() == 176 &&
            Marshal.OffsetOf<TimedSurfaceMarkWire>(nameof(TimedSurfaceMarkWire.Overflow)).ToInt64() == 180 &&
            Marshal.OffsetOf<TimedSurfaceMarkServicesWire>(nameof(TimedSurfaceMarkServicesWire.Place)).ToInt64() == 560,
            "Independent timed surface mark layout mismatch.");
        Check(typeof(ControlContext).GetMethod(nameof(GameContext.PlaceTimedSurfaceMark)) == null,
            "Timed placement leaked into Control.");

        Call call = new() { Version = 1, Output = output, OutputCapacity = 65536, State = state, StateBytes = 4, Tick = 123 };
        foreach (var host in new JsonElement?[] { null, SurfaceMarkHost(560, TexturedMarkFeature),
            SurfaceMarkHost(567, TimedMarkFeature), SurfaceMarkHost(568, TexturedMarkFeature) })
        {
            Check(LoadExtension(&call, typeof(TimedSurfaceMarkConstructorProbe), host) != 0 &&
                !Output(output).Contains("TIMED MARK CONSTRUCTOR REACHED"), "Incompatible host entered the timed mark constructor.");
            NoExtensionModules(&call);
        }
        Check(LoadExtension(&call, typeof(TimedSurfaceMarkConstructorProbe), SurfaceMarkHost(576, TimedMarkFeature)) != 0 &&
            Output(output).Contains("TIMED MARK CONSTRUCTOR REACHED"), "A larger compatible prefix was rejected.");
        NoExtensionModules(&call);

        TimedSurfaceMarkServicesWire services = new()
        {
            Textured = new()
            {
                Directional = new()
                {
                    Marks = new() { Particles = new() { Baseline = good, Burst = &PlaceSurfaceParticle }, Place = &PlaceSurfaceMark },
                    Place = &PlaceDirectionalSurfaceMark
                },
                Place = &PlaceTexturedSurfaceMark
            },
            Place = &PlaceTimedSurfaceMark
        };
        services.Textured.Directional.Marks.Particles.Baseline.Bytes = 568;
        new Span<byte>(services.Textured.Directional.Marks.Particles.PreviousExtensions, 352).Fill(0xa5);
        int oldTextured = texturedMarkCalls, oldDirectional = directionalMarkCalls;
        int oldMarks = surfaceMarkCalls, oldBursts = surfaceParticleCalls;
        OpenSurfaceMarkProbe(&call, typeof(TimedSurfaceMarkProbe), SurfaceMarkHost(568, TimedMarkFeature),
            ["baseline_v7", TimedMarkFeature], bytes: 568);
        foreach (int bytes in new[] { 176, 560, 567 })
        {
            using var guard = new GuardedHeader(bytes);
            var baseline = (Services*)guard.Header;
            *baseline = good;
            baseline->Bytes = (uint)bytes;
            call.Services = baseline;
            call.Operation = 3;
            Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 8200 && timedMarkCalls == 0,
                "Short protected service allocation entered timed mark Tick.");
        }
        var missing = services;
        missing.Place = null;
        call.Services = &missing.Textured.Directional.Marks.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 8200 && timedMarkCalls == 0,
            "Missing timed callback entered Tick.");

        call.Services = &services.Textured.Directional.Marks.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0 && ((ProbeState*)state)->Count == 8201 &&
            timedMarkCalls == 2 && !badTimedMarkPayload && texturedMarkCalls == oldTextured &&
            directionalMarkCalls == oldDirectional && surfaceMarkCalls == oldMarks && surfaceParticleCalls == oldBursts, Output(output));
        refuseTimedMark = true;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 8201 &&
            timedMarkCalls == 3 && Output(output).Contains("lifecycle rejected"), "Timed callback rejection was lost.");
        refuseTimedMark = false;
        CloseSurfaceMarkProbe(&call);
        checks.Add("timed surface marks negotiate 568 guarded bytes, send exact 184-byte packets with optional profiles, and validate lifetime and overflow");

        Check(LoadExtension(&call, typeof(TimedSurfaceMarkCombinedProbe),
            SurfaceMarkHost(568, TimedMarkFeature, DirectionalMarkFeature, "surface_mark_v1", "particle_burst_v1")) != 0,
            "Combined host silently supplied the textured mark feature.");
        NoExtensionModules(&call);
        OpenSurfaceMarkProbe(&call, typeof(TimedSurfaceMarkCombinedProbe),
            SurfaceMarkHost(568, "particle_burst_v1", "surface_mark_v1", DirectionalMarkFeature, TexturedMarkFeature, TimedMarkFeature),
            ["baseline_v7", "particle_burst_v1", "surface_mark_v1", DirectionalMarkFeature, TexturedMarkFeature, TimedMarkFeature],
            bytes: 568);
        missing = services;
        missing.Textured.Place = null;
        call.Services = &missing.Textured.Directional.Marks.Particles.Baseline;
        call.Operation = 3;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 8300 && timedMarkCalls == 3 &&
            texturedMarkCalls == oldTextured && directionalMarkCalls == oldDirectional &&
            surfaceMarkCalls == oldMarks && surfaceParticleCalls == oldBursts, "Missing textured callback entered combined Tick.");
        call.Services = &services.Textured.Directional.Marks.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0 && ((ProbeState*)state)->Count == 8301 && timedMarkCalls == 4 &&
            texturedMarkCalls == oldTextured + 1 && directionalMarkCalls == oldDirectional + 1 &&
            surfaceMarkCalls == oldMarks + 1 && surfaceParticleCalls == oldBursts + 1 &&
            !badTimedMarkPayload && !badTexturedMarkPayload && !badDirectionalMarkPayload &&
            !badSurfaceMarkPayload && !badSurfaceParticlePayload, Output(output));
        CloseSurfaceMarkProbe(&call);

        OpenSurfaceMarkProbe(&call, typeof(TexturedMarkCallingTimedProbe),
            SurfaceMarkHost(568, TexturedMarkFeature, TimedMarkFeature), ["baseline_v7", TexturedMarkFeature], bytes: 560);
        using (var guard = new GuardedHeader(560))
        {
            var prefix = (TexturedSurfaceMarkServicesWire*)guard.Header;
            *prefix = services.Textured;
            prefix->Directional.Marks.Particles.Baseline.Bytes = 560;
            call.Services = &prefix->Directional.Marks.Particles.Baseline;
            call.Operation = 3;
            Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && Output(output).Contains("Declare ITimedSurfaceMarkGame") &&
                ((ProbeState*)state)->Count == 8400 && timedMarkCalls == 4,
                "Textured-only module reached a protected timed callback tail.");
        }
        CloseSurfaceMarkProbe(&call);
        checks.Add("timed and earlier mark callbacks compose across independent marker permissions and preserve the 560-byte V3 contract");
    }
}
