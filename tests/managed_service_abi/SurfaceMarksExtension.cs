// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)]
unsafe struct SurfaceMarkWire
{
    public uint Version, Bytes;
    public Id Receiver, Source;
    public fixed double Origin[3];
    public fixed double Direction[3];
    public double Distance;
    public fixed float HalfExtent[2];
    public float HalfDepth, MinimumNormalDot, EdgeFraction;
    public fixed float LinearColor[3];
    public float Opacity, Roughness;
}

[StructLayout(LayoutKind.Sequential)]
unsafe struct SurfaceMarkServicesWire
{
    public ParticleServicesWire Particles;
    public delegate* unmanaged[Cdecl]<void*, SurfaceMarkWire*, void*, int> Place;
}

[GameModule("poima-test-surface-mark")]
public sealed class SurfaceMarkProbe : Game<ProbeState>, ISurfaceMarkGame
{
    internal static SurfaceMarkStyle Style => new(new(.125f, .25f), .03125f, .75f, .25f,
        new(.5f, .25f, .125f), .875f, .625f);

    public override void Initialize(ref ProbeState state) => state.Count = 7300;

    public override void Tick(ref ProbeState state, GameContext context)
    {
        EntityId receiver = new(23, 71);
        Vector3d origin = new(10, 20, -30), direction = new(0, -1, 0);
        Reject(context, default, origin, direction, 3, Style, "receiver");
        Reject(context, receiver, new(double.NaN, 0, 0), direction, 3, Style, "origin");
        Reject(context, receiver, new(0, -1000001, 0), direction, 3, Style, "origin");
        Reject(context, receiver, origin, new(0, 0, 1e-13), 3, Style, "direction");
        Reject(context, receiver, origin, new(double.PositiveInfinity, 0, 0), 3, Style, "direction");
        Reject(context, receiver, origin, new(0, 0, 1000001), 3, Style, "direction");
        Reject(context, receiver, origin, direction, .0005, Style, "distance");
        Reject(context, receiver, origin, direction, 10001, Style, "distance");
        Reject(context, receiver, origin, direction, double.NaN, Style, "distance");
        foreach (SurfaceMarkStyle style in new SurfaceMarkStyle[]
        {
            Style with { HalfExtentMetres = new(.004f, .25f) },
            Style with { HalfExtentMetres = new(.25f, float.NaN) },
            Style with { HalfDepthMetres = .251f },
            Style with { MinimumNormalDot = -1 },
            Style with { EdgeFraction = 1.1f },
            Style with { LinearColor = new(0, 1, float.PositiveInfinity) },
            Style with { Opacity = -.125f },
            Style with { Roughness = .044f },
        })
            Reject(context, receiver, origin, direction, 3, style, "style");

        try { context.BurstParticles(receiver, 1); }
        catch (ArgumentException error) when (error.Message.Contains("Declare IParticleBurstGame"))
        {
            context.PlaceSurfaceMark(receiver, origin, direction, 3, Style);
            PlaceLocal(context);
            ++state.Count;
            return;
        }
        throw new Exception("The surface mark marker unexpectedly granted particle access.");
    }

    internal static void PlaceLocal(GameContext context) => context.PlaceSurfaceMark(new(0, 72), new(1e6, -1e6, .5),
        new(0, 3, 4), 10000, new(new(.005f, 4), .25f, 0, 1, Vector3.One, 1, .045f), new(24, 75));

    private static void Reject(GameContext context, EntityId receiver, Vector3d origin, Vector3d direction,
        double distance, SurfaceMarkStyle style, string parameter)
    {
        try { context.PlaceSurfaceMark(receiver, origin, direction, distance, style); }
        catch (ArgumentException error) when (error.ParamName == parameter) { return; }
        throw new Exception("An invalid surface mark " + parameter + " reached the callback.");
    }
}

[GameModule("poima-test-surface-mark-constructor")]
public sealed class SurfaceMarkConstructorProbe : Game<ProbeState>, ISurfaceMarkGame
{
    public SurfaceMarkConstructorProbe() => throw new Exception("SURFACE MARK CONSTRUCTOR REACHED");
    public override void Initialize(ref ProbeState state) { }
    public override void Tick(ref ProbeState state, GameContext context) { }
}

