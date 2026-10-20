// SPDX-License-Identifier: Apache-2.0
using System;
namespace Poima.Fps;

/// <summary>Saved flight phase. Impacted and Expired do not emit another contact.</summary>
public enum ProjectilePhase { Flying=0,Impacted=1,Expired=2 }
/// <summary>Constant acceleration in m/s², physical sphere radius in metres and lifetime in seconds.
/// MaxChordError bounds the arithmetic curvature envelope, not native contact/position precision.</summary>
public readonly record struct ProjectileDefinition(Vector3d Acceleration,double Radius,double Lifetime,double MaxChordError,int MaxSegments);
/// <summary>Transient helper value; persist its individual primitive operands in game state.</summary>
public readonly record struct ProjectileState(Vector3d Position,Vector3d Velocity,double Age,ProjectilePhase Phase);
/// <summary>One linear sphere query. Direction is nonzero/unit length even when Distance is zero.
/// Seconds is the segment duration; ArcDeviation is its curvature bound. Radius includes that bound
/// and outward binary32 rounding, so Radius minus the physical radius can exceed ArcDeviation.</summary>
public readonly record struct ProjectileSweep(Vector3d Origin,Vector3d Direction,double Distance,double Radius,double Seconds,double ArcDeviation);
/// <summary>Owned result. Hit is the provider's original contact; State.Position is the stopped
/// swept sphere's chord center, not either contact point. MaxArcDeviation covers queries actually made.</summary>
public readonly record struct ProjectileStep(ProjectileState State,CapsuleCastHit? Hit,int Queries,double ConsumedSeconds,double MaxArcDeviation);
/// <summary>Queries committed collision geometry. Exceptions propagate; no miss is synthesized.</summary>
public interface IProjectileQueries { CapsuleCastHit? Sweep(in ProjectileSweep sweep); }

/// <summary>Stateless compiled constant-acceleration sphere flight with bounded linear sweeps.</summary>
/// <remarks>
/// Units are metres/seconds. Radius .001..9, lifetime (0,3600], chord error 1e-6..1,
/// MaxSegments 1..32, dt (0,1], positions +/-1e6 and velocity/acceleration axes +/-1e5.
/// Full trajectory extrema, velocity endpoints, segment budget and native query bounds are checked
/// before the first query. Invalid inputs throw ArgumentOutOfRangeException; unrepresentable motion
/// or an insufficient segment budget throws ArithmeticException. Invalid contact fractions throw
/// InvalidOperationException. Provider exceptions propagate without publishing a partial state.
/// Callers still own provider side effects and the aggregate native32-query budget per Tick.
///
/// The quadratic path differs from each chord by at most |acceleration|*h²/8 in exact arithmetic.
/// The query radius adds its computed outward curvature envelope and binary32 radius padding.
/// This can stop early, including an inflated-envelope initial overlap which is not proof that the
/// physical initial sphere overlaps. Native geometry/position/contact tolerances and floating-point
/// arithmetic remain relevant; this is not exact parabolic TOI or moving-target relative CCD.
/// No penetration, ricochet, drag, wind, pooling or hidden global clock/state is supplied.
/// </remarks>
public static class Projectiles
{
    private const double PositionBound=1e6,MotionBound=1e5,TravelBound=10000;
    private readonly record struct Segment(ProjectileSweep Sweep,double Start,double End);

    /// <summary>Starts validated flight at age zero; zero velocity is allowed.</summary>
    public static ProjectileState Launch(Vector3d position,Vector3d velocity)
    {
        ValidateVector(position,PositionBound,nameof(position));ValidateVector(velocity,MotionBound,nameof(velocity));
        return new(position,velocity,0,ProjectilePhase.Flying);
    }

