// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using System.Runtime.InteropServices;

namespace Poima;

/// <summary>Opts into persistent surface mark placement during Tick.</summary>
public interface ISurfaceMarkGame { }

/// <summary>Opts into persistent surface marks aligned to a direction of travel during Tick.</summary>
public interface IDirectionalSurfaceMarkGame { }

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

[StructLayout(LayoutKind.Sequential)]
internal struct NativeSurfaceMarkCommandV2
{
    public NativeSurfaceMarkCommandV1 Mark;
    public Vector3d TravelDirection;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeSurfaceMarkDirectionServicesV2
{
    public NativeSurfaceMarkServicesV1 Marks;
    public delegate* unmanaged[Cdecl]<void*, NativeSurfaceMarkCommandV2*, NativeError*, int> PlaceDirectional;
}

internal static unsafe class SurfaceMarkServiceAbi
{
    internal const uint RequiredBytes = 544;
    internal const string Feature = "surface_mark_v1";
    internal const uint DirectionRequiredBytes = 552;
    internal const string DirectionFeature = "surface_mark_direction_v2";

    internal static bool LayoutValid()
    {
        NativeSurfaceMarkServicesV1 services = default;
        NativeSurfaceMarkCommandV1 command = default;
        NativeSurfaceMarkDirectionServicesV2 directionServices = default;
        NativeSurfaceMarkCommandV2 directionCommand = default;
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
            (byte*)&command.Roughness - (byte*)&command == 132 &&
            sizeof(NativeSurfaceMarkDirectionServicesV2) == DirectionRequiredBytes &&
            (byte*)&directionServices.Marks - (byte*)&directionServices == 0 &&
            (byte*)&directionServices.PlaceDirectional - (byte*)&directionServices == RequiredBytes &&
            sizeof(NativeSurfaceMarkCommandV2) == 160 &&
            (byte*)&directionCommand.Mark - (byte*)&directionCommand == 0 &&
            (byte*)&directionCommand.TravelDirection - (byte*)&directionCommand == 136;
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

    internal static NativeSurfaceMarkDirectionServicesV2* ValidateDirectional(NativeServices* services)
    {
        if (services == null || services->Version != ServiceAbi.Epoch || services->Bytes < DirectionRequiredBytes)
            throw new ArgumentException("Directional surface marks require services epoch 7 with at least 552 bytes.");
        var api = (NativeSurfaceMarkDirectionServicesV2*)services;
        if (api->PlaceDirectional == null)
            throw new ArgumentException("Gameplay directional surface mark callback is absent.");
        return api;
    }

    internal static NativeSurfaceMarkCommandV1 EncodeMark(EntityId receiver, Vector3d origin, Vector3d direction,
        double distance, SurfaceMarkStyle style, EntityId source, uint version, uint bytes)
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

        return new NativeSurfaceMarkCommandV1
        {
            Version = version,
            Bytes = bytes,
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
    }

    internal static void ValidateTravelDirection(Vector3d travelDirection)
    {
        if (!BoundedVector(travelDirection) || travelDirection.X * travelDirection.X +
            travelDirection.Y * travelDirection.Y + travelDirection.Z * travelDirection.Z < 1e-16)
            throw new ArgumentOutOfRangeException(nameof(travelDirection),
                "Surface mark travel direction must be finite, within ±1000000 per axis and at least 1e-8 in length.");
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
        var command = SurfaceMarkServiceAbi.EncodeMark(receiver, origin, direction, distance, style, source,
            1, (uint)sizeof(NativeSurfaceMarkCommandV1));
        var api = SurfaceMarkServiceAbi.Validate(services);
        NativeError error = default;
        Check(api->Place(services->Context, &command, &error), &error);
    }

    /// <summary>Queues a ray-placed mark whose first axis follows travel projected onto the hit plane.
    /// With a source entity, the ray and travel direction use its final local frame.
    /// Otherwise they use world space. Nearly normal travel uses the default tangent.</summary>
    public void PlaceDirectionalSurfaceMark(EntityId receiver, Vector3d origin, Vector3d direction, double distance,
        SurfaceMarkStyle style, Vector3d travelDirection, EntityId source = default)
    {
        RequireFeature(GameplayRequiredFeatures.SurfaceMarkDirection, nameof(IDirectionalSurfaceMarkGame));
        SurfaceMarkServiceAbi.ValidateTravelDirection(travelDirection);
        NativeSurfaceMarkCommandV2 command = new()
        {
            Mark = SurfaceMarkServiceAbi.EncodeMark(receiver, origin, direction, distance, style, source,
                2, (uint)sizeof(NativeSurfaceMarkCommandV2)),
            TravelDirection = travelDirection
        };
        var api = SurfaceMarkServiceAbi.ValidateDirectional(services);
        NativeError error = default;
        Check(api->PlaceDirectional(services->Context, &command, &error), &error);
    }
}
