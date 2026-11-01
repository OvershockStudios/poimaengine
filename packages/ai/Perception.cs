// SPDX-License-Identifier: Apache-2.0
using System;
namespace Poima.Ai;

/// <summary>Why pure geometry rejected a sample. Precedence is Coincident, range, cone, horizontal, vertical.</summary>
public enum SightReject : byte { None=0,Coincident=1,TooClose=2,TooFar=3,OutsideCone=4,OutsideHorizontal=5,OutsideVertical=6 }
/// <summary>FirstVisible stops casting after the first visible sample; All casts every accepted sample within budget.</summary>
public enum SightSampling : byte { FirstVisible=0,All=1 }
/// <summary>Per-sample outcome. Only Clear, TargetHit and Occluded issued a ray. BudgetExhausted and Skipped are
/// unchecked: they are never counted as visible or occluded.</summary>
public enum SightSampleStatus : byte { None=0,Rejected=1,BudgetExhausted=2,Skipped=3,Clear=4,TargetHit=5,Occluded=6 }
/// <summary>Visible: a cast sample was Clear or hit a declared target collider. Occluded: every accepted sample was cast
/// and none was visible. OutOfView: geometry rejected every sample (zero rays). Inconclusive: nothing visible, and at
/// least one accepted sample was left unchecked by the caller's ray budget.</summary>
public enum SightStatus : byte { None=0,Visible=1,Occluded=2,OutOfView=3,Inconclusive=4 }

/// <summary>Eye position in metres. Forward and Up are unit vectors (within <see cref="Sight.UnitTolerance"/>), not
/// parallel. Up is a reference: it is orthogonalized against Forward to define local horizontal/vertical.</summary>
public readonly record struct SightObserver(Vector3d Eye,Vector3d Forward,Vector3d Up);

/// <summary>Angles in radians, ranges in metres. Every sample must satisfy all three angular limits and the inclusive
/// range. HalfAngle is the circular cone about Forward [0,π]; HorizontalHalfAngle limits |azimuth| [0,π];
/// VerticalHalfAngle limits |elevation| [0,π/2]. π, π and π/2 disable the respective limit. Range is
/// MinRange [.001, MaxRange] through MaxRange [MinRange, 10000]. default(SightSettings) is invalid.</summary>
public readonly record struct SightSettings(double HalfAngle,double HorizontalHalfAngle,double VerticalHalfAngle,
    double MinRange,double MaxRange,SightSampling Sampling)
{
    /// <summary>Circular cone; yaw/pitch limits disabled.</summary>
    public static SightSettings Cone(double halfAngle,double minRange,double maxRange,SightSampling sampling=SightSampling.FirstVisible)
        =>new(halfAngle,Math.PI,Math.PI/2,minRange,maxRange,sampling);
    /// <summary>Angular rectangle in the observer's local azimuth/elevation; cone disabled.</summary>
    public static SightSettings YawPitch(double horizontalHalfAngle,double verticalHalfAngle,double minRange,double maxRange,SightSampling sampling=SightSampling.FirstVisible)
        =>new(Math.PI,horizontalHalfAngle,verticalHalfAngle,minRange,maxRange,sampling);
}

/// <summary>Pure measurement of one point. Distance is eye-to-point in metres. Angle is the angle off Forward [0,π].
/// Azimuth is positive toward the observer's right (Forward × Up), in (−π,π]; Elevation is positive toward local up,
/// in [−π/2,π/2]. Directly along local up/down Azimuth is defined as 0. A Coincident point reports 0 for every value
/// and is never accepted, so it cannot be confused with an exactly on-axis point.</summary>
public readonly record struct SightGeometry(SightReject Reject,double Distance,double Angle,double Azimuth,double Elevation)
{ public bool Accepted=>Reject==SightReject.None; }

/// <summary>Transient per-sample result. HitEntity/HitDistance describe the provider's first collider for
/// TargetHit/Occluded and are default/0 otherwise. A hit at distance 0 means the eye starts inside or touches that
/// collider, typically an unignored self collider.</summary>
public readonly record struct SightSample(Vector3d Point,SightGeometry Geometry,SightSampleStatus Status,EntityId HitEntity,double HitDistance)
{
    public bool Visible=>Status is SightSampleStatus.Clear or SightSampleStatus.TargetHit;
    public bool RayCast=>Status is SightSampleStatus.Clear or SightSampleStatus.TargetHit or SightSampleStatus.Occluded;
}

