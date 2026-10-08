// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Examples;

// All decisions, including the remembered target and timers, are native-owned
// registered state. The game instance holds no mutable fields or wall clock.
public struct PatrolState
{
    public EntityId Player, Guard;
    public int Mode, Outcome, Waypoint, LostTicks, SearchTicks, Alerts, Seen;
    public int Ticks, Patrolled, NoiseCount, Paused;
    public double LastSeenX, LastSeenZ, Heading;
}

[GameModule("poima.example.patrol-room")]
public sealed class PatrolGame : Game<PatrolState>, ICharacterInputGame
{
    static readonly UiId Status = new(0, 2), Hint = new(0, 3);
    const double VisionRange = 13, CatchDistance = 1.15;
    const int LoseSightTicks = 45, InvestigateTicks = 180;

    public override void Initialize(ref PatrolState state)
    {
        state = default;
        state.Player = new(0, 100); state.Guard = new(0, 300);
        state.Waypoint = 1;
    }
    static double Distance(double x, double z) => Math.Sqrt(x*x + z*z);
    static double Wrap(double angle)
    {
        while(angle > 180) angle -= 360;
        while(angle < -180) angle += 360;
        return angle;
    }
    static Vector3d Waypoint(int index) => index switch
    {
        0 => new(-4, 0, -6), 1 => new(4, 0, -6),
        2 => new(4, 0, -3), _ => new(-4, 0, -3)
    };
    static string StatusText(in PatrolState state) => state.Outcome switch
    {
        1 => "EVADED | Extraction reached after alerting the guard.",
        2 => "CAUGHT | The guard reached you with a clear line of sight.",
        _ => state.Mode switch { 2 => "CHASE | Break line of sight and reach extraction.",
            1 => "INVESTIGATE | The guard is checking the last known position.",
            _ => "PATROL | The guard follows its route. Solid cover blocks vision." }
    };
    public override void Tick(ref PatrolState state, GameContext context)
    {
        if(state.Paused != 0 || state.Outcome != 0) return;
        ++state.Ticks;
        // Get observes this tick's prepared, pre-physics state. In particular,
        // neutral preparation may already have cleared the guard's XZ velocity.
        var guard = context.Get(state.Guard).Transform;
        var player = context.Get(state.Player).Transform;
        var gp = guard.Position; var pp = player.Position;
        double dx = pp.X-gp.X, dz = pp.Z-gp.Z, distance = Distance(dx,dz);
        var forward = guard.Forward;
        double horizontalForward = Distance(forward.X,forward.Z);
        double facing = distance < .0001 ? 1 : (forward.X*dx+forward.Z*dz)/(Math.Max(.0001,horizontalForward)*distance);
        bool seen = false;
        if(distance <= VisionRange && facing >= .25881904510252074) // 150-degree field of view
        {
            var origin = new Vector3d(gp.X,gp.Y+.65,gp.Z);
            var target = new Vector3d(pp.X,pp.Y+.65,pp.Z);
            double dy = target.Y-origin.Y, length = Math.Sqrt(dx*dx+dy*dy+dz*dz);
            if(length < .0001) seen = true;
            else
            {
                var hit = context.Raycast(origin,new(dx/length,dy/length,dz/length),length+.05,[state.Guard]);
                seen = hit is { } value && value.Entity == state.Player;
            }
        }
        state.Seen = seen ? 1 : 0;
        bool noise = context.Pressed(state.Player,GameAction.Use) && distance <= 16;
        if(seen)
        {
            if(state.Mode != 2) ++state.Alerts;
            state.Mode = 2; state.LostTicks = 0;
            state.LastSeenX = pp.X; state.LastSeenZ = pp.Z;
            if(distance <= CatchDistance) state.Outcome = 2;
        }
        else if(noise)
        {
            ++state.NoiseCount;
            state.LastSeenX = pp.X; state.LastSeenZ = pp.Z;
            state.Mode = 1; state.SearchTicks = 0; state.LostTicks = 0;
        }
        else if(state.Mode == 2 && ++state.LostTicks >= LoseSightTicks)
        {
            state.Mode = 1; state.SearchTicks = 0;
        }
        else if(state.Mode == 1 && ++state.SearchTicks >= InvestigateTicks)
        {
            state.Mode = 0; state.SearchTicks = 0;
        }
        // Extraction is earned only after a visual alert. Noise alone is not a win.
        if(state.Outcome == 0 && state.Alerts > 0 && pp.X <= -9 && pp.Z >= 8)
            state.Outcome = 1;
        context.SetUi(Status,text:StatusText(in state));
        if(state.Outcome != 0)
        {
            context.SetUi(Hint,text:"Guard stopped. Load a checkpoint, or relaunch with a new output folder to retry.");
            return; // No intent next tick means neutral; never teleport a character.
        }
        Vector3d targetPosition;
        float movement;
        if(state.Mode == 0)
        {
            targetPosition = Waypoint(state.Waypoint); movement = .55f;
            if(Distance(targetPosition.X-gp.X,targetPosition.Z-gp.Z) < .35)
            {
                state.Waypoint = (state.Waypoint+1)%4; ++state.Patrolled;
                targetPosition = Waypoint(state.Waypoint);
            }
        }
        else { targetPosition = new(state.LastSeenX,0,state.LastSeenZ); movement = state.Mode == 2 ? 1f : .7f; }
        dx = targetPosition.X-gp.X; dz = targetPosition.Z-gp.Z;
        if(Distance(dx,dz) < .25) return;
        double currentYaw = Math.Atan2(-forward.X,-forward.Z)*180/Math.PI;
        double desired = Math.Atan2(-dx,-dz)*180/Math.PI;
        double delta = Wrap(desired-currentYaw);
        state.Heading = desired;
        // Turn in place until aligned, then move. Obstacle avoidance/pathfinding
        // is deliberately outside this four-waypoint example's contract.
        context.SetCharacterInput(state.Guard,0,Math.Abs(delta) > 35 ? 0 : movement,
            (float)Math.Clamp(delta,-6,6));
    }
    public override void Control(ref PatrolState state,ControlContext context)
    {
        switch(context.Action)
        {
            case "pause": state.Paused=1;context.RequestPause();break;
            case "resume": state.Paused=0;context.RequestResume();break;
            case "save":
            case "load":
                var request=context.Action=="save" ? context.TryRequestSave("patrol-room") : context.TryRequestLoad("patrol-room");
                context.SetUi(Hint,text:request.Accepted ? "Checkpoint requested; the native result reports completion." : "Checkpoint request rejected. Configure save storage first.");
                break;
            default:throw new InvalidOperationException("Unknown patrol control.");
        }
    }
}
