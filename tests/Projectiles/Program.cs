// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
using Poima;
using Poima.Fps;

internal static class Program
{
    private static int checks;
    private static readonly Vector3d Zero=new(0,0,0);
    private static readonly ProjectileDefinition Basic=new(Zero,.02,10,.01,32);

    private static void Require(bool condition,string name)
    {
        ++checks;
        if(!condition)throw new InvalidOperationException(name);
    }
    // Analytic comparisons allow floating arithmetic error only; they are not collision tolerances.
    private static void Near(double actual,double expected,string name,double tolerance=2e-12)
    {
        Require(double.IsFinite(actual) && Math.Abs(actual-expected)<=tolerance*Math.Max(1,Math.Abs(expected)),
            $"{name}: {actual:R} != {expected:R}");
    }
    private static void Near(Vector3d actual,Vector3d expected,string name,double tolerance=2e-12)
    {Near(actual.X,expected.X,name+".x",tolerance);Near(actual.Y,expected.Y,name+".y",tolerance);Near(actual.Z,expected.Z,name+".z",tolerance);}
    private static Vector3d Add(Vector3d a,Vector3d b)=>new(a.X+b.X,a.Y+b.Y,a.Z+b.Z);
    private static Vector3d Mul(Vector3d a,double b)=>new(a.X*b,a.Y*b,a.Z*b);
    private static Vector3d Sub(Vector3d a,Vector3d b)=>new(a.X-b.X,a.Y-b.Y,a.Z-b.Z);
    private static double Norm(Vector3d a)=>Math.Sqrt(a.X*a.X+a.Y*a.Y+a.Z*a.Z);
    private static Vector3d AnalyticPosition(ProjectileState s,Vector3d a,double t)=>
        Add(s.Position,Add(Mul(s.Velocity,t),Mul(a,.5*t*t)));
    private static Vector3d AnalyticVelocity(ProjectileState s,Vector3d a,double t)=>Add(s.Velocity,Mul(a,t));
    private static Vector3d Endpoint(ProjectileSweep s)=>Add(s.Origin,Mul(s.Direction,s.Distance));
    private static long D(double x)=>BitConverter.DoubleToInt64Bits(x);
    private static long[] Image(ProjectileState s)=>[D(s.Position.X),D(s.Position.Y),D(s.Position.Z),
        D(s.Velocity.X),D(s.Velocity.Y),D(s.Velocity.Z),D(s.Age),(long)s.Phase];
    private static long[] Image(ProjectileDefinition d)=>[D(d.Acceleration.X),D(d.Acceleration.Y),D(d.Acceleration.Z),
        D(d.Radius),D(d.Lifetime),D(d.MaxChordError),d.MaxSegments];
    private static void Exact(ProjectileState a,ProjectileState b,string name)=>Require(Image(a).AsSpan().SequenceEqual(Image(b)),name);
    private static void Exact(ProjectileStep a,ProjectileStep b,string name)
    {
        Exact(a.State,b.State,name+" state");Require(a.Hit==b.Hit && a.Queries==b.Queries &&
            D(a.ConsumedSeconds)==D(b.ConsumedSeconds) && D(a.MaxArcDeviation)==D(b.MaxArcDeviation),name+" result");
    }

