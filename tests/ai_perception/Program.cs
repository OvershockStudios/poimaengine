// SPDX-License-Identifier: Apache-2.0
using Poima;
using Poima.Ai;

// Synthetic managed checks. Providers here are scripted tapes or an independent analytic box model; they are not
// native physics evidence. Native Raycast behavior requires the compiled sample/protocol fixture.
internal static class Program
{
    private static int checks;
    private static readonly Vector3d Origin=new(0,0,0);
    private static readonly SightObserver Ahead=new(Origin,new(0,0,-1),new(0,1,0));
    private static readonly SightSettings Wide=SightSettings.Cone(Math.PI,.001,10000);

    private static EntityId E(ulong id)=>new(0,id);
    private static void Require(bool condition,string name)
    {
        ++checks;
        if(!condition)throw new InvalidOperationException(name);
    }
    private static void Near(double actual,double expected,string name,double tolerance=1e-15)
    {
        Require(double.IsFinite(actual) && Math.Abs(actual-expected)<=tolerance*Math.Max(1,Math.Abs(expected)),$"{name}: {actual:R} != {expected:R}");
    }
    private static SightGeometry G(SightObserver o,SightSettings s,Vector3d p)=>Sight.Classify(o,s,p);
    private static Vector3d Dir(double azimuth,double elevation,double r)=>
        new(Math.Sin(azimuth)*Math.Cos(elevation)*r,Math.Sin(elevation)*r,-Math.Cos(azimuth)*Math.Cos(elevation)*r);
    private static RayHit Hit(ulong id,double distance)=>new(E(id),distance,new(0,0,-distance),null);

    // Records every query; replies by call order; can throw on a chosen 1-based call.
    private struct Tape:ISightQueries
    {
        public Vector3d[] Origins,Directions;
        public double[] Distances;
        public RayHit?[] Replies;
        public EntityId[]? ExpectedIgnore;
        public int Count,IgnoreMismatches,ThrowAt;
        public Exception? Failure;
        public static Tape Create(params RayHit?[] replies)=>new(){Origins=new Vector3d[16],Directions=new Vector3d[16],Distances=new double[16],Replies=replies};
        public RayHit? Cast(Vector3d origin,Vector3d direction,double distance,ReadOnlySpan<EntityId> ignore)
        {
            int i=Count++;Origins[i]=origin;Directions[i]=direction;Distances[i]=distance;
            if(ExpectedIgnore is {} expected && !ignore.SequenceEqual((ReadOnlySpan<EntityId>)expected))++IgnoreMismatches;
            if(Count==ThrowAt)throw Failure!;
            return i<Replies.Length ? Replies[i] : null;
        }
    }

    // Independent synthetic model of axis-aligned solid boxes: closest entry along the segment (endpoint
    // inclusive), origin-inside reports distance 0, equal distances choose the smaller (High,Low) ID.
    private readonly record struct Box(EntityId Id,Vector3d Min,Vector3d Max);
    private static Box B(ulong id,Vector3d c,Vector3d e)=>new(E(id),new(c.X-e.X,c.Y-e.Y,c.Z-e.Z),new(c.X+e.X,c.Y+e.Y,c.Z+e.Z));
    private struct Boxes:ISightQueries
    {
        public Box[] Items;
        public int Count;
        public RayHit? Cast(Vector3d o,Vector3d d,double length,ReadOnlySpan<EntityId> ignore)
        {
            ++Count;bool found=false;double best=0;EntityId id=default;
            foreach(var box in Items)
            {
                if(ignore.Contains(box.Id))continue;
                double near=0,far=length;
                if(!Axis(o.X,d.X,box.Min.X,box.Max.X,ref near,ref far) || !Axis(o.Y,d.Y,box.Min.Y,box.Max.Y,ref near,ref far) ||
                    !Axis(o.Z,d.Z,box.Min.Z,box.Max.Z,ref near,ref far))continue;
                bool smaller=box.Id.High<id.High || (box.Id.High==id.High && box.Id.Low<id.Low);
                if(!found || near<best || (near==best && smaller)){found=true;best=near;id=box.Id;}
            }
            return found ? new RayHit(id,best,new(o.X+d.X*best,o.Y+d.Y*best,o.Z+d.Z*best),null) : null;
        }
        private static bool Axis(double o,double d,double min,double max,ref double near,ref double far)
        {
            if(d==0)return o>=min && o<=max;
            double a=(min-o)/d,b=(max-o)/d;if(a>b)(a,b)=(b,a);
            near=Math.Max(near,a);far=Math.Min(far,b);return near<=far;
        }
    }

    private static SightSample[] Sentinels(int count)
    {
        var values=new SightSample[count];
        for(int i=0;i<count;++i)values[i]=new(new(9,9,i),new(SightReject.TooFar,1,2,3,4),SightSampleStatus.Occluded,E(99),7);
        return values;
    }

