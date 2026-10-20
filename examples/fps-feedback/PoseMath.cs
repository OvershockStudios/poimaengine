// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Examples;

/// <summary>Camera-local radians: pitchX/yawY/rollZ, fixed quaternion composition Y*X*Z.</summary>
internal static class FpsPoseMath
{
    public static Quaterniond Rotation(Vector3d radians)
    {
        if(!double.IsFinite(radians.X)||!double.IsFinite(radians.Y)||!double.IsFinite(radians.Z))
            throw new ArgumentOutOfRangeException(nameof(radians));
        var x=new Quaterniond(Math.Sin(radians.X/2),0,0,Math.Cos(radians.X/2));
        var y=new Quaterniond(0,Math.Sin(radians.Y/2),0,Math.Cos(radians.Y/2));
        var z=new Quaterniond(0,0,Math.Sin(radians.Z/2),Math.Cos(radians.Z/2));
        return Product(Product(y,x),z);
    }
    public static Quaterniond Product(Quaterniond a,Quaterniond b)=>new(
        a.W*b.X+a.X*b.W+a.Y*b.Z-a.Z*b.Y,
        a.W*b.Y-a.X*b.Z+a.Y*b.W+a.Z*b.X,
        a.W*b.Z+a.X*b.Y-a.Y*b.X+a.Z*b.W,
        a.W*b.W-a.X*b.X-a.Y*b.Y-a.Z*b.Z);
    public static Vector3d Rotate(Quaterniond q,Vector3d v)
    {
        double tx=2*(q.Y*v.Z-q.Z*v.Y),ty=2*(q.Z*v.X-q.X*v.Z),tz=2*(q.X*v.Y-q.Y*v.X);
        return new(v.X+q.W*tx+q.Y*tz-q.Z*ty,
                   v.Y+q.W*ty+q.Z*tx-q.X*tz,
                   v.Z+q.W*tz+q.X*ty-q.Y*tx);
    }
    public static Vector3d BaseDirection(in WorldTransform basis,Quaterniond logicalAim)
    {
        var v=Rotate(logicalAim,new(0,0,-1));
        double x=basis[0,0]*v.X+basis[1,0]*v.Y+basis[2,0]*v.Z;
        double y=basis[0,1]*v.X+basis[1,1]*v.Y+basis[2,1]*v.Z;
        double z=basis[0,2]*v.X+basis[1,2]*v.Y+basis[2,2]*v.Z;
        double length=Math.Sqrt(x*x+y*y+z*z);
        if(!double.IsFinite(length)||length<=0)throw new InvalidOperationException("Logical camera direction is not finite/nonzero.");
        return new(x/length,y/length,z/length);
    }
}
