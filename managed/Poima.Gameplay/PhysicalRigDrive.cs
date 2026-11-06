// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into bounded authored joint drives and the Driven summary mode.</summary>
public interface IPhysicalRigDriveGame : IPhysicalRigGame { }
/// <summary>Committed joint-frame target and strength, plus immutable authored gains.
/// Stiffness is N*m/rad, damping N*m*s/rad and maximum torque N*m.</summary>
public readonly record struct PhysicalRigDriveState(EntityId Entity,string Joint,Quaterniond TargetRotation,
    double Strength,double Stiffness,double Damping,double MaxTorque);
/// <summary>A joint-frame target and strength; requires existing physical ownership.
/// Use an earlier activation command in the same ordered tick to transfer ownership.</summary>
public readonly record struct PhysicalRigDriveCommand(EntityId Entity,string Joint,Quaterniond TargetRotation,double Strength);

[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigDriveRequestV1
{ public uint Version,Bytes;public EntityId Entity;public byte* Joint;public uint JointBytes,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigDriveStateV1
{
    public uint Version,Bytes,Present,Reserved;public EntityId Entity;public uint JointBytes,ReservedName;public fixed byte Joint[64];
    public Quaterniond TargetRotation;public double Strength,Stiffness,Damping,MaxTorque;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigDriveCommandV1
{
    public uint Version,Bytes;public EntityId Entity;public byte* Joint;public uint JointBytes,Reserved;
    public Quaterniond TargetRotation;public double Strength;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigDriveServicesV1
{
    public NativePhysicalRigServicesV1 Physical;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativePhysicalRigSummaryV1*,NativeError*,int> SummaryV2;
    public delegate* unmanaged[Cdecl]<void*,NativePhysicalRigDriveRequestV1*,NativePhysicalRigDriveStateV1*,NativeError*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,NativePhysicalRigDriveCommandV1*,NativeError*,int> Set;
}
internal static unsafe class PhysicalRigDriveServiceAbi
{
    internal const uint RequiredBytes=448;
    internal static bool LayoutValid()
    {
        NativePhysicalRigDriveServicesV1 s=default;NativePhysicalRigDriveRequestV1 r=default;
        NativePhysicalRigDriveStateV1 v=default;NativePhysicalRigDriveCommandV1 c=default;
        return PhysicalRigServiceAbi.LayoutValid() && sizeof(NativePhysicalRigDriveServicesV1)==448 &&
            (byte*)&s.Physical-(byte*)&s==0 && (byte*)&s.SummaryV2-(byte*)&s==424 && (byte*)&s.Get-(byte*)&s==432 && (byte*)&s.Set-(byte*)&s==440 &&
            sizeof(NativePhysicalRigDriveRequestV1)==40 && (byte*)&r.Entity-(byte*)&r==8 && (byte*)&r.Joint-(byte*)&r==24 && (byte*)&r.JointBytes-(byte*)&r==32 && (byte*)&r.Reserved-(byte*)&r==36 &&
            sizeof(NativePhysicalRigDriveStateV1)==168 && (byte*)&v.Entity-(byte*)&v==16 && (byte*)&v.JointBytes-(byte*)&v==32 && (byte*)&v.ReservedName-(byte*)&v==36 && (byte*)v.Joint-(byte*)&v==40 &&
            (byte*)&v.TargetRotation-(byte*)&v==104 && (byte*)&v.Strength-(byte*)&v==136 && (byte*)&v.Stiffness-(byte*)&v==144 && (byte*)&v.Damping-(byte*)&v==152 && (byte*)&v.MaxTorque-(byte*)&v==160 &&
            sizeof(NativePhysicalRigDriveCommandV1)==80 && (byte*)&c.Entity-(byte*)&c==8 && (byte*)&c.Joint-(byte*)&c==24 && (byte*)&c.JointBytes-(byte*)&c==32 && (byte*)&c.Reserved-(byte*)&c==36 && (byte*)&c.TargetRotation-(byte*)&c==40 && (byte*)&c.Strength-(byte*)&c==72;
    }
    internal static NativePhysicalRigDriveServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Physical joint drives require services epoch 7 with at least 448 bytes. Declare IPhysicalRigDriveGame.");
        _=PhysicalRigServiceAbi.Validate(services);var s=(NativePhysicalRigDriveServicesV1*)services;
        if(s->SummaryV2==null || s->Get==null || s->Set==null)throw new ArgumentException("Physical joint drive callbacks are absent.");return s;
    }
    private static bool Range(double x,double minimum,double maximum)=>double.IsFinite(x) && x>=minimum && x<=maximum;
    internal static bool Rotation(Quaterniond q)
    {
        if(!Range(q.X,-1.000001,1.000001) || !Range(q.Y,-1.000001,1.000001) || !Range(q.Z,-1.000001,1.000001) || !Range(q.W,-1.000001,1.000001))return false;
        return Math.Abs(Math.Sqrt(q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W)-1)<=1e-6;
    }
    internal static void Input(in PhysicalRigDriveCommand value)
    {
        if(value.Entity==default)throw new ArgumentException("Physical drive entity is absent.");PhysicalRigServiceAbi.Name(value.Joint);
        if(!Rotation(value.TargetRotation) || !Range(value.Strength,0,1))throw new ArgumentException("Physical drive requires a finite unit target quaternion and strength in [0,1].");
    }
    internal static PhysicalRigDriveState? Decode(NativePhysicalRigDriveStateV1* value,EntityId entity,string joint)
    {
        if(value->Version!=1 || value->Bytes!=168 || value->Present>1 || value->Reserved!=0 || value->ReservedName!=0 || value->Entity!=entity)
            throw new InvalidOperationException("Invalid native physical drive header.");
        if(value->Present==0)
        {
            for(int i=32;i<168;++i)if(((byte*)value)[i]!=0)throw new InvalidOperationException("Absent physical drive has payload.");return null;
        }
        if(entity==default || value->JointBytes!=(uint)joint.Length)throw new InvalidOperationException("Native physical drive identity differs.");
        for(int i=0;i<64;++i)if(value->Joint[i]!=(i<joint.Length?(byte)joint[i]:(byte)0))throw new InvalidOperationException("Native physical drive joint name differs.");
        if(!Rotation(value->TargetRotation) || !Range(value->Strength,0,1) || !Range(value->Stiffness,double.Epsilon,1e6) ||
            !Range(value->Damping,0,1e4) || !Range(value->MaxTorque,double.Epsilon,1e5))throw new InvalidOperationException("Invalid native physical drive state.");
        return new(entity,joint,value->TargetRotation,value->Strength,value->Stiffness,value->Damping,value->MaxTorque);
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads one committed joint drive. A known joint without an authored drive
    /// returns null; unknown joints are errors. Staged writes remain invisible.</summary>
    public PhysicalRigDriveState? GetPhysicalRigDrive(EntityId entity,string joint)
    {
        RequireFeature(GameplayRequiredFeatures.PhysicalRigDrive,nameof(IPhysicalRigDriveGame));var api=PhysicalRigDriveServiceAbi.Validate(services);
        if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));PhysicalRigServiceAbi.Name(joint);
        byte* name=stackalloc byte[64];for(int i=0;i<joint.Length;++i)name[i]=(byte)joint[i];
        NativePhysicalRigDriveRequestV1 request=new(){Version=1,Bytes=40,Entity=entity,Joint=name,JointBytes=(uint)joint.Length};
        NativePhysicalRigDriveStateV1 result=new(){Version=1,Bytes=168};NativeError error=default;
        Check(api->Get(services->Context,&request,&result,&error),&error);return PhysicalRigDriveServiceAbi.Decode(&result,entity,joint);
    }
    /// <summary>Stages an ordered drive write against immutable authored gains. Zero
    /// strength disables this joint's drive; it does not restore animation ownership.</summary>
    public void SetPhysicalRigDrive(in PhysicalRigDriveCommand command)
    {
        RequireFeature(GameplayRequiredFeatures.PhysicalRigDrive,nameof(IPhysicalRigDriveGame));var api=PhysicalRigDriveServiceAbi.Validate(services);PhysicalRigDriveServiceAbi.Input(command);
        byte* name=stackalloc byte[64];for(int i=0;i<command.Joint.Length;++i)name[i]=(byte)command.Joint[i];
        NativePhysicalRigDriveCommandV1 native=new(){Version=1,Bytes=80,Entity=command.Entity,Joint=name,JointBytes=(uint)command.Joint.Length,TargetRotation=command.TargetRotation,Strength=command.Strength};
        NativeError error=default;Check(api->Set(services->Context,&native,&error),&error);
    }
    public void SetPhysicalRigDrive(EntityId entity,string joint,Quaterniond targetRotation,double strength)=>SetPhysicalRigDrive(new(entity,joint,targetRotation,strength));
}
