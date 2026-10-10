// SPDX-License-Identifier: Apache-2.0
using Poima;

namespace Poima.Examples;

// This is an opt-in workload component, not Relay Yard's persistent global
// state. Its counters describe committed movement, never requested movement.
[GameplayComponent("c0940000000000000000000000000001")]
public partial struct PerformanceRoute
{
    [GameplayField("00000000000000000000000000000001",Default="0")] public int Enabled;
    [GameplayField("00000000000000000000000000000002",Default="0")] public int Corner;
    [GameplayField("00000000000000000000000000000003",Default="0")] public long Laps;
    [GameplayField("00000000000000000000000000000004",Default="0")] public long Samples;
    [GameplayField("00000000000000000000000000000005",Default="0")] public int PreviousValid;
    [GameplayField("00000000000000000000000000000006",Default="0")] public long PreviousTick;
    [GameplayField("00000000000000000000000000000007",Default="0",Unit="m")] public double PreviousX;
    [GameplayField("00000000000000000000000000000008",Default="0",Unit="m")] public double PreviousY;
    [GameplayField("00000000000000000000000000000009",Default="0",Unit="m")] public double PreviousZ;
    [GameplayField("0000000000000000000000000000000a",Default="0",Unit="m")] public double TravelMeters;
    [GameplayField("0000000000000000000000000000000b",Default="0",Unit="m")] public double LastDisplacement;
    [GameplayField("0000000000000000000000000000000c",Default="0")] public long StageCommands;
    [GameplayField("0000000000000000000000000000000d",Default="0")] public long Plans;
    [GameplayField("0000000000000000000000000000000e",Default="0")] public long MotionTicks;
    [GameplayField("0000000000000000000000000000000f",Default="-1")] public int LastPathStatus;
    [GameplayField("00000000000000000000000000000010",Default="0")] public int RouteValid;
    [GameplayField("00000000000000000000000000000011",Default="0")] public int LastCornerCount;
    [GameplayField("00000000000000000000000000000012",Default="0")] public int FirstCornerReached;
}

