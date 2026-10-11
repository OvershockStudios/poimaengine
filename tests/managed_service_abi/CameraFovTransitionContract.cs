// SPDX-License-Identifier: Apache-2.0
using Poima;

static unsafe partial class Program
{
    static void CameraFovTransitionContract(List<string> checks)
    {
        static bool Exact(double a,double b)=>BitConverter.DoubleToInt64Bits(a)==BitConverter.DoubleToInt64Bits(b);
        static double Perspective(double degrees)=>Math.Tan(degrees*Math.PI/360);
        static void Close(double actual,double expected,string message)
        {
            Check(double.IsFinite(actual) && Math.Abs(actual-expected)<=2e-12*Math.Max(1,Math.Abs(expected)),message);
        }
        static void Reject(Action action,string parameter)
        {
            try {action();}
            catch(ArgumentOutOfRangeException error) {Check(error.ParamName==parameter,"Wrong transition parameter rejected.");return;}
            throw new Exception("Invalid transition argument was admitted.");
        }
        foreach(double fov in new[]{5.0,35.125,60.0,150.0})
        {
            Check(Exact(CameraFovTransition.Evaluate(fov,42.5,0,8),fov),"Transition start was rounded.");
            Check(Exact(CameraFovTransition.Evaluate(42.5,fov,8,8),fov),"Transition end was rounded.");
            Check(Exact(CameraFovTransition.Evaluate(42.5,fov,3600,8),fov),"Completed transition counter changed its endpoint.");
            Check(Exact(CameraFovTransition.Evaluate(42.5,fov,0,0),fov),"Zero duration did not select the exact target.");
            foreach(uint elapsed in new uint[]{0,1,1800,3599,3600})
                Check(Exact(CameraFovTransition.Evaluate(fov,fov,elapsed,3600),fov),"Unchanged FOV drifted.");
        }
        checks.Add("FOV transition exact start/end/unchanged/zero-duration semantics");

        // Independent perspective-domain oracle: fractional powers instead of
        // the helper's logarithmic interpolation; quarter weights are 5/32,27/32.0
        double q0=Perspective(110),q1=Perspective(25);
        foreach(var sample in new[]{(Tick:2u,Weight:5.0/32),(Tick:4u,Weight:.5),(Tick:6u,Weight:27.0/32)})
        {
            double actual=CameraFovTransition.Evaluate(110,25,sample.Tick,8);
            Close(Perspective(actual),q0*Math.Pow(q1/q0,sample.Weight),"Perspective-domain smoothstep disagrees with independent curve.");
        }
        double midpoint=CameraFovTransition.Evaluate(110,25,4,8);
        Close(Perspective(midpoint),Math.Sqrt(q0*q1),"Midpoint is not geometric perspective mean.");
        Check(Math.Abs(midpoint-(110+25)/2.0)>1,"FOV transition silently used linear angle interpolation.");
        checks.Add("FOV transition independent quarter and geometric midpoint oracles");

        foreach(var pair in new[]{(Start:5.0,Target:150.0),(Start:150.0,Target:5.0),(Start:73.5,Target:24.25)})
        {
            double previous=pair.Start;
            for(uint elapsed=0;elapsed<=3600;++elapsed)
            {
                double actual=CameraFovTransition.Evaluate(pair.Start,pair.Target,elapsed,3600);
                Check(double.IsFinite(actual) && actual>=Math.Min(pair.Start,pair.Target) && actual<=Math.Max(pair.Start,pair.Target),"Transition exceeded endpoint bounds.");
                Check(pair.Start<pair.Target ? actual>=previous : actual<=previous,"Transition reversed away from its target.");
                Close(actual,CameraFovTransition.Evaluate(pair.Target,pair.Start,3600-elapsed,3600),"Reverse curve lost perspective symmetry.");
                previous=actual;
            }
        }
        checks.Add("FOV transition bounded monotonic full-range and reverse curves");

        double current=CameraFovTransition.Evaluate(90,25,3,12);
        foreach(double target in new[]{105.0,20.0,90.0,35.0})
        {
            Check(Exact(CameraFovTransition.Evaluate(current,target,0,12),current),"Retarget broke position continuity.");
            current=CameraFovTransition.Evaluate(current,target,2,12);
            Check(current>=20 && current<=105,"Rapid reversal overshot its endpoint interval.");
        }
        Check(Exact(CameraFovTransition.Evaluate(current,60,12,12),60),"Rapid reversal failed to settle exactly.");
        double savedSample=CameraFovTransition.Evaluate(90,25,3,12);
        _=CameraFovTransition.Evaluate(5,150,17,90);
        Check(Exact(savedSample,CameraFovTransition.Evaluate(90,25,3,12)),"Evaluation retained hidden transition state.");
        checks.Add("FOV transition caller-owned rapid retarget and stateless repeat");

        foreach(double invalid in new[]{double.NaN,double.PositiveInfinity,double.NegativeInfinity,4.999,150.001,double.MaxValue})
        {
            Reject(()=>CameraFovTransition.Evaluate(invalid,60,0,0),"startFov");
            Reject(()=>CameraFovTransition.Evaluate(60,invalid,0,0),"targetFov");
        }
        foreach(uint invalid in new[]{3601u,uint.MaxValue,unchecked((uint)-1)})
        {
            Reject(()=>CameraFovTransition.Evaluate(60,60,invalid,0),"elapsedTicks");
            Reject(()=>CameraFovTransition.Evaluate(60,60,0,invalid),"durationTicks");
        }
        checks.Add("FOV transition finite endpoints and overflow-sized counters reject before shortcuts");
    }
}
