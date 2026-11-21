// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using System.Runtime.InteropServices;

namespace Poima;

/// <summary>Opts into persistent surface mark placement during Tick.</summary>
public interface ISurfaceMarkGame { }

/// <summary>Defines the ellipse, falloff and material blend of a surface mark.
/// A zero edge fraction produces a hard edge.</summary>
public readonly record struct SurfaceMarkStyle(Vector2 HalfExtentMetres, float HalfDepthMetres,
    float MinimumNormalDot, float EdgeFraction, Vector3 LinearColor, float Opacity, float Roughness);

[StructLayout(LayoutKind.Sequential)]
internal struct NativeSurfaceMarkCommandV1
{
    public uint Version, Bytes;
    public EntityId Receiver, Source;
    public Vector3d Origin, Direction;
    public double Distance;
    public Vector2 HalfExtentMetres;
    public float HalfDepthMetres, MinimumNormalDot, EdgeFraction;
    public Vector3 LinearColor;
    public float Opacity, Roughness;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeSurfaceMarkServicesV1
{
    public NativeParticleServicesV1 Particles;
    public delegate* unmanaged[Cdecl]<void*, NativeSurfaceMarkCommandV1*, NativeError*, int> Place;
}

internal static unsafe class SurfaceMarkServiceAbi
{
    internal const uint RequiredBytes = 544;
    internal const string Feature = "surface_mark_v1";

    internal static bool LayoutValid()
    {
        NativeSurfaceMarkServicesV1 services = default;
        NativeSurfaceMarkCommandV1 command = default;
        return sizeof(NativeSurfaceMarkServicesV1) == RequiredBytes &&
            (byte*)&services.Place - (byte*)&services == 536 &&
            sizeof(NativeSurfaceMarkCommandV1) == 136 &&
            (byte*)&command.Receiver - (byte*)&command == 8 &&
            (byte*)&command.Source - (byte*)&command == 24 &&
            (byte*)&command.Origin - (byte*)&command == 40 &&
            (byte*)&command.Direction - (byte*)&command == 64 &&
            (byte*)&command.Distance - (byte*)&command == 88 &&
            (byte*)&command.HalfExtentMetres - (byte*)&command == 96 &&
            (byte*)&command.HalfDepthMetres - (byte*)&command == 104 &&
            (byte*)&command.MinimumNormalDot - (byte*)&command == 108 &&
            (byte*)&command.EdgeFraction - (byte*)&command == 112 &&
            (byte*)&command.LinearColor - (byte*)&command == 116 &&
            (byte*)&command.Opacity - (byte*)&command == 128 &&
            (byte*)&command.Roughness - (byte*)&command == 132;
    }

    internal static NativeSurfaceMarkServicesV1* Validate(NativeServices* services)
    {
        if (services == null || services->Version != ServiceAbi.Epoch || services->Bytes < RequiredBytes)
            throw new ArgumentException("Surface marks require services epoch 7 with at least 544 bytes.");
        var api = (NativeSurfaceMarkServicesV1*)services;
        if (api->Place == null)
            throw new ArgumentException("Gameplay surface mark callback is absent.");
        return api;
    }

    internal static void ValidateCommand(EntityId receiver, Vector3d origin, Vector3d direction, double distance,
        SurfaceMarkStyle style)
    {
        if (receiver == default)
            throw new ArgumentException("A surface mark requires a nonzero receiver entity ID.", nameof(receiver));
        if (!BoundedVector(origin))
            throw new ArgumentOutOfRangeException(nameof(origin), "Surface mark ray origin must be finite and within ±1000000 metres per axis.");
        if (!BoundedVector(direction) || direction.X * direction.X + direction.Y * direction.Y +
            direction.Z * direction.Z < 1e-24)
            throw new ArgumentOutOfRangeException(nameof(direction), "Surface mark ray direction must be finite, bounded and at least 1e-12 in length.");
        if (!(distance is >= .001 and <= 10000))
            throw new ArgumentOutOfRangeException(nameof(distance), "Surface mark ray distance must be .001..10000 metres.");
        if (!(style.HalfExtentMetres.X is >= .005f and <= 4f) || !(style.HalfExtentMetres.Y is >= .005f and <= 4f) ||
            !(style.HalfDepthMetres is >= .001f and <= .25f) || !(style.Roughness is >= .045f and <= 1f) ||
            !UnitInterval(style.MinimumNormalDot) || !UnitInterval(style.EdgeFraction) || !UnitInterval(style.Opacity) ||
            !UnitInterval(style.LinearColor.X) || !UnitInterval(style.LinearColor.Y) || !UnitInterval(style.LinearColor.Z))
            throw new ArgumentOutOfRangeException(nameof(style), "Surface mark style is outside the bounded material ranges.");
    }

    private static bool BoundedVector(Vector3d value) =>
        value.X is >= -1e6 and <= 1e6 && value.Y is >= -1e6 and <= 1e6 && value.Z is >= -1e6 and <= 1e6;

    private static bool UnitInterval(float value) => value is >= 0f and <= 1f;
}

public readonly unsafe ref partial struct GameContext
{
    /// <summary>Queues a ray-placed mark for the final pose of the current tick.
    /// The first hit must be on the receiver. With a source entity, the origin
    /// and direction use its final local frame. Otherwise they use world space.</summary>
    public void PlaceSurfaceMark(EntityId receiver, Vector3d origin, Vector3d direction, double distance,
        SurfaceMarkStyle style, EntityId source = default)
    {
        RequireFeature(GameplayRequiredFeatures.SurfaceMark, nameof(ISurfaceMarkGame));
        SurfaceMarkServiceAbi.ValidateCommand(receiver, origin, direction, distance, style);

        var api = SurfaceMarkServiceAbi.Validate(services);
        NativeSurfaceMarkCommandV1 command = new()
        {
            Version = 1,
            Bytes = (uint)sizeof(NativeSurfaceMarkCommandV1),
            Receiver = receiver,
            Source = source,
            Origin = origin,
            Direction = direction,
            Distance = distance,
            HalfExtentMetres = style.HalfExtentMetres,
            HalfDepthMetres = style.HalfDepthMetres,
            MinimumNormalDot = style.MinimumNormalDot,
            EdgeFraction = style.EdgeFraction,
            LinearColor = style.LinearColor,
            Opacity = style.Opacity,
            Roughness = style.Roughness
        };
        NativeError error = default;
        Check(api->Place(services->Context, &command, &error), &error);
    }
}
