// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)]
unsafe struct DirectionalSurfaceMarkWire
{
    public SurfaceMarkWire Mark;
    public fixed double Travel[3];
}

[StructLayout(LayoutKind.Sequential)]
unsafe struct DirectionalSurfaceMarkServicesWire
{
    public SurfaceMarkServicesWire Marks;
    public delegate* unmanaged[Cdecl]<void*, DirectionalSurfaceMarkWire*, void*, int> Place;
}

[GameModule("poima-test-directional-surface-mark")]
public sealed class DirectionalSurfaceMarkProbe : Game<ProbeState>, IDirectionalSurfaceMarkGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 7600;

    public override void Tick(ref ProbeState state, GameContext context)
    {
        foreach (Vector3d travel in new Vector3d[]
        {
            default, new(0, 0, -1e-9), new(1e6 + 1, 0, 0),
            new(1, double.NaN, 1), new(0, 0, double.NegativeInfinity)
        })
        {
            try { PlaceWorld(context, travel); }
            catch (ArgumentOutOfRangeException error) when (error.ParamName == "travelDirection") { continue; }
            throw new Exception("Invalid travel direction reached the callback.");
        }
        try
        {
            SurfaceMarkProbe.PlaceLocal(context);
            throw new Exception("Directional placement granted V1 placement rights.");
        }
        catch (ArgumentException error) when (error.Message.Contains("Declare ISurfaceMarkGame")) { }
        try
        {
            context.BurstParticles(new(21, 74), 32);
            throw new Exception("Directional placement granted particle burst rights.");
        }
        catch (ArgumentException error) when (error.Message.Contains("Declare IParticleBurstGame")) { }

        // A valid direction parallel to the expected normal exercises the native fallback path.
        PlaceWorld(context, new(0, 1e-8, 0));
        context.PlaceDirectionalSurfaceMark(new(0, 72), new(1e6, -1e6, .5), new(0, 3, 4), 10000,
            new(new(.005f, 4), .25f, 0, 1, Vector3.One, 1, .045f), new(-1e6, .125, 1e6), new(24, 75));
        ++state.Count;
    }

    internal static void PlaceWorld(GameContext context, Vector3d travel) =>
        context.PlaceDirectionalSurfaceMark(new(23, 71), new(10, 20, -30), new(0, -1, 0), 3,
            SurfaceMarkProbe.Style, travel);
}

[GameModule("poima-test-directional-surface-mark-constructor")]
public sealed class DirectionalSurfaceMarkConstructorProbe : Game<ProbeState>, IDirectionalSurfaceMarkGame
{
    public DirectionalSurfaceMarkConstructorProbe() => throw new Exception("DIRECTIONAL MARK CONSTRUCTOR REACHED");
    public override void Initialize(ref ProbeState state) { }
    public override void Tick(ref ProbeState state, GameContext context) { }
}

[GameModule("poima-test-directional-surface-mark-combined")]
public sealed class DirectionalSurfaceMarkCombinedProbe : Game<ProbeState>,
    IDirectionalSurfaceMarkGame, ISurfaceMarkGame, IParticleBurstGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 7700;
    public override void Tick(ref ProbeState state, GameContext context)
    {
        context.BurstParticles(new(21, 74), 32);
        SurfaceMarkProbe.PlaceLocal(context);
        DirectionalSurfaceMarkProbe.PlaceWorld(context, new(0, 1e-8, 0));
        ++state.Count;
    }
}

[GameModule("poima-test-directional-surface-mark-v1")]
public sealed class SurfaceMarkV1CallingDirectionalProbe : Game<ProbeState>, ISurfaceMarkGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 7800;
    public override void Tick(ref ProbeState state, GameContext context) =>
        DirectionalSurfaceMarkProbe.PlaceWorld(context, new(0, 1e-8, 0));
}

static unsafe partial class Program
{
    const string DirectionalMarkFeature = "surface_mark_direction_v2";
    static int directionalMarkCalls;
    static bool badDirectionalMarkPayload, refuseDirectionalMark;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int PlaceDirectionalSurfaceMark(void* context, DirectionalSurfaceMarkWire* command, void* error)
    {
        ++directionalMarkCalls;
        var travel = (command->Travel[0], command->Travel[1], command->Travel[2]);
        var expected = command->Mark.Receiver.High == 23 ? (0, 1e-8, 0) : (-1e6, .125, 1e6);
        badDirectionalMarkPayload |= !SurfaceMarkPayloadMatches(&command->Mark) || travel != expected ||
            (nint)context != 0x1234 || command->Mark.Version != 2 || command->Mark.Bytes != 160;
        return refuseDirectionalMark ? LifecycleError(error) : 0;
    }