// Reuse the actual sample's gameplay, native audio cadence and save callbacks.
// The separate module/content identity is not an upgrade of old Relay saves.
// Run an autonomous native window without a supplied player controller: even
// neutral caller input owns that controller and would conflict with Tick intent.
[GameModule("poima.examples.performance-yard")]
public sealed class PerformanceGame : Game<RelayState>, INavigationGame,
    ICharacterInputGame, IInertialAnimationGame, IPlayerPreferencesGame
{
    static readonly RelayYardGame Relay = new();
    const double ArrivalDistance=.12, SlowdownDistance=.8;

    public override void Initialize(ref RelayState state) => Relay.Initialize(ref state);

    public override void Control(ref RelayState state,ControlContext context) => Relay.Control(ref state,context);

    static void Check(bool condition,string message)
    {
        if(!condition)throw new InvalidOperationException(message);
    }

    static long Increment(long value)
    {
        Check(value<long.MaxValue,"Performance route counter exhausted.");
        return value+1;
    }

    static double Distance(double x,double z)=>Math.Sqrt(x*x+z*z);

    static double Wrap(double angle)
    {
        while(angle>180)angle-=360;
        while(angle<-180)angle+=360;
        return angle;
    }

    static Vector3d Goal(int corner)=>corner switch
    {
        0=>new(-7,0,6.5),1=>new(7,0,6.5),2=>new(7,0,-6.5),
        3=>new(-7,0,-6.5),_=>throw new InvalidOperationException("Invalid performance route corner.")
    };

    static void Validate(in PerformanceRoute route,in LocomotionRoute path)
    {
        Check(route.Enabled is 0 or 1 && route.PreviousValid is 0 or 1 && route.RouteValid is 0 or 1 &&
              route.FirstCornerReached is 0 or 1,
            "Performance route flags must be Boolean scalars.");
        Check(route.Corner is >=0 and <=3 && route.LastPathStatus is >=-1 and <=4 &&
              route.LastCornerCount is >=0 and <=10,"Invalid performance route path state.");
        Check(route.Laps>=0 && route.Samples>=0 && route.StageCommands>=0 && route.Plans>=0 && route.MotionTicks>=0 &&
              route.PreviousTick is >=0 and <=9007199254740991L,"Invalid performance route chronology.");
        Check(double.IsFinite(route.PreviousX) && double.IsFinite(route.PreviousY) && double.IsFinite(route.PreviousZ) &&
              double.IsFinite(route.TravelMeters) && route.TravelMeters>=0 &&
              double.IsFinite(route.LastDisplacement) && route.LastDisplacement>=0,"Invalid performance route displacement.");
        Check(path.Coordinates.Count is >=0 and <=30 && path.Coordinates.Count%3==0 &&
              path.Cursor>=0 && path.Cursor<=path.Coordinates.Count/3,"Invalid native performance route buffer.");
        Check(route.RouteValid==0 || path.Coordinates.Count>=3,"A valid performance route needs native corners.");
        for(int i=0;i<path.Coordinates.Count;++i)
            Check(float.IsFinite(path.Coordinates[i]),"Nonfinite performance navigation corner.");
    }

    public override void Tick(ref RelayState state,GameContext context)
    {
        Relay.Tick(ref state,context);
        if(!context.TryGet<PerformanceRoute>(RelayYardGame.Player,out var route))return;
        var path=context.Get<LocomotionRoute>(RelayYardGame.Player);
        Validate(in route,in path);
        if(route.Enabled==0 || state.Started==0)
        {
            // Re-enabling after manually controlled ticks must not attribute
            // an unsampled jump in position to the autonomous workload.
            if(route.PreviousValid!=0 || route.LastDisplacement!=0 || route.RouteValid!=0)
            {
                route.PreviousValid=0;route.LastDisplacement=0;route.RouteValid=0;
                route.Corner=0;route.FirstCornerReached=0;
                context.Set(RelayYardGame.Player,in route);
            }
            return;
        }

        var actor=context.Get(RelayYardGame.Player);
        var position=actor.Transform.Position;
        Check(double.IsFinite(position.X) && double.IsFinite(position.Y) && double.IsFinite(position.Z),
            "Performance controller position is nonfinite.");
        Check(context.Tick<=9007199254740991UL,"Performance tick exceeded the supported native chronology.");
        route.LastDisplacement=0;
        if(route.PreviousValid!=0 && (ulong)route.PreviousTick+1==context.Tick)
        {
            double displacement=Distance(position.X-route.PreviousX,position.Z-route.PreviousZ);
            // Bounded consecutive horizontal capsule motion. A chronology
            // discontinuity or large vertical change does not invent travel.
            if(Math.Abs(position.Y-route.PreviousY)<=.3 && displacement<=8*context.DeltaTime+.001)
            {
                route.LastDisplacement=displacement;
                route.TravelMeters+=displacement;
                Check(double.IsFinite(route.TravelMeters),"Performance travel counter overflowed.");
                if(displacement>.001)route.MotionTicks=Increment(route.MotionTicks);
            }
        }
        route.PreviousX=position.X;route.PreviousY=position.Y;route.PreviousZ=position.Z;
        route.PreviousTick=(long)context.Tick;route.PreviousValid=1;
        route.Samples=Increment(route.Samples);

        var goal=Goal(route.Corner);
        bool changed=false;
        if(route.RouteValid==0)
        {
            Span<NavigationPoint> corners=stackalloc NavigationPoint[10];
            var answer=context.FindNavigationPath(RelayYardGame.Player,goal,corners,new(.5,2,.5),256,4096);
            route.Plans=Increment(route.Plans);route.LastPathStatus=(int)answer.Status;
            route.LastCornerCount=answer.CornerCount;
            Check(answer.Complete && answer.CornerCount is >=1 and <=10,
                $"Performance route requires a complete native path; status={answer.Status}, corners={answer.CornerCount}.");
            path.Coordinates.Clear();path.Cursor=0;changed=true;
            for(int i=0;i<answer.CornerCount;++i)
                Check(path.Coordinates.TryAdd(corners[i].X) && path.Coordinates.TryAdd(corners[i].Y) &&
                      path.Coordinates.TryAdd(corners[i].Z),"Performance native route exceeded its buffer.");
            route.RouteValid=1;
        }
        int final=path.Coordinates.Count-3;
        Check(Distance(path.Coordinates[final]-goal.X,path.Coordinates[final+2]-goal.Z)<=.75,
            "Native performance endpoint projected too far from its authored destination.");
        int previousCursor=path.Cursor;
        while(path.Cursor<path.Coordinates.Count/3 &&
              Distance(path.Coordinates[path.Cursor*3]-position.X,path.Coordinates[path.Cursor*3+2]-position.Z)<=ArrivalDistance)
            ++path.Cursor;
        changed|=path.Cursor!=previousCursor;
        if(path.Cursor==path.Coordinates.Count/3)
        {
            // A complete native route may project onto a nearby walkable point.
            // Arrival is actual capsule proximity to that bounded endpoint.
            // The initial approach to corner zero is not a completed lap.
            if(route.Corner==0)
            {
                if(route.FirstCornerReached!=0)route.Laps=Increment(route.Laps);
                else route.FirstCornerReached=1;
            }
            route.Corner=(route.Corner+1)%4;route.RouteValid=0;
            if(changed)context.Set(RelayYardGame.Player,in path);
            context.Set(RelayYardGame.Player,in route);
            return; // Neutral for this tick; plan the next leg on the next Tick.
        }
        double dx=path.Coordinates[path.Cursor*3]-position.X,dz=path.Coordinates[path.Cursor*3+2]-position.Z;
        double desired=Math.Atan2(-dx,-dz)*180/Math.PI;
        var forward=actor.Transform.Forward;
        double turn=Wrap(desired-Math.Atan2(-forward.X,-forward.Z)*180/Math.PI);
        float move=Math.Abs(turn)>4 ? 0 : (float)(.75*Math.Min(1,Distance(dx,dz)/SlowdownDistance));
        context.SetCharacterInput(RelayYardGame.Player,0,move,(float)Math.Clamp(turn,-8,8));
        route.StageCommands=Increment(route.StageCommands);
        if(changed)context.Set(RelayYardGame.Player,in path);
        context.Set(RelayYardGame.Player,in route);
    }
}