    /// <summary>Advances against a caller-owned provider without delegate allocation or boxing.
    /// Terminal states return unchanged after input validation. Flying at its lifetime expires with
    /// zero queries. First contact wins even at the lifetime endpoint and preserves incident velocity.</summary>
    public static ProjectileStep Step<TQuery>(in ProjectileDefinition definition,in ProjectileState state,double deltaTime,ref TQuery query)
        where TQuery:IProjectileQueries,allows ref struct
    {
        Validate(in definition,in state,deltaTime);
        if(state.Phase!=ProjectilePhase.Flying)return new(state,null,0,0,0);
        double remaining=definition.Lifetime-state.Age;
        if(remaining==0)return new(state with {Phase=ProjectilePhase.Expired},null,0,0,0);
        double total=Math.Min(deltaTime,remaining);
        if(!(total>0) || !(state.Age+total>state.Age) || !double.IsFinite(state.Age+total) || (total!=remaining && state.Age+total>definition.Lifetime))throw new ArithmeticException("Projectile age cannot advance at this precision.");
        Vector3d endPosition=PositionAt(in state,in definition,total),endVelocity=VelocityAt(in state,in definition,total);
        ValidateTrajectory(in definition,in state,total,endPosition,endVelocity);
        double acceleration=UpperLength(definition.Acceleration);
        double speedBound=Length(state.Velocity)+acceleration*total;
        int segments=0;
        for(int count=1;count<=definition.MaxSegments;++count)
        {
            bool fits=true;
            for(int i=0;i<count;++i)
            {
                Times(total,count,i,out double start,out double end);double seconds=end-start;
                double deviation=Deviation(acceleration,seconds);
                if(!(seconds>0) || deviation>definition.MaxChordError || speedBound*seconds>TravelBound || InflatedRadius(definition.Radius,deviation)>10){fits=false;break;}
            }
            if(fits){segments=count;break;}
        }
        if(segments==0)throw new ArithmeticException("Projectile curvature/travel does not fit the configured segment budget.");
        // This separate complete pass prevents a later malformed native query
        // from failing only after earlier provider calls have already happened.
        for(int i=0;i<segments;++i)_=MakeSegment(in definition,in state,total,segments,i,acceleration);
        double maximumDeviation=0;
        for(int i=0;i<segments;++i)
        {
            var segment=MakeSegment(in definition,in state,total,segments,i,acceleration);var sweep=segment.Sweep;
            maximumDeviation=Math.Max(maximumDeviation,sweep.ArcDeviation);
            var contact=query.Sweep(in sweep);
            if(contact is {} hit)
            {
                double fraction=hit.Fraction;
                if(!double.IsFinite(fraction) || fraction<0 || fraction>1)throw new InvalidOperationException("Projectile query returned an invalid contact fraction.");
                double consumed=fraction==1 ? segment.End : fraction==0 ? segment.Start : segment.Start+sweep.Seconds*fraction;
                Vector3d center=Add(sweep.Origin,Scale(sweep.Direction,sweep.Distance*fraction));
                double age=consumed==total && total==remaining ? definition.Lifetime : state.Age+consumed;
                var stopped=new ProjectileState(center,VelocityAt(in state,in definition,consumed),age,ProjectilePhase.Impacted);
                return new(stopped,contact,i+1,consumed,maximumDeviation);
            }
        }
        bool expires=total==remaining;
        return new(new(endPosition,endVelocity,expires ? definition.Lifetime : state.Age+total,expires ? ProjectilePhase.Expired : ProjectilePhase.Flying),null,segments,total,maximumDeviation);
    }

    /// <summary>Uses the existing native sphere/capsule query service; the game declares ICapsuleCastGame.
    /// Queries see committed geometry, including staged marker poses only after their tick commits.
    /// Ignore IDs must be unique/nonzero, at most128, and live in the native runtime.</summary>
    public static ProjectileStep Step(in ProjectileDefinition definition,in ProjectileState state,double deltaTime,GameContext context,ReadOnlySpan<EntityId> ignore=default)
    {
        if(ignore.Length>128)throw new ArgumentOutOfRangeException(nameof(ignore));
        for(int i=0;i<ignore.Length;++i)
        {
            if(ignore[i]==default)throw new ArgumentOutOfRangeException(nameof(ignore));
            for(int j=0;j<i;++j)if(ignore[i]==ignore[j])throw new ArgumentOutOfRangeException(nameof(ignore));
        }
        var adapter=new NativeQueries(context,ignore);
        return Step(in definition,in state,deltaTime,ref adapter);
    }