    private struct Miss:IProjectileQueries
    {
        public int Count;
        public CapsuleCastHit? Sweep(in ProjectileSweep sweep){++Count;return null;}
    }
    private struct Tape:IProjectileQueries
    {
        public ProjectileSweep[] Sweeps;
        public int Count;
        public int HitAt;
        public CapsuleCastHit? Contact;
        public Exception? Failure;
        public Tape(ProjectileSweep[] sweeps,int hitAt=0,CapsuleCastHit? contact=null,Exception? failure=null)
        {Sweeps=sweeps;Count=0;HitAt=hitAt;Contact=contact;Failure=failure;}
        public CapsuleCastHit? Sweep(in ProjectileSweep sweep)
        {
            Sweeps[Count++]=sweep;
            if(Count!=HitAt)return null;
            if(Failure is {} error)throw error;
            return Contact;
        }
    }
    private ref struct SpanMiss(Span<ProjectileSweep> sweeps):IProjectileQueries
    {
        private Span<ProjectileSweep> sweeps=sweeps;
        public int Count;
        public CapsuleCastHit? Sweep(in ProjectileSweep sweep){sweeps[Count++]=sweep;return null;}
    }
    // Independent infinite x-plane: the sphere touches when center.X+radius reaches PlaneX.
    // Both contact surface points are on the plane; neither is the stopped sphere center.
    private struct Plane:IProjectileQueries
    {
        public double PlaneX;
        public int Count;
        public ProjectileSweep Last;
        public CapsuleCastHit? LastContact;
        public Plane(double x){PlaneX=x;Count=0;Last=default;LastContact=null;}
        public CapsuleCastHit? Sweep(in ProjectileSweep sweep)
        {
            ++Count;Last=sweep;
            bool overlap=sweep.Origin.X+sweep.Radius>=PlaneX;
            double dx=sweep.Direction.X*sweep.Distance;
            if(!overlap && (dx<=0 || sweep.Origin.X+dx+sweep.Radius<PlaneX))return null;
            double f=overlap ? 0 : (PlaneX-sweep.Radius-sweep.Origin.X)/dx;
            Vector3d center=Add(sweep.Origin,Mul(sweep.Direction,sweep.Distance*f));
            Vector3d surface=new(PlaneX,center.Y,center.Z);
            LastContact=new(new EntityId(0,17),f,sweep.Distance*f,surface,surface,new Vector3d(-1,0,0),
                overlap ? sweep.Origin.X+sweep.Radius-PlaneX : 0,null,overlap);
            return LastContact;
        }
    }
    private static CapsuleCastHit Contact(double fraction,bool overlap=false)=>new(
        new EntityId(7,9),fraction,13,new Vector3d(91,92,93),new Vector3d(-71,-72,-73),null,0,19,overlap);

    private static void AnalyticFlight()
    {
        var original=Projectiles.Launch(new Vector3d(1,-2,3),new Vector3d(4,2,-1));
        Exact(original,new(new Vector3d(1,-2,3),new Vector3d(4,2,-1),0,ProjectilePhase.Flying),"launch primitives");
        var d=Basic with {Acceleration=new Vector3d(2,-4,1)};
        var q=new Miss();var single=Projectiles.Step(in d,in original,.5,ref q);
        Near(single.State.Position,new Vector3d(3.25,-1.5,2.625),"known binary trajectory");
        Near(single.State.Velocity,new Vector3d(5,0,-.5),"known binary velocity");
        Require(single.State.Age==.5 && single.State.Phase==ProjectilePhase.Flying && single.Hit is null &&
            single.Queries==q.Count && q.Count>0 && single.ConsumedSeconds==.5,"free flight ownership");
        var state=original;var partitionQuery=new Miss();
        foreach(double dt in new[]{.125,.125,.25})state=Projectiles.Step(in d,in state,dt,ref partitionQuery).State;
        Near(state.Position,single.State.Position,"binary partition position");Near(state.Velocity,single.State.Velocity,"binary partition velocity");
        Require(state.Age==single.State.Age && state.Phase==single.State.Phase,"binary partition clock");
        // A non-binary gravity profile compares to its closed form, not a second implementation loop.
        var gravity=Basic with {Acceleration=new Vector3d(.3,-9.81,-.7),MaxChordError=.001};
        var start=Projectiles.Launch(new Vector3d(-3.4,12.7,.2),new Vector3d(13.6,1.2,-.9));
        var miss=new Miss();var full=Projectiles.Step(in gravity,in start,.6,ref miss);
        Near(full.State.Position,AnalyticPosition(start,gravity.Acceleration,.6),"gravity closed form");
        Near(full.State.Velocity,AnalyticVelocity(start,gravity.Acceleration,.6),"gravity incident velocity");
        var split=start;var mq=new Miss();foreach(double dt in new[]{.1,.2,.3})split=Projectiles.Step(in gravity,in split,dt,ref mq).State;
        Near(split.Position,full.State.Position,"gravity partition position",8e-12);
        Near(split.Velocity,full.State.Velocity,"gravity partition velocity",8e-12);
        Exact(start,Projectiles.Launch(new Vector3d(-3.4,12.7,.2),new Vector3d(13.6,1.2,-.9)),"input retained after success");
    }

