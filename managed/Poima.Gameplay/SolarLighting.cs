// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using System.Runtime.InteropServices;

namespace Poima;

/// <summary>Declares a game that supplies solar lighting from its own clock.</summary>
public interface ISolarLightingGame { }

[StructLayout(LayoutKind.Sequential)]
internal struct NativeSolarLightingV1
{
    public uint Version, Bytes;
    public ulong WorldMilliseconds;
    public Vector3d TowardSun;
    public float SunGain;
    public Vector3 SunTint;
    public float SkyGain;
    public Vector3 Ambient;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeSolarLightingServicesV1
{
    public NativeWetnessServicesV1 Wetness;
    public delegate* unmanaged[Cdecl]<void*, NativeSolarLightingV1*, NativeError*, int> Set;
}

internal static unsafe class SolarLightingServiceAbi
{
    internal const string Feature = "solar_lighting_v1";
    internal const uint RequiredBytes = 584;
    internal const uint SampleBytes = 72;
    internal const ulong MaximumTimeMilliseconds = 315_359_999_999_999;

    internal static NativeSolarLightingServicesV1* Validate(NativeServices* services)
    {
        if (services == null || services->Version != ServiceAbi.Epoch || services->Bytes < RequiredBytes)
            throw new ArgumentException("Solar lighting requires services epoch 7 with at least 584 bytes.");

        NativeSolarLightingServicesV1 layout = default;
        NativeSolarLightingV1 sample = default;
        if (sizeof(NativeSolarLightingServicesV1) != RequiredBytes ||
            (byte*)&layout.Set - (byte*)&layout != 576 ||
            sizeof(NativeSolarLightingV1) != SampleBytes ||
            (byte*)&sample.WorldMilliseconds - (byte*)&sample != 8 ||
            (byte*)&sample.TowardSun - (byte*)&sample != 16 ||
            (byte*)&sample.SunGain - (byte*)&sample != 40 ||
            (byte*)&sample.SunTint - (byte*)&sample != 44 ||
            (byte*)&sample.SkyGain - (byte*)&sample != 56 ||
            (byte*)&sample.Ambient - (byte*)&sample != 60)
            throw new InvalidOperationException("Solar lighting service layout mismatch.");

        var api = (NativeSolarLightingServicesV1*)services;
        if (api->Set == null)
            throw new ArgumentException("Gameplay solar lighting callback is absent.");
        return api;
    }
}

public readonly unsafe ref partial struct GameContext
{
    /// <summary>Stages a complete solar sample for the authored sun and sky during Tick.</summary>
    /// <param name="worldMilliseconds">Elapsed game time in milliseconds.</param>
    /// <param name="towardSun">Unit vector pointing from the observer toward the sun in world axes.</param>
    /// <param name="sunGain">Direct light and sun disk gain in [0, 1].</param>
    /// <param name="sunTint">Linear RGB multipliers for the authored sun color in [0, 1].</param>
    /// <param name="skyGain">Linear gain for the authored sky gradient in [0, 1].</param>
    /// <param name="ambient">Linear RGB ambient fill in [0, 1000000].</param>
    public void SetSolarLighting(ulong worldMilliseconds, Vector3d towardSun, float sunGain,
        Vector3 sunTint, float skyGain, Vector3 ambient)
    {
        RequireFeature(GameplayRequiredFeatures.SolarLighting, nameof(ISolarLightingGame));
        if (worldMilliseconds > SolarLightingServiceAbi.MaximumTimeMilliseconds)
            throw new ArgumentOutOfRangeException(nameof(worldMilliseconds));
        double length = Math.Sqrt(towardSun.X * towardSun.X + towardSun.Y * towardSun.Y + towardSun.Z * towardSun.Z);
        if (!double.IsFinite(length) || Math.Abs(length - 1) > 1e-6)
            throw new ArgumentOutOfRangeException(nameof(towardSun), "Solar direction must be a finite unit vector.");
        if (!SolarChannel(sunGain, 1))
            throw new ArgumentOutOfRangeException(nameof(sunGain));
        if (!SolarRgb(sunTint, 1))
            throw new ArgumentOutOfRangeException(nameof(sunTint));
        if (!SolarChannel(skyGain, 1))
            throw new ArgumentOutOfRangeException(nameof(skyGain));
        if (!SolarRgb(ambient, 1_000_000))
            throw new ArgumentOutOfRangeException(nameof(ambient));

        var api = SolarLightingServiceAbi.Validate(services);
        NativeSolarLightingV1 sample = new()
        {
            Version = 1,
            Bytes = SolarLightingServiceAbi.SampleBytes,
            WorldMilliseconds = worldMilliseconds,
            TowardSun = towardSun,
            SunGain = sunGain,
            SunTint = sunTint,
            SkyGain = skyGain,
            Ambient = ambient,
        };
        NativeError error = default;
        Check(api->Set(services->Context, &sample, &error), &error);
    }

    private static bool SolarChannel(float value, float maximum) => value >= 0 && value <= maximum;

    private static bool SolarRgb(Vector3 value, float maximum) =>
        SolarChannel(value.X, maximum) && SolarChannel(value.Y, maximum) && SolarChannel(value.Z, maximum);
}