    private static void ConeBoundaries()
    {
        var axis=G(Ahead,Wide,new(0,0,-5));
        Require(axis.Accepted && axis.Angle==0 && axis.Azimuth==0 && axis.Elevation==0 && axis.Distance==5,"exact on-axis point");
        Require(G(Ahead,SightSettings.Cone(0,.001,10000),new(0,0,-5)).Accepted,"zero-width cone admits the exact axis");
        foreach(double k in new[]{1.0,1000.0})
        {
            var diagonal=G(Ahead,Wide,new(k,0,-k));
            Near(diagonal.Angle,Math.PI/4,"45 degree cone angle");
            Require(G(Ahead,SightSettings.Cone(diagonal.Angle,.001,10000),new(k,0,-k)).Accepted,"cone boundary is inclusive");
            var narrow=G(Ahead,SightSettings.Cone(Math.BitDecrement(diagonal.Angle),.001,10000),new(k,0,-k));
            Require(narrow.Reject==SightReject.OutsideCone && narrow.Angle==diagonal.Angle,"one ulp narrower cone rejects, same diagnostic");
        }
        // 90 degrees is an ordinary atan2 value, not a tangent singularity.
        var side=G(Ahead,Wide,new(3,0,0));
        Require(side.Angle==Math.PI/2 && side.Azimuth==Math.PI/2 && side.Elevation==0,"perpendicular point at pi/2, right is positive");
        Require(G(Ahead,SightSettings.Cone(Math.PI/2,.001,10000),new(3,0,0)).Accepted,"pi/2 cone admits perpendicular");
        Require(G(Ahead,SightSettings.Cone(Math.BitDecrement(Math.PI/2),.001,10000),new(3,0,0)).Reject==SightReject.OutsideCone,"perpendicular outside narrower cone");
        var behind=G(Ahead,Wide,new(0,0,7));
        Require(behind.Angle==Math.PI && behind.Accepted,"omnidirectional cone admits directly behind");
        Require(G(Ahead,SightSettings.Cone(Math.BitDecrement(Math.PI),.001,10000),new(0,0,7)).Reject==SightReject.OutsideCone,"behind outside non-full cone");
        // A 1 nrad offset keeps relative precision (an acos(dot) test would round it to zero).
        var tiny=G(Ahead,Wide,new(1e-6,0,-1000));
        Require(Math.Abs(tiny.Angle/1e-9-1)<1e-12 && Math.Abs(tiny.Azimuth/1e-9-1)<1e-12,"tiny off-axis angle precision");
        var range=SightSettings.Cone(Math.PI,2,5);
        Require(G(Ahead,range,new(0,0,-2)).Accepted && G(Ahead,range,new(0,0,-5)).Accepted,"inclusive min/max range");
        Require(G(Ahead,range,new(0,0,-1.999)).Reject==SightReject.TooClose && G(Ahead,range,new(0,0,-5.001)).Reject==SightReject.TooFar,"range rejection reasons");
        var pythagorean=G(Ahead,range,new(3,0,-4));
        Require(pythagorean.Distance==5 && pythagorean.Accepted,"scaled exact distance");
        Require(G(Ahead,SightSettings.Cone(.1,.001,10),new(100,0,0)).Reject==SightReject.TooFar,"range precedes angle");
        var coincident=G(Ahead,Wide,Origin);
        Require(coincident==new SightGeometry(SightReject.Coincident,0,0,0,0) && !coincident.Accepted,"coincident point rejected with zero diagnostics");
        Require(G(new SightObserver(new(4,5,6),new(0,0,-1),new(0,1,0)),Wide,new(4,5,6)).Reject==SightReject.Coincident,"coincident is not confused with on-axis");
    }

