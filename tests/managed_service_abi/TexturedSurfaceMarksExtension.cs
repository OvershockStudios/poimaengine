// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)]
unsafe struct TexturedSurfaceMarkWire
{
    public DirectionalSurfaceMarkWire Directional;
    public Id Profile;
}

[StructLayout(LayoutKind.Sequential)]
unsafe struct TexturedSurfaceMarkServicesWire
{
    public DirectionalSurfaceMarkServicesWire Directional;
    public delegate* unmanaged[Cdecl]<void*, TexturedSurfaceMarkWire*, void*, int> Place;
}

[GameModule("poima-test-textured-surface-mark")]
public sealed class TexturedSurfaceMarkProbe : Game<ProbeState>, ITexturedSurfaceMarkGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 7900;

    public override void Tick(ref ProbeState state, GameContext context)
    {
        try
        {
            PlaceWorld(context, default, new(0, 1e-8, 0));
            throw new Exception("A zero profile reached the textured mark callback.");
        }
        catch (ArgumentException error) when (error.ParamName == "profile") { }

        foreach (Vector3d travel in new Vector3d[] { default, new(0, 0, -1e-9), new(1e6 + 1, 0, 0), new(1, 0, double.NaN) })
        {
            try { PlaceWorld(context, new(43, 81), travel); }
            catch (ArgumentOutOfRangeException error) when (error.ParamName == "travelDirection") { continue; }
            throw new Exception("Invalid travel direction reached the textured mark callback.");
        }

        try
        {
            DirectionalSurfaceMarkProbe.PlaceWorld(context, new(0, 1e-8, 0));
            throw new Exception("The textured marker granted directional placement rights.");
        }
        catch (ArgumentException error) when (error.Message.Contains("Declare IDirectionalSurfaceMarkGame")) { }
        try
        {
            SurfaceMarkProbe.PlaceLocal(context);
            throw new Exception("The textured marker granted V1 placement rights.");
        }
        catch (ArgumentException error) when (error.Message.Contains("Declare ISurfaceMarkGame")) { }

        PlaceWorld(context, new(43, 81), new(0, 1e-8, 0));
        context.PlaceTexturedSurfaceMark(new(0, 82), new(0, 72), new(1e6, -1e6, .5), new(0, 3, 4), 10000,
            new(new(.005f, 4), .25f, 0, 1, Vector3.One, 1, .045f), new(-1e6, .125, 1e6), new(24, 75));
        ++state.Count;
    }

    internal static void PlaceWorld(GameContext context, EntityId profile, Vector3d travel) =>
        context.PlaceTexturedSurfaceMark(profile, new(23, 71), new(10, 20, -30), new(0, -1, 0), 3,
            SurfaceMarkProbe.Style, travel);
}

[GameModule("poima-test-textured-surface-mark-constructor")]
public sealed class TexturedSurfaceMarkConstructorProbe : Game<ProbeState>, ITexturedSurfaceMarkGame
{
    public TexturedSurfaceMarkConstructorProbe() => throw new Exception("TEXTURED MARK CONSTRUCTOR REACHED");
    public override void Initialize(ref ProbeState state) { }
    public override void Tick(ref ProbeState state, GameContext context) { }
}

[GameModule("poima-test-textured-surface-mark-combined")]
public sealed class TexturedSurfaceMarkCombinedProbe : Game<ProbeState>,
    ITexturedSurfaceMarkGame, IDirectionalSurfaceMarkGame, ISurfaceMarkGame, IParticleBurstGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 8000;

    public override void Tick(ref ProbeState state, GameContext context)
    {
        context.BurstParticles(new(21, 74), 32);
        SurfaceMarkProbe.PlaceLocal(context);
        DirectionalSurfaceMarkProbe.PlaceWorld(context, new(0, 1e-8, 0));
        TexturedSurfaceMarkProbe.PlaceWorld(context, new(43, 81), new(0, 1e-8, 0));
        ++state.Count;
    }
}