/// <summary>Transient aggregate. FirstVisible is a sample index or −1. Counts partition Samples:
/// Rejected + Clear + TargetHit + Occluded + Unchecked + Skipped == Samples, and RaysCast == Clear + TargetHit + Occluded.</summary>
public readonly record struct SightResult(SightStatus Status,int Samples,int RejectedSamples,int RaysCast,int ClearSamples,
    int TargetHitSamples,int OccludedSamples,int UncheckedSamples,int SkippedSamples,int FirstVisible)
{
    public bool Visible=>Status==SightStatus.Visible;
    public bool BudgetExhausted=>UncheckedSamples>0;
    public int AcceptedSamples=>Samples-RejectedSamples;
}

/// <summary>One closest-hit segment query from origin along a unit direction for distance metres, excluding the
/// caller's ignore list. Return null for no collider through the endpoint. Exceptions propagate.</summary>
public interface ISightQueries { RayHit? Cast(Vector3d origin,Vector3d direction,double distance,ReadOnlySpan<EntityId> ignore); }

/// <summary>Stateless sight cones with caller-owned sample points, budgets and collider identities.</summary>
/// <remarks>
/// All validation happens before the first query; invalid inputs throw ArgumentOutOfRangeException (numeric domain,
/// counts) or ArgumentException (IDs, destination length). Results are written to the destination only after every
/// query succeeds, so exceptions leave it unchanged. Provider hits with a zero/ignored entity or a distance outside
/// [0, query distance] throw InvalidOperationException. No allocation, statics, clock, randomness or caching is used.
///
/// Visibility is per authored sample point, not silhouette coverage. A ray runs from the eye exactly to the point;
/// the closest collider decides: a declared target collider is TargetHit, any other collider is Occluded, none is
/// Clear. Clear and TargetHit both establish sight; inspect ClearSamples/TargetHitSamples for a stricter policy.
/// Undeclared parts of a compound target occlude (fail closed). Same inputs and provider replies give identical
/// results in one process; atan2/sqrt-based diagnostics are not promised bit-identical across platforms.
/// </remarks>
public static class Sight
{
    public const int MaxSamples=8,MaxTargetColliders=32,MaxIgnored=128;
    /// <summary>Per-axis eye/sample bound in metres, well inside the native ±1,000,000 ray endpoint guard.</summary>
    public const double PositionBound=1e5;
    /// <summary>Native ray distance domain.</summary>
    public const double MinRangeLimit=.001,MaxRangeLimit=10000;
    public const double UnitTolerance=1e-6;
    /// <summary>Minimum |Forward × Up| for a defined local frame.</summary>
    public const double MinUpSeparation=1e-6;

    private readonly record struct Basis(Vector3d Forward,Vector3d Right,Vector3d Up);

    /// <summary>Validates observer, settings and point, then measures without any query.</summary>
    public static SightGeometry Classify(in SightObserver observer,in SightSettings settings,Vector3d point)
    {
        var basis=Prepare(in observer,in settings);
        CheckPoint(point,nameof(point));
        return Measure(in basis,in settings,observer.Eye,point,out _);
    }