    private static void CurvatureEnvelope()
    {
        var d=Basic with {Acceleration=new Vector3d(0,-8,0),MaxChordError=.016};
        var start=Projectiles.Launch(new Vector3d(1,2,3),new Vector3d(3,0,0));
        var tape=new Tape(new ProjectileSweep[32]);var result=Projectiles.Step(in d,in start,.5,ref tape);
        Require(tape.Count==4 && result.Queries==4,"curvature requires four chords with rounding margin");
        double max=0;
        for(int i=0;i<tape.Count;++i)
        {
            var s=tape.Sweeps[i];double t=i*.125;
            Near(s.Origin,AnalyticPosition(start,d.Acceleration,t),"chord analytic origin");
            Near(Endpoint(s),AnalyticPosition(start,d.Acceleration,t+.125),"chord analytic endpoint");
            Require(s.Seconds==.125 && s.ArcDeviation>=1.0/64 && s.ArcDeviation<=d.MaxChordError,"outward sag bound");
            Near(s.ArcDeviation,1.0/64,"independent quadratic sag",1e-14);Near(Norm(s.Direction),1,"normalized chord");
            Require(s.Radius>=d.Radius+s.ArcDeviation && (double)(float)s.Radius>=d.Radius+s.ArcDeviation,
                "native float radius cannot shrink curvature envelope");
            Require(s.Radius-(d.Radius+s.ArcDeviation)<2e-8,"radius padding remains representational");
            for(int j=0;j<=8;++j)
            {
                double f=j/8.0;Vector3d actual=AnalyticPosition(start,d.Acceleration,t+s.Seconds*f);
                Vector3d chord=Add(s.Origin,Mul(Sub(Endpoint(s),s.Origin),f));
                Require(Norm(Sub(actual,chord))<=s.ArcDeviation+2e-14,"parabolic samples within inflated chord");
                if(j==4)Near(Norm(Sub(actual,chord)),1.0/64,"sag maximum at midpoint",1e-14);
            }
            max=Math.Max(max,s.ArcDeviation);
        }
        Require(D(result.MaxArcDeviation)==D(max),"reported deviation covers actual submitted sweeps");
        // Non-binary physical radius must be rounded outward even without acceleration.
        var flat=Basic with {Radius=.02};var still=Projectiles.Launch(Zero,Zero);var tf=new Tape(new ProjectileSweep[1]);
        Projectiles.Step(in flat,in still,.1,ref tf);
        Require(tf.Sweeps[0].ArcDeviation==0 && tf.Sweeps[0].Radius>=.02 &&
            (double)(float)tf.Sweeps[0].Radius>=.02,"binary32 physical radius round-up");
        var large=Basic with {Radius=9,Acceleration=new Vector3d(0,4,0),MaxChordError=.75};
        var lt=new Tape(new ProjectileSweep[32]);Projectiles.Step(in large,in still,1,ref lt);
        Require(lt.Count==1 && lt.Sweeps[0].Radius>=9.5 && lt.Sweeps[0].Radius<=10,"large valid inflated native sphere");
    }