    private static void YawPitchFrames()
    {
        var box=SightSettings.YawPitch(.5,.25,.001,100);
        var inside=G(Ahead,box,Dir(.5-1e-9,0,10));
        Require(inside.Accepted,"just inside horizontal limit");Near(inside.Azimuth,.5-1e-9,"azimuth recovers construction",1e-14);
        Require(G(Ahead,box,Dir(.5+1e-9,0,10)).Reject==SightReject.OutsideHorizontal && G(Ahead,box,Dir(-.5-1e-9,0,10)).Reject==SightReject.OutsideHorizontal,"symmetric horizontal limit");
        Require(G(Ahead,box,Dir(0,.25-1e-9,10)).Accepted && G(Ahead,box,Dir(0,.25+1e-9,10)).Reject==SightReject.OutsideVertical &&
            G(Ahead,box,Dir(0,-.25-1e-9,10)).Reject==SightReject.OutsideVertical,"vertical limit");
        Require(G(Ahead,box,Dir(.6,.3,10)).Reject==SightReject.OutsideHorizontal,"horizontal precedes vertical");
        var combined=new SightSettings(.3,.5,.25,.001,100,SightSampling.FirstVisible);
        Require(G(Ahead,combined,Dir(.4,0,10)).Reject==SightReject.OutsideCone && G(Ahead,combined,Dir(.6,0,10)).Reject==SightReject.OutsideCone,"cone limit intersects and precedes yaw/pitch");
        Near(G(Ahead,Wide,new(2,0,-2)).Azimuth,Math.PI/4,"right positive");Near(G(Ahead,Wide,new(-2,0,-2)).Azimuth,-Math.PI/4,"left negative");
        Near(G(Ahead,Wide,new(0,2,-2)).Elevation,Math.PI/4,"up positive");
        var pole=G(Ahead,Wide,new(0,5,0));
        Require(pole.Azimuth==0 && pole.Elevation==Math.PI/2,"pole azimuth defined as zero");
        Require(G(Ahead,SightSettings.YawPitch(Math.PI,Math.PI/2,.001,100),new(0,5,0)).Accepted,"full vertical limit admits pole");
        Require(G(Ahead,SightSettings.YawPitch(Math.PI,Math.BitDecrement(Math.PI/2),.001,100),new(0,5,0)).Reject==SightReject.OutsideVertical,"pole outside narrower vertical");
        Require(Math.Abs(G(Ahead,Wide,new(0,0,5)).Azimuth)==Math.PI,"behind azimuth magnitude pi");
        Require(G(Ahead,SightSettings.YawPitch(Math.BitDecrement(Math.PI),Math.PI/2,.001,100),new(0,0,5)).Reject==SightReject.OutsideHorizontal &&
            G(Ahead,SightSettings.YawPitch(Math.PI,Math.PI/2,.001,100),new(0,0,5)).Accepted,"behind needs full horizontal limit");
        // Angles are relative to the gaze: pitched 30 degrees down, the world horizon is 30 degrees up.
        var pitched=new SightObserver(Origin,new(0,-.5,-Math.Sqrt(.75)),new(0,1,0));
        var along=G(pitched,Wide,new(0,-5,-10*Math.Sqrt(.75)));
        Require(along.Azimuth==0 && Math.Abs(along.Elevation)<1e-15 && along.Angle<1e-15,"point along pitched gaze");
        var horizon=G(pitched,Wide,new(0,0,-10));
        Near(horizon.Elevation,Math.PI/6,"horizon elevation relative to gaze",1e-14);Require(horizon.Azimuth==0,"horizon centered");
        // Reference up is orthogonalized; a tilted reference gives the same frame.
        var tilted=Ahead with {Up=new(0,.8,.6)};
        foreach(var p in new Vector3d[]{new(1,2,-3),new(-4,.5,-1),new(2,-3,4)})
        {
            var a=G(Ahead,Wide,p);var b=G(tilted,Wide,p);
            Near(b.Azimuth,a.Azimuth,"tilted reference azimuth",1e-15);Near(b.Elevation,a.Elevation,"tilted reference elevation",1e-15);
        }
        // Rigidly rotating observer and points preserves all classifications and diagnostics.
        double kl=Math.Sqrt(.3*.3+.7*.7+.648*.648);var k=new Vector3d(.3/kl,-.7/kl,.648/kl);const double theta=1.234;
        var eye=new Vector3d(1,2,3);var observer=new SightObserver(eye,new(0,0,-1),new(0,1,0));
        var rotated=new SightObserver(Rotate(eye,k,theta),Rotate(observer.Forward,k,theta),Rotate(observer.Up,k,theta));
        var settings=SightSettings.YawPitch(.8,.5,.5,50);
        var expected=new[]{SightReject.None,SightReject.OutsideHorizontal,SightReject.OutsideVertical,SightReject.TooFar,SightReject.TooClose,SightReject.None};
        var offsets=new Vector3d[]{new(.5,.3,-6),new(5,0,-2),new(0,4,-3),new(0,0,-60),new(0,0,-.2),new(-2,-1,-8)};
        for(int i=0;i<offsets.Length;++i)
        {
            var point=new Vector3d(eye.X+offsets[i].X,eye.Y+offsets[i].Y,eye.Z+offsets[i].Z);
            var a=G(observer,settings,point);var b=G(rotated,settings,Rotate(point,k,theta));
            Require(a.Reject==expected[i] && b.Reject==a.Reject,"rotation preserves reason "+i);
            Near(b.Distance,a.Distance,"rotated distance",1e-12);Near(b.Angle,a.Angle,"rotated angle",1e-12);
            Near(b.Azimuth,a.Azimuth,"rotated azimuth",1e-12);Near(b.Elevation,a.Elevation,"rotated elevation",1e-12);
        }
    }
    private static Vector3d Rotate(Vector3d v,Vector3d k,double t)
    {
        double c=Math.Cos(t),s=Math.Sin(t),d=k.X*v.X+k.Y*v.Y+k.Z*v.Z;
        return new(v.X*c+(k.Y*v.Z-k.Z*v.Y)*s+k.X*d*(1-c),v.Y*c+(k.Z*v.X-k.X*v.Z)*s+k.Y*d*(1-c),v.Z*c+(k.X*v.Y-k.Y*v.X)*s+k.Z*d*(1-c));
    }

