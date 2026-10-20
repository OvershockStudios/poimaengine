// SPDX-License-Identifier: Apache-2.0
using Poima;
using Poima.Fps;
namespace Poima.Examples;

internal static class FeedbackMotion
{
    private const double AimHz=6,WeaponHz=8;
    private static readonly Vector3d[] AimPattern=[
        new(.012,-.002,0),new(.015,.003,0),new(.017,-.004,0),new(.020,.004,0)];
    private static readonly Vector3d[] WeaponPattern=[
        new(.040,-.010,.004),new(.046,.012,-.006),new(.052,-.014,.008),new(.056,.016,-.008)];
    private static VectorSpring Aim(in FpsFeedbackState s)=>new(new(s.AimX,s.AimY,s.AimZ),new(s.AimVX,s.AimVY,s.AimVZ));
    private static VectorSpring Weapon(in FpsFeedbackState s)=>new(new(s.WeaponX,s.WeaponY,s.WeaponZ),new(s.WeaponVX,s.WeaponVY,s.WeaponVZ));
    private static void StoreAim(ref FpsFeedbackState s,VectorSpring a)
    {s.AimX=a.Position.X;s.AimY=a.Position.Y;s.AimZ=a.Position.Z;s.AimVX=a.Velocity.X;s.AimVY=a.Velocity.Y;s.AimVZ=a.Velocity.Z;}
    private static void StoreWeapon(ref FpsFeedbackState s,VectorSpring a)
    {s.WeaponX=a.Position.X;s.WeaponY=a.Position.Y;s.WeaponZ=a.Position.Z;s.WeaponVX=a.Velocity.X;s.WeaponVY=a.Velocity.Y;s.WeaponVZ=a.Velocity.Z;}
    private static Vector3d Scale(Vector3d v,double scale)=>new(v.X*scale,v.Y*scale,v.Z*scale);
    private static Vector3d Add(Vector3d a,Vector3d b)=>new(a.X+b.X,a.Y+b.Y,a.Z+b.Z);