    private static void PlaneContacts()
    {
        var d=Basic;var start=Projectiles.Launch(Zero,new Vector3d(1200,0,0));var plane=new Plane(5);
        var hit=Projectiles.Step(in d,in start,1.0/60,ref plane);
        Require(hit.State.Phase==ProjectilePhase.Impacted && hit.Queries==1 && plane.Count==1,"fast thin plane sweep stops flight");
        Near(hit.State.Position,new Vector3d(5-plane.Last.Radius,0,0),"center at sphere-plane contact");
        Near(hit.ConsumedSeconds,(5-plane.Last.Radius)/1200,"fast impact time");
        Near(hit.State.Age,hit.ConsumedSeconds,"impact age");Near(hit.State.Velocity,start.Velocity,"incident velocity retained");
        Require(hit.Hit==plane.LastContact && hit.Hit is {} contact && contact.PointOnQuery.X==5 &&
            Math.Abs(hit.State.Position.X-contact.PointOnQuery.X)>.019,"contact surface is not projectile center");
        // Contact in the third curved chord must include the first two clear intervals.
        var gravity=Basic with {Acceleration=new Vector3d(0,-8,0),MaxChordError=.016};
        var falling=Projectiles.Launch(Zero,new Vector3d(3,0,0));var later=new Plane(.8);
        var result=Projectiles.Step(in gravity,in falling,.5,ref later);
        Require(later.Count==3 && result.Queries==3 && result.Hit==later.LastContact,"later chord first hit and unchanged contact");
        double fraction=later.LastContact!.Value.Fraction;double time=.25+.125*fraction;
        Near(result.ConsumedSeconds,time,"previous clear times included");Near(result.State.Velocity,new Vector3d(3,-8*time,0),"later analytic incident velocity");
        Near(result.State.Position,new Vector3d(.75+.375*fraction,-.25-.3125*fraction,0),"later stopped chord center");
        Require(Math.Abs(result.State.Position.Y-AnalyticPosition(falling,gravity.Acceleration,time).Y)>1e-5,
            "chord contact is not falsely reported as exact parabolic time of impact");
        Require(result.MaxArcDeviation==later.Last.ArcDeviation,"only attempted chords contribute to max deviation");
        var impacted=result.State;var next=new Miss();var after=Projectiles.Step(in gravity,in impacted,.5,ref next);
        Exact(after.State,result.State,"impact terminal state stable");Require(after.Hit is null && next.Count==0,"no repeated impact query/event");
    }

    private static void OverlapAndClosedChord()
    {
        var d=Basic;var s=Projectiles.Launch(new Vector3d(1,2,3),new Vector3d(4,5,6));
        var originalContact=Contact(0,true);var overlap=new Tape(new ProjectileSweep[1],1,originalContact);
        var hit=Projectiles.Step(in d,in s,.1,ref overlap);
        Near(hit.State.Position,s.Position,"initial overlap center");Near(hit.State.Velocity,s.Velocity,"initial overlap incident velocity");
        Require(hit.State.Phase==ProjectilePhase.Impacted && hit.ConsumedSeconds==0 && hit.State.Age==0 &&
            hit.Hit==originalContact && hit.Queries==1,"overlap preserves provider contact and null normal");
        // The trajectory returns to its start while bending half a metre away at mid-time.
        var curved=Basic with {Acceleration=new Vector3d(0,4,0),MaxChordError=.75};
        var loop=Projectiles.Launch(Zero,new Vector3d(0,-2,0));var tape=new Tape(new ProjectileSweep[32]);
        var full=Projectiles.Step(in curved,in loop,1,ref tape);
        Require(tape.Count==1 && tape.Sweeps[0].Distance==0 && Norm(tape.Sweeps[0].Direction)>0,
            "closed chord still makes nonzero-direction overlap query");
        Require(tape.Sweeps[0].ArcDeviation>=.5 && tape.Sweeps[0].Radius>=.52,"closed chord encloses curved excursion");
        Near(full.State.Position,Zero,"closed loop analytic endpoint");Near(full.State.Velocity,new Vector3d(0,2,0),"closed loop velocity");
        var immobile=Projectiles.Launch(Zero,Zero);var mq=new Tape(new ProjectileSweep[1]);Projectiles.Step(in d,in immobile,.25,ref mq);
        Require(mq.Count==1 && mq.Sweeps[0].Distance==0,"stationary sphere still checks overlap");
        // Inflation alone can overlap a plane which the physical initial sphere does not touch.
        var inflated=new Plane(.3);var overlapOnly=Projectiles.Step(in curved,in loop,1,ref inflated);
        Require(d.Radius<.3 && overlapOnly.Hit is {InitialOverlap:true} && overlapOnly.ConsumedSeconds==0,
            "envelope overlap is not claimed to be physical initial overlap");
    }