[GameModule("poima-test-textured-surface-mark-v2")]
public sealed class DirectionalMarkCallingTexturedProbe : Game<ProbeState>, IDirectionalSurfaceMarkGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 8100;

    public override void Tick(ref ProbeState state, GameContext context) =>
        TexturedSurfaceMarkProbe.PlaceWorld(context, new(43, 81), new(0, 1e-8, 0));
}

static unsafe partial class Program
{
    const string TexturedMarkFeature = "surface_mark_texture_v3";
    static int texturedMarkCalls;
    static bool badTexturedMarkPayload, refuseTexturedMark;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int PlaceTexturedSurfaceMark(void* context, TexturedSurfaceMarkWire* command, void* error)
    {
        ++texturedMarkCalls;
        bool world = command->Directional.Mark.Receiver.High == 23;
        var expectedTravel = world ? (0, 1e-8, 0) : (-1e6, .125, 1e6);
        var expectedProfile = world ? (43UL, 81UL) : (0UL, 82UL);
        badTexturedMarkPayload |= !SurfaceMarkPayloadMatches(&command->Directional.Mark) ||
            (command->Directional.Travel[0], command->Directional.Travel[1], command->Directional.Travel[2]) != expectedTravel ||
            (command->Profile.High, command->Profile.Low) != expectedProfile || (nint)context != 0x1234 ||
            command->Directional.Mark.Version != 3 || command->Directional.Mark.Bytes != 176;
        return refuseTexturedMark ? LifecycleError(error) : 0;
    }