    public static void Shot(ref FpsFeedbackState s)
    {
        if(s.Shots<=0)throw new InvalidOperationException("Feedback requires a committed accepted-shot ordinal.");
        var mode=(RecoilMode)s.FeedbackMode;uint seed=unchecked((uint)s.FeedbackSeed);
        ulong ordinal=(ulong)(s.Shots-1);
        var aim=Recoil.AngularKick(AimPattern,ordinal,seed,new(.003,.002,0),mode,stream:0);
        var weapon=Recoil.AngularKick(WeaponPattern,ordinal,seed,new(.010,.008,.004),mode,stream:1);
        // A critically damped impulse peaks at t=1/omega. This converts an
        // authored per-shot peak angle into an explicit delta-velocity impulse.
        StoreAim(ref s,Feedback.ApplyImpulse(Aim(s),Scale(aim,2*Math.PI*AimHz*Math.E)));
        StoreWeapon(ref s,Feedback.ApplyImpulse(Weapon(s),Scale(weapon,2*Math.PI*WeaponHz*Math.E)));
        var back=Feedback.ApplyImpulse(new ScalarSpring(s.WeaponBack,s.WeaponBackVelocity),.035*2*Math.PI*WeaponHz*Math.E);
        s.WeaponBack=back.Position;s.WeaponBackVelocity=back.Velocity;
        var shake=Feedback.ApplyImpulse(new ScalarSpring(s.ShakeEnvelope,s.ShakeEnvelopeVelocity),1.5);
        s.ShakeEnvelope=shake.Position;s.ShakeEnvelopeVelocity=shake.Velocity;
    }
    public static void Damage(ref FpsFeedbackState s)
    {
        var shake=Feedback.ApplyImpulse(new ScalarSpring(s.ShakeEnvelope,s.ShakeEnvelopeVelocity),12);
        s.ShakeEnvelope=shake.Position;s.ShakeEnvelopeVelocity=shake.Velocity;
    }
    public static (Vector3d Origin,Vector3d Direction) AimRay(in FpsFeedbackState s,GameContext c)
    {
        var camera=c.GetCameraPose(new EntityId(0,3))??throw new InvalidOperationException("FPS Feedback requires camera_pose_v1 on camera3.");
        var basis=camera.BaseWorld;
        return(basis.Position,FpsPoseMath.BaseDirection(in basis,FpsPoseMath.Rotation(Aim(s).Position)));
    }
    public static void Update(ref FpsFeedbackState s,GameContext c)
    {
        double dt=c.DeltaTime;var actor=c.Get(new EntityId(0,2)).Transform.Position;
        double distance=0;
        if(s.HavePreviousPlayer!=0&&s.PreviousPlayerTick==(long)c.Tick-1)
        {
            double x=actor.X-s.PreviousPlayerX,z=actor.Z-s.PreviousPlayerZ;
            distance=Math.Sqrt(x*x+z*z);
        }
        s.PreviousPlayerX=actor.X;s.PreviousPlayerZ=actor.Z;s.PreviousPlayerTick=(long)c.Tick;s.HavePreviousPlayer=1;
        // This reused range has one flat floor at Y0. Airborne feet do not bob.
        bool grounded=Math.Abs(actor.Y)<=.08;
        if(grounded)s.BobPhase=Feedback.AdvanceBobPhase(s.BobPhase,distance,1.6);
        var envelope=Feedback.Step(new ScalarSpring(s.BobEnvelope,s.BobEnvelopeVelocity),
            grounded?Math.Clamp(distance/(4*dt),0,1):0,4,dt);
        s.BobEnvelope=envelope.Position;s.BobEnvelopeVelocity=envelope.Velocity;
        double radians=Math.PI/180,lookPitch=0,lookYaw=0;
        foreach(var input in c.Inputs)if(input.Entity==new EntityId(0,2))
        {lookPitch=input.LookPitch*radians;lookYaw=input.LookYaw*radians;break;}
        var target=Feedback.SwayTarget(new Vector3d(lookPitch,lookYaw,0),-.6,.045);
        var sway=Feedback.Step(new VectorSpring(new(s.SwayX,s.SwayY,s.SwayZ),new(s.SwayVX,s.SwayVY,s.SwayVZ)),target,7,dt);
        s.SwayX=sway.Position.X;s.SwayY=sway.Position.Y;s.SwayZ=sway.Position.Z;s.SwayVX=sway.Velocity.X;s.SwayVY=sway.Velocity.Y;s.SwayVZ=sway.Velocity.Z;
        StoreAim(ref s,Feedback.Step(Aim(s),new(0,0,0),AimHz,dt));
        StoreWeapon(ref s,Feedback.Step(Weapon(s),new(0,0,0),WeaponHz,dt));
        var back=Feedback.Step(new ScalarSpring(s.WeaponBack,s.WeaponBackVelocity),0,WeaponHz,dt);
        s.WeaponBack=back.Position;s.WeaponBackVelocity=back.Velocity;
        var shake=Feedback.Step(new ScalarSpring(s.ShakeEnvelope,s.ShakeEnvelopeVelocity),0,3,dt);
        s.ShakeEnvelope=shake.Position;s.ShakeEnvelopeVelocity=shake.Velocity;
        if(s.FeedbackTicks<0)throw new InvalidOperationException("Feedback clock must be nonnegative.");
        s.FeedbackTicks=checked(s.FeedbackTicks+1);
        double ads=Math.Clamp((70-s.AdsWorld)/35,0,1),gain=1-.65*ads;
        var bob=Feedback.EvaluateBob(s.BobPhase,Math.Clamp(s.BobEnvelope,0,1),.010,.007,.008,.004);
        var noise=Feedback.Shake(Feedback.NoiseAt((ulong)s.FeedbackTicks,3),unchecked((uint)s.FeedbackSeed)^0x41371u,
            new Vector3d(.040,.028,.018));
        var visualShake=Scale(noise,Math.Clamp(s.ShakeEnvelope,0,1)*gain);
        var cameraVisual=Add(visualShake,new Vector3d(bob.PitchRadians*.18*gain,0,bob.RollRadians*.18*gain));
        var world=new CameraPoseOffset(new(bob.LateralMetres*.25*gain,bob.VerticalMetres*.25*gain,0),
            FpsPoseMath.Product(FpsPoseMath.Rotation(Aim(s).Position),FpsPoseMath.Rotation(cameraVisual)));
        var weaponVisual=Scale(Add(Add(Weapon(s).Position,sway.Position),new(bob.PitchRadians,0,bob.RollRadians)),gain);
        var viewmodel=new CameraPoseOffset(new((bob.LateralMetres+sway.Position.Y*.1)*gain,
            (bob.VerticalMetres-sway.Position.X*.1)*gain,s.WeaponBack*gain),FpsPoseMath.Rotation(weaponVisual));
        c.SetCameraPose(new EntityId(0,3),world,viewmodel);
    }
}
