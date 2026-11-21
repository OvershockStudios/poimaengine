// SPDX-License-Identifier: Apache-2.0
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)]
struct ParticleBurstWire
{
    public uint Version, Bytes;
    public Id Emitter;
    public uint Count, Reserved;
}

[StructLayout(LayoutKind.Sequential)]
unsafe struct ParticleServicesWire
{
    public Services Baseline;
    public fixed byte PreviousExtensions[352];
    public delegate* unmanaged[Cdecl]<void*, ParticleBurstWire*, void*, int> Burst;
}

[GameModule("poima-test-particle-bursts")]
public sealed class ParticleBurstProbe : Game<ProbeState>, IParticleBurstGame
{
    public override void Initialize(ref ProbeState state) => state.Count = 7100;

    public override void Tick(ref ProbeState state, GameContext context)
    {
        bool rejected = false;
        try { context.BurstParticles(default, 1); }
        catch (ArgumentException error) { rejected = error.ParamName == "emitter"; }
        if (!rejected) throw new Exception("A zero emitter ID reached the host.");

        foreach (uint count in new[] { 0u, 1025u, uint.MaxValue })
        {
            rejected = false;
            try { context.BurstParticles(new(11, 71), count); }
            catch (ArgumentOutOfRangeException error) { rejected = error.ParamName == "count"; }
            if (!rejected) throw new Exception("An invalid particle count reached the host.");
        }

        rejected = false;
        try { _ = context.GetItemAttachment(new(11, 71)); }
        catch (ArgumentException error) { rejected = error.Message.Contains("Declare IItemAttachmentGame"); }
        if (!rejected) throw new Exception("The particle prefix granted item attachment access.");

        context.BurstParticles(new(11, 71), 1);
        context.BurstParticles(new(0, 72), 1024);
        ++state.Count;
    }
}

[GameModule("poima-test-particle-constructor")]
public sealed class ParticleBurstConstructorProbe : Game<ProbeState>, IParticleBurstGame
{
    public ParticleBurstConstructorProbe() => throw new Exception("PARTICLE CONSTRUCTOR REACHED");
    public override void Initialize(ref ProbeState state) { }
    public override void Tick(ref ProbeState state, GameContext context) { }
}

[GameModule("poima-test-particle-unmarked")]
public sealed class UnmarkedParticleBurstProbe : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state) => state.Count = 7200;
    public override void Tick(ref ProbeState state, GameContext context) => context.BurstParticles(new(11, 71), 1);
}

static unsafe partial class Program
{
    static int particleBurstCalls;
    static bool particleBurstBadPayload, particleBurstReject;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int ParticleBurst(void* context, ParticleBurstWire* command, void* error)
    {
        ++particleBurstCalls;
        if ((nint)context != 0x1234 || command->Version != 1 || command->Bytes != 32 || command->Reserved != 0 ||
            (command->Emitter.High, command->Emitter.Low, command->Count) is not ((11, 71, 1) or (0, 72, 1024)))
            particleBurstBadPayload = true;
        return particleBurstReject ? LifecycleError(error) : 0;
    }

    static JsonElement ParticleHost(uint bytes = 536, bool bursts = true)
    {
        string[] features = bursts ? ["baseline_v7", "particle_burst_v1"] : ["baseline_v7"];
        return JsonSerializer.SerializeToElement(new { call_version = 1, call_bytes = 80, services_version = 7,
            services_bytes = bytes, features });
    }