[GameModule("poima-test-surface-mark-unmarked")]
public sealed class UnmarkedSurfaceMarkProbe : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state) => state.Count = 7400;
    public override void Tick(ref ProbeState state, GameContext context) => SurfaceMarkProbe.PlaceLocal(context);
}

[GameModule("poima-test-surface-mark-particles")]
public sealed class SurfaceMarkParticleProbe : Game<ProbeState>, ISurfaceMarkGame, IParticleBurstGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 7500;
    public override void Tick(ref ProbeState state, GameContext context)
    {
        context.BurstParticles(new(21, 74), 32);
        SurfaceMarkProbe.PlaceLocal(context);
        ++state.Count;
    }
}

static unsafe partial class Program
{
    static int surfaceMarkCalls, surfaceParticleCalls;
    static bool badSurfaceMarkPayload, badSurfaceParticlePayload, refuseSurfaceMark;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int PlaceSurfaceMark(void* context, SurfaceMarkWire* command, void* error)
    {
        ++surfaceMarkCalls;
        badSurfaceMarkPayload |= !SurfaceMarkPayloadMatches(command) || (nint)context != 0x1234 ||
            command->Version != 1 || command->Bytes != 136;
        return refuseSurfaceMark ? LifecycleError(error) : 0;
    }

    static bool SurfaceMarkPayloadMatches(SurfaceMarkWire* command)
    {
        bool world = (command->Receiver.High, command->Receiver.Low) == (23UL, 71UL);
        return world
            ? (command->Source.High, command->Source.Low) == (0UL, 0UL) &&
                (command->Origin[0], command->Origin[1], command->Origin[2]) == (10, 20, -30) &&
                (command->Direction[0], command->Direction[1], command->Direction[2]) == (0, -1, 0) &&
                command->Distance == 3 && (command->HalfExtent[0], command->HalfExtent[1]) == (.125f, .25f) &&
                (command->HalfDepth, command->MinimumNormalDot, command->EdgeFraction) == (.03125f, .75f, .25f) &&
                (command->LinearColor[0], command->LinearColor[1], command->LinearColor[2]) == (.5f, .25f, .125f) &&
                (command->Opacity, command->Roughness) == (.875f, .625f)
            : (command->Receiver.High, command->Receiver.Low) == (0UL, 72UL) &&
                (command->Source.High, command->Source.Low) == (24UL, 75UL) &&
                (command->Origin[0], command->Origin[1], command->Origin[2]) == (1e6, -1e6, .5) &&
                (command->Direction[0], command->Direction[1], command->Direction[2]) == (0, 3, 4) &&
                command->Distance == 10000 && (command->HalfExtent[0], command->HalfExtent[1]) == (.005f, 4) &&
                (command->HalfDepth, command->MinimumNormalDot, command->EdgeFraction) == (.25f, 0, 1) &&
                (command->LinearColor[0], command->LinearColor[1], command->LinearColor[2]) == (1, 1, 1) &&
                (command->Opacity, command->Roughness) == (1, .045f);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int PlaceSurfaceParticle(void* context, ParticleBurstWire* command, void* error)
    {
        ++surfaceParticleCalls;
        badSurfaceParticlePayload |= (nint)context != 0x1234 || command->Version != 1 || command->Bytes != 32 ||
            (command->Emitter.High, command->Emitter.Low, command->Count, command->Reserved) != (21UL, 74UL, 32U, 0U);
        return 0;
    }

    static JsonElement SurfaceMarkHost(uint bytes, params string[] features) =>
        JsonSerializer.SerializeToElement(new
        {
            call_version = 1, call_bytes = 80, services_version = 7, services_bytes = bytes,
            features = new[] { "baseline_v7" }.Concat(features).ToArray()
        });

    static void SurfaceMarkContract(byte* output, byte* state, Services good, List<string> checks)
    {
        GeneratedFeatureUnionContract(checks);
        Check(sizeof(SurfaceMarkWire) == 136 && sizeof(SurfaceMarkServicesWire) == 544 &&
            Marshal.OffsetOf<SurfaceMarkWire>(nameof(SurfaceMarkWire.Source)).ToInt64() == 24 &&
            Marshal.OffsetOf<SurfaceMarkWire>(nameof(SurfaceMarkWire.Origin)).ToInt64() == 40 &&
            Marshal.OffsetOf<SurfaceMarkWire>(nameof(SurfaceMarkWire.Distance)).ToInt64() == 88 &&
            Marshal.OffsetOf<SurfaceMarkWire>(nameof(SurfaceMarkWire.LinearColor)).ToInt64() == 116 &&
            Marshal.OffsetOf<SurfaceMarkWire>(nameof(SurfaceMarkWire.Roughness)).ToInt64() == 132 &&
            Marshal.OffsetOf<SurfaceMarkServicesWire>(nameof(SurfaceMarkServicesWire.Place)).ToInt64() == 536,
            "Independent surface mark layout mismatch.");
        Check(typeof(ControlContext).GetMethod(nameof(GameContext.PlaceSurfaceMark)) == null,
            "Surface mark placement leaked into Control.");

        Call call = new() { Version = 1, Output = output, OutputCapacity = 65536, State = state, StateBytes = 4, Tick = 123 };
        foreach (var host in new JsonElement?[] { null, SurfaceMarkHost(536, "surface_mark_v1"),
                     SurfaceMarkHost(543, "surface_mark_v1"), SurfaceMarkHost(544, "particle_burst_v1") })
        {
            Check(LoadExtension(&call, typeof(SurfaceMarkConstructorProbe), host) != 0 &&
                !Output(output).Contains("SURFACE MARK CONSTRUCTOR REACHED"), "Incompatible host entered surface mark constructor.");
            NoExtensionModules(&call);
        }
        Check(LoadExtension(&call, typeof(SurfaceMarkConstructorProbe), SurfaceMarkHost(552, "surface_mark_v1")) != 0 &&
            Output(output).Contains("SURFACE MARK CONSTRUCTOR REACHED"), "A larger compatible prefix was rejected.");
        NoExtensionModules(&call);

        SurfaceMarkServicesWire services = new() { Particles = new() { Baseline = good, Burst = &PlaceSurfaceParticle },
            Place = &PlaceSurfaceMark };
        services.Particles.Baseline.Bytes = 544;
        new Span<byte>(services.Particles.PreviousExtensions, 352).Fill(0xa5);

        OpenSurfaceMarkProbe(&call, typeof(SurfaceMarkProbe), SurfaceMarkHost(544, "surface_mark_v1"),
            ["baseline_v7", "surface_mark_v1"]);
        foreach (int bytes in new[] { 176, 536, 543 })
        {
            using var guard = new GuardedHeader(bytes);
            var baseline = (Services*)guard.Header;
            *baseline = good;
            baseline->Bytes = (uint)bytes;
            call.Services = baseline;
            call.Operation = 3;
            Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7300 && surfaceMarkCalls == 0,
                "Short protected service allocation entered surface mark Tick.");
        }
        var missing = services;
        missing.Place = null;
        call.Services = &missing.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7300 && surfaceMarkCalls == 0,
            "Missing surface mark callback entered Tick.");

