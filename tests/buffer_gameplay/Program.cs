// SPDX-License-Identifier: Apache-2.0
using Poima;
using System.Buffers.Binary;
using System.Reflection;
using System.Text.Json;

[GameplayBuffer(typeof(int),2)] public partial struct Ints {}
[GameplayBuffer(typeof(long),2)] public partial struct Longs {}
[GameplayBuffer(typeof(float),2)] public partial struct Floats {}
[GameplayBuffer(typeof(double),2)] public partial struct Doubles {}
[GameplayBuffer(typeof(EntityId),2)] public partial struct Entities {}
[GameplayBuffer(typeof(EntityId),31)] public partial struct FullBuffer {}
[GameplayComponent("33333333333333333333333333333333")]
public partial struct Mixed
{
    [GameplayField("00000000000000000000000000000006")] public Entities Links;
    [GameplayField("00000000000000000000000000000005")] public Doubles Precise;
    [GameplayField("00000000000000000000000000000004")] public Floats Weights;
    [GameplayField("00000000000000000000000000000003")] public Longs Totals;
    [GameplayField("00000000000000000000000000000002",Default="7")] public int Marker;
    [GameplayField("00000000000000000000000000000001")] public Ints Counts;
}
[GameplayComponent("44444444444444444444444444444444")]
public partial struct Maximum { [GameplayField("00000000000000000000000000000001")] public FullBuffer Links; }
[GameplayComponent("55555555555555555555555555555555")]
public partial struct Scalar { [GameplayField("00000000000000000000000000000001",Default="7")] public int Value; }
public static class Program
{
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    static byte[] Encode<T>(in T value) where T:unmanaged,IGameplayComponent<T> {var bytes=new byte[T.Descriptor.Bytes];T.Encode(in value,bytes);return bytes;}
    static T Decode<T>(byte[] bytes) where T:unmanaged,IGameplayComponent<T> =>T.Decode(bytes);
    static void Reject(byte[] wire,Action<byte[]> mutate){var bytes=(byte[])wire.Clone();mutate(bytes);bool failed=false;try{Decode<Mixed>(bytes);}catch(ArgumentException){failed=true;}Check(failed,"Malformed collection wire accepted");}
    public static void Main(string[] args)
    {
        Mixed value=default;value.Marker=77;
        Check(value.Counts.TryAdd(int.MinValue) && value.Counts.TryAdd(int.MaxValue) && !value.Counts.TryAdd(7),"Int/full append failed");
        value.Totals.TryAdd(long.MinValue);value.Totals.TryAdd(long.MaxValue);
        value.Weights.TryAdd(-0.0f);value.Weights.TryAdd(1.25f);value.Precise.TryAdd(-0.0);value.Precise.TryAdd(-2.5);
        value.Links.TryAdd(new(ulong.MaxValue,ulong.MaxValue));value.Links.TryAdd(default);
        var wire=Encode(in value);Check(wire.Length==256,"Mixed offsets/payload size differ");var restored=Decode<Mixed>(wire);
        Check(restored.Marker==77 && restored.Counts[0]==int.MinValue && restored.Counts[1]==int.MaxValue && restored.Totals[0]==long.MinValue && restored.Totals[1]==long.MaxValue,"Integer/scalar values differ");
        Check(restored.Weights[1]==1.25f && restored.Precise[1]==-2.5 && restored.Links[0]==new EntityId(ulong.MaxValue,ulong.MaxValue),"Floating/entity values differ");
        Check(BitConverter.SingleToInt32Bits(restored.Weights[0])==0 && BitConverter.DoubleToInt64Bits(restored.Precise[0])==0,"Signed zero not normalized");
        restored.Counts.RemoveAt(0);Check(restored.Counts.Count==1 && restored.Counts[0]==int.MaxValue,"Removal order differs");
        Check(restored.Counts.TryAdd(12),"Append after removal failed");restored.Counts[1]=13;Check(restored.Counts[1]==13,"Indexed setter failed");
        bool rejected=false;try{restored.Counts.RemoveAt(-1);}catch(ArgumentOutOfRangeException){rejected=true;}Check(rejected && restored.Counts.Count==2,"Invalid removal mutated buffer");
        rejected=false;try{restored.Weights[0]=float.NaN;}catch(ArgumentException){rejected=true;}Check(rejected && restored.Weights[0]==0,"Invalid float mutated buffer");
        Floats empty=default;rejected=false;try{empty.TryAdd(float.PositiveInfinity);}catch(ArgumentException){rejected=true;}Check(rejected && empty.Count==0,"Invalid append mutated length");
        foreach(var offset in new[]{0,64,112,160,208})
        {
            Reject(wire,b=>BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(offset),3));
            Reject(wire,b=>b[offset+4]=1);
            Reject(wire,b=>BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(offset),0)); // Existing active values become forbidden inactive bytes.
        }
        Reject(wire,b=>b[20]=1);Reject(wire,b=>b[64+24]=1);
        Reject(wire,b=>BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(112+16),int.MinValue));
        Reject(wire,b=>BinaryPrimitives.WriteSingleLittleEndian(b.AsSpan(112+16),float.NaN));
        Reject(wire,b=>BinaryPrimitives.WriteDoubleLittleEndian(b.AsSpan(160+16),double.PositiveInfinity));
        restored=default;wire=Encode(in restored);Check(wire.All(b=>b==0),"Empty collection encoding leaked data");
        Maximum max=default;for(ulong i=0;i<31;++i)Check(max.Links.TryAdd(new(0,i)),"Maximum buffer append failed");Check(!max.Links.TryAdd(default) && Encode(in max).Length==512,"512-byte boundary differs");max.Links.Clear();Check(max.Links.Count==0 && Encode(in max).All(b=>b==0),"Clear leaked backing state");
        // Unmanaged state can be corrupted by unsafe callers; checked APIs must
        // reject before changing even the raw storage representation.
        foreach(int invalidLength in new[]{-1,3,int.MaxValue})for(int operation=0;operation<5;++operation)
        {
            Ints corrupt=default;corrupt.TryAdd(11);corrupt.TryAdd(22);
            System.Runtime.CompilerServices.Unsafe.As<Ints,int>(ref corrupt)=invalidLength;
            var before=System.Runtime.InteropServices.MemoryMarshal.AsBytes(System.Runtime.InteropServices.MemoryMarshal.CreateReadOnlySpan(ref corrupt,1)).ToArray();
            rejected=false;
            try {switch(operation){case 0:_=corrupt.Count;break;case 1:_=corrupt[0];break;case 2:corrupt[0]=5;break;case 3:corrupt.TryAdd(5);break;case 4:corrupt.RemoveAt(0);break;}}
            catch(ArgumentException){rejected=true;}
            Check(rejected && before.AsSpan().SequenceEqual(System.Runtime.InteropServices.MemoryMarshal.AsBytes(System.Runtime.InteropServices.MemoryMarshal.CreateReadOnlySpan(ref corrupt,1))),"Invalid internal length mutated state before rejection");
            corrupt.Clear();Check(corrupt.Count==0,"Clear could not repair corrupt state");
        }
        var metadata=Assembly.GetExecutingAssembly().GetCustomAttribute<GameplayComponentManifestAttribute>()!.Json;
        using var doc=JsonDocument.Parse(metadata);var schemas=doc.RootElement.GetProperty("schemas");
        Check(schemas[0].GetProperty("version").GetInt32()==2 && schemas[2].GetProperty("version").GetInt32()==1,"Scalar legacy schema version changed");
        if(args.Length==1)File.WriteAllText(args[0],metadata);
        Console.WriteLine("Generated buffers passed five scalar kinds, mixed offsets, extrema, fixed capacity, bounds, canonical wire validation and scalar schema1 preservation.");
    }
}