    /// <summary>Evaluates 1–8 samples in order against a caller-owned provider without boxing or allocation.
    /// Each accepted sample consumes one unit of rayBudget (≥0) until it is spent; rejected and skipped samples
    /// consume none. results must hold at least samples.Length entries.</summary>
    public static SightResult Evaluate<TQuery>(in SightObserver observer,in SightSettings settings,ReadOnlySpan<Vector3d> samples,
        ReadOnlySpan<EntityId> targetColliders,ReadOnlySpan<EntityId> ignore,int rayBudget,Span<SightSample> results,ref TQuery query)
        where TQuery:ISightQueries,allows ref struct
    {
        var basis=Prepare(in observer,in settings);
        ValidateInputs(samples,targetColliders,ignore,rayBudget,results.Length);
        Span<SightSample> buffer=stackalloc SightSample[MaxSamples];
        int remaining=rayBudget,rays=0,rejected=0,clear=0,targetHits=0,occluded=0,pending=0,skipped=0,first=-1;
        for(int i=0;i<samples.Length;++i)
        {
            Vector3d point=samples[i];
            var geometry=Measure(in basis,in settings,observer.Eye,point,out Vector3d direction);
            SightSampleStatus status;EntityId hitEntity=default;double hitDistance=0;
            if(!geometry.Accepted){status=SightSampleStatus.Rejected;++rejected;}
            else if(settings.Sampling==SightSampling.FirstVisible && first>=0){status=SightSampleStatus.Skipped;++skipped;}
            else if(remaining==0){status=SightSampleStatus.BudgetExhausted;++pending;}
            else
            {
                --remaining;++rays;
                RayHit? reply=query.Cast(observer.Eye,direction,geometry.Distance,ignore);
                if(reply is not {} hit){status=SightSampleStatus.Clear;++clear;}
                else
                {
                    if(hit.Entity==default || !double.IsFinite(hit.Distance) || hit.Distance<0 || hit.Distance>geometry.Distance || ignore.Contains(hit.Entity))
                        throw new InvalidOperationException("Sight query returned a zero/ignored entity or a distance outside the queried segment.");
                    hitEntity=hit.Entity;hitDistance=hit.Distance;
                    if(targetColliders.Contains(hit.Entity)){status=SightSampleStatus.TargetHit;++targetHits;}
                    else{status=SightSampleStatus.Occluded;++occluded;}
                }
                if(first<0 && (status==SightSampleStatus.Clear || status==SightSampleStatus.TargetHit))first=i;
            }
            buffer[i]=new(point,geometry,status,hitEntity,hitDistance);
        }
        SightStatus overall=first>=0 ? SightStatus.Visible : rejected==samples.Length ? SightStatus.OutOfView :
            pending>0 ? SightStatus.Inconclusive : SightStatus.Occluded;
        buffer[..samples.Length].CopyTo(results);
        return new(overall,samples.Length,rejected,rays,clear,targetHits,occluded,pending,skipped,first);
    }

    /// <summary>Uses the existing GameContext.Raycast service (no new ABI or marker). After pure validation, every
    /// ignore and target ID must be a live runtime entity (checked with IsAlive before the first ray). Ignore lists
    /// exclude bodies, not descendants: list every self and compound-target collider explicitly. Queries see committed
    /// native geometry, not motion staged earlier in this Tick. Callers own any aggregate per-Tick ray budget.</summary>
    public static SightResult Evaluate(in SightObserver observer,in SightSettings settings,ReadOnlySpan<Vector3d> samples,
        ReadOnlySpan<EntityId> targetColliders,ReadOnlySpan<EntityId> ignore,int rayBudget,Span<SightSample> results,GameContext context)
    {
        _=Prepare(in observer,in settings);
        ValidateInputs(samples,targetColliders,ignore,rayBudget,results.Length);
        foreach(var id in ignore)
            if(!context.IsAlive(id))throw new ArgumentException("Ignored sight collider is not a live runtime entity.",nameof(ignore));
        foreach(var id in targetColliders)
            if(!context.IsAlive(id))throw new ArgumentException("Target sight collider is not a live runtime entity.",nameof(targetColliders));
        var adapter=new NativeQueries(context);
        return Evaluate(in observer,in settings,samples,targetColliders,ignore,rayBudget,results,ref adapter);
    }

    private ref struct NativeQueries:ISightQueries
    {
        private readonly GameContext context;
        public NativeQueries(GameContext context){this.context=context;}
        public RayHit? Cast(Vector3d origin,Vector3d direction,double distance,ReadOnlySpan<EntityId> ignore)=>context.Raycast(origin,direction,distance,ignore);
    }

    private static Basis Prepare(in SightObserver observer,in SightSettings settings)
    {
        CheckPoint(observer.Eye,nameof(observer));
        Vector3d forward=Unit(observer.Forward),up=Unit(observer.Up);
        Vector3d cross=Cross(forward,up);double separation=Length(cross);
        if(!(separation>=MinUpSeparation))throw new ArgumentOutOfRangeException(nameof(observer),"Observer Up must not be parallel to Forward.");
        Vector3d right=new(cross.X/separation,cross.Y/separation,cross.Z/separation);
        if(!(settings.HalfAngle>=0 && settings.HalfAngle<=Math.PI) || !(settings.HorizontalHalfAngle>=0 && settings.HorizontalHalfAngle<=Math.PI) ||
            !(settings.VerticalHalfAngle>=0 && settings.VerticalHalfAngle<=Math.PI/2))
            throw new ArgumentOutOfRangeException(nameof(settings),"Sight angles must be finite radians: cone/horizontal 0..π, vertical 0..π/2.");
        if(!(settings.MinRange>=MinRangeLimit && settings.MaxRange<=MaxRangeLimit && settings.MinRange<=settings.MaxRange))
            throw new ArgumentOutOfRangeException(nameof(settings),"Sight range must satisfy .001 <= MinRange <= MaxRange <= 10000 metres.");
        if(settings.Sampling is not (SightSampling.FirstVisible or SightSampling.All))
            throw new ArgumentOutOfRangeException(nameof(settings),"Unknown sight sampling policy.");
        return new(forward,right,Cross(right,forward));
    }