    static void ParticleBurstContract(byte* output, byte* state, Services good, List<string> checks)
    {
        Check(sizeof(ParticleBurstWire) == 32 && sizeof(ParticleServicesWire) == 536 &&
            Marshal.OffsetOf<ParticleBurstWire>(nameof(ParticleBurstWire.Emitter)).ToInt64() == 8 &&
            Marshal.OffsetOf<ParticleBurstWire>(nameof(ParticleBurstWire.Count)).ToInt64() == 24 &&
            Marshal.OffsetOf<ParticleBurstWire>(nameof(ParticleBurstWire.Reserved)).ToInt64() == 28 &&
            Marshal.OffsetOf<ParticleServicesWire>(nameof(ParticleServicesWire.Burst)).ToInt64() == 528,
            "Independent particle burst layout mismatch.");
        Check(typeof(ControlContext).GetMethod(nameof(GameContext.BurstParticles)) == null, "Particle burst API leaked into Control.");

        Call call = new() { Version = 1, Output = output, OutputCapacity = 65536, State = state, StateBytes = 4, Tick = 123 };
        foreach (var host in new JsonElement?[] { null, ParticleHost(528), ParticleHost(535), ParticleHost(bursts: false) })
        {
            Check(LoadExtension(&call, typeof(ParticleBurstConstructorProbe), host) != 0 &&
                !Output(output).Contains("PARTICLE CONSTRUCTOR REACHED"), "Incompatible particle host entered constructor.");
            NoExtensionModules(&call);
        }
        Check(LoadExtension(&call, typeof(ParticleBurstConstructorProbe), ParticleHost(544)) != 0 &&
            Output(output).Contains("PARTICLE CONSTRUCTOR REACHED"), "Compatible particle host was rejected before constructor.");
        NoExtensionModules(&call);

        Check(LoadExtension(&call, typeof(ParticleBurstProbe), ParticleHost()) == 0, Output(output));
        using (var manifest = JsonDocument.Parse(Output(output)))
        {
            call.Handle = manifest.RootElement.GetProperty("handle").GetUInt64();
            var requirements = manifest.RootElement.GetProperty("requirements");
            Check(requirements.GetProperty("services_bytes").GetUInt32() == 536 &&
                requirements.GetProperty("features").EnumerateArray().Select(feature => feature.GetString())
                    .SequenceEqual(new[] { "baseline_v7", "particle_burst_v1" }), "Burst declared unrelated extensions.");
        }
        call.Operation = 2;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0, Output(output));

        ParticleServicesWire services = new() { Baseline = good, Burst = &ParticleBurst };
        services.Baseline.Bytes = 536;
        new Span<byte>(services.PreviousExtensions, 352).Fill(0xa5);
        call.Operation = 3;
        foreach (int bytes in new[] { 176, 528, 535 })
        {
            using var guard = new GuardedHeader(bytes);
            var baseline = (Services*)guard.Header;
            *baseline = good;
            baseline->Bytes = (uint)bytes;
            call.Services = baseline;
            Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7100 && particleBurstCalls == 0,
                "Short protected service allocation entered particle Tick.");
        }
        var missing = services;
        missing.Burst = null;
        call.Services = &missing.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7100 && particleBurstCalls == 0,
            "Missing particle callback entered Tick.");

        call.Services = &services.Baseline;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0 && ((ProbeState*)state)->Count == 7101 &&
            particleBurstCalls == 2 && !particleBurstBadPayload, Output(output));
        particleBurstReject = true;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && ((ProbeState*)state)->Count == 7101 &&
            particleBurstCalls == 3 && Output(output).Contains("lifecycle rejected"), "Native burst rejection failed to propagate.");
        call.Operation = 4;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0, Output(output));
        NoExtensionModules(&call);

        Check(LoadExtension(&call, typeof(UnmarkedParticleBurstProbe), null) == 0, Output(output));
        using (var manifest = JsonDocument.Parse(Output(output)))
            call.Handle = manifest.RootElement.GetProperty("handle").GetUInt64();
        call.Operation = 2;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0, Output(output));
        using (var guard = new GuardedHeader(176))
        {
            *(Services*)guard.Header = good;
            call.Services = (Services*)guard.Header;
            call.Operation = 3;
            Check(Entry.Invoke((nint)(&call), sizeof(Call)) != 0 && Output(output).Contains("Declare IParticleBurstGame") &&
                ((ProbeState*)state)->Count == 7200 && particleBurstCalls == 3, "Unmarked burst touched the protected service tail.");
        }
        call.Operation = 4;
        Check(Entry.Invoke((nint)(&call), sizeof(Call)) == 0, Output(output));
        NoExtensionModules(&call);
        checks.Add("particle burst independently negotiates 536 bytes, checks protected and absent callbacks, and transfers bounded Tick commands and native errors");
    }
}
