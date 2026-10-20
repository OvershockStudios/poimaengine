// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.Examples;
using Poima.Fps;

static class Program
{
    private static int checks;
    private static void Require(bool value,string message)
    {++checks;if(!value)throw new Exception(message);}
    private static void Near(double actual,double expected,double tolerance=2e-13)
    {Require(double.IsFinite(actual)&&Math.Abs(actual-expected)<=tolerance,$"Expected {expected:R}, got {actual:R}.");}
    private static bool Bits(double a,double b)=>BitConverter.DoubleToInt64Bits(a)==BitConverter.DoubleToInt64Bits(b);
    private static void Vector(Vector3d actual,Vector3d expected,double tolerance=2e-13)
    {Near(actual.X,expected.X,tolerance);Near(actual.Y,expected.Y,tolerance);Near(actual.Z,expected.Z,tolerance);}
    private static void Reject(Action action)
    {bool rejected=false;try{action();}catch(ArgumentOutOfRangeException){rejected=true;}catch(ArithmeticException){rejected=true;}Require(rejected,"Invalid math input/output must reject.");}

    private static void AnalyticSpring()
    {
        // At omega1, x''+2x'+x=0, x(0)=1,v(0)=0 gives x(1)=2/e,v(1)=-1/e.
        var a=Feedback.Step(new ScalarSpring(1,0),0,1/(2*Math.PI),1);
        Near(a.Position,2/Math.E);Near(a.Velocity,-1/Math.E);
        // Free impulse response at its analytically known peak t=1/omega.
        var impulse=Feedback.ApplyImpulse(new ScalarSpring(0,0),3);
        Near(impulse.Position,0);Near(impulse.Velocity,3);
        var peak=Feedback.Step(impulse,0,1/Math.PI,.5);
        Near(peak.Position,1.5/Math.E);Near(peak.Velocity,0);
        var equilibrium=Feedback.Step(new ScalarSpring(7,0),7,25,.75);
        Require(equilibrium==new ScalarSpring(7,0),"A constant equilibrium must remain exact.");
        var vector=Feedback.Step(new VectorSpring(new(1,-2,0),new(0,0,3)),new(0,0,0),1/(2*Math.PI),1);
        Vector(vector.Position,new(2/Math.E,-4/Math.E,3/Math.E));
        Vector(vector.Velocity,new(-1/Math.E,2/Math.E,0));
        var original=new ScalarSpring(-.7,.4);var whole=Feedback.Step(original,.25,2,.8);
        foreach(int pieces in new[]{2,8,32,96})
        {
            var divided=original;
            for(int i=0;i<pieces;++i)divided=Feedback.Step(divided,.25,2,.8/pieces);
            Near(divided.Position,whole.Position,2e-12);Near(divided.Velocity,whole.Velocity,2e-12);
        }
        double minusZero=BitConverter.Int64BitsToDouble(long.MinValue);
        var zero=Feedback.Step(new ScalarSpring(minusZero,minusZero),0,2,0);
        Require(Bits(zero.Position,minusZero)&&Bits(zero.Velocity,minusZero),"dt0 must preserve signed zero bits.");
        var noImpulse=Feedback.ApplyImpulse(new ScalarSpring(minusZero,minusZero),0);
        Require(Bits(noImpulse.Position,minusZero)&&Bits(noImpulse.Velocity,minusZero),"Null impulse must preserve signed zero bits.");
        foreach(double invalid in new[]{double.NaN,double.PositiveInfinity,double.NegativeInfinity})
        {
            Reject(()=>Feedback.Step(new ScalarSpring(invalid,0),0,2,0));
            Reject(()=>Feedback.Step(new ScalarSpring(0,0),invalid,2,0));
            Reject(()=>Feedback.Step(new ScalarSpring(0,0),0,invalid,0));
            Reject(()=>Feedback.ApplyImpulse(new ScalarSpring(0,0),invalid));
        }
        Reject(()=>Feedback.Step(new ScalarSpring(0,0),0,0,0));
        Reject(()=>Feedback.Step(new ScalarSpring(0,0),0,2,-.1));
        Reject(()=>Feedback.Step(new ScalarSpring(0,0),0,2,1.1));
        var unchanged=new ScalarSpring(0,Feedback.MaxSpringVelocity);
        Reject(()=>Feedback.ApplyImpulse(unchanged,1));
        Require(unchanged==new ScalarSpring(0,Feedback.MaxSpringVelocity),"Value rejection cannot publish partial spring state.");
    }
    private static void SwayAndDistanceBob()
    {
        Near(Feedback.SwayTarget(.04,-.5,.1),-.02);
        Near(Feedback.SwayTarget(Math.PI/2,-.5,.1),-.1);
        Near(Feedback.SwayTarget(-Math.PI/2,-.5,.1),.1);
        Vector(Feedback.SwayTarget(new Vector3d(.02,-.03,.01),-.5,.1),new(-.01,.015,-.005));
        double phase=.25;
        Near(Feedback.AdvanceBobPhase(phase,.5,2),phase);
        Near(Feedback.AdvanceBobPhase(Feedback.AdvanceBobPhase(phase,.125,2),.375,2),phase);
        Require(Bits(Feedback.AdvanceBobPhase(phase,0,2),phase),"Blocked feet do not advance distance-driven phase.");
        var quarter=Feedback.EvaluateBob(.25,.5,.02,.03,.1,.2);
        Near(quarter.LateralMetres,.01);Near(quarter.VerticalMetres,0);Near(quarter.RollRadians,.05);Near(quarter.PitchRadians,0);
        var eighth=Feedback.EvaluateBob(.125,1,.02,.03,.1,.2);
        Near(eighth.LateralMetres,.02/Math.Sqrt(2));Near(eighth.VerticalMetres,.03);Near(eighth.PitchRadians,.2);
        var idle=Feedback.EvaluateBob(.125,0,.02,.03,.1,.2);
        Require(idle==new BobTarget(0,0,0,0),"Idle envelope removes every bob target.");
        Reject(()=>Feedback.SwayTarget(double.NaN,1,.1));
        Reject(()=>Feedback.SwayTarget(1,1,0));
        Reject(()=>Feedback.AdvanceBobPhase(.2,-1,1));
        Reject(()=>Feedback.AdvanceBobPhase(1,0,1));
        Reject(()=>Feedback.EvaluateBob(0,1.01,0,0,0,0));
    }
    private static void SeededShake()
    {
        var large=Feedback.NoiseAt(ulong.MaxValue,10);
        Require(large.Cell==1844674407370955161UL,"Large tick cell must retain integer precision.");Near(large.Fraction,.5,0);
        var amplitude=new Vector3d(.1,.2,.3);var coordinate=Feedback.NoiseAt(12345,7);
        var a=Feedback.Shake(coordinate,123,amplitude);var b=Feedback.Shake(coordinate,123,amplitude);
        Require(Bits(a.X,b.X)&&Bits(a.Y,b.Y)&&Bits(a.Z,b.Z),"Same saved coordinate/seed gives exact future noise.");
        Require(a!=Feedback.Shake(coordinate,124,amplitude),"Distinct seed changes selected noise.");
        for(ulong tick=0;tick<512;++tick)
        {
            var n=Feedback.Shake(Feedback.NoiseAt(tick,7),123,amplitude);
            Require(Math.Abs(n.X)<=.1&&Math.Abs(n.Y)<=.2&&Math.Abs(n.Z)<=.3,"Noise respects all per-axis artistic bounds.");
        }
        // Quintic lattice interpolation is continuous and has zero endpoint slope.
        var left=Feedback.Shake(new(7,1-1e-7),123,amplitude);
        var right=Feedback.Shake(new(8,0),123,amplitude);
        Vector(left,right,2e-14);
        Require(Feedback.Shake(coordinate,123,new(0,0,0))==new Vector3d(0,0,0),"Zero amplitude is zero motion.");
        Reject(()=>Feedback.NoiseAt(1,0));Reject(()=>Feedback.Shake(new(0,1),0,amplitude));
        Reject(()=>Feedback.Shake(new(0,0),0,new(-.1,0,0)));
    }
    private static void RecoilProfiles()
    {
        Vector3d[] pattern=[new(.01,.02,.03),new(.04,.05,.06)];var variation=new Vector3d(.1,.1,.1);
        Vector(Recoil.AngularKick(pattern,0,17,variation,RecoilMode.Authored),pattern[0],0);
        Vector(Recoil.AngularKick(pattern,2,17,variation,RecoilMode.Authored),pattern[0],0);
        Vector(Recoil.AngularKick(pattern,100,17,variation,RecoilMode.Authored,RecoilPatternEnd.Hold),pattern[1],0);
        Vector(Recoil.AngularKick(pattern,2,17,variation,RecoilMode.Authored,RecoilPatternEnd.Stop),new(0,0,0),0);
        Vector(Recoil.AngularKick(pattern,2,17,variation,RecoilMode.Hybrid,RecoilPatternEnd.Stop),new(0,0,0),0);
        var noise=Recoil.AngularKick([],0,17,variation,RecoilMode.Seeded);
        Require(Math.Abs(noise.X)<=.1&&Math.Abs(noise.Y)<=.1&&Math.Abs(noise.Z)<=.1,"Seeded kick is bounded per axis.");
        var hybrid=Recoil.AngularKick(pattern,0,17,variation,RecoilMode.Hybrid);
        Vector(hybrid,new(pattern[0].X+noise.X,pattern[0].Y+noise.Y,pattern[0].Z+noise.Z),0);
        Require(hybrid==Recoil.AngularKick(pattern,0,17,variation,RecoilMode.Hybrid),"Same saved accepted-shot index/seed is repeatable.");
        Require(noise!=Recoil.AngularKick([],0,17,variation,RecoilMode.Seeded,stream:1),"Aim and weapon streams are independent.");
        Reject(()=>Recoil.AngularKick([],0,0,variation,RecoilMode.Authored));
        Reject(()=>Recoil.AngularKick([new(double.NaN,0,0)],0,0,variation,RecoilMode.Seeded));
        Reject(()=>Recoil.AngularKick(pattern,0,0,variation,(RecoilMode)99));
        // Independently construct a valid authored kick whose combination must exceed the domain.
        Require(noise.X!=0,"Selected deterministic random channel is nonzero.");
        double limit=Math.CopySign(Recoil.MaxKickRadians,noise.X);
        Reject(()=>Recoil.AngularKick([new(limit,0,0)],0,17,variation,RecoilMode.Hybrid));
    }
    public readonly record struct Saved(double Position,double Velocity,double Phase,ulong Tick,uint Seed,ulong Shots);
    private static Saved Advance(Saved saved)
    {
        var spring=new ScalarSpring(saved.Position,saved.Velocity);ulong shots=saved.Shots;
        if(saved.Tick%7==0)
        {
            var kick=Recoil.AngularKick([new(.02,0,0)],shots,saved.Seed,new(.005,0,0),RecoilMode.Hybrid);
            spring=Feedback.ApplyImpulse(spring,kick.X);++shots;
        }
        spring=Feedback.Step(spring,0,4,1.0/60);
        return new(spring.Position,spring.Velocity,Feedback.AdvanceBobPhase(saved.Phase,.025,2),saved.Tick+1,saved.Seed,shots);
    }
    private static void SavedContinuation()
    {
        var original=new Saved(0,0,0,0,14567,0);
        for(int i=0;i<11;++i)original=Advance(original);
        var bytes=JsonSerializer.SerializeToUtf8Bytes(original);
        var restored=JsonSerializer.Deserialize<Saved>(bytes);
        Require(JsonSerializer.SerializeToUtf8Bytes(restored).SequenceEqual(bytes),"Every saved primitive roundtrips without filtering.");
        for(int i=0;i<120;++i)
        {
            original=Advance(original);restored=Advance(restored);
            Require(JsonSerializer.SerializeToUtf8Bytes(original).SequenceEqual(JsonSerializer.SerializeToUtf8Bytes(restored)),"Complete future helper state agrees after fresh reconstruction.");
        }
    }
    private static void LogicalAimSeparation()
    {
        WorldTransform basis=default;
        var matrix=MemoryMarshal.Cast<WorldTransform,double>(MemoryMarshal.CreateSpan(ref basis,1));
        matrix[0]=matrix[5]=matrix[10]=matrix[15]=1;
        matrix[12]=3;matrix[13]=2;matrix[14]=1;
        Vector(FpsPoseMath.BaseDirection(in basis,Quaterniond.Identity),new(0,0,-1));
        Vector(FpsPoseMath.BaseDirection(in basis,FpsPoseMath.Rotation(new(Math.PI/2,0,0))),new(0,1,0));
        Vector(FpsPoseMath.BaseDirection(in basis,FpsPoseMath.Rotation(new(0,Math.PI/2,0))),new(-1,0,0));
        var aim=FpsPoseMath.Rotation(new(.1,.2,0));var pureShake=FpsPoseMath.Rotation(new(.7,-.4,.3));
        var logical=FpsPoseMath.BaseDirection(in basis,aim);
        var presented=FpsPoseMath.BaseDirection(in basis,FpsPoseMath.Product(aim,pureShake));
        Require(logical!=presented,"Presentation shake would alter the hit ray if incorrectly composed into aim.");
        Vector(FpsPoseMath.BaseDirection(in basis,aim),logical,0);
        Reject(()=>FpsPoseMath.Rotation(new(double.NaN,0,0)));
    }
    public static void Main()
    {
        AnalyticSpring();SwayAndDistanceBob();SeededShake();RecoilProfiles();SavedContinuation();LogicalAimSeparation();
        Console.WriteLine(JsonSerializer.Serialize(new{passed=true,groups=6,checks,scope="Pure managed math and caller-owned primitive continuation; no native runtime/save/renderer execution."}));
    }
}
