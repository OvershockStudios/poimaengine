// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;

namespace Poima;

/// <summary>Declares gameplay-driven rain and drying on authored wettable surfaces.</summary>
public interface ISurfaceWetnessGame { }

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeWetnessServicesV1
{
    public NativeSurfaceMarkTimedServicesV4 TimedSurfaceMarks;
    public delegate* unmanaged[Cdecl]<void*, ulong, uint*, uint, NativeError*, int> Advance;
}

internal static unsafe class WetnessServiceAbi
{
    internal const string Feature = "surface_wetness_v1";
    internal const uint RequiredBytes = 576;
    internal const int MaximumBatch = 64;
    internal const uint MaximumRainNanometers = 1_000_000;
    internal const ulong MaximumQuantumIndex = 315_359_999_998;

    internal static NativeWetnessServicesV1* Validate(NativeServices* services)
    {
        if (services == null || services->Version != ServiceAbi.Epoch || services->Bytes < RequiredBytes)
            throw new ArgumentException("Surface wetness requires services epoch 7 with at least 576 bytes.");

        NativeWetnessServicesV1 layout = default;
        if (sizeof(NativeWetnessServicesV1) != RequiredBytes ||
            (byte*)&layout.Advance - (byte*)&layout != 568)
            throw new InvalidOperationException("Surface wetness service layout mismatch.");

        var api = (NativeWetnessServicesV1*)services;
        if (api->Advance == null)
            throw new ArgumentException("Gameplay surface wetness callback is absent.");
        return api;
    }
}

public readonly unsafe ref partial struct GameContext
{
    /// <summary>
    /// Stages rain depths for 1 to 64 consecutive game-clock seconds during Tick.
    /// Each depth is in nanometers. Zero rain still applies drying.
    /// </summary>
    public void AdvanceSurfaceWetness(ulong firstQuantum, scoped ReadOnlySpan<uint> rainNanometers)
    {
        RequireFeature(GameplayRequiredFeatures.SurfaceWetness, nameof(ISurfaceWetnessGame));
        if (rainNanometers.Length is < 1 or > WetnessServiceAbi.MaximumBatch)
            throw new ArgumentOutOfRangeException(nameof(rainNanometers), "Supply 1 to 64 rain depths.");
        if (firstQuantum > WetnessServiceAbi.MaximumQuantumIndex - (ulong)(rainNanometers.Length - 1))
            throw new ArgumentOutOfRangeException(nameof(firstQuantum), "Rain seconds must fit within the supported game clock.");

        foreach (uint depth in rainNanometers)
            if (depth > WetnessServiceAbi.MaximumRainNanometers)
                throw new ArgumentOutOfRangeException(nameof(rainNanometers), "Each rain depth must be at most 1000000 nanometers.");

        var api = WetnessServiceAbi.Validate(services);
        NativeError error = default;
        fixed (uint* rain = rainNanometers)
            Check(api->Advance(services->Context, firstQuantum, rain, (uint)rainNanometers.Length, &error), &error);
    }
}
