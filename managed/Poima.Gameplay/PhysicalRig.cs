// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into committed physical-rig reads and staged writes during Tick.</summary>
public interface IPhysicalRigGame { }
public enum PhysicalRigMode : uint { Animated=0,Passive=1,Driven=2 }
public readonly record struct PhysicalRigState(EntityId Entity,PhysicalRigMode Mode,uint BodyCount,uint JointCount);
/// <summary>Symmetric body-space inertia tensor in kg*m², stored without normalization.</summary>
public readonly record struct PhysicalRigInertia(double XX,double XY,double XZ,double YX,double YY,double YZ,double ZX,double ZY,double ZZ);
/// <summary>Actual allocated center-of-mass state. In Animated mode this is the off-world
/// allocation, not the current animation target. Rotation copies the native committed
/// canonical diagnostic observation without SDK normalization. Exact raw solver
/// checkpoint encoding is a separate contract.</summary>
public readonly record struct PhysicalRigPartState(EntityId Entity,EntityId NodeEntity,uint Node,uint BodyId,string Part,
    Vector3d Position,Quaterniond Rotation,Vector3d LinearVelocity,Vector3d AngularVelocity,double Mass,PhysicalRigInertia Inertia,
    bool Enrolled,bool Active,bool Sleeping,double MaxLinearVelocity,double MaxAngularVelocity,
    double LinearDamping,double AngularDamping,double Friction,double Restitution,bool AtLinearVelocityLimit,bool AtAngularVelocityLimit);
