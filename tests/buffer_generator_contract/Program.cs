// SPDX-License-Identifier: Apache-2.0
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;
using System.Security.Cryptography;
if(args.Length is <3 or >4)throw new ArgumentException("SDK, generator, output required");
var output=Path.GetFullPath(args[2]);Directory.CreateDirectory(output);
var sdk=Path.GetFullPath(args[0]);var generator=Path.GetFullPath(args[1]);
var assembly=AssemblyLoadContext.Default.LoadFromAssemblyPath(generator);
var type=assembly.GetType("Poima.Components.Generator.ComponentGenerator")!;
var refs=((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator).Select(p=>MetadataReference.CreateFromFile(p)).ToList();refs.Add(MetadataReference.CreateFromFile(sdk));
const string component="[GameplayComponent(\"11111111111111111111111111111111\")] public partial struct Item { [GameplayField(\"00000000000000000000000000000001\")] public Slots Values; }";
string Buffer(string args,string body="",string declaration="public partial struct Slots")=>$"using Poima; [GameplayBuffer({args})] {declaration} {{{body}}} {component}";
var cases=new Dictionary<string,string>{
 ["capacity_zero"]=Buffer("typeof(int),0"),["capacity_32"]=Buffer("typeof(int),32"),["unsupported_string"]=Buffer("typeof(string),2"),
 ["managed_wrapper_field"]=Buffer("typeof(int),2","public string Hidden;"),["scalar_wrapper_field"]=Buffer("typeof(int),2","public int Hidden;"),
 ["user_method"]=Buffer("typeof(int),2","public void Clear(){}"),["nonpartial"]=Buffer("typeof(int),2","","public struct Slots"),
 ["readonly"]=Buffer("typeof(int),2","","public readonly partial struct Slots"),["internal"]=Buffer("typeof(int),2","","internal partial struct Slots"),
 ["explicit_layout"]="using Poima; using System.Runtime.InteropServices; [StructLayout(LayoutKind.Explicit)] [GameplayBuffer(typeof(int),2)] public partial struct Slots {} "+component,
 ["managed_component_field"]=Buffer("typeof(int),2").Replace("public Slots Values;","public Slots Values; [GameplayField(\"00000000000000000000000000000002\")] public string Hidden;"),
 ["wire_over_512"]=Buffer("typeof(int),31").Replace("public Slots Values;","public Slots Values; [GameplayField(\"00000000000000000000000000000002\")] public int Extra;"),
 ["nonempty_default"]=Buffer("typeof(int),2").Replace("GameplayField(\"00000000000000000000000000000001\")","GameplayField(\"00000000000000000000000000000001\",Default=\"[1]\")"),
 ["foreign_unannotated_unmanaged_wrapper"]="using Poima; public struct Slots{public int Count;} "+component
};
var results=new List<object>();
string Run(string name,string source,bool valid)
{
 var parse=new CSharpParseOptions(LanguageVersion.CSharp14);
 var c=CSharpCompilation.Create("Fixture",[CSharpSyntaxTree.ParseText(source,parse)],refs,new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
 var instance=(IIncrementalGenerator)Activator.CreateInstance(type)!;
 GeneratorDriver driver=CSharpGeneratorDriver.Create([instance.AsSourceGenerator()],parseOptions:parse);
 driver=driver.RunGeneratorsAndUpdateCompilation(c,out var updated,out var diagnostics);
 var errors=diagnostics.Where(d=>d.Severity==DiagnosticSeverity.Error).ToArray();
 if(valid ? errors.Length!=0 : !errors.Any(d=>d.Id=="POIMA001"))throw new Exception(name+": unexpected generator diagnostics "+string.Join('\n',errors.Select(x=>x.ToString())));
 if(valid){using var image=new MemoryStream();var emitted=updated.Emit(image);if(!emitted.Success)throw new Exception(name+": "+string.Join('\n',emitted.Diagnostics));}
 var generated=string.Join('\n',driver.GetRunResult().GeneratedTrees.Select(t=>t.ToString()));
 File.WriteAllText(Path.Combine(output,name+".source.txt"),source);File.WriteAllText(Path.Combine(output,name+".generated.txt"),generated);
 results.Add(new{name,valid,diagnostics=errors.Select(x=>x.ToString()).ToArray()});return generated;
}
foreach(var entry in cases)Run(entry.Key,entry.Value,false);
Run("good_buffer",Buffer("typeof(int),2"),true);
var scalar="using Poima; [GameplayComponent(\"55555555555555555555555555555555\")] public partial struct Scalar { [GameplayField(\"00000000000000000000000000000001\",Default=\"7\")] public int Value;}";
Run("scalar_only",scalar,true);
var golden="using Poima; [GameplayBuffer(typeof(EntityId),2)] public partial struct Slots {} [GameplayComponent(\"00000000000000000000000000000010\")] public partial struct Golden {[GameplayField(\"00000000000000000000000000000001\",Default=\"-7\")] public int Marker; [GameplayField(\"00000000000000000000000000000002\")] public Slots Values;}";
if(args.Length==4)Run("retained_scalar_fixture","global using System;\n"+File.ReadAllText(args[3]),true);
var text=Run("native_golden",golden,true);if(!text.Contains("56a87fdbbe3be40db7c56af10667f52e76d4311060b5f4a7d8d155ba09f4435f"))throw new Exception("Native golden fingerprint differs");
File.WriteAllText(Path.Combine(output,"evidence.json"),JsonSerializer.Serialize(new{passed=true,negative_cases=cases.Count,positive_cases=args.Length==4?4:3,native_golden_match=true,sdk_sha256=Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(sdk))),generator_sha256=Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(generator))),results},new JsonSerializerOptions{WriteIndented=true}));
Console.WriteLine($"PASS: actual generator {cases.Count} negative diagnostics, 3 compiled positives and independent native fingerprint golden.");
