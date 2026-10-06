// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Poima;
using Poima.Build;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)]unsafe struct Call
{
    public uint Version,Operation;public ulong Handle;public byte* Text,State;
    public uint StateBytes,InputCount;public void* Services,Inputs;public ulong Tick;
    public byte* Output;public uint OutputCapacity,Reserved;
}
unsafe class Program
{
    const string Id="00000000000000000000000000000001";
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static Type State(int? revision,params (string Name,Type Type,string? Id,string? Default,string? Label,string? Unit)[] fields)
    {
        var assembly=AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("MetadataCase"+Guid.NewGuid().ToString("N")),AssemblyBuilderAccess.RunAndCollect);
        var type=assembly.DefineDynamicModule("State").DefineType("State",TypeAttributes.Public|TypeAttributes.Sealed|TypeAttributes.SequentialLayout,typeof(ValueType));
        if(revision is {} value)type.SetCustomAttribute(new CustomAttributeBuilder(typeof(GameplayPersistenceAttribute).GetConstructor([typeof(int)])!,[value]));
        foreach(var field in fields) {
            var member=type.DefineField(field.Name,field.Type,FieldAttributes.Public);
            if(field.Id is not null) {
                var properties=new List<PropertyInfo>();var values=new List<object>();
                foreach(var pair in new[]{("Default",field.Default),("Name",field.Label),("Unit",field.Unit)})if(pair.Item2 is not null){properties.Add(typeof(GameplayFieldAttribute).GetProperty(pair.Item1)!);values.Add(pair.Item2);}
                member.SetCustomAttribute(new CustomAttributeBuilder(typeof(GameplayFieldAttribute).GetConstructor([typeof(string)])!,[field.Id],properties.ToArray(),values.ToArray()));
            }
        }
        return type.CreateType()!;
    }
    static JsonObject Bridge(Type game)
    {
        byte* output=stackalloc byte[65536];Call call=new(){Version=1,Operation=1,Output=output,OutputCapacity=65536};
        var request=Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new{assembly=Assembly.GetExecutingAssembly().Location,type=game.FullName})+"\0");
        fixed(byte* text=request){call.Text=text;Check(Entry.Invoke((nint)(&call),80)==0,Marshal.PtrToStringUTF8((nint)output)!);}
        var manifest=JsonNode.Parse(Marshal.PtrToStringUTF8((nint)output)!)!.AsObject();call.Handle=manifest["handle"]!.GetValue<ulong>();
        call.Operation=4;Check(Entry.Invoke((nint)(&call),80)==0,"Release failed");
        manifest.Remove("handle");manifest.Remove("assembly_sha256");return manifest;
    }
    static string Hash(string file)=>Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(file)));
    static int Main(string[] args)
    {
        try {
            Check(args.Length==3,"Usage: DOTNET GENERATOR_DLL OUTPUT_DIRECTORY");Check(sizeof(Call)==80,"Independent call layout mismatch");
            var output=Path.GetFullPath(args[2]);Directory.CreateDirectory(output);var checks=new List<string>();
            var persistent=PersistenceMetadata.Read(typeof(PersistentState))!.Value;
            const string expected="""
                {"format":"poima.gameplay-persistence","version":1,"revision":9,"fields":[
                {"id":"00000000000000000000000000000001","name":"Count","kind":"int32","default":-2147483648},
                {"id":"00000000000000000000000000000002","name":"Amount","kind":"float64","default":1.25},
                {"id":"00000000000000000000000000000003","name":"Total","kind":"int64","default":"-9223372036854775808"},
                {"id":"00000000000000000000000000000004","name":"Target","kind":"entity","default":"00000000000000000000000000000000"},
                {"id":"00000000000000000000000000000005","name":"Rate","kind":"float32","default":0}]}
                """;
            Check(JsonNode.DeepEquals(JsonNode.Parse(expected),JsonNode.Parse(persistent.GetRawText())),"Independent expected persistent metadata differs");
            Check(!persistent.GetRawText().Contains("-0"),"Negative zero was not normalized");
            var zero=PersistenceMetadata.Read(typeof(ZeroState))!.Value;
            Check(zero.GetProperty("revision").GetInt32()==int.MaxValue,"Maximum revision changed");
            var defaults=zero.GetProperty("fields").EnumerateArray().Select(f=>f.GetProperty("default").GetRawText()).ToArray();
            Check(defaults.SequenceEqual(new[]{"0","\"0\"","0","0","\"00000000000000000000000000000000\""}),"Omitted literal defaults differ from typed zero");
            Check(PersistenceMetadata.Read(typeof(LegacyState)) is null,"Non-opt-in annotations changed legacy schema");
            checks.Add("All scalar defaults, int bounds, typed zero, positive floating zero, ID ordering and maximum revision match independent expected JSON; unrelated throwing attribute constructors never run");
            int negatives=0;
            void Reject(Type state){bool rejected=false;try{PersistenceMetadata.Read(state);}catch(ArgumentException){rejected=true;}catch(FormatException){rejected=true;}catch(OverflowException){rejected=true;}Check(rejected,"Invalid persistence metadata accepted");++negatives;}
            foreach(int revision in new[]{0,-1})Reject(State(revision,("Value",typeof(int),Id,null,null,null)));
            foreach(string? id in new string?[]{null,"",new string('0',32),"ABCDEF0123456789abcdef0123456789ab","short"})Reject(State(1,("Value",typeof(int),id,null,null,null)));
            Reject(State(1,("A",typeof(int),Id,null,null,null),("B",typeof(long),Id,null,null,null)));
            Reject(State(1,("Value",typeof(int),Id,null,"Display label",null)));
            Reject(State(1,("Value",typeof(int),Id,null,null,"m")));
            foreach(var item in new[]{(typeof(int),"2147483648"),(typeof(int),"+1"),(typeof(int),"01"),(typeof(long),"9223372036854775808"),(typeof(long),"-0"),(typeof(float),"NaN"),(typeof(float),"1e100"),(typeof(double),"Infinity"),(typeof(EntityId),Id),(typeof(EntityId),"null")})Reject(State(1,("Value",item.Item1,Id,item.Item2,null,null)));
            Reject(State(1,("Value",typeof(bool),Id,null,null,null)));
            checks.Add($"{negatives} invalid revision, missing/duplicate/malformed ID, renamed label/unit, overflow/noncanonical integer, nonfinite float, non-null entity and unsupported kind cases rejected");
            foreach(var game in new[]{typeof(PersistentGame),typeof(ZeroGame),typeof(LegacyGame)}) {
                var bridge=Bridge(game);var directory=Path.Combine(output,game.Name);
                var start=new ProcessStartInfo(args[0]){RedirectStandardOutput=true,RedirectStandardError=true,UseShellExecute=false};
                foreach(var arg in new[]{Path.GetFullPath(args[1]),Assembly.GetExecutingAssembly().Location,game.FullName!,directory})start.ArgumentList.Add(arg);
                using var process=Process.Start(start)!;var stdout=process.StandardOutput.ReadToEndAsync();var stderr=process.StandardError.ReadToEndAsync();
                Check(process.WaitForExit(60000),"Generator timeout");Task.WaitAll(stdout,stderr);File.WriteAllText(directory+".log",stdout.Result+stderr.Result);
                Check(process.ExitCode==0,$"Generator rejected {game.Name}: {stderr.Result}");
                var generated=JsonNode.Parse(File.ReadAllText(Path.Combine(directory,"schema.json")));
                Check(JsonNode.DeepEquals(bridge,generated),$"CoreCLR and native-generator schema disagree for {game.Name}");
                if(game==typeof(LegacyGame))Check(!bridge.ContainsKey("persistent") && bridge.Count==3,"Legacy schema shape changed");
                else Check(bridge.ContainsKey("persistent"),"Opt-in schema missing persistence");
            }
            checks.Add("Real bridge and NativeGame generator emit identical complete schemas for opt-in literal/default-zero and legacy states; throwing Initialize is never called");
            File.WriteAllText(Path.Combine(output,"evidence.json"),JsonSerializer.Serialize(new{passed=true,checks,platform=RuntimeInformation.OSDescription,
                bridge_sha256=Hash(typeof(Entry).Assembly.Location),sdk_sha256=Hash(typeof(Game<>).Assembly.Location),generator_sha256=Hash(args[1]),fixture_sha256=Hash(Assembly.GetExecutingAssembly().Location)},new JsonSerializerOptions{WriteIndented=true})+"\n");
            Console.WriteLine(string.Join("\n",checks));return 0;
        }catch(Exception error){Console.Error.WriteLine(error);return 1;}
    }
}
