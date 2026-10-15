// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using Poima;
namespace Poima.Tests;

[GameplayBuffer(typeof(EntityId),4)] public partial struct ReadQueueItems {}
[GameplayBuffer(typeof(int),2)] public partial struct ReadInts {}
[GameplayBuffer(typeof(long),2)] public partial struct ReadLongs {}
[GameplayBuffer(typeof(float),2)] public partial struct ReadFloats {}
[GameplayBuffer(typeof(double),2)] public partial struct ReadDoubles {}
[GameplayBuffer(typeof(EntityId),2)] public partial struct ReadEntities {}

[GameplayComponent("cc150000000000000000000000000001")]
public partial struct ReadQueue
{
    [GameplayField("cc150000000000000000000000000011")] public ReadQueueItems Items;
}

// Five scalar cells and five capacity-two arrays: 320 wire bytes.
[GameplayComponent("cc150000000000000000000000000002")]
public partial struct ReadUnit
{
    [GameplayField("cc150000000000000000000000000021",Default="0")] public int Count;
    [GameplayField("cc150000000000000000000000000022",Default="0")] public long Exact;
    [GameplayField("cc150000000000000000000000000023",Default="0")] public float Single;
    [GameplayField("cc150000000000000000000000000024",Default="0")] public double Precise;
    [GameplayField("cc150000000000000000000000000025")] public EntityId Owner;
    [GameplayField("cc150000000000000000000000000026")] public ReadInts Ints;
    [GameplayField("cc150000000000000000000000000027")] public ReadLongs Longs;
    [GameplayField("cc150000000000000000000000000028")] public ReadFloats Floats;
    [GameplayField("cc150000000000000000000000000029")] public ReadDoubles Doubles;
    [GameplayField("cc15000000000000000000000000002a")] public ReadEntities Entities;
}

[StructLayout(LayoutKind.Sequential)]
public struct ControlReadState
{
    public int Controls,Ticks,ReadChecks,QueryPages,QueryMembers,MissingFalse,DeadFalse,
        CopyChecks,ObservedFirstCount,Sum,Refusals;
}