    private static void LifetimeAndTerminal()
    {
        var d=Basic with {Lifetime=.5};var s=new ProjectileState(Zero,new Vector3d(4,0,0),.25,ProjectilePhase.Flying);
        var q=new Tape(new ProjectileSweep[1]);var end=Projectiles.Step(in d,in s,1,ref q);
        Require(end.State.Phase==ProjectilePhase.Expired && end.State.Age==.5 && end.ConsumedSeconds==.25 && q.Count==1,"partial final lifetime");
        Near(end.State.Position,new Vector3d(1,0,0),"lifetime partial position");Require(q.Sweeps[0].Seconds==.25,"provider sees capped time");
        var endpoint=Contact(1);var eq=new Tape(new ProjectileSweep[1],1,endpoint);var endpointHit=Projectiles.Step(in d,in s,1,ref eq);
        Require(endpointHit.State.Phase==ProjectilePhase.Impacted && endpointHit.State.Age==.5 && endpointHit.Hit==endpoint,
            "contact at lifetime endpoint wins over expiry");
        foreach(var phase in new[]{ProjectilePhase.Impacted,ProjectilePhase.Expired})
        {
            var terminal=new ProjectileState(new Vector3d(-0.0,2,3),new Vector3d(4,-0.0,6),.125,phase);var miss=new Miss();
            var result=Projectiles.Step(in d,in terminal,.25,ref miss);
            Exact(result.State,terminal,"caller terminal phase and all signed primitives retained");
            Require(miss.Count==0 && result.Queries==0 && result.Hit is null && result.ConsumedSeconds==0 && result.MaxArcDeviation==0,
                "terminal phase no query/time/event");
            ExpectFailure<ArgumentOutOfRangeException>(d,terminal,0,"terminal still validates dt");
        }
        var due=s with {Age=d.Lifetime};var dq=new Miss();var expired=Projectiles.Step(in d,in due,.25,ref dq);
        Exact(expired.State,due with {Phase=ProjectilePhase.Expired},"already due flying expires");
        Require(dq.Count==0 && expired.ConsumedSeconds==0 && expired.MaxArcDeviation==0 && expired.Hit is null,"due flight no queries");
        // Terminal validation does not preflight a hypothetical future path.
        var extreme=new ProjectileState(new Vector3d(1e6,0,0),new Vector3d(1e5,0,0),.1,ProjectilePhase.Expired);var tq=new Miss();
        Exact(Projectiles.Step(in d,in extreme,1,ref tq).State,extreme,"valid terminal operands need no future trajectory");
    }