    private ref struct NativeQueries:IProjectileQueries
    {
        private readonly GameContext context;
        private readonly ReadOnlySpan<EntityId> ignore;
        public NativeQueries(GameContext context,ReadOnlySpan<EntityId> ignore){this.context=context;this.ignore=ignore;}
        public CapsuleCastHit? Sweep(in ProjectileSweep sweep)=>context.CapsuleCast(sweep.Origin,new(0,0,0,1),sweep.Radius,0,sweep.Direction,sweep.Distance,ignore);
    }
    private static void Validate(in ProjectileDefinition definition,in ProjectileState state,double dt)
    {
        ValidateVector(definition.Acceleration,MotionBound,nameof(definition));
        if(!double.IsFinite(definition.Radius) || definition.Radius<.001 || definition.Radius>9 ||
            !double.IsFinite(definition.Lifetime) || definition.Lifetime<=0 || definition.Lifetime>3600 ||
            !double.IsFinite(definition.MaxChordError) || definition.MaxChordError<1e-6 || definition.MaxChordError>1 ||
            definition.MaxSegments<1 || definition.MaxSegments>32)throw new ArgumentOutOfRangeException(nameof(definition));
        ValidateVector(state.Position,PositionBound,nameof(state));ValidateVector(state.Velocity,MotionBound,nameof(state));
        if(!double.IsFinite(state.Age) || state.Age<0 || state.Age>definition.Lifetime || state.Phase is not(ProjectilePhase.Flying or ProjectilePhase.Impacted or ProjectilePhase.Expired))throw new ArgumentOutOfRangeException(nameof(state));
        if(!double.IsFinite(dt) || dt<=0 || dt>1)throw new ArgumentOutOfRangeException(nameof(dt));
    }
    private static void ValidateVector(Vector3d v,double bound,string name)
    {
        if(!double.IsFinite(v.X) || !double.IsFinite(v.Y) || !double.IsFinite(v.Z) || Math.Abs(v.X)>bound || Math.Abs(v.Y)>bound || Math.Abs(v.Z)>bound)throw new ArgumentOutOfRangeException(name);
    }
    private static void ArithmeticVector(Vector3d v,double bound)
    {
        if(!double.IsFinite(v.X) || !double.IsFinite(v.Y) || !double.IsFinite(v.Z) || Math.Abs(v.X)>bound || Math.Abs(v.Y)>bound || Math.Abs(v.Z)>bound)throw new ArithmeticException("Projectile trajectory exceeds the finite query/state domain.");
    }
    private static double Length(Vector3d v)
    {
        double scale=Math.Max(Math.Abs(v.X),Math.Max(Math.Abs(v.Y),Math.Abs(v.Z)));
        if(scale==0)return 0;
        double x=v.X/scale,y=v.Y/scale,z=v.Z/scale;return scale*Math.Sqrt(x*x+y*y+z*z);
    }
    private static double UpperProduct(double a,double b)=>a==0 || b==0 ? 0 : Math.BitIncrement(a*b);
    private static double UpperLength(Vector3d v)
    {
        if(v.X==0 && v.Y==0 && v.Z==0)return 0;
        double x=Math.Abs(v.X),y=Math.Abs(v.Y),z=Math.Abs(v.Z);
        double sum=Math.BitIncrement(UpperProduct(x,x)+UpperProduct(y,y));
        sum=Math.BitIncrement(sum+UpperProduct(z,z));
        return Math.BitIncrement(Math.Sqrt(sum));
    }
    private static Vector3d Add(Vector3d a,Vector3d b)=>new(a.X+b.X,a.Y+b.Y,a.Z+b.Z);
    private static Vector3d Scale(Vector3d v,double k)=>new(v.X*k,v.Y*k,v.Z*k);
    private static Vector3d PositionAt(in ProjectileState state,in ProjectileDefinition definition,double t)=>new(
        Math.FusedMultiplyAdd(.5*definition.Acceleration.X*t,t,Math.FusedMultiplyAdd(state.Velocity.X,t,state.Position.X)),
        Math.FusedMultiplyAdd(.5*definition.Acceleration.Y*t,t,Math.FusedMultiplyAdd(state.Velocity.Y,t,state.Position.Y)),
        Math.FusedMultiplyAdd(.5*definition.Acceleration.Z*t,t,Math.FusedMultiplyAdd(state.Velocity.Z,t,state.Position.Z)));
    private static Vector3d VelocityAt(in ProjectileState state,in ProjectileDefinition definition,double t)=>new(
        Math.FusedMultiplyAdd(definition.Acceleration.X,t,state.Velocity.X),Math.FusedMultiplyAdd(definition.Acceleration.Y,t,state.Velocity.Y),Math.FusedMultiplyAdd(definition.Acceleration.Z,t,state.Velocity.Z));
    private static void ValidateTrajectory(in ProjectileDefinition definition,in ProjectileState state,double total,Vector3d endPosition,Vector3d endVelocity)
    {
        ArithmeticVector(endPosition,PositionBound);ArithmeticVector(endVelocity,MotionBound);
        Axis(state.Position.X,state.Velocity.X,definition.Acceleration.X,total);Axis(state.Position.Y,state.Velocity.Y,definition.Acceleration.Y,total);Axis(state.Position.Z,state.Velocity.Z,definition.Acceleration.Z,total);
        static void Axis(double p,double v,double a,double total)
        {
            if(a==0)return;double t=-v/a;
            if(t>0 && t<total)
            {
                double value=Math.FusedMultiplyAdd(.5*a*t,t,Math.FusedMultiplyAdd(v,t,p));
                if(!double.IsFinite(value) || Math.Abs(value)>PositionBound)throw new ArithmeticException("Projectile axis extremum exceeds the query domain.");
            }
        }
    }
    private static void Times(double total,int count,int index,out double start,out double end)
    {start=total*index/count;end=index+1==count ? total : total*(index+1)/count;}
    private static double Deviation(double acceleration,double seconds)
    {
        if(acceleration==0)return 0;
        double duration=Math.BitIncrement(seconds);
        double result=UpperProduct(UpperProduct(acceleration,duration),duration)/8;
        return Math.BitIncrement(result); // Also retain a positive subnormal envelope after underflow.
    }
    private static double InflatedRadius(double radius,double deviation)
    {
        double sum=radius+deviation;
        if(deviation>0)sum=Math.BitIncrement(sum); // Positive curvature must not disappear in double addition.
        float rounded=(float)sum;
        if((double)rounded<sum)rounded=MathF.BitIncrement(rounded);
        return rounded;
    }
    private static Segment MakeSegment(in ProjectileDefinition definition,in ProjectileState state,double total,int count,int index,double acceleration)
    {
        Times(total,count,index,out double start,out double end);double seconds=end-start;
        if(!double.IsFinite(start) || !double.IsFinite(end) || !double.IsFinite(seconds) || start<0 || end>total || !(seconds>0))throw new ArithmeticException("Projectile segment time is not representable.");
        Vector3d origin=PositionAt(in state,in definition,start),endpoint=PositionAt(in state,in definition,end);
        ArithmeticVector(origin,PositionBound);ArithmeticVector(endpoint,PositionBound);
        Vector3d delta=new(endpoint.X-origin.X,endpoint.Y-origin.Y,endpoint.Z-origin.Z);double distance=Length(delta);
        if(!double.IsFinite(distance) || distance>TravelBound)throw new ArithmeticException("Projectile chord exceeds native travel bounds.");
        Vector3d direction=distance==0 ? new(1,0,0) : new(delta.X/distance,delta.Y/distance,delta.Z/distance);
        ArithmeticVector(direction,1e6);double directionLength=Math.Sqrt(direction.X*direction.X+direction.Y*direction.Y+direction.Z*direction.Z);
        if(!double.IsFinite(directionLength) || directionLength<1e-12)throw new ArithmeticException("Projectile chord direction is not representable.");
        // Native API normalizes the submitted direction. Check that actual
        // endpoint reconstruction as well as the mathematical chord endpoint.
        ArithmeticVector(Add(origin,Scale(direction,distance)),PositionBound);
        ArithmeticVector(new(origin.X+direction.X/directionLength*distance,origin.Y+direction.Y/directionLength*distance,origin.Z+direction.Z/directionLength*distance),PositionBound);
        double deviation=Deviation(acceleration,seconds),radius=InflatedRadius(definition.Radius,deviation);
        if(!double.IsFinite(deviation) || deviation>definition.MaxChordError || !double.IsFinite(radius) || radius<.001 || radius>10)throw new ArithmeticException("Projectile swept envelope exceeds native bounds.");
        return new(new(origin,direction,distance,radius,seconds,deviation),start,end);
    }
}