    private static void ValidateInputs(ReadOnlySpan<Vector3d> samples,ReadOnlySpan<EntityId> targets,ReadOnlySpan<EntityId> ignore,int budget,int destination)
    {
        if(samples.Length is <1 or >MaxSamples)throw new ArgumentOutOfRangeException(nameof(samples),"Sight needs 1..8 sample points.");
        foreach(var point in samples)CheckPoint(point,nameof(samples));
        if(targets.Length>MaxTargetColliders)throw new ArgumentOutOfRangeException("targetColliders","At most 32 target colliders.");
        if(ignore.Length>MaxIgnored)throw new ArgumentOutOfRangeException(nameof(ignore),"At most 128 ignored colliders.");
        CheckIds(targets,"targetColliders");CheckIds(ignore,nameof(ignore));
        foreach(var id in targets)
            if(ignore.Contains(id))throw new ArgumentException("A target collider cannot also be ignored.",nameof(ignore));
        if(budget<0)throw new ArgumentOutOfRangeException("rayBudget","Ray budget cannot be negative.");
        if(destination<samples.Length)throw new ArgumentException("Destination is shorter than the sample list.","results");
    }

    private static void CheckIds(ReadOnlySpan<EntityId> ids,string name)
    {
        for(int i=0;i<ids.Length;++i)
        {
            if(ids[i]==default)throw new ArgumentException("Collider IDs must be nonzero.",name);
            for(int j=0;j<i;++j)if(ids[i]==ids[j])throw new ArgumentException("Collider IDs must be unique.",name);
        }
    }

    private static void CheckPoint(Vector3d v,string name)
    {
        if(!(Math.Abs(v.X)<=PositionBound) || !(Math.Abs(v.Y)<=PositionBound) || !(Math.Abs(v.Z)<=PositionBound))
            throw new ArgumentOutOfRangeException(name,"Sight positions must be finite and within ±100,000 metres per axis.");
    }

    private static Vector3d Unit(Vector3d v)
    {
        if(!double.IsFinite(v.X) || !double.IsFinite(v.Y) || !double.IsFinite(v.Z))
            throw new ArgumentOutOfRangeException("observer","Observer directions must be finite.");
        double length=Length(v);
        if(!(Math.Abs(length-1)<=UnitTolerance))throw new ArgumentOutOfRangeException("observer","Observer Forward and Up must be unit vectors.");
        return new(v.X/length,v.Y/length,v.Z/length);
    }

    // Range precedes angles. Angles use atan2 on a max-component-scaled delta: no tangent singularity at 90°,
    // no acos precision loss near the axis and no underflow/overflow for any representable nonzero delta.
    private static SightGeometry Measure(in Basis basis,in SightSettings settings,Vector3d eye,Vector3d point,out Vector3d direction)
    {
        double dx=point.X-eye.X,dy=point.Y-eye.Y,dz=point.Z-eye.Z;
        direction=default;
        if(dx==0 && dy==0 && dz==0)return new(SightReject.Coincident,0,0,0,0);
        double scale=Math.Max(Math.Abs(dx),Math.Max(Math.Abs(dy),Math.Abs(dz)));
        Vector3d s=new(dx/scale,dy/scale,dz/scale);double n=Length(s),distance=scale*n;
        direction=new(s.X/n,s.Y/n,s.Z/n);
        double angle=Math.Atan2(Length(Cross(basis.Forward,s)),Dot(basis.Forward,s));
        double x=Dot(s,basis.Right),y=Dot(s,basis.Up),z=Dot(s,basis.Forward);
        double azimuth=x==0 && z==0 ? 0 : Math.Atan2(x,z);
        double elevation=Math.Atan2(y,Math.Sqrt(x*x+z*z));
        SightReject reject=distance<settings.MinRange ? SightReject.TooClose : distance>settings.MaxRange ? SightReject.TooFar :
            angle>settings.HalfAngle ? SightReject.OutsideCone : Math.Abs(azimuth)>settings.HorizontalHalfAngle ? SightReject.OutsideHorizontal :
            Math.Abs(elevation)>settings.VerticalHalfAngle ? SightReject.OutsideVertical : SightReject.None;
        return new(reject,distance,angle,azimuth,elevation);
    }