    private static void ExpectFailure<T>(ProjectileDefinition definition,ProjectileState state,double dt,string label) where T:Exception
    {
        long[] beforeD=Image(definition),beforeS=Image(state);var query=new Miss();bool rejected=false;
        try{Projectiles.Step(in definition,in state,dt,ref query);}
        catch(T){rejected=true;}
        Require(rejected,label+" rejects with expected exception");Require(query.Count==0,label+" rejects before any query");
        Require(beforeD.AsSpan().SequenceEqual(Image(definition)) && beforeS.AsSpan().SequenceEqual(Image(state)),label+" all caller primitives unchanged");
    }
    private static void InvalidAndPreflight()
    {
        var s=Projectiles.Launch(Zero,Zero);
        foreach(double x in new[]{double.NaN,double.PositiveInfinity,double.NegativeInfinity})
        {
            ExpectFailure<ArgumentOutOfRangeException>(Basic with {Acceleration=new Vector3d(x,0,0)},s,.1,"nonfinite acceleration");
            ExpectFailure<ArgumentOutOfRangeException>(Basic with {Radius=x},s,.1,"nonfinite radius");
            ExpectFailure<ArgumentOutOfRangeException>(Basic with {Lifetime=x},s,.1,"nonfinite lifetime");
            ExpectFailure<ArgumentOutOfRangeException>(Basic with {MaxChordError=x},s,.1,"nonfinite error");
            ExpectFailure<ArgumentOutOfRangeException>(Basic,s with {Position=new Vector3d(0,x,0)},.1,"nonfinite position");
            ExpectFailure<ArgumentOutOfRangeException>(Basic,s with {Velocity=new Vector3d(0,0,x)},.1,"nonfinite velocity");
            ExpectFailure<ArgumentOutOfRangeException>(Basic,s with {Age=x},.1,"nonfinite age");
        }
        foreach(double radius in new[]{0,.000999,9.00001})ExpectFailure<ArgumentOutOfRangeException>(Basic with {Radius=radius},s,.1,"radius domain");
        foreach(double life in new[]{0,-1,3600.01})ExpectFailure<ArgumentOutOfRangeException>(Basic with {Lifetime=life},s,.1,"lifetime domain");
        foreach(double error in new[]{0,1e-7,1.00001})ExpectFailure<ArgumentOutOfRangeException>(Basic with {MaxChordError=error},s,.1,"chord error domain");
        foreach(int count in new[]{0,-1,33})ExpectFailure<ArgumentOutOfRangeException>(Basic with {MaxSegments=count},s,.1,"segment domain");
        foreach(double dt in new[]{0,-.1,1.00001,double.NaN,double.PositiveInfinity,double.NegativeInfinity})
            ExpectFailure<ArgumentOutOfRangeException>(Basic,s,dt,"dt domain");
        ExpectFailure<ArgumentOutOfRangeException>(Basic,s with {Age=-.1},.1,"negative age");
        ExpectFailure<ArgumentOutOfRangeException>(Basic,s with {Age=11},.1,"age beyond definition");
        ExpectFailure<ArgumentOutOfRangeException>(Basic,s with {Phase=(ProjectilePhase)3},.1,"unknown phase");
        foreach(double sign in new[]{-1.0,1.0})
        {
            ExpectFailure<ArgumentOutOfRangeException>(Basic,s with {Position=new Vector3d(sign*1000001,0,0)},.1,"position bound");
            ExpectFailure<ArgumentOutOfRangeException>(Basic,s with {Velocity=new Vector3d(0,sign*100001,0)},.1,"velocity bound");
            ExpectFailure<ArgumentOutOfRangeException>(Basic with {Acceleration=new Vector3d(0,0,sign*100001)},s,.1,"acceleration bound");
        }
        foreach(var bad in new[]{new Vector3d(double.NaN,0,0),new Vector3d(1000001,0,0)})
        {bool rejected=false;try{Projectiles.Launch(bad,Zero);}catch(ArgumentOutOfRangeException){rejected=true;}Require(rejected,"launch validates center");}
        foreach(var bad in new[]{new Vector3d(0,double.PositiveInfinity,0),new Vector3d(0,0,-100001)})
        {bool rejected=false;try{Projectiles.Launch(Zero,bad);}catch(ArgumentOutOfRangeException){rejected=true;}Require(rejected,"launch validates velocity");}
        ExpectFailure<ArithmeticException>(Basic with {Acceleration=new Vector3d(0,100000,0),MaxChordError=1e-6},s,1,"curvature budget");
        var fast=s with {Velocity=new Vector3d(100000,100000,100000)};
        ExpectFailure<ArithmeticException>(Basic with {MaxSegments=17},fast,1,"vector travel budget");
        var bounded=Basic with {MaxSegments=32};var tape=new Tape(new ProjectileSweep[32]);var valid=Projectiles.Step(in bounded,in fast,1,ref tape);
        Require(tape.Count==18 && valid.Queries==18 && tape.Sweeps.Take(tape.Count).All(x=>x.Distance<=10000),"travel splits by vector speed");
        var turning=new ProjectileState(new Vector3d(999999.9,0,0),new Vector3d(1,0,0),0,ProjectilePhase.Flying);
        ExpectFailure<ArithmeticException>(Basic with {Acceleration=new Vector3d(-2,0,0)},turning,1,"interior extremum beyond bound despite valid endpoints");
        var outbound=s with {Position=new Vector3d(999999,0,0),Velocity=new Vector3d(2,0,0)};
        ExpectFailure<ArithmeticException>(Basic,outbound,1,"future position bound");
        ExpectFailure<ArithmeticException>(Basic with {Acceleration=new Vector3d(100000,0,0)},s with {Velocity=new Vector3d(100000,0,0)},.01,"future velocity bound");
        var old=s with {Age=1};ExpectFailure<ArithmeticException>(Basic,old,double.Epsilon,"age advance unrepresentable");
        // Exact legal endpoint domains do not become arbitrary strict inequalities.
        var edge=Basic with {Radius=.001,Lifetime=3600,MaxChordError=1e-6,MaxSegments=1};
        var still=new ProjectileState(new Vector3d(1e6,-1e6,0),Zero,0,ProjectilePhase.Flying);var miss=new Miss();
        Require(Projectiles.Step(in edge,in still,1,ref miss).State.Phase==ProjectilePhase.Flying && miss.Count==1,"inclusive lower/upper bounds");
    }