    private static void Rejects<T>(string label,SightObserver o,SightSettings s,Vector3d[] points,EntityId[]? targets=null,EntityId[]? ignore=null,int budget=4,int resultLength=8) where T:Exception
    {
        var results=Sentinels(resultLength);var before=(SightSample[])results.Clone();var query=Tape.Create();Exception? caught=null;
        try{Sight.Evaluate(o,s,points,targets ?? Array.Empty<EntityId>(),ignore ?? Array.Empty<EntityId>(),budget,results,ref query);}
        catch(Exception error){caught=error;}
        Require(caught?.GetType()==typeof(T),$"{label}: expected {typeof(T).Name}, got {caught?.GetType().Name ?? "none"}");
        Require(query.Count==0,label+" rejects before any query");
        Require(results.AsSpan().SequenceEqual(before.AsSpan()),label+" leaves destination unchanged");
    }
    private static void InvalidInputs()
    {
        Vector3d[] one=[new(0,0,-5)];
        foreach(double bad in new[]{double.NaN,double.PositiveInfinity,double.NegativeInfinity})
        {
            Rejects<ArgumentOutOfRangeException>("nonfinite eye",Ahead with {Eye=new(bad,0,0)},Wide,one);
            Rejects<ArgumentOutOfRangeException>("nonfinite forward",Ahead with {Forward=new(0,bad,-1)},Wide,one);
            Rejects<ArgumentOutOfRangeException>("nonfinite up",Ahead with {Up=new(0,1,bad)},Wide,one);
            Rejects<ArgumentOutOfRangeException>("nonfinite sample",Ahead,Wide,[new(0,bad,-5)]);
            Rejects<ArgumentOutOfRangeException>("nonfinite cone",Ahead,Wide with {HalfAngle=bad},one);
            Rejects<ArgumentOutOfRangeException>("nonfinite horizontal",Ahead,Wide with {HorizontalHalfAngle=bad},one);
            Rejects<ArgumentOutOfRangeException>("nonfinite vertical",Ahead,Wide with {VerticalHalfAngle=bad},one);
            Rejects<ArgumentOutOfRangeException>("nonfinite min range",Ahead,Wide with {MinRange=bad},one);
            Rejects<ArgumentOutOfRangeException>("nonfinite max range",Ahead,Wide with {MaxRange=bad},one);
            bool rejected=false;try{Sight.Classify(Ahead,Wide,new(bad,0,0));}catch(ArgumentOutOfRangeException){rejected=true;}
            Require(rejected,"classify rejects nonfinite point");
        }
        Rejects<ArgumentOutOfRangeException>("eye beyond bound",Ahead with {Eye=new(Math.BitIncrement(Sight.PositionBound),0,0)},Wide,one);
        Rejects<ArgumentOutOfRangeException>("sample beyond bound",Ahead,Wide,[new(0,0,-Math.BitIncrement(Sight.PositionBound))]);
        foreach(var forward in new Vector3d[]{new(0,0,-.6),new(0,0,-1-2e-6),new(0,0,0)})
            Rejects<ArgumentOutOfRangeException>("non-unit forward",Ahead with {Forward=forward},Wide,one);
        foreach(var up in new Vector3d[]{new(0,0,-1),new(0,0,1),new(1e-7,0,-1),new(0,1.1,0)})
            Rejects<ArgumentOutOfRangeException>("parallel/non-unit up",Ahead with {Up=up},Wide,one);
        foreach(var settings in new[]{Wide with {HalfAngle=-1e-12},Wide with {HalfAngle=Math.BitIncrement(Math.PI)},
            Wide with {HorizontalHalfAngle=Math.BitIncrement(Math.PI)},Wide with {VerticalHalfAngle=Math.BitIncrement(Math.PI/2)},
            Wide with {MinRange=.000999},Wide with {MaxRange=10000.001},SightSettings.Cone(Math.PI,5,4),
            Wide with {Sampling=(SightSampling)2},default})
            Rejects<ArgumentOutOfRangeException>("settings domain",Ahead,settings,one);
        Rejects<ArgumentOutOfRangeException>("no samples",Ahead,Wide,[]);
        Rejects<ArgumentOutOfRangeException>("nine samples",Ahead,Wide,Enumerable.Repeat(new Vector3d(0,0,-5),9).ToArray(),resultLength:9);
        Rejects<ArgumentException>("zero target",Ahead,Wide,one,[default]);
        Rejects<ArgumentException>("duplicate target",Ahead,Wide,one,[E(1),E(1)]);
        Rejects<ArgumentOutOfRangeException>("33 targets",Ahead,Wide,one,Enumerable.Range(1,33).Select(i=>E((ulong)i)).ToArray());
        Rejects<ArgumentOutOfRangeException>("129 ignored",Ahead,Wide,one,null,Enumerable.Range(1,129).Select(i=>E((ulong)i)).ToArray());
        Rejects<ArgumentException>("zero ignored",Ahead,Wide,one,null,[default]);
        Rejects<ArgumentException>("duplicate ignored",Ahead,Wide,one,null,[E(2),E(2)]);
        Rejects<ArgumentException>("target also ignored",Ahead,Wide,one,[E(5)],[E(5)]);
        Rejects<ArgumentOutOfRangeException>("negative budget",Ahead,Wide,one,budget:-1);
        Rejects<ArgumentException>("short destination",Ahead,Wide,[new(0,0,-5),new(0,0,-6)],resultLength:1);
        // Inclusive upper ID bounds remain valid.
        var results=new SightSample[1];var accepted=Tape.Create();
        Sight.Evaluate(Ahead,Wide,one,Enumerable.Range(1,32).Select(i=>E((ulong)i)).ToArray(),Enumerable.Range(100,128).Select(i=>E((ulong)i)).ToArray(),1,results,ref accepted);
        Require(accepted.Count==1,"32 targets and 128 ignored IDs accepted");
    }