    private static double Dot(Vector3d a,Vector3d b)=>a.X*b.X+a.Y*b.Y+a.Z*b.Z;
    private static double Length(Vector3d v)=>Math.Sqrt(v.X*v.X+v.Y*v.Y+v.Z*v.Z);
    private static Vector3d Cross(Vector3d a,Vector3d b)=>new(a.Y*b.Z-a.Z*b.Y,a.Z*b.X-a.X*b.Z,a.X*b.Y-a.Y*b.X);
}

/// <summary>Empty: nothing remembered. Seeing: seen on the latest update. Remembering: unseen, within retention.
/// Expired: retention elapsed; target, position and ticks are retained for inspection until the game forgets.</summary>
public enum SightMemoryPhase : byte { Empty=0,Seeing=1,Remembering=2,Expired=3 }
/// <summary>Transitions produced by one update. Lost and Expired can occur together.</summary>
[Flags] public enum SightMemoryEvents : byte { None=0,Acquired=1,Refreshed=2,Lost=4,Expired=8,Reacquired=16 }

/// <summary>Transient single-target memory slot. Persist each scalar operand explicitly in game state; this record is
/// not a supported nested state field. FirstSeenTick starts the current/latest continuous sighting. RetainUntilTick is
/// the last tick at which an unseen update still remembers; it is fixed when the target is seen.</summary>
public readonly record struct SightMemoryState(SightMemoryPhase Phase,EntityId Target,Vector3d LastKnownPosition,
    long FirstSeenTick,long LastSeenTick,long RetainUntilTick,long LastUpdateTick,long Reacquisitions)
{ public static SightMemoryState Empty=>default; }
public readonly record struct SightMemoryStep(SightMemoryState State,SightMemoryEvents Events);

/// <summary>Explicit last-seen memory over fixed simulation ticks. Stateless; no clock, allocation or hidden state.</summary>
/// <remarks>
/// A non-Empty slot accepts one update per strictly increasing tick, for its own target only; forget it (assign
/// Empty) before tracking another target. Combine several sensors into one seen/unseen update per tick. An Empty slot
/// ignores unseen updates. Unseen means "not positively seen": an Inconclusive sight result is unseen, never occluded.
/// Invalid states/inputs throw before a result is produced, so the caller's stored fields remain unchanged.
/// </remarks>
public static class SightMemory
{
    /// <summary>Largest tick, matching the engine's lossless JSON integer bound.</summary>
    public const long MaxTick=9_007_199_254_740_991L;
    /// <summary>Largest retention (about 193 days at 60 Hz).</summary>
    public const long MaxRetainTicks=1_000_000_000L;

    /// <summary>Converts GameContext.Tick, rejecting values beyond MaxTick.</summary>
    public static long Tick(ulong tick)=>tick<=(ulong)MaxTick ? (long)tick : throw new ArgumentOutOfRangeException(nameof(tick),"Tick exceeds the bounded memory domain.");