    private static void ProviderFailures()
    {
        var d=Basic with {Acceleration=new Vector3d(0,-8,0),MaxChordError=.016};var s=Projectiles.Launch(Zero,new Vector3d(3,0,0));
        long[] beforeD=Image(d),beforeS=Image(s);
        foreach(double fraction in new[]{-.00001,1.00001,double.NaN,double.PositiveInfinity,double.NegativeInfinity})
        {
            var bad=new Tape(new ProjectileSweep[32],2,Contact(fraction));bool rejected=false;
            try{Projectiles.Step(in d,in s,.5,ref bad);}catch(InvalidOperationException){rejected=true;}
            Require(rejected && bad.Count==2,"invalid fraction after a clear chord is not miss or partial success");
            Require(beforeD.AsSpan().SequenceEqual(Image(d)) && beforeS.AsSpan().SequenceEqual(Image(s)),"fraction failure preserves every input primitive");
        }
        var marker=new ApplicationException("provider sentinel");var throwing=new Tape(new ProjectileSweep[32],2,null,marker);Exception? caught=null;
        try{Projectiles.Step(in d,in s,.5,ref throwing);}catch(Exception e){caught=e;}
        Require(ReferenceEquals(caught,marker) && throwing.Count==2,"provider exception identity and attempted-call count retained");
        Require(beforeD.AsSpan().SequenceEqual(Image(d)) && beforeS.AsSpan().SequenceEqual(Image(s)),"provider failure publishes no input mutation");
        var opaque=Contact(.375);var query=new Tape(new ProjectileSweep[32],1,opaque);var result=Projectiles.Step(in d,in s,.5,ref query);
        Require(result.Hit==opaque && result.Queries==1,"valid fraction preserves all opaque provider contact operands");
        Near(result.State.Position,Add(query.Sweeps[0].Origin,Mul(query.Sweeps[0].Direction,query.Sweeps[0].Distance*.375)),"provider surface cannot replace center");
    }

