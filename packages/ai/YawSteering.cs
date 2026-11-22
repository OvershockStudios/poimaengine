// SPDX-License-Identifier: Apache-2.0
using System;
using System.Numerics;

namespace Poima.Ai;

/// <summary>Direction to choose for an antipodal target in a Y-up world.</summary>
public enum YawHalfTurn
{
    Positive = 0,
    Negative = 1,
}

/// <summary>Maximum angular speed (rad/s), acceleration (rad/s²) and angular dead zone (rad).</summary>
public readonly record struct YawTurnProfile(double MaxSpeed, double MaxAcceleration, double DeadZone);

/// <summary>Caller-owned commanded angular velocity (rad/s) and antipodal tie policy.</summary>
public readonly record struct YawSteeringState(double AngularVelocity, YawHalfTurn HalfTurn);

/// <summary>A one-tick yaw command and its diagnostics, with all angles in radians.</summary>
public readonly record struct YawSteeringStep(Quaternion Rotation, YawSteeringState State,
    double CommittedYaw, double CommandedYaw, double ErrorRadians, double AppliedRadians, bool HasDirection);

/// <summary>Acceleration-limited yaw steering for upright actors with local −Z forward.</summary>
public static class YawSteering
{
    public const int TickRate = 60;
    public const double DirectionEpsilon = 1e-6;

    // A displacement can span two positions at opposite native world bounds.
    private const double VectorComponentLimit = 2_000_000;
    private const double HalfTurnTolerance = 1e-12;

    /// <summary>Advance one fixed tick using the committed forward and the desired world-space direction projected
    /// onto XZ. A direction as short as <see cref="DirectionEpsilon"/> or shorter requests a stop. Positive angles
    /// rotate −Z toward −X. The result carries the next commanded speed and a pure Y rotation for a kinematic target.
    /// </summary>
    public static YawSteeringStep Step(Vector3d committedForward, Vector3d desiredDirection,
        in YawSteeringState state, in YawTurnProfile profile)
    {
        Validate(in state, in profile);
        ValidateVector(committedForward, nameof(committedForward));
        ValidateVector(desiredDirection, nameof(desiredDirection));
        const double minimumLengthSquared = DirectionEpsilon * DirectionEpsilon;
        if (PlanarLengthSquared(committedForward) <= minimumLengthSquared)
            throw new ArgumentException("Committed forward must have a heading in XZ.", nameof(committedForward));

        double committedYaw = Wrap(Math.Atan2(-committedForward.X, -committedForward.Z));
        bool hasDirection = PlanarLengthSquared(desiredDirection) > minimumLengthSquared;
        double error = 0;
        if (hasDirection)
        {
            double cross = committedForward.Z * desiredDirection.X - committedForward.X * desiredDirection.Z;
            double dot = committedForward.X * desiredDirection.X + committedForward.Z * desiredDirection.Z;
            error = dot < 0 && Math.Abs(cross) <= -dot * HalfTurnTolerance
                ? (state.HalfTurn == YawHalfTurn.Positive ? Math.PI : -Math.PI)
                : Math.Atan2(cross, dot);
        }

        double velocityChange = profile.MaxAcceleration / TickRate;
        double requestedVelocity = 0;
        double distance = Math.Abs(error);
        if (distance > profile.DeadZone && error * state.AngularVelocity >= 0)
        {
            double speed = Math.Min(profile.MaxSpeed, StoppingSpeed(distance, velocityChange));
            requestedVelocity = Math.CopySign(speed, error);
        }

        double velocity = Math.Clamp(requestedVelocity,
            state.AngularVelocity - velocityChange, state.AngularVelocity + velocityChange);
        double applied = velocity / TickRate;
        double yaw = Wrap(committedYaw + applied);
        double halfYaw = yaw * 0.5;
        var rotation = new Quaternion(0, (float)Math.Sin(halfYaw), 0, (float)Math.Cos(halfYaw));
        return new(rotation, new(velocity, state.HalfTurn), committedYaw, yaw, error == 0 ? 0 : error,
            applied, hasDirection);
    }

    private static double StoppingSpeed(double distance, double velocityChange)
    {
        // At speed u followed by decrements d, n moving ticks cover (n*u - d*n*(n-1)/2) / TickRate.
        // Find the first triangular bound n and invert that interval to get the speed that fits.
        double scaledDistance = distance * TickRate / velocityChange;
        double ticks = Math.Ceiling(4 * scaledDistance / (Math.Sqrt(1 + 8 * scaledDistance) + 1));
        return distance * TickRate / ticks + velocityChange * (ticks - 1) * 0.5;
    }

    private static double PlanarLengthSquared(Vector3d vector) => vector.X * vector.X + vector.Z * vector.Z;

    private static double Wrap(double yaw)
    {
        if (yaw > Math.PI) yaw -= Math.Tau;
        else if (yaw <= -Math.PI) yaw += Math.Tau;
        return yaw == 0 ? 0 : yaw;
    }

    private static void Validate(in YawSteeringState state, in YawTurnProfile profile)
    {
        // Leave headroom under the native one-tick kinematic limit of 20 rad/s.
        if (!InRange(profile.MaxSpeed, 0.01, 19) || !InRange(profile.MaxAcceleration, 0.01, 600) ||
            !InRange(profile.DeadZone, 1e-5, Math.PI / 4))
            throw new ArgumentOutOfRangeException(nameof(profile),
                "Require speed [0.01, 19], acceleration [0.01, 600], and dead zone [1e-5, π/4].");
        if (!InRange(state.AngularVelocity, -profile.MaxSpeed, profile.MaxSpeed) ||
            state.HalfTurn is not (YawHalfTurn.Positive or YawHalfTurn.Negative))
            throw new ArgumentOutOfRangeException(nameof(state),
                "Require angular velocity within the profile speed and a supported half-turn direction.");
    }

    private static void ValidateVector(Vector3d vector, string parameter)
    {
        if (!InRange(vector.X, -VectorComponentLimit, VectorComponentLimit) ||
            !InRange(vector.Y, -VectorComponentLimit, VectorComponentLimit) ||
            !InRange(vector.Z, -VectorComponentLimit, VectorComponentLimit))
            throw new ArgumentOutOfRangeException(parameter, "Require finite components within ±2,000,000.");
    }

    private static bool InRange(double value, double minimum, double maximum) => value >= minimum && value <= maximum;
}