    /// <summary>seenAt is the game-chosen known position (for example the target root), or null when not seen.
    /// A sighting sets RetainUntilTick = tick + retainTicks; an unseen update after it expires the memory.</summary>
    public static SightMemoryStep Update(in SightMemoryState state,EntityId target,Vector3d? seenAt,long tick,long retainTicks)
    {
        Validate(in state);
        if(target==default)throw new ArgumentException("Memory target must be a nonzero entity ID.",nameof(target));
        if(tick is <0 or >MaxTick)throw new ArgumentOutOfRangeException(nameof(tick),"Tick must be within 0..MaxTick.");
        if(retainTicks is <0 or >MaxRetainTicks)throw new ArgumentOutOfRangeException(nameof(retainTicks),"Retention must be within 0..MaxRetainTicks.");
        if(seenAt is {} seen && !Bounded(seen))throw new ArgumentOutOfRangeException(nameof(seenAt),"Known position must be finite and within ±100,000 metres.");
        if(state.Phase==SightMemoryPhase.Empty)
        {
            if(seenAt is not {} acquired)return new(state,SightMemoryEvents.None);
            return new(new(SightMemoryPhase.Seeing,target,acquired,tick,tick,tick+retainTicks,tick,0),SightMemoryEvents.Acquired);
        }
        if(target!=state.Target)throw new ArgumentException("Memory slot belongs to another target; forget it first.",nameof(target));
        if(tick<=state.LastUpdateTick)throw new ArgumentOutOfRangeException(nameof(tick),"Memory ticks must strictly increase.");
        if(seenAt is {} position)
        {
            bool continuing=state.Phase==SightMemoryPhase.Seeing;
            long reacquisitions=continuing ? state.Reacquisitions : state.Reacquisitions+1;
            if(reacquisitions>MaxTick)throw new ArithmeticException("Reacquisition count exceeds its bound.");
            return new(new(SightMemoryPhase.Seeing,target,position,continuing ? state.FirstSeenTick : tick,tick,tick+retainTicks,tick,reacquisitions),
                continuing ? SightMemoryEvents.Refreshed : SightMemoryEvents.Reacquired);
        }
        var phase=tick>state.RetainUntilTick ? SightMemoryPhase.Expired : SightMemoryPhase.Remembering;
        var events=(state.Phase==SightMemoryPhase.Seeing ? SightMemoryEvents.Lost : SightMemoryEvents.None)|
            (phase==SightMemoryPhase.Expired && state.Phase!=SightMemoryPhase.Expired ? SightMemoryEvents.Expired : SightMemoryEvents.None);
        return new(state with {Phase=phase,LastUpdateTick=tick},events);
    }

    /// <summary>First tick at which an unseen update expires a non-Empty memory.</summary>
    public static long FirstExpiredTick(in SightMemoryState state)
    {
        Validate(in state);
        if(state.Phase==SightMemoryPhase.Empty)throw new ArgumentOutOfRangeException(nameof(state),"Empty memory has no expiry.");
        return state.RetainUntilTick+1;
    }

    /// <summary>Ticks since the last sighting; tick must not precede the latest update.</summary>
    public static long TicksSinceSeen(in SightMemoryState state,long tick)
    {
        Validate(in state);
        if(state.Phase==SightMemoryPhase.Empty)throw new ArgumentOutOfRangeException(nameof(state),"Empty memory has no sighting.");
        if(tick<state.LastUpdateTick || tick>MaxTick)throw new ArgumentOutOfRangeException(nameof(tick),"Tick precedes the latest update or exceeds MaxTick.");
        return tick-state.LastSeenTick;
    }

    /// <summary>Checks the complete invariant set, for example after restoring game fields.</summary>
    public static void Validate(in SightMemoryState state)
    {
        const string name="state";
        if(state.Phase==SightMemoryPhase.Empty)
        {
            if(state!=default)throw new ArgumentOutOfRangeException(name,"Empty memory must have all fields zero.");
            return;
        }
        if(state.Phase is not (SightMemoryPhase.Seeing or SightMemoryPhase.Remembering or SightMemoryPhase.Expired))
            throw new ArgumentOutOfRangeException(name,"Unknown memory phase.");
        if(state.Target==default || !Bounded(state.LastKnownPosition))
            throw new ArgumentOutOfRangeException(name,"Remembered target and bounded finite position are required.");
        if(!(state.FirstSeenTick>=0 && state.FirstSeenTick<=state.LastSeenTick && state.LastSeenTick<=state.LastUpdateTick && state.LastUpdateTick<=MaxTick) ||
            !(state.RetainUntilTick>=state.LastSeenTick && state.RetainUntilTick-state.LastSeenTick<=MaxRetainTicks) ||
            state.Reacquisitions is <0 or >MaxTick)
            throw new ArgumentOutOfRangeException(name,"Memory ticks, retention or reacquisition count are out of order or bounds.");
        bool consistent=state.Phase switch
        {
            SightMemoryPhase.Seeing=>state.LastUpdateTick==state.LastSeenTick,
            SightMemoryPhase.Remembering=>state.LastUpdateTick>state.LastSeenTick && state.LastUpdateTick<=state.RetainUntilTick,
            _=>state.LastUpdateTick>state.RetainUntilTick
        };
        if(!consistent)throw new ArgumentOutOfRangeException(name,"Memory phase disagrees with its ticks.");
    }

    private static bool Bounded(Vector3d v)=>Math.Abs(v.X)<=Sight.PositionBound && Math.Abs(v.Y)<=Sight.PositionBound && Math.Abs(v.Z)<=Sight.PositionBound;
}