public readonly record struct PhysicalRigImpulse(string Part,Vector3d WorldPoint,Vector3d Impulse);
/// <summary>Activation and optional world-point impulse. Impulse alone never activates a rig.</summary>
public readonly record struct PhysicalRigCommand(EntityId Entity,bool Activate=false,PhysicalRigImpulse? Impulse=null);
[StructLayout(LayoutKind.Sequential)] internal struct NativePhysicalRigSummaryV1
{public uint Version,Bytes,Present,Mode;public EntityId Entity;public uint BodyCount,JointCount;public ulong Reserved;}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigPartRequestV1
{public uint Version,Bytes;public EntityId Entity;public byte* Part;public uint PartBytes,Reserved;}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigPartStateV1
{
    public uint Version,Bytes,Present,Flags;public EntityId Entity,NodeEntity;public uint Node,BodyId,PartBytes,Reserved;public fixed byte Part[64];
    public Vector3d Position;public Quaterniond Rotation;public Vector3d LinearVelocity,AngularVelocity;public double Mass;public fixed double Inertia[9];
    public double MaxLinearVelocity,MaxAngularVelocity,LinearDamping,AngularDamping,Friction,Restitution;public ulong ReservedTail;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigCommandV1
{public uint Version,Bytes;public EntityId Entity;public uint Flags,Reserved;public byte* Part;public uint PartBytes,ReservedName;public Vector3d WorldPoint,Impulse;}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigServicesV1
{
    public NativeCapsuleQueryServicesV1 Capsule;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativePhysicalRigSummaryV1*,NativeError*,int> Summary;
    public delegate* unmanaged[Cdecl]<void*,NativePhysicalRigPartRequestV1*,NativePhysicalRigPartStateV1*,NativeError*,int> Part;
    public delegate* unmanaged[Cdecl]<void*,NativePhysicalRigCommandV1*,NativeError*,int> Command;
}
internal static unsafe class PhysicalRigServiceAbi
{
    internal const uint RequiredBytes=424;
    internal static bool LayoutValid()
    {
        NativePhysicalRigServicesV1 s=default;NativePhysicalRigSummaryV1 a=default;NativePhysicalRigPartRequestV1 q=default;NativePhysicalRigPartStateV1 p=default;NativePhysicalRigCommandV1 c=default;
        return CapsuleCastServiceAbi.LayoutValid() && sizeof(NativePhysicalRigServicesV1)==424 && sizeof(NativePhysicalRigSummaryV1)==48 && sizeof(NativePhysicalRigPartRequestV1)==40 && sizeof(NativePhysicalRigPartStateV1)==368 && sizeof(NativePhysicalRigCommandV1)==96 &&
            (byte*)&s.Summary-(byte*)&s==400 && (byte*)&s.Part-(byte*)&s==408 && (byte*)&s.Command-(byte*)&s==416 &&
            (byte*)&a.Entity-(byte*)&a==16 && (byte*)&a.BodyCount-(byte*)&a==32 && (byte*)&a.Reserved-(byte*)&a==40 &&
            (byte*)&q.Entity-(byte*)&q==8 && (byte*)&q.Part-(byte*)&q==24 && (byte*)&q.PartBytes-(byte*)&q==32 && (byte*)&q.Reserved-(byte*)&q==36 &&
            (byte*)&p.Entity-(byte*)&p==16 && (byte*)&p.NodeEntity-(byte*)&p==32 && (byte*)&p.Node-(byte*)&p==48 && (byte*)&p.BodyId-(byte*)&p==52 && (byte*)&p.PartBytes-(byte*)&p==56 && (byte*)&p.Reserved-(byte*)&p==60 &&
            (byte*)p.Part-(byte*)&p==64 && (byte*)&p.Position-(byte*)&p==128 && (byte*)&p.Rotation-(byte*)&p==152 && (byte*)&p.LinearVelocity-(byte*)&p==184 && (byte*)&p.AngularVelocity-(byte*)&p==208 && (byte*)&p.Mass-(byte*)&p==232 && (byte*)p.Inertia-(byte*)&p==240 &&
            (byte*)&p.MaxLinearVelocity-(byte*)&p==312 && (byte*)&p.MaxAngularVelocity-(byte*)&p==320 && (byte*)&p.LinearDamping-(byte*)&p==328 && (byte*)&p.AngularDamping-(byte*)&p==336 && (byte*)&p.Friction-(byte*)&p==344 && (byte*)&p.Restitution-(byte*)&p==352 && (byte*)&p.ReservedTail-(byte*)&p==360 &&
            (byte*)&c.Entity-(byte*)&c==8 && (byte*)&c.Flags-(byte*)&c==24 && (byte*)&c.Reserved-(byte*)&c==28 && (byte*)&c.Part-(byte*)&c==32 && (byte*)&c.PartBytes-(byte*)&c==40 && (byte*)&c.ReservedName-(byte*)&c==44 && (byte*)&c.WorldPoint-(byte*)&c==48 && (byte*)&c.Impulse-(byte*)&c==72;
    }
    internal static NativePhysicalRigServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)throw new ArgumentException("Physical rigs require services epoch7 with at least424 bytes. Declare IPhysicalRigGame.");
        var s=(NativePhysicalRigServicesV1*)services;
        if(s->Summary==null || s->Part==null || s->Command==null)throw new ArgumentException("Physical rig callbacks are absent.");return s;
    }
    private static bool Alnum(char c)=>c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9';
    internal static void Name(string part)
    {
        if(string.IsNullOrEmpty(part) || part.Length>64 || !Alnum(part[0]))throw new ArgumentException("Physical part must be1..64 portable ASCII bytes.",nameof(part));
        foreach(char c in part)if(!Alnum(c) && c is not ('_' or '.' or '-'))throw new ArgumentException("Physical part must be portable ASCII.",nameof(part));
    }
    private static bool Bounded(double x,double bound)=>double.IsFinite(x) && Math.Abs(x)<=bound;
    private static bool Vector(Vector3d v,double bound)=>Bounded(v.X,bound) && Bounded(v.Y,bound) && Bounded(v.Z,bound);
    private static bool Range(double x,double min,double max)=>double.IsFinite(x) && x>=min && x<=max;
    internal static void Input(in PhysicalRigCommand c)
    {
        if(c.Entity==default || (!c.Activate && c.Impulse==null))throw new ArgumentException("Physical command needs an entity and activation or impulse.");
        if(c.Impulse is {} i){Name(i.Part);if(!Vector(i.WorldPoint,1e6) || !Vector(i.Impulse,1e5) || i.Impulse.X*i.Impulse.X+i.Impulse.Y*i.Impulse.Y+i.Impulse.Z*i.Impulse.Z>1e10)throw new ArgumentException("Physical impulse requires bounded finite world point and impulse magnitude at most100000 kg*m/s.");}
    }
    private static void Zero(byte* p,int n){for(int i=0;i<n;++i)if(p[i]!=0)throw new InvalidOperationException("Absent native physical state is not canonical.");}
    internal static PhysicalRigState? Decode(NativePhysicalRigSummaryV1* p,EntityId entity)
        =>DecodeSummary(p,entity,1,1);
    internal static PhysicalRigState? DecodeSummary(NativePhysicalRigSummaryV1* p,EntityId entity,uint version,uint maximumMode)
    {
        if(p->Version!=version || p->Bytes!=48 || p->Present>1 || p->Mode>maximumMode || p->Entity!=entity || p->Reserved!=0)throw new InvalidOperationException("Invalid native physical summary header.");
        if(p->Present==0){if(p->Mode!=0 || p->BodyCount!=0 || p->JointCount!=0)throw new InvalidOperationException("Absent native physical summary has payload.");return null;}
        if(entity==default || p->BodyCount is <1 or >64 || p->JointCount>63 || p->JointCount!=p->BodyCount-1)throw new InvalidOperationException("Invalid native physical summary membership.");
        return new(entity,(PhysicalRigMode)p->Mode,p->BodyCount,p->JointCount);
    }
    internal static PhysicalRigPartState? Decode(NativePhysicalRigPartStateV1* p,EntityId entity,string part)
    {
        if(p->Version!=1 || p->Bytes!=368 || p->Present>1 || (p->Flags&~31u)!=0 || p->Entity!=entity || p->Reserved!=0 || p->ReservedTail!=0)throw new InvalidOperationException("Invalid native physical part header.");
        if(p->Present==0){if(p->Flags!=0)throw new InvalidOperationException("Absent physical part has flags.");Zero((byte*)p+32,336);return null;}
        // Pinned Jolt BodyID:23 index bits,8 sequence bits at bit23, broadphase bit31.
        if(entity==default || p->NodeEntity==default || p->Node>=10000 || (p->BodyId&0x007fffffu)>=4096 || (p->BodyId&0x80000000u)!=0 || p->PartBytes!=(uint)part.Length)throw new InvalidOperationException("Invalid native physical part identity.");
        for(int i=0;i<64;++i)if(p->Part[i]!=(i<part.Length?(byte)part[i]:(byte)0))throw new InvalidOperationException("Native physical part name differs.");
        var q=p->Rotation;double q2=q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W;
        if(!Vector(p->Position,1e6) || !Bounded(q.X,1.00001) || !Bounded(q.Y,1.00001) || !Bounded(q.Z,1.00001) || !Bounded(q.W,1.00001) || Math.Abs(q2-1)>1e-5 ||
            !Vector(p->LinearVelocity,1e6) || !Vector(p->AngularVelocity,1e6) || !Range(p->Mass,.0001*(1-1e-5),10000*(1+1e-5)) || !Range(p->MaxLinearVelocity,.001,1e6) || !Range(p->MaxAngularVelocity,.001,1e6) ||
            !Range(p->LinearDamping,0,10) || !Range(p->AngularDamping,0,10) || !Range(p->Friction,0,2) || !Range(p->Restitution,0,1) ||
            (((p->Flags&1)!=0)!=((p->Flags&6)!=0)) || ((p->Flags&6)==6))throw new InvalidOperationException("Invalid native physical part state.");
        double scale=0;for(int i=0;i<9;++i){if(!Bounded(p->Inertia[i],1e12))throw new InvalidOperationException("Invalid native physical inertia.");scale=Math.Max(scale,Math.Abs(p->Inertia[i]));}
        // Jolt returns float-derived mass and tensor entries. Preserve those values;
        // use a relative float-rounding allowance only for admission checks.
        if(scale==0)throw new InvalidOperationException("Zero native physical inertia.");
        double a=p->Inertia[0]/scale,b=p->Inertia[1]/scale,c=p->Inertia[2]/scale,d=p->Inertia[4]/scale,e=p->Inertia[5]/scale,f=p->Inertia[8]/scale;
        if(Math.Abs(b-p->Inertia[3]/scale)>1e-5 || Math.Abs(c-p->Inertia[6]/scale)>1e-5 || Math.Abs(e-p->Inertia[7]/scale)>1e-5 || a<=0 || a*d-b*b<=0 || a*(d*f-e*e)-b*(b*f-c*e)+c*(b*e-c*d)<=0)throw new InvalidOperationException("Native physical inertia is not symmetric positive definite.");
        return new(entity,p->NodeEntity,p->Node,p->BodyId,part,p->Position,p->Rotation,p->LinearVelocity,p->AngularVelocity,p->Mass,
            new(p->Inertia[0],p->Inertia[1],p->Inertia[2],p->Inertia[3],p->Inertia[4],p->Inertia[5],p->Inertia[6],p->Inertia[7],p->Inertia[8]),
            (p->Flags&1)!=0,(p->Flags&2)!=0,(p->Flags&4)!=0,p->MaxLinearVelocity,p->MaxAngularVelocity,p->LinearDamping,p->AngularDamping,p->Friction,p->Restitution,(p->Flags&8)!=0,(p->Flags&16)!=0);
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads committed membership; at most256 attempts per Tick. Staged writes stay invisible.</summary>
    public PhysicalRigState? GetPhysicalRig(EntityId entity)
    {
        if((requiredFeatures & GameplayRequiredFeatures.PhysicalRigDrive)!=0)
        {
            var drive=PhysicalRigDriveServiceAbi.Validate(services);if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));
            NativePhysicalRigSummaryV1 extended=new(){Version=2,Bytes=48};NativeError extendedError=default;
            Check(drive->SummaryV2(services->Context,&entity,&extended,&extendedError),&extendedError);
            return PhysicalRigServiceAbi.DecodeSummary(&extended,entity,2,2);
        }
        RequireFeature(GameplayRequiredFeatures.PhysicalRig,nameof(IPhysicalRigGame));var api=PhysicalRigServiceAbi.Validate(services);if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));
        NativePhysicalRigSummaryV1 result=new(){Version=1,Bytes=48};NativeError error=default;Check(api->Summary(services->Context,&entity,&result,&error),&error);return PhysicalRigServiceAbi.Decode(&result,entity);
    }
    /// <summary>Reads one actual allocated COM body; at most1024 attempts per Tick. No pointers escape.</summary>
    public PhysicalRigPartState? GetPhysicalRigPart(EntityId entity,string part)
    {
        RequireFeature(GameplayRequiredFeatures.PhysicalRig,nameof(IPhysicalRigGame));var api=PhysicalRigServiceAbi.Validate(services);if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));PhysicalRigServiceAbi.Name(part);
        byte* name=stackalloc byte[64];for(int i=0;i<part.Length;++i)name[i]=(byte)part[i];NativePhysicalRigPartRequestV1 request=new(){Version=1,Bytes=40,Entity=entity,Part=name,PartBytes=(uint)part.Length};NativePhysicalRigPartStateV1 result=new(){Version=1,Bytes=368};NativeError error=default;
        Check(api->Part(services->Context,&request,&result,&error),&error);return PhysicalRigServiceAbi.Decode(&result,entity,part);
    }
    /// <summary>Stages one ordered command, copied before return. At most64 attempts and64 combined
    /// caller/game writes per Tick. Reads remain committed. No recovery-to-animation is implied.</summary>
    public void SetPhysicalRig(in PhysicalRigCommand command)
    {
        RequireFeature(GameplayRequiredFeatures.PhysicalRig,nameof(IPhysicalRigGame));var api=PhysicalRigServiceAbi.Validate(services);PhysicalRigServiceAbi.Input(command);
        NativePhysicalRigCommandV1 native=new(){Version=1,Bytes=96,Entity=command.Entity,Flags=command.Activate?1u:0u};byte* name=stackalloc byte[64];
        if(command.Impulse is {} impulse){for(int i=0;i<impulse.Part.Length;++i)name[i]=(byte)impulse.Part[i];native.Flags|=2;native.Part=name;native.PartBytes=(uint)impulse.Part.Length;native.WorldPoint=impulse.WorldPoint;native.Impulse=impulse.Impulse;}
        NativeError error=default;Check(api->Command(services->Context,&native,&error),&error);
    }
    public void ActivatePhysicalRig(EntityId entity)=>SetPhysicalRig(new(entity,true));
    public void ImpulsePhysicalRig(EntityId entity,string part,Vector3d worldPoint,Vector3d impulse,bool activate=false)=>SetPhysicalRig(new(entity,activate,new PhysicalRigImpulse(part,worldPoint,impulse)));
}