    static void TexturedSurfaceMarkContract(byte* output, byte* state, Services good, List<string> checks)
    {
        Check(sizeof(TexturedSurfaceMarkWire) == 176 && sizeof(TexturedSurfaceMarkServicesWire) == 560 &&
            Marshal.OffsetOf<TexturedSurfaceMarkWire>(nameof(TexturedSurfaceMarkWire.Directional)).ToInt64() == 0 &&
            Marshal.OffsetOf<TexturedSurfaceMarkWire>(nameof(TexturedSurfaceMarkWire.Profile)).ToInt64() == 160 &&
            Marshal.OffsetOf<TexturedSurfaceMarkServicesWire>(nameof(TexturedSurfaceMarkServicesWire.Place)).ToInt64() == 552,
            "Independent textured surface mark layout mismatch.");
        Check(typeof(ControlContext).GetMethod(nameof(GameContext.PlaceTexturedSurfaceMark)) == null,
            "Textured placement leaked into Control.");

        Call call = new() { Version = 1, Output = output, OutputCapacity = 65536, State = state, StateBytes = 4, Tick = 123 };
        foreach (var host in new JsonElement?[] { null, SurfaceMarkHost(552, DirectionalMarkFeature),
            SurfaceMarkHost(559, TexturedMarkFeature), SurfaceMarkHost(560, DirectionalMarkFeature) })
        {
            Check(LoadExtension(&call, typeof(TexturedSurfaceMarkConstructorProbe), host) != 0 &&
                !Output(output).Contains("TEXTURED MARK CONSTRUCTOR REACHED"), "Incompatible host entered the textured mark constructor.");
            NoExtensionModules(&call);
        }
        Check(LoadExtension(&call, typeof(TexturedSurfaceMarkConstructorProbe), SurfaceMarkHost(568, TexturedMarkFeature)) != 0 &&
            Output(output).Contains("TEXTURED MARK CONSTRUCTOR REACHED"), "A larger compatible prefix was rejected.");
        NoExtensionModules(&call);

        TexturedSurfaceMarkServicesWire services = new()
        {
            Directional = new()
            {
                Marks = new() { Particles = new() { Baseline = good, Burst = &PlaceSurfaceParticle }, Place = &PlaceSurfaceMark },
                Place = &PlaceDirectionalSurfaceMark
            },
            Place = &PlaceTexturedSurfaceMark
        };
        services.Directional.Marks.Particles.Baseline.Bytes = 560;
        new Span<byte>(services.Directional.Marks.Particles.PreviousExtensions, 352).Fill(0xa5);
        int oldDirectional = directionalMarkCalls, oldMarks = surfaceMarkCalls, oldBursts = surfaceParticleCalls;
        OpenSurfaceMarkProbe(&call, typeof(TexturedSurfaceMarkProbe), SurfaceMarkHost(560, TexturedMarkFeature),
            ["baseline_v7", TexturedMarkFeature], bytes: 560);
        foreach (int bytes in new[] { 176, 544, 552, 559 })
        {
            using var guard = new GuardedHeader(bytes);
            var baseline = (Services*)guard.Header;
            *baseline = good;
            baseline->Bytes = (uint)bytes;
            call.Services = baseline;
            call.Operation = 3;
            Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7900 && texturedMarkCalls == 0,
                "Short protected service allocation entered textured mark Tick.");
        }
        var missing = services;
        missing.Place = null;
        call.Services = &missing.Directional.Marks.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7900 && texturedMarkCalls == 0,
            "Missing textured callback entered Tick.");

        call.Services = &services.Directional.Marks.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0 && ((ProbeState*)state)->Count == 7901 &&
            texturedMarkCalls == 2 && !badTexturedMarkPayload && directionalMarkCalls == oldDirectional &&
            surfaceMarkCalls == oldMarks && surfaceParticleCalls == oldBursts, Output(output));
        refuseTexturedMark = true;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7901 &&
            texturedMarkCalls == 3 && Output(output).Contains("lifecycle rejected"), "Textured callback rejection was lost.");
        refuseTexturedMark = false;
        CloseSurfaceMarkProbe(&call);
        checks.Add("textured surface marks negotiate 560 protected bytes, transmit 176-byte world and source-local packets and reject invalid profile IDs or travel");

        Check(LoadExtension(&call, typeof(TexturedSurfaceMarkCombinedProbe),
            SurfaceMarkHost(560, TexturedMarkFeature, "surface_mark_v1", "particle_burst_v1")) != 0,
            "Combined host silently supplied the directional mark feature.");
        NoExtensionModules(&call);
        OpenSurfaceMarkProbe(&call, typeof(TexturedSurfaceMarkCombinedProbe),
            SurfaceMarkHost(560, "particle_burst_v1", "surface_mark_v1", DirectionalMarkFeature, TexturedMarkFeature),
            ["baseline_v7", "particle_burst_v1", "surface_mark_v1", DirectionalMarkFeature, TexturedMarkFeature], bytes: 560);
        missing = services;
        missing.Directional.Place = null;
        call.Services = &missing.Directional.Marks.Particles.Baseline;
        call.Operation = 3;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 8000 && texturedMarkCalls == 3 &&
            directionalMarkCalls == oldDirectional && surfaceMarkCalls == oldMarks && surfaceParticleCalls == oldBursts,
            "Missing directional callback entered combined Tick.");
        call.Services = &services.Directional.Marks.Particles.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0 && ((ProbeState*)state)->Count == 8001 && texturedMarkCalls == 4 &&
            directionalMarkCalls == oldDirectional + 1 && surfaceMarkCalls == oldMarks + 1 && surfaceParticleCalls == oldBursts + 1 &&
            !badTexturedMarkPayload && !badDirectionalMarkPayload && !badSurfaceMarkPayload && !badSurfaceParticlePayload, Output(output));
        CloseSurfaceMarkProbe(&call);

        OpenSurfaceMarkProbe(&call, typeof(DirectionalMarkCallingTexturedProbe),
            SurfaceMarkHost(560, DirectionalMarkFeature, TexturedMarkFeature), ["baseline_v7", DirectionalMarkFeature], bytes: 552);
        using (var guard = new GuardedHeader(552))
        {
            var prefix = (DirectionalSurfaceMarkServicesWire*)guard.Header;
            *prefix = services.Directional;
            prefix->Marks.Particles.Baseline.Bytes = 552;
            call.Services = &prefix->Marks.Particles.Baseline;
            call.Operation = 3;
            Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && Output(output).Contains("Declare ITexturedSurfaceMarkGame") &&
                ((ProbeState*)state)->Count == 8100 && texturedMarkCalls == 4,
                "Directional-only module reached a protected textured callback tail.");
        }
        CloseSurfaceMarkProbe(&call);
        checks.Add("textured, directional and V1 marks compose with particle bursts while each marker grants its own callback");
    }
}
