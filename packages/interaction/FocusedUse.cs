// SPDX-License-Identifier: Apache-2.0
namespace Poima.Interaction;

public enum FocusStatus
{
    Miss,
    Blocked,
    Target,
}

public readonly record struct FocusResult(FocusStatus Status, RayHit? Hit)
{
    public bool IsTarget => Status == FocusStatus.Target;
}

public static class FocusedUse
{
    public const int MaxTargets = 64;

    public static FocusResult Probe(GameContext context, EntityId actor, EntityId camera,
        ReadOnlySpan<EntityId> targets, double reach)
    {
        if (actor == default)
            throw new ArgumentException("A nonzero actor ID is required.", nameof(actor));
        if (camera == default)
            throw new ArgumentException("A nonzero camera ID is required.", nameof(camera));
        if (!double.IsFinite(reach) || reach is < .001 or > 10_000)
            throw new ArgumentOutOfRangeException(nameof(reach), "Reach must be .001 to 10000 metres.");
        if (targets.Length > MaxTargets)
            throw new ArgumentOutOfRangeException(nameof(targets), $"At most {MaxTargets} focus targets are allowed.");
        foreach (var target in targets)
            if (target == default || target == actor)
                throw new ArgumentException("Focus targets must have nonzero IDs distinct from the actor.", nameof(targets));

        var view = context.Get(camera).Transform;
        Span<EntityId> ignore = stackalloc EntityId[1] { actor };
        var hit = context.Raycast(view.Position, view.Forward, reach, ignore);
        if (hit is not { } first)
            return new(FocusStatus.Miss, null);
        return new(targets.Contains(first.Entity) ? FocusStatus.Target : FocusStatus.Blocked, first);
    }
}
