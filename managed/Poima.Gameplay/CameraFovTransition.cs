// SPDX-License-Identifier: Apache-2.0
namespace Poima;

/// <summary>Stateless authoritative fixed-Tick vertical FOV transitions, in degrees.</summary>
/// <remarks>
/// Save the start, target, elapsed and duration as primitive gameplay state fields.
/// Advance elapsed only on successful fixed Ticks, clamping it to duration.
/// To retarget, use the currently evaluated FOV as the new start and reset elapsed
/// to zero. This preserves position, not velocity, continuity. Rendering and
/// wall time do not advance the transition; no camera service or feature marker
/// is required to evaluate it.
/// </remarks>
public static class CameraFovTransition
{
    /// <summary>Maximum transition duration and elapsed count: 3,600 fixed Ticks.</summary>
    public const uint MaximumTicks=3600;

    /// <summary>
    /// Evaluates smoothstep in log(tan(vertical FOV / 2)). Endpoints must be
    /// finite within 5..150 degrees and both Tick counts within 0..3,600.
    /// Zero duration or elapsed at/after duration returns the exact target;
    /// zero elapsed returns the exact start for a nonzero duration.
    /// </summary>
    public static double Evaluate(double startFov,double targetFov,uint elapsedTicks,uint durationTicks)
    {
        if(!double.IsFinite(startFov) || startFov<5 || startFov>150)
            throw new ArgumentOutOfRangeException(nameof(startFov));
        if(!double.IsFinite(targetFov) || targetFov<5 || targetFov>150)
            throw new ArgumentOutOfRangeException(nameof(targetFov));
        if(elapsedTicks>MaximumTicks)throw new ArgumentOutOfRangeException(nameof(elapsedTicks));
        if(durationTicks>MaximumTicks)throw new ArgumentOutOfRangeException(nameof(durationTicks));
        if(durationTicks==0 || elapsedTicks>=durationTicks)return targetFov;
        if(elapsedTicks==0 || startFov==targetFov)return startFov;
        const double halfRadians=Math.PI/360;
        double progress=(double)elapsedTicks/durationTicks;
        double weight=progress*progress*(3-2*progress);
        double start=Math.Log(Math.Tan(startFov*halfRadians));
        double target=Math.Log(Math.Tan(targetFov*halfRadians));
        double result=Math.Atan(Math.Exp(start+(target-start)*weight))/halfRadians;
        // Keep floating-point rounding within the admitted endpoint interval.
        return Math.Clamp(result,Math.Min(startFov,targetFov),Math.Max(startFov,targetFov));
    }
}