    private static void QueryAccounting()
    {
        var o=new SightObserver(new(1,2,3),new(0,0,-1),new(0,1,0));
        EntityId[] targets=[E(20),E(21)],ignore=[E(10),E(11)];
        // Accepted at 4 m, rejected perpendicular, accepted at 6 m.
        Vector3d[] three=[new(1,2,-1),new(11,2,3),new(1,2,-3)];
        var all=SightSettings.Cone(Math.PI/3,.001,100,SightSampling.All);
        var q=Tape.Create(Hit(30,2),Hit(21,5.5));q.ExpectedIgnore=ignore;var results=Sentinels(3);
        var r=Sight.Evaluate(o,all,three,targets,ignore,8,results,ref q);
        Require(q.Count==2 && r.RaysCast==2 && q.IgnoreMismatches==0,"one query per accepted sample with the exact caller ignore list");
        Require(q.Origins[0]==o.Eye && q.Origins[1]==o.Eye && q.Directions[0]==new Vector3d(0,0,-1) && q.Directions[1]==new Vector3d(0,0,-1) &&
            q.Distances[0]==4 && q.Distances[1]==6,"rays run from the eye exactly to each sample in order");
        Require(results[0].Status==SightSampleStatus.Occluded && results[0].HitEntity==E(30) && results[0].HitDistance==2,"unrelated collider occludes");
        Require(results[1].Status==SightSampleStatus.Rejected && results[1].Geometry.Reject==SightReject.OutsideCone && results[1].HitEntity==default,"rejected sample casts nothing");
        Require(results[2].Status==SightSampleStatus.TargetHit && results[2].HitEntity==E(21) && results[2].Visible,"declared collider is a target hit");
        Require(r==new SightResult(SightStatus.Visible,3,1,2,0,1,1,0,0,2),"complete aggregate accounting");
        Vector3d[] two=[new(1,2,-1),new(1,2,-3)];
        var firstOnly=SightSettings.Cone(Math.PI/3,.001,100);
        SightResult Run(SightSettings s,int budget,EntityId[] t,out Tape tape,out SightSample[] samples,params RayHit?[] replies)
        {tape=Tape.Create(replies);samples=new SightSample[2];return Sight.Evaluate(o,s,two,t,ignore,budget,samples,ref tape);}
        var first=Run(firstOnly,8,targets,out var t1,out var s1,Hit(20,3.8));
        Require(t1.Count==1 && s1[1].Status==SightSampleStatus.Skipped && first==new SightResult(SightStatus.Visible,2,0,1,0,1,0,0,1,0),"FirstVisible skips later samples");
        var none=Run(firstOnly,0,targets,out var t2,out var s2);
        Require(t2.Count==0 && s2[0].Status==SightSampleStatus.BudgetExhausted && s2[1].Status==SightSampleStatus.BudgetExhausted &&
            none.Status==SightStatus.Inconclusive && none.BudgetExhausted && none.UncheckedSamples==2,"zero budget is inconclusive, not occluded");
        var partial=Run(firstOnly,1,targets,out var t3,out var s3,Hit(30,2));
        Require(t3.Count==1 && s3[0].Status==SightSampleStatus.Occluded && s3[1].Status==SightSampleStatus.BudgetExhausted &&
            partial.Status==SightStatus.Inconclusive,"unchecked sample prevents an occluded verdict");
        var clear=Run(firstOnly,1,targets,out var t4,out var s4,(RayHit?)null);
        Require(t4.Count==1 && s4[0].Status==SightSampleStatus.Clear && s4[1].Status==SightSampleStatus.Skipped &&
            clear==new SightResult(SightStatus.Visible,2,0,1,1,0,0,0,1,0),"clear segment establishes sight; skip outranks exhausted budget");
        var blocked=Run(all,2,targets,out var t5,out _,Hit(30,2),Hit(31,3));
        Require(t5.Count==2 && blocked==new SightResult(SightStatus.Occluded,2,0,2,0,0,2,0,0,-1),"all accepted samples cast and blocked");
        var undeclared=Run(firstOnly,8,[],out _,out var s6,Hit(20,3.8),Hit(20,5.8));
        Require(undeclared.Status==SightStatus.Occluded && s6[0].Status==SightSampleStatus.Occluded,"without declared colliders any hit occludes");
        var huge=Run(all,int.MaxValue,targets,out var t7,out _,null,null);
        Require(t7.Count==2 && huge.ClearSamples==2,"large shared budget accepted");
        var behind=Tape.Create();var b=new SightSample[1];
        var outOfView=Sight.Evaluate(o,firstOnly,[new Vector3d(1,2,13)],targets,ignore,8,b,ref behind);
        Require(behind.Count==0 && outOfView==new SightResult(SightStatus.OutOfView,1,1,0,0,0,0,0,0,-1),"all-rejected samples are out of view with zero rays");
        var slanted=Tape.Create();var d=new SightSample[1];
        Sight.Evaluate(o,Wide,[new Vector3d(4,2,-1)],targets,ignore,1,d,ref slanted);
        Near(slanted.Directions[0].X,.6,"normalized direction x");Near(slanted.Directions[0].Z,-.8,"normalized direction z");
        Require(slanted.Distances[0]==5 && d[0].Geometry.Distance==5,"ray distance equals measured distance");
    }

    private static void ProviderFailures()
    {
        var o=new SightObserver(new(1,2,3),new(0,0,-1),new(0,1,0));
        Vector3d[] two=[new(1,2,-1),new(1,2,-3)];
        EntityId[] targets=[E(20)],ignore=[E(10)];
        var all=Wide with {Sampling=SightSampling.All};
        var marker=new ApplicationException("provider sentinel");
        var throwing=Tape.Create(Hit(30,2));throwing.ThrowAt=2;throwing.Failure=marker;
        var results=Sentinels(2);var before=(SightSample[])results.Clone();Exception? caught=null;
        try{Sight.Evaluate(o,all,two,targets,ignore,8,results,ref throwing);}catch(Exception error){caught=error;}
        Require(ReferenceEquals(caught,marker) && throwing.Count==2,"provider exception identity and attempted calls retained");
        Require(results.AsSpan().SequenceEqual(before.AsSpan()),"provider failure publishes no partial samples");
        foreach(var bad in new[]{new RayHit(default,1,Origin,null),Hit(30,double.NaN),Hit(30,double.PositiveInfinity),Hit(30,-1e-9),
            Hit(30,Math.BitIncrement(4.0)),Hit(10,1)})
        {
            var tape=Tape.Create(bad);caught=null;
            try{Sight.Evaluate(o,all,two,targets,ignore,8,results,ref tape);}catch(Exception error){caught=error;}
            Require(caught is InvalidOperationException && tape.Count==1,"malformed provider hit rejected at its query");
            Require(results.AsSpan().SequenceEqual(before.AsSpan()),"malformed hit publishes no partial samples");
        }
        var endpoint=Tape.Create(Hit(20,4));var ok=new SightSample[2];
        Require(Sight.Evaluate(o,all,two,targets,ignore,1,ok,ref endpoint).TargetHitSamples==1,"hit exactly at the sample endpoint is valid");
    }