[GameModule("poima.test.control-component-reads")]
public sealed class ControlReadGame : Game<ControlReadState>
{
    static readonly EntityId Queue=new(0,1),First=new(0,2),Spare=new(0,10),Unknown=new(0,65535);
    static readonly UiId Label=new(0,100);
    static void Check(bool value,string message)
    { if(!value)throw new InvalidOperationException(message); }
    static void Known(in ReadUnit value)
    {
        Check(value.Exact==9007199254740993L && value.Single==0.5f && value.Precise==1e-200 && value.Owner==Queue,
            "Control scalar read differs.");
        Check(value.Ints.Count==2 && value.Ints[0]==int.MinValue && value.Ints[1]==int.MaxValue,"Control int32 array differs.");
        Check(value.Longs.Count==2 && value.Longs[0]==long.MinValue && value.Longs[1]==long.MaxValue,"Control int64 array differs.");
        Check(value.Floats.Count==2 && value.Floats[0]==0.5f && value.Floats[1]==-65536f,"Control float32 array differs.");
        Check(value.Doubles.Count==2 && value.Doubles[0]==1e-200 && value.Doubles[1]==-1e200,"Control float64 array differs.");
        Check(value.Entities.Count==2 && value.Entities[0]==Queue && value.Entities[1]==default,"Control entity array differs.");
    }
    static void Read(ref ControlReadState state,ControlContext context)
    {
        var queue=context.Get<ReadQueue>(Queue);
        Check(queue.Items.Count==4,"Control did not read a full published native FIFO.");
        int sum=0;
        for(int i=0;i<4;++i)
        {
            Check(queue.Items[i]==new EntityId(0,(ulong)(2+i*2)) && context.IsAlive(queue.Items[i]),"Control FIFO identity differs.");
            Check(context.TryGet<ReadUnit>(queue.Items[i],out var value),"Published work component absent.");
            Known(in value);sum=checked(sum+value.Count);
        }
        var copy=context.Get<ReadUnit>(First);
        state.ObservedFirstCount=copy.Count;state.Sum=sum;
        copy.Count=-999;copy.Entities[0]=default;copy.Longs.Clear();copy.Ints.RemoveAt(0);
        var again=context.Get<ReadUnit>(First);Known(in again);
        Check(again.Count==state.ObservedFirstCount,"Control local copy changed native scalar storage.");
        ++state.CopyChecks;
        Span<EntityId> page=stackalloc EntityId[2];EntityId after=default;int members=0,pages=0,written;
        while((written=context.Query<ReadUnit>(page,after))!=0)
        {
            ++pages;Check(written==2 && pages<=2,"Control query page length differs.");
            for(int i=0;i<written;++i)
            {
                Check(page[i]==new EntityId(0,(ulong)(2+members*2)),"Control query order/exclusive cursor differs.");
                after=page[i];++members;
            }
        }
        ++pages;Check(members==4 && pages==3,"Control query lost complete membership or empty tail.");
        Check(context.Query<ReadUnit>(page,new EntityId(0,3))==2 && page[0]==new EntityId(0,4)
            && page[1]==new EntityId(0,6),"Nonmember exclusive query cursor differs.");
        state.QueryPages=pages;state.QueryMembers=members;++state.ReadChecks;
        context.SetUi(Label,text:$"Read {state.ReadChecks}: {members} units, first {state.ObservedFirstCount}, sum {sum}");
    }
    static void StageFailure(ref ControlReadState state,ControlContext context)
    {
        state.ReadChecks=999;context.SetUi(Label,text:"This failed callback must not publish.");
        context.RequestSave("control-read-rollback");
    }
    public override void Initialize(ref ControlReadState state) { state=default; }
    public override void Tick(ref ControlReadState state,GameContext context) { ++state.Ticks; }
    public override void Control(ref ControlReadState state,ControlContext context)
    {
        ++state.Controls;
        switch(context.Action)
        {
            case "read":Read(ref state,context);break;
            case "refuse":
                Check(context.Get<ReadQueue>(Queue).Items.Count==4,"Refusal requires actual full queue.");
                ++state.Refusals;context.SetUi(Label,text:"Full queue refused without a tick.");break;
            case "missing":
                Check(context.IsAlive(Spare),"Expected existing spare entity.");
                bool present=context.TryGet<ReadUnit>(Spare,out var absent);
                Check(!present,"Existing absent component distinction differs.");
                Check(absent.Count==0 && absent.Entities.Count==0 && absent.Longs.Count==0,"TryGet false did not return default value.");
                bool missingThrew=false;
                try { context.Get<ReadUnit>(Spare); }catch(InvalidOperationException error) { missingThrew=error.Message=="Entity does not have the requested component."; }
                Check(missingThrew,"Get of existing entity without component did not throw.");
                state.MissingFalse=1;
                Check(!context.IsAlive(default) && !context.IsAlive(Unknown),"Unknown/null liveness differs.");
                bool unknownThrew=false;
                try { context.TryGet<ReadUnit>(Unknown,out _); }catch(InvalidOperationException error) { unknownThrew=error.Message.Contains("Runtime entity does not exist.",StringComparison.Ordinal); }
                Check(unknownThrew,"TryGet of unknown entity incorrectly returned false.");
                state.DeadFalse=1;context.SetUi(Label,text:"Missing component and unknown entity distinguished.");break;
            case "fail-unknown":
                StageFailure(ref state,context);context.Get<ReadUnit>(Unknown);throw new InvalidOperationException("Unreachable unknown read.");
            case "fail-missing":
                StageFailure(ref state,context);context.Get<ReadUnit>(Spare);throw new InvalidOperationException("Unreachable missing read.");
            case "fail-empty-query":
                StageFailure(ref state,context);context.Query<ReadUnit>(Span<EntityId>.Empty);throw new InvalidOperationException("Unreachable invalid query.");
            default:throw new InvalidOperationException("Unknown control read fixture action.");
        }
    }
}