    static void DirectionalSurfaceMarkContract(byte* output, byte* state, Services good, List<string> checks)
    {
        Check(sizeof(DirectionalSurfaceMarkWire) == 160 && sizeof(DirectionalSurfaceMarkServicesWire) == 552 &&
            Marshal.OffsetOf<DirectionalSurfaceMarkWire>(nameof(DirectionalSurfaceMarkWire.Mark)).ToInt64() == 0 &&
            Marshal.OffsetOf<DirectionalSurfaceMarkWire>(nameof(DirectionalSurfaceMarkWire.Travel)).ToInt64() == 136 &&
            Marshal.OffsetOf<DirectionalSurfaceMarkServicesWire>(nameof(DirectionalSurfaceMarkServicesWire.Place)).ToInt64() == 544,
            "Independent directional surface mark layout mismatch.");
        Check(typeof(ControlContext).GetMethod(nameof(GameContext.PlaceDirectionalSurfaceMark)) == null,
            "Directional placement leaked into Control.");

        Call call = new() { Version = 1, Output = output, OutputCapacity = 65536, State = state, StateBytes = 4, Tick = 123 };
        foreach (var host in new JsonElement?[]
        {
            null, SurfaceMarkHost(544, "surface_mark_v1"), SurfaceMarkHost(551, DirectionalMarkFeature),
            SurfaceMarkHost(552, "surface_mark_v1")
        })
        {
            Check(LoadExtension(&call, typeof(DirectionalSurfaceMarkConstructorProbe), host) != 0 &&
                !Output(output).Contains("DIRECTIONAL MARK CONSTRUCTOR REACHED"),
                "Incompatible host entered directional mark constructor.");
            NoExtensionModules(&call);
        }
        Check(LoadExtension(&call, typeof(DirectionalSurfaceMarkConstructorProbe), SurfaceMarkHost(560, DirectionalMarkFeature)) != 0 &&
            Output(output).Contains("DIRECTIONAL MARK CONSTRUCTOR REACHED"), "A larger compatible prefix was rejected.");
        NoExtensionModules(&call);

        DirectionalSurfaceMarkServicesWire services = new()
        {
            Marks = new() { Particles = new() { Baseline = good, Burst = &PlaceSurfaceParticle }, Place = &PlaceSurfaceMark },
            Place = &PlaceDirectionalSurfaceMark
        };
        services.Marks.Particles.Baseline.Bytes = 552;
        new Span<byte>(services.Marks.Particles.PreviousExtensions, 352).Fill(0xa5);
        int oldMarks = surfaceMarkCalls, oldBursts = surfaceParticleCalls;

        OpenSurfaceMarkProbe(&call, typeof(DirectionalSurfaceMarkProbe), SurfaceMarkHost(552, DirectionalMarkFeature),
            ["baseline_v7", DirectionalMarkFeature], bytes: 552);
        foreach (int bytes in new[] { 176, 544, 551 })
        {
            using var guard = new GuardedHeader(bytes);
            var baseline = (Services*)guard.Header;
            *baseline = good;
            baseline->Bytes = (uint)bytes;
            call.Services = baseline;
            call.Operation = 3;
            Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7600 && directionalMarkCalls == 0,
                "Short protected service allocation entered directional mark Tick.");
        }
        var missing = services;
        missing.Place = null;
        call.Services = &missing.Marks.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7600 && directionalMarkCalls == 0,
            "Missing directional callback entered Tick.");

        call.Services = &services.Marks.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0 && ((ProbeState*)state)->Count == 7601 &&
            directionalMarkCalls == 2 && !badDirectionalMarkPayload && surfaceMarkCalls == oldMarks &&
            surfaceParticleCalls == oldBursts, Output(output));
        refuseDirectionalMark = true;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7601 &&
            directionalMarkCalls == 3 && Output(output).Contains("lifecycle rejected"), "Directional callback rejection was lost.");
        refuseDirectionalMark = false;
        CloseSurfaceMarkProbe(&call);
        checks.Add("directional marks independently negotiate 552 bytes, transmit exact 160-byte world and source-local commands and reject invalid travel");

        Check(LoadExtension(&call, typeof(DirectionalSurfaceMarkCombinedProbe),
            SurfaceMarkHost(552, DirectionalMarkFeature, "particle_burst_v1")) != 0,
            "Combined host silently supplied the V1 surface mark feature.");
        NoExtensionModules(&call);
        OpenSurfaceMarkProbe(&call, typeof(DirectionalSurfaceMarkCombinedProbe),
            SurfaceMarkHost(552, "particle_burst_v1", "surface_mark_v1", DirectionalMarkFeature),
            ["baseline_v7", "particle_burst_v1", "surface_mark_v1", DirectionalMarkFeature], bytes: 552);
        missing = services;
        missing.Marks.Place = null;
        call.Services = &missing.Marks.Particles.Baseline;
        call.Operation = 3;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7700 &&
            directionalMarkCalls == 3 && surfaceMarkCalls == oldMarks && surfaceParticleCalls == oldBursts,
            "Missing V1 callback entered combined Tick.");
        call.Services = &services.Marks.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0 && ((ProbeState*)state)->Count == 7701 &&
            directionalMarkCalls == 4 && surfaceMarkCalls == oldMarks + 1 && surfaceParticleCalls == oldBursts + 1 &&
            !badDirectionalMarkPayload && !badSurfaceMarkPayload && !badSurfaceParticlePayload, Output(output));
        CloseSurfaceMarkProbe(&call);

        OpenSurfaceMarkProbe(&call, typeof(SurfaceMarkV1CallingDirectionalProbe),
            SurfaceMarkHost(552, "surface_mark_v1", DirectionalMarkFeature), ["baseline_v7", "surface_mark_v1"]);
        call.Services = &services.Marks.Particles.Baseline;
        call.Operation = 3;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && Output(output).Contains("Declare IDirectionalSurfaceMarkGame") &&
            ((ProbeState*)state)->Count == 7800 && directionalMarkCalls == 4,
            "V1 marker improperly gave access to directional placement.");
        CloseSurfaceMarkProbe(&call);
        checks.Add("directional placement, V1 placement and particle bursts compose without granting undeclared callbacks");
    }
}
