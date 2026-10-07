// SPDX-License-Identifier: Apache-2.0
namespace Poima.Editor;

// Exact integration of a first-order velocity response. This affects the
// inspection camera only; gameplay input and the fixed simulation tick are separate.
internal sealed class SceneCameraMotion
{
    private readonly double[] velocity = new double[3];
    private double? zoomTarget;
    private int zoomDirection;
    internal bool Zooming => zoomTarget is not null;
    internal double Speed => Math.Sqrt(velocity.Sum(v => v*v));
    internal void Cancel()
    {
        Array.Clear(velocity); zoomTarget = null; zoomDirection = 0;
    }
    internal void QueueZoom(double distance, double multiplier)
    {
        if (!double.IsFinite(distance) || distance <= 0 || !double.IsFinite(multiplier) || multiplier <= 0)
            throw new ArgumentException("Zoom needs positive finite distances.");
        var direction = Math.Sign(multiplier-1);
        // Reversing the wheel responds from the displayed distance immediately,
        // instead of first consuming an unseen queue in the opposite direction.
        var start = zoomTarget is double target && direction == zoomDirection ? target : distance;
        zoomTarget = Math.Clamp(start*multiplier, .05, 1e6); zoomDirection = direction;
    }
    internal double StepZoom(double distance, double seconds)
    {
        if (!double.IsFinite(distance) || distance <= 0) throw new ArgumentException("Invalid focus distance.");
        var dt = Delta(seconds);
        if (zoomTarget is not double target || dt == 0) return distance;
        var value = Math.Exp(Math.Log(target)+(Math.Log(distance)-Math.Log(target))*Math.Exp(-dt/.10));
        if (Math.Abs(value-target) <= Math.Max(1e-6, target*1e-5))
        { zoomTarget = null; zoomDirection = 0; return target; }
        return value;
    }
    internal double[] StepFlight(double[] direction, double speed, double seconds)
    {
        if (direction.Length != 3 || direction.Any(v => !double.IsFinite(v)) || !double.IsFinite(speed) || speed < 0)
            throw new ArgumentException("Invalid flight target.");
        var dt = Delta(seconds); var length = Math.Sqrt(direction.Sum(v => v*v));
        var response = length > 1e-8 ? .12 : .08;
        var decay = Math.Exp(-dt/response); var distance = new double[3];
        for (var i=0; i<3; ++i)
        {
            var target = length > 1e-8 ? direction[i]/length*speed : 0;
            distance[i] = target*dt+(velocity[i]-target)*response*(1-decay);
            velocity[i] = target+(velocity[i]-target)*decay;
        }
        if (length <= 1e-8 && Speed < 1e-5) Array.Clear(velocity);
        return distance;
    }
    private static double Delta(double seconds)
    {
        if (!double.IsFinite(seconds) || seconds < 0) throw new ArgumentException("Invalid camera time step.");
        return Math.Min(seconds, .1); // Do not jump across a blocked editor interval.
    }
}