    private static readonly Vector3d Eye=new(0,1.6,6);
    private static readonly Vector3d Torso=new(0,.8,-2),Head=new(0,1.55,-2);
    private static readonly double TorsoRay=Math.Sqrt(64.64),HeadRay=Math.Sqrt(64.0025);
    private static Box[] Scene(params Box[] extra)=>[B(10,new(0,1,6),new(.3,1,.3)),B(11,new(0,1.6,5.6),new(.25,.08,.1)),
        B(20,Torso,new(.3,.5,.2)),B(21,Head,new(.15,.15,.15)),..extra];
    private static (SightResult Result,SightSample[] Samples,int Queries) Look(Box[] items,EntityId[] targets,EntityId[] ignore,
        SightSampling sampling=SightSampling.All,int budget=8,Vector3d[]? points=null)
    {
        var observer=new SightObserver(Eye,new(0,0,-1),new(0,1,0));
        var settings=SightSettings.YawPitch(.7,.45,.25,12,sampling);
        var scene=new Boxes{Items=items};var samples=new SightSample[2];
        var result=Sight.Evaluate(observer,settings,points ?? new[]{Torso,Head},targets,ignore,budget,samples,ref scene);
        return (result,samples,scene.Count);
    }
    private static void Blockers()
    {
        EntityId[] compound=[E(20),E(21)],self=[E(10),E(11)];
        var unignored=Look(Scene(),compound,[]);
        Require(unignored.Result.Status==SightStatus.Occluded && unignored.Samples.All(s=>s.HitEntity==E(10) && s.HitDistance==0),
            "eye inside unignored self collider blocks at distance zero");
        var postOnly=Look(Scene(),compound,[E(10)]);
        Require(postOnly.Result.Status==SightStatus.Occluded && postOnly.Samples.All(s=>s.HitEntity==E(11)),"second self collider still blocks");
        Near(postOnly.Samples[0].HitDistance,.3/8*TorsoRay,"visor distance on torso ray",1e-12);
        Near(postOnly.Samples[1].HitDistance,.3/8*HeadRay,"visor distance on head ray",1e-12);
        var seen=Look(Scene(),compound,self);
        Require(seen.Result==new SightResult(SightStatus.Visible,2,0,2,0,2,0,0,0,0) && seen.Samples[0].HitEntity==E(20) && seen.Samples[1].HitEntity==E(21),
            "explicit compound self ignore reveals both target parts");
        Near(seen.Samples[0].HitDistance,7.8/8*TorsoRay,"torso front face",1e-12);Near(seen.Samples[1].HitDistance,7.85/8*HeadRay,"head front face",1e-12);
        // Low wall hides the torso line (y 1.21 at the wall) but not the head line (y 1.575).
        var low=Scene(B(31,new(0,.7,2),new(1,.7,.1)));
        var over=Look(low,compound,self,SightSampling.FirstVisible);
        Require(over.Result==new SightResult(SightStatus.Visible,2,0,2,0,1,1,0,0,1) && over.Samples[0].HitEntity==E(31) && over.Samples[1].HitEntity==E(21),
            "head over low wall: ordered samples, second establishes sight");
        Near(over.Samples[0].HitDistance,3.9/8*TorsoRay,"low wall front face",1e-12);
        var partial=Look(low,[E(20)],self,SightSampling.FirstVisible);
        Require(partial.Result.Status==SightStatus.Occluded && partial.Samples[1].Status==SightSampleStatus.Occluded && partial.Samples[1].HitEntity==E(21),
            "undeclared compound part fails closed as an occluder");
        var budget=Look(low,compound,self,SightSampling.FirstVisible,1);
        Require(budget.Result.Status==SightStatus.Inconclusive && budget.Queries==1 && budget.Samples[1].Status==SightSampleStatus.BudgetExhausted,
            "budget leaves the visible head unchecked rather than claiming occlusion");
        var tall=Look(Scene(B(30,new(0,1.25,2),new(1,1.25,.1))),compound,self);
        Require(tall.Result.Status==SightStatus.Occluded && tall.Samples.All(s=>s.HitEntity==E(30)),"tall wall occludes all samples");
        Near(tall.Samples[1].HitDistance,3.9/8*HeadRay,"tall wall on head ray",1e-12);
        var spot=Look(Scene(),[],self,points:[new(2,1.6,-2)]);
        Require(spot.Result.Status==SightStatus.Visible && spot.Samples[0].Status==SightSampleStatus.Clear,"empty point of interest is clear");
        var hidden=Look(Scene(),[],self,points:[Torso]);
        Require(hidden.Result.Status==SightStatus.Occluded && hidden.Samples[0].HitEntity==E(20),"point inside an undeclared collider is occluded");
    }

