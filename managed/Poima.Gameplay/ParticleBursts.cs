// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;

namespace Poima;

/// <summary>Declares gameplay-driven bursts from authored particle emitters.</summary>
public interface IParticleBurstGame { }

[StructLayout(LayoutKind.Sequential)]
internal struct NativeParticleBurstV1
{
    public uint Version, Bytes;
    public EntityId Emitter;
    public uint Count, Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeParticleServicesV1
{
    public NativeItemAttachmentServicesV1 Attachments;
    public delegate* unmanaged[Cdecl]<void*, NativeParticleBurstV1*, NativeError*, int> Burst;
}

internal static unsafe class ParticleBurstServiceAbi
{
    internal const uint RequiredBytes = 536;
    internal const string Feature = "particle_burst_v1";

    internal static bool LayoutValid()
    {
        NativeParticleServicesV1 services = default;
        NativeParticleBurstV1 command = default;
        return sizeof(NativeParticleServicesV1) == RequiredBytes &&
            (byte*)&services.Burst - (byte*)&services == 528 &&
            sizeof(NativeParticleBurstV1) == 32 &&
            (byte*)&command.Emitter - (byte*)&command == 8 &&
            (byte*)&command.Count - (byte*)&command == 24 &&
            (byte*)&command.Reserved - (byte*)&command == 28;
    }

    internal static NativeParticleServicesV1* Validate(NativeServices* services)
    {
        if (services == null || services->Version != ServiceAbi.Epoch || services->Bytes < RequiredBytes)
            throw new ArgumentException("Particle bursts require services epoch 7 with at least 536 bytes.");
        var api = (NativeParticleServicesV1*)services;
        if (api->Burst == null)
            throw new ArgumentException("Gameplay particle burst callback is absent.");
        return api;
    }
}

public readonly unsafe ref partial struct GameContext
{
    /// <summary>Queues 1 to 1024 particles from an authored emitter during Tick.
    /// The burst uses the emitter pose at the end of the tick and rolls back with the batch on failure.</summary>
    public void BurstParticles(EntityId emitter, uint count)
    {
        RequireFeature(GameplayRequiredFeatures.ParticleBurst, nameof(IParticleBurstGame));
        if (emitter == default)
            throw new ArgumentException("A particle burst requires a nonzero emitter entity ID.", nameof(emitter));
        if (count is < 1 or > 1024)
            throw new ArgumentOutOfRangeException(nameof(count), "Particle burst count must be 1..1024.");

        var api = ParticleBurstServiceAbi.Validate(services);
        NativeParticleBurstV1 command = new()
        {
            Version = 1,
            Bytes = (uint)sizeof(NativeParticleBurstV1),
            Emitter = emitter,
            Count = count
        };
        NativeError error = default;
        Check(api->Burst(services->Context, &command, &error), &error);
    }
}