        call.Services = &services.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0 && ((ProbeState*)state)->Count == 7301 &&
            surfaceMarkCalls == 2 && surfaceParticleCalls == 0 && !badSurfaceMarkPayload, Output(output));
        refuseSurfaceMark = true;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7301 &&
            surfaceMarkCalls == 3 && Output(output).Contains("lifecycle rejected"), "Surface mark callback rejection was lost.");
        refuseSurfaceMark = false;
        CloseSurfaceMarkProbe(&call);

        Check(LoadExtension(&call, typeof(SurfaceMarkParticleProbe), SurfaceMarkHost(544, "surface_mark_v1")) != 0,
            "The surface mark feature silently supplied the particle marker.");
        NoExtensionModules(&call);
        OpenSurfaceMarkProbe(&call, typeof(SurfaceMarkParticleProbe), SurfaceMarkHost(544, "surface_mark_v1", "particle_burst_v1"),
            ["baseline_v7", "particle_burst_v1", "surface_mark_v1"]);
        missing = services;
        missing.Particles.Burst = null;
        call.Services = &missing.Particles.Baseline;
        call.Operation = 3;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7500 &&
            surfaceMarkCalls == 3 && surfaceParticleCalls == 0, "Missing particle callback entered combined Tick.");
        call.Services = &services.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0 && ((ProbeState*)state)->Count == 7501 &&
            surfaceMarkCalls == 4 && surfaceParticleCalls == 1 && !badSurfaceMarkPayload && !badSurfaceParticlePayload,
            Output(output));
        CloseSurfaceMarkProbe(&call);

        OpenSurfaceMarkProbe(&call, typeof(UnmarkedSurfaceMarkProbe), null, ["baseline_v7"], bytes: 176);
        using (var guard = new GuardedHeader(176))
        {
            *(Services*)guard.Header = good;
            call.Services = (Services*)guard.Header;
            call.Operation = 3;
            Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && Output(output).Contains("Declare ISurfaceMarkGame") &&
                ((ProbeState*)state)->Count == 7400 && surfaceMarkCalls == 4, "Unmarked game accessed the protected surface mark tail.");
        }
        CloseSurfaceMarkProbe(&call);
        checks.Add("surface marks independently negotiate 544 bytes, transmit 136-byte world and source-local commands, validate bounds and preserve particle permissions");
    }

    static void GeneratedFeatureUnionContract(List<string> checks)
    {
        var assembly = typeof(GameContext).Assembly;
        var requirements = assembly.GetType("Poima.GameplayRequirements", throwOnError: true)!;
        var features = assembly.GetType("Poima.GameplayRequiredFeatures", throwOnError: true)!;
        const BindingFlags internals = BindingFlags.NonPublic | BindingFlags.Static;
        var serialize = requirements.GetMethod("Json", internals, [features])!;
        var parse = requirements.GetMethod("DeclaredFeatures", internals)!;
        ulong mask = 0;
        foreach (var feature in Enum.GetValues(features)) mask |= Convert.ToUInt64(feature);
        var manifest = (string)serialize.Invoke(null, [Enum.ToObject(features, mask)])!;
        using var document = JsonDocument.Parse(manifest);
        var root = document.RootElement;
        var names = root.GetProperty("features");
        int count = names.GetArrayLength();
        Check(count > 24 && count <= 64 && root.GetProperty("services_bytes").GetInt32() == 568 &&
            names[count - 4].GetString() == "surface_mark_v1" &&
            names[count - 3].GetString() == "surface_mark_direction_v2" &&
            names[count - 2].GetString() == "surface_mark_texture_v3" &&
            names[count - 1].GetString() == "surface_mark_timed_v4", "Generated surface mark feature-union mismatch.");
        var parsed = parse.Invoke(null, [manifest]);
        Check(Convert.ToUInt64(parsed) == mask && Equals(serialize.Invoke(null, [parsed]), manifest),
            "Generated feature union failed the Native AOT manifest round trip.");

        foreach (string invalid in new[] { "\"unknown_v1\"", "\"baseline_v7\"",
                     string.Join(",", Enumerable.Repeat("\"surface_mark_v1\"", 65)) })
        {
            try
            {
                parse.Invoke(null, [manifest.Replace("\"surface_mark_v1\"", invalid, StringComparison.Ordinal)]);
            }
            catch (TargetInvocationException error) when (error.InnerException is ArgumentException)
            {
                continue;
            }
            throw new Exception("Native AOT manifest accepted an unknown, duplicate or over-limit feature list.");
        }
        checks.Add($"Native AOT manifest round trip with {count} composed feature names and rejection of unknown, duplicate and over-limit names");
    }

    static void OpenSurfaceMarkProbe(Call* call, Type game, JsonElement? host, string[] features, uint bytes = 544)
    {
        Check(LoadExtension(call, game, host) == 0, Output(call->Output));
        using var manifest = JsonDocument.Parse(Output(call->Output));
        call->Handle = manifest.RootElement.GetProperty("handle").GetUInt64();
        bool advertisesRequirements = manifest.RootElement.TryGetProperty("requirements", out var declared);
        Check(advertisesRequirements == (bytes > 176), "Surface mark probe changed the legacy baseline manifest shape.");
        if (advertisesRequirements)
            Check(declared.GetProperty("services_bytes").GetUInt32() == bytes &&
                declared.GetProperty("features").EnumerateArray().Select(feature => feature.GetString()).SequenceEqual(features),
                "Surface mark probe requirements lost the service prefix or named features.");
        call->Operation = 2;
        Check(Entry.Invoke((nint)call, sizeof(Call)) == 0, Output(call->Output));
    }

    static void CloseSurfaceMarkProbe(Call* call)
    {
        call->Operation = 4;
        Check(Entry.Invoke((nint)call, sizeof(Call)) == 0, Output(call->Output));
        NoExtensionModules(call);
    }
}
