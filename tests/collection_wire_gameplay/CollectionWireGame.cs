// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using Poima;
namespace Poima.Tests;

[GameplayBuffer(typeof(int),2)] public partial struct WireInts {}
[GameplayBuffer(typeof(long),2)] public partial struct WireLongs {}
[GameplayBuffer(typeof(float),2)] public partial struct WireFloats {}
[GameplayBuffer(typeof(double),2)] public partial struct WireDoubles {}
[GameplayBuffer(typeof(EntityId),2)] public partial struct WireEntities {}

// Five (16-byte header + two 16-byte cells) arrays and one scalar: 256 bytes.
[GameplayComponent("7c0e14507c0e14507c0e14507c0e1450")]
public partial struct CollectionWire
{
    [GameplayField("00000000000000000000000000014501")] public WireInts IntValues;
    [GameplayField("00000000000000000000000000014502")] public WireLongs LongValues;
    [GameplayField("00000000000000000000000000014503")] public WireFloats FloatValues;
    [GameplayField("00000000000000000000000000014504")] public WireDoubles DoubleValues;
    [GameplayField("00000000000000000000000000014505")] public WireEntities EntityValues;
    [GameplayField("00000000000000000000000000014506",Default="0")] public int Revision;
}

[StructLayout(LayoutKind.Sequential)]
public struct CollectionWireState
{
    public int Mode,Ticks,ObservedCount,ReadBeforeWrite;
}

[GameModule("poima.test.collection-wire")]
public sealed class CollectionWireGame : Game<CollectionWireState>
{
    static readonly EntityId Owner=new(0,1),Other=new(0,2);
    static void Check(bool condition,string message)
    {
        if(!condition)throw new InvalidOperationException(message);
    }
    static bool Same(in CollectionWire a,in CollectionWire b)
    {
        if(a.Revision!=b.Revision || a.IntValues.Count!=b.IntValues.Count ||
           a.LongValues.Count!=b.LongValues.Count || a.FloatValues.Count!=b.FloatValues.Count ||
           a.DoubleValues.Count!=b.DoubleValues.Count || a.EntityValues.Count!=b.EntityValues.Count)return false;
        for(int i=0;i<a.IntValues.Count;++i)if(a.IntValues[i]!=b.IntValues[i])return false;
        for(int i=0;i<a.LongValues.Count;++i)if(a.LongValues[i]!=b.LongValues[i])return false;
        for(int i=0;i<a.FloatValues.Count;++i)if(a.FloatValues[i]!=b.FloatValues[i])return false;
        for(int i=0;i<a.DoubleValues.Count;++i)if(a.DoubleValues[i]!=b.DoubleValues[i])return false;
        for(int i=0;i<a.EntityValues.Count;++i)if(a.EntityValues[i]!=b.EntityValues[i])return false;
        return true;
    }
    static void Known(in CollectionWire value)
    {
        Check(value.IntValues.Count==2 && value.IntValues[0]==int.MinValue && value.IntValues[1]==int.MaxValue,"Published int32 array differs.");
        Check(value.LongValues.Count==2 && value.LongValues[0]==long.MinValue && value.LongValues[1]==long.MaxValue,"Published int64 array differs.");
        Check(value.FloatValues.Count==2 && value.FloatValues[0]==0.5f && value.FloatValues[1]==-65536f,"Published float32 array differs.");
        Check(value.DoubleValues.Count==2 && value.DoubleValues[0]==1e-200 && value.DoubleValues[1]==-1e200,"Published float64 array differs.");
        Check(value.EntityValues.Count==2 && value.EntityValues[0]==Owner && value.EntityValues[1]==Other,"Published entity array differs.");
    }
    public override void Initialize(ref CollectionWireState state) { state=default; }
    public override void Tick(ref CollectionWireState state,GameContext context)
    {
        ++state.Ticks;
        Span<EntityId> page=stackalloc EntityId[1];EntityId after=default;int count;
        state.ObservedCount=0;
        while((count=context.Query<CollectionWire>(page,after))!=0)
        {
            Check(count==1 && (page[0].High>after.High || (page[0].High==after.High && page[0].Low>after.Low)),"Collection query order differs.");
            after=page[0];++state.ObservedCount;
            var member=context.Get<CollectionWire>(page[0]);
            for(int i=0;i<member.FloatValues.Count;++i)Check(float.IsFinite(member.FloatValues[i]),"Nonfinite published float32.");
            for(int i=0;i<member.DoubleValues.Count;++i)Check(double.IsFinite(member.DoubleValues[i]),"Nonfinite published float64.");
            for(int i=0;i<member.EntityValues.Count;++i)Check(member.EntityValues[i]==default || context.IsAlive(member.EntityValues[i]),"Published collection entity is not alive.");
        }
        if(state.Mode==0)return;
        Check(state.Mode is >=1 and <=3,"Unknown collection wire mode.");
        var previous=context.Get<CollectionWire>(Owner);
        if(previous.Revision>0)Known(in previous); // A later Tick reads the last published write.
        var next=default(CollectionWire);
        Check(next.IntValues.TryAdd(int.MinValue) && next.IntValues.TryAdd(int.MaxValue),"Cannot fill int32 array.");
        Check(next.LongValues.TryAdd(long.MinValue) && next.LongValues.TryAdd(long.MaxValue),"Cannot fill int64 array.");
        Check(next.FloatValues.TryAdd(0.5f) && next.FloatValues.TryAdd(-65536f),"Cannot fill float32 array.");
        Check(next.DoubleValues.TryAdd(1e-200) && next.DoubleValues.TryAdd(-1e200),"Cannot fill float64 array.");
        Check(next.EntityValues.TryAdd(Owner) && next.EntityValues.TryAdd(Other),"Cannot fill entity array.");
        next.Revision=checked(previous.Revision+1);
        if(state.Mode==3)next.EntityValues[1]=new(ulong.MaxValue,ulong.MaxValue);
        context.Set(Owner,in next);
        var published=context.Get<CollectionWire>(Owner);
        state.ReadBeforeWrite=Same(in previous,in published)?1:0;
        Check(state.ReadBeforeWrite==1,"Queued collection write leaked into reads.");
        if(state.Mode==2)context.Set(Owner,in next); // Duplicate staged target rejects the complete Tick.
    }
}