    private static void ReplayAndAllocation()
    {
        var d=Basic with {Acceleration=new Vector3d(.3,-9.81,.2),MaxChordError=.001};
        var s=Projectiles.Launch(new Vector3d(1,7,-2),new Vector3d(8,3,.5));var miss=new Miss();
        for(int i=0;i<7;++i)s=Projectiles.Step(in d,in s,1.0/60,ref miss).State;
        // All definition/state properties are serialized; no field exclusion or tolerance at restore.
        string stateJson=JsonSerializer.Serialize(s),definitionJson=JsonSerializer.Serialize(d);
        var restored=JsonSerializer.Deserialize<ProjectileState>(stateJson);var restoredDefinition=JsonSerializer.Deserialize<ProjectileDefinition>(definitionJson);
        Exact(restored,s,"all state primitives survive caller serialization");Require(Image(restoredDefinition).AsSpan().SequenceEqual(Image(d)),"all recipe primitives survive serialization");
        var a=s;var b=restored;var qa=new Miss();var qb=new Miss();
        for(int i=0;i<40;++i)
        {
            var ar=Projectiles.Step(in d,in a,1.0/60,ref qa);var br=Projectiles.Step(in restoredDefinition,in b,1.0/60,ref qb);
            Exact(ar,br,"full restored continuation result");a=ar.State;b=br.State;
        }
        Require(qa.Count==qb.Count,"restored continuation query count");
        Span<ProjectileSweep> storage=stackalloc ProjectileSweep[32];var spanQuery=new SpanMiss(storage);
        var value=Projectiles.Step(in d,in s,.1,ref spanQuery);
        Require(value.Queries==spanQuery.Count && spanQuery.Count>0,"ref-struct provider works without boxing");
        // Warm the same constrained generic path first. Nothing in this measured loop allocates
        // arrays, delegates, strings, serializers, interfaces or growing provider collections.
        var flat=Basic;var stationary=Projectiles.Launch(Zero,Zero);var counter=new Miss();
        var curved=Basic with {Acceleration=new Vector3d(0,-8,0),MaxChordError=.016};
        var moving=Projectiles.Launch(Zero,new Vector3d(3,0,0));
        for(int i=0;i<2048;++i)
        {Projectiles.Step(in flat,in stationary,.125,ref counter);Projectiles.Step(in curved,in moving,.5,ref counter);}
        int beforeCount=counter.Count;double total=0;long before=GC.GetAllocatedBytesForCurrentThread();
        for(int i=0;i<2048;++i)
        {
            total+=Projectiles.Step(in flat,in stationary,.125,ref counter).ConsumedSeconds;
            total+=Projectiles.Step(in curved,in moving,.5,ref counter).ConsumedSeconds;
        }
        long allocated=GC.GetAllocatedBytesForCurrentThread()-before;
        Require(allocated==0,"normal constrained-generic path allocates zero managed bytes");
        Require(counter.Count-beforeCount==2048*5 && total==1280,"allocation loop performed actual straight/curved provider calls");
    }

    private static int Main()
    {
        (string Name,Action Run)[] groups=[("analytic free flight and partitions",AnalyticFlight),
            ("quadratic curvature and native radius envelope",CurvatureEnvelope),
            ("independent plane contacts and incident state",PlaneContacts),
            ("initial overlap and closed chord",OverlapAndClosedChord),
            ("partial lifetime and terminal ownership",LifetimeAndTerminal),
            ("strict inputs and pre-query rejection",InvalidAndPreflight),
            ("provider failures and immutable inputs",ProviderFailures),
            ("complete caller replay and allocation",ReplayAndAllocation)];
        try
        {
            foreach(var group in groups){group.Run();Console.WriteLine("PASS "+group.Name);}
            Console.WriteLine($"Projectile tests passed: {groups.Length} groups, {checks} checks.");return 0;
        }
        catch(Exception error){Console.Error.WriteLine(error);return 1;}
    }
}