    private static void MemoryRejects<T>(string label,SightMemoryState state,EntityId target,Vector3d? seen,long tick,long retain) where T:Exception
    {
        var before=state;Exception? caught=null;
        try{SightMemory.Update(state,target,seen,tick,retain);}catch(Exception error){caught=error;}
        Require(caught?.GetType()==typeof(T),$"{label}: expected {typeof(T).Name}, got {caught?.GetType().Name ?? "none"}");
        Require(state==before,label+" leaves caller state unchanged");
    }
    private static void Memory()
    {
        var target=E(20);Vector3d a=new(1,0,-2),b=new(2,0,-2),c=new(-3,0,-2);
        var empty=SightMemoryState.Empty;
        var idle=SightMemory.Update(empty,target,null,5,3);
        Require(idle.State==empty && idle.Events==SightMemoryEvents.None,"unseen update keeps empty memory");
        var acquired=SightMemory.Update(empty,target,a,10,3);
        Require(acquired==new SightMemoryStep(new(SightMemoryPhase.Seeing,target,a,10,10,13,10,0),SightMemoryEvents.Acquired),"acquisition");
        var refreshed=SightMemory.Update(acquired.State,target,b,11,3);
        Require(refreshed==new SightMemoryStep(new(SightMemoryPhase.Seeing,target,b,10,11,14,11,0),SightMemoryEvents.Refreshed),"refresh keeps first seen");
        var lost=SightMemory.Update(refreshed.State,target,null,12,3);
        Require(lost==new SightMemoryStep(new(SightMemoryPhase.Remembering,target,b,10,11,14,12,0),SightMemoryEvents.Lost),"loss keeps last known position");
        Require(SightMemory.TicksSinceSeen(lost.State,12)==1 && SightMemory.FirstExpiredTick(lost.State)==15,"inspectable elapsed time and expiry");
        var held=SightMemory.Update(lost.State,target,null,14,3);
        Require(held.State.Phase==SightMemoryPhase.Remembering && held.Events==SightMemoryEvents.None,"retained through RetainUntilTick");
        var expired=SightMemory.Update(held.State,target,null,15,3);
        Require(expired==new SightMemoryStep(new(SightMemoryPhase.Expired,target,b,10,11,14,15,0),SightMemoryEvents.Expired),"expiry retains identity for inspection");
        var later=SightMemory.Update(expired.State,target,null,20,3);
        Require(later.State.Phase==SightMemoryPhase.Expired && later.Events==SightMemoryEvents.None && later.State.LastUpdateTick==20,"expiry is reported once");
        var reacquired=SightMemory.Update(later.State,target,c,21,3);
        Require(reacquired==new SightMemoryStep(new(SightMemoryPhase.Seeing,target,c,21,21,24,21,1),SightMemoryEvents.Reacquired),"reacquire after expiry");
        var lostAgain=SightMemory.Update(reacquired.State,target,null,22,3);
        var again=SightMemory.Update(lostAgain.State,target,a,23,3);
        Require(lostAgain.Events==SightMemoryEvents.Lost && again.Events==SightMemoryEvents.Reacquired &&
            again.State.Reacquisitions==2 && again.State.FirstSeenTick==23,"reacquire while remembering");
        var instant=SightMemory.Update(SightMemory.Update(empty,target,a,100,0).State,target,null,101,0);
        Require(instant.Events==(SightMemoryEvents.Lost|SightMemoryEvents.Expired) && instant.State.Phase==SightMemoryPhase.Expired,"zero retention");
        var sparse=SightMemory.Update(SightMemory.Update(empty,target,a,100,3).State,target,null,200,3);
        Require(sparse.Events==(SightMemoryEvents.Lost|SightMemoryEvents.Expired),"sparse updates lose and expire together");

        var seeing=refreshed.State;
        MemoryRejects<ArgumentOutOfRangeException>("same tick",seeing,target,b,11,3);
        MemoryRejects<ArgumentOutOfRangeException>("earlier tick",seeing,target,null,10,3);
        MemoryRejects<ArgumentOutOfRangeException>("negative tick",seeing,target,null,-1,3);
        MemoryRejects<ArgumentOutOfRangeException>("tick beyond bound",seeing,target,null,SightMemory.MaxTick+1,3);
        MemoryRejects<ArgumentOutOfRangeException>("negative retention",seeing,target,b,12,-1);
        MemoryRejects<ArgumentOutOfRangeException>("retention beyond bound",seeing,target,b,12,SightMemory.MaxRetainTicks+1);
        MemoryRejects<ArgumentException>("other target",seeing,E(21),b,12,3);
        MemoryRejects<ArgumentException>("zero target",seeing,default,b,12,3);
        MemoryRejects<ArgumentException>("zero target on empty",empty,default,null,12,3);
        MemoryRejects<ArgumentOutOfRangeException>("nonfinite position",seeing,target,new Vector3d(double.NaN,0,0),12,3);
        MemoryRejects<ArgumentOutOfRangeException>("position beyond bound",empty,target,new Vector3d(0,0,Math.BitIncrement(Sight.PositionBound)),12,3);
        foreach(var corrupt in new SightMemoryState[]{new(SightMemoryPhase.Empty,target,default,0,0,0,0,0),new(SightMemoryPhase.Empty,default,default,0,0,0,3,0),
            seeing with {LastUpdateTick=12},lost.State with {LastUpdateTick=15},expired.State with {LastUpdateTick=14},seeing with {FirstSeenTick=12},
            seeing with {Reacquisitions=-1},seeing with {RetainUntilTick=10},seeing with {RetainUntilTick=11+SightMemory.MaxRetainTicks+1},
            seeing with {Phase=(SightMemoryPhase)9},seeing with {Target=default},seeing with {LastKnownPosition=new(0,double.PositiveInfinity,0)},
            seeing with {FirstSeenTick=-1}})
        {
            MemoryRejects<ArgumentOutOfRangeException>("corrupt state",corrupt,target,b,30,3);
            bool rejected=false;try{SightMemory.Validate(corrupt);}catch(ArgumentOutOfRangeException){rejected=true;}
            Require(rejected,"validate rejects corrupt state");
        }
        var saturated=lost.State with {Reacquisitions=SightMemory.MaxTick};
        MemoryRejects<ArithmeticException>("reacquisition bound",saturated,target,a,13,3);
        var edge=SightMemory.Update(empty,target,a,SightMemory.MaxTick,SightMemory.MaxRetainTicks);
        Require(edge.State.RetainUntilTick==SightMemory.MaxTick+SightMemory.MaxRetainTicks,"maximum tick and retention fit");
        MemoryRejects<ArgumentOutOfRangeException>("no tick after maximum",edge.State,target,null,SightMemory.MaxTick+1,3);
        Require(SightMemory.Tick((ulong)SightMemory.MaxTick)==SightMemory.MaxTick,"maximum context tick converts");
        bool overflow=false;try{SightMemory.Tick((ulong)SightMemory.MaxTick+1);}catch(ArgumentOutOfRangeException){overflow=true;}
        Require(overflow,"context tick beyond bound rejected");

        // The documented ownership: store primitive fields, rebuild the transient record, continue identically.
        var original=again.State;var restored=Load(Store(original));
        Require(restored==original,"scalar fields reconstruct the complete memory");
        for(long tick=24;tick<40;++tick)
        {
            Vector3d? sighting=tick%5==0 ? new Vector3d(tick,0,-tick) : null;
            var x=SightMemory.Update(original,target,sighting,tick,3);var y=SightMemory.Update(restored,target,sighting,tick,3);
            Require(x==y,"restored continuation is identical");original=x.State;restored=Load(Store(y.State));
        }
    }
    private struct MemoryFields{public int Phase;public EntityId Target;public double X,Y,Z;public long First,Last,Retain,Update,Reacquisitions;}
    private static MemoryFields Store(SightMemoryState s)=>new(){Phase=(int)s.Phase,Target=s.Target,X=s.LastKnownPosition.X,Y=s.LastKnownPosition.Y,
        Z=s.LastKnownPosition.Z,First=s.FirstSeenTick,Last=s.LastSeenTick,Retain=s.RetainUntilTick,Update=s.LastUpdateTick,Reacquisitions=s.Reacquisitions};
    private static SightMemoryState Load(MemoryFields f)=>new((SightMemoryPhase)f.Phase,f.Target,new(f.X,f.Y,f.Z),f.First,f.Last,f.Retain,f.Update,f.Reacquisitions);

    private static void DeterminismAndAllocation()
    {
        var observer=new SightObserver(Eye,new(0,0,-1),new(0,1,0));
        var settings=SightSettings.YawPitch(.7,.45,.25,12);
        Vector3d[] points=[Torso,Head];EntityId[] targets=[E(20),E(21)],ignore=[E(10),E(11)];
        var scene=new Boxes{Items=Scene(B(31,new(0,.7,2),new(1,.7,.1)))};
        SightSample[] first=new SightSample[2],second=new SightSample[2];
        var a=Sight.Evaluate(observer,settings,points,targets,ignore,8,first,ref scene);
        var b=Sight.Evaluate(observer,settings,points,targets,ignore,8,second,ref scene);
        Require(a==b && first.AsSpan().SequenceEqual(second.AsSpan()),"same inputs give identical results");
        var memory=SightMemoryState.Empty;long total=0;
        for(int i=0;i<256;++i)
        {
            total+=Sight.Evaluate(observer,settings,points,targets,ignore,8,first,ref scene).RaysCast;
            memory=SightMemory.Update(memory,E(20),i%3==0 ? Torso : null,i,4).State;
        }
        int queries=scene.Count;long before=GC.GetAllocatedBytesForCurrentThread();
        for(int i=256;i<2304;++i)
        {
            total+=Sight.Evaluate(observer,settings,points,targets,ignore,8,first,ref scene).RaysCast;
            memory=SightMemory.Update(memory,E(20),i%3==0 ? Torso : null,i,4).State;
        }
        long allocated=GC.GetAllocatedBytesForCurrentThread()-before;
        Require(allocated==0,"steady evaluation and memory updates allocate zero managed bytes");
        Require(scene.Count-queries==2048*2 && total==2304*2 && memory.Phase!=SightMemoryPhase.Empty,"measured loop performed actual queries and updates");
    }

    private static int Main()
    {
        (string Name,Action Run)[] groups=[("cone boundaries, range and coincident points",ConeBoundaries),
            ("yaw/pitch frames, orientation and rotation invariance",YawPitchFrames),
            ("strict inputs and pre-query atomic rejection",InvalidInputs),
            ("ray accounting, sampling and budget status",QueryAccounting),
            ("provider failures and unpublished results",ProviderFailures),
            ("synthetic self, compound-target and wall blockers",Blockers),
            ("memory expiration, reacquisition and scalar restore",Memory),
            ("determinism and steady allocation",DeterminismAndAllocation)];
        try
        {
            foreach(var group in groups){group.Run();Console.WriteLine("PASS "+group.Name);}
            Console.WriteLine($"Synthetic perception tests passed: {groups.Length} groups, {checks} checks. Not native evidence.");return 0;
        }
        catch(Exception error){Console.Error.WriteLine(error);return 1;}
    }
}
