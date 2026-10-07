// SPDX-License-Identifier: Apache-2.0
using System.Buffers.Binary;
using System.Collections.Immutable;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Poima.Components.Generator;
[Generator]
public sealed partial class ComponentGenerator : IIncrementalGenerator
{
    private static readonly DiagnosticDescriptor Invalid=new("POIMA001","Invalid gameplay component","{0}","Poima",DiagnosticSeverity.Error,true);
    private sealed record Field(string Id,string Name,string Member,string Kind,string Unit,object Initial,byte[] Cell,BufferDefinition? Buffer=null) { public int Bytes=>Cell.Length; }
    private sealed record Definition(INamedTypeSymbol Type,string Id,string Name,Field[] Fields,string Fingerprint) { public int Version=>Fields.Any(f=>f.Buffer is not null)?2:1; public int Bytes=>Fields.Sum(f=>f.Bytes); }
    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        var types=context.SyntaxProvider.ForAttributeWithMetadataName("Poima.GameplayComponentAttribute",
            static (node,_)=>node is TypeDeclarationSyntax,static (item,_)=> (INamedTypeSymbol)item.TargetSymbol).Collect();
        var buffers=context.SyntaxProvider.ForAttributeWithMetadataName("Poima.GameplayBufferAttribute",
            static (node,_)=>node is TypeDeclarationSyntax,static (item,_)=>(INamedTypeSymbol)item.TargetSymbol).Collect();
        context.RegisterSourceOutput(types.Combine(buffers),static (context,pair)=>Emit(context,pair.Left,pair.Right));
    }
    private static void Emit(SourceProductionContext context,ImmutableArray<INamedTypeSymbol> types,ImmutableArray<INamedTypeSymbol> bufferTypes)
    {
        try
        {
            if(bufferTypes.Length>64)throw new ArgumentException("A game assembly supports at most 64 buffer declarations.");
            var buffers=new Dictionary<string,BufferDefinition>(StringComparer.Ordinal);
            foreach(var type in bufferTypes)
            {
                try { var buffer=ParseBuffer(type);buffers.Add(type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),buffer); }
                catch(Exception error) {context.ReportDiagnostic(Diagnostic.Create(Invalid,type.Locations.FirstOrDefault(),error.Message));return;}
            }
            foreach(var buffer in buffers.Values)context.AddSource("Poima.Buffer."+Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(buffer.Type.ToDisplayString())))+".g.cs",SourceText(BufferCodec(buffer)));
            if(types.Length==0)return;
            if(types.Length>64)throw new ArgumentException("A game assembly supports at most 64 component types.");
            var definitions=new List<Definition>();
            foreach(var type in types)
            {
                try { definitions.Add(Parse(type,buffers)); }
                catch(Exception error) { context.ReportDiagnostic(Diagnostic.Create(Invalid,type.Locations.FirstOrDefault(),error.Message));return; }
            }
            if(definitions.Select(d=>d.Id).Distinct(StringComparer.Ordinal).Count()!=definitions.Count)
                throw new ArgumentException("Duplicate persistent component ID.");
            definitions.Sort((a,b)=>StringComparer.Ordinal.Compare(a.Id,b.Id));
            var schemas=definitions.Select(d=>new {id=d.Id,name=d.Name,version=d.Version,fields=d.Fields.Select(FieldMetadata),fingerprint=d.Fingerprint});
            var manifest=JsonSerializer.Serialize(new {format="poima.components",version=1,schemas});
            if(Encoding.UTF8.GetByteCount(manifest)>512*1024)throw new ArgumentException("Component manifest exceeds 512 KiB.");
            context.AddSource("Poima.ComponentManifest.g.cs",SourceText("// Generated persistent component declarations.\n[assembly: global::Poima.GameplayComponentManifestAttribute("+Literal(manifest)+")]\n"));
            foreach(var definition in definitions)context.AddSource("Poima.Component."+definition.Id+".g.cs",SourceText(Codec(definition)));
        }
        catch(Exception error) { context.ReportDiagnostic(Diagnostic.Create(Invalid,Location.None,error.Message)); }
    }
    private static Microsoft.CodeAnalysis.Text.SourceText SourceText(string text)=>Microsoft.CodeAnalysis.Text.SourceText.From(text,Encoding.UTF8);
    private static string Literal(string text)=>SymbolDisplay.FormatLiteral(text,true);
    private static AttributeData Attribute(ISymbol symbol,string name)=>symbol.GetAttributes().Single(a=>a.AttributeClass?.ToDisplayString()==name);
    private static string? Named(AttributeData attribute,string key)=>attribute.NamedArguments.FirstOrDefault(p=>p.Key==key).Value.Value as string;
    private static string Id(AttributeData attribute)
    {
        var text=attribute.ConstructorArguments.Single().Value as string;
        if(text is null || text.Length!=32 || text.All(c=>c=='0') || text.Any(c=>!(c>='0'&&c<='9') && !(c>='a'&&c<='f')))
            throw new ArgumentException("Persistent IDs must be nonzero 32-character lowercase hexadecimal strings.");
        return text;
    }
    private static void Text(string value,int min,int max,string role)
    {
        if(Encoding.UTF8.GetByteCount(value)<min || Encoding.UTF8.GetByteCount(value)>max || value.Any(char.IsControl))throw new ArgumentException(role+" has invalid UTF-8 length or control characters.");
    }
    private static Definition Parse(INamedTypeSymbol type,IReadOnlyDictionary<string,BufferDefinition> buffers)
    {
        if(type.DeclaredAccessibility!=Accessibility.Public || type.ContainingType is not null || type.IsGenericType || type.IsReadOnly || type.IsRefLikeType || !type.IsUnmanagedType ||
            type.DeclaringSyntaxReferences.Any(r=>r.GetSyntax() is not StructDeclarationSyntax s || !s.Modifiers.Any(SyntaxKind.PartialKeyword)))
            throw new ArgumentException("Gameplay components must be public, top-level, nongeneric, unmanaged mutable partial structs.");
        if(type.GetAttributes().Any(a=>a.AttributeClass?.ToDisplayString()=="System.Runtime.InteropServices.StructLayoutAttribute" && a.ConstructorArguments.Length>0 && a.ConstructorArguments[0].Value is int layout && layout==2))
            throw new ArgumentException("Overlapping explicit struct layout is not supported for gameplay components.");
        var declaration=Attribute(type,"Poima.GameplayComponentAttribute");var id=Id(declaration);var name=Named(declaration,"Name")??type.Name;Text(name,1,64,"Component name");
        var fields=new List<Field>();
        foreach(var field in type.GetMembers().OfType<IFieldSymbol>().Where(f=>!f.IsStatic))
        {
            if(field.DeclaredAccessibility!=Accessibility.Public || field.IsReadOnly || field.IsFixedSizeBuffer || field.IsImplicitlyDeclared ||
                !field.GetAttributes().Any(a=>a.AttributeClass?.ToDisplayString()=="Poima.GameplayFieldAttribute"))
                throw new ArgumentException("Every instance field must be a public mutable GameplayField; auto-properties and hidden state are unsupported.");
            var attribute=Attribute(field,"Poima.GameplayFieldAttribute");var fieldId=Id(attribute);var label=Named(attribute,"Name")??field.Name;var unit=Named(attribute,"Unit")??"";
            Text(label,1,64,"Field name");Text(unit,0,24,"Field unit");
            if(buffers.TryGetValue(field.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),out var buffer))
            {
                if(Named(attribute,"Default") is { } initial && initial!="[]")throw new ArgumentException("Buffer defaults must be empty arrays (omit Default or use []).");
                fields.Add(new(fieldId,label,field.Name,"array",unit,Array.Empty<object>(),new byte[(buffer.Capacity+1)*16],buffer));continue;
            }
            var kind=ScalarKind(field.Type);
            var text=Named(attribute,"Default")??(kind=="entity"?new string('0',32):"0");var cell=new byte[16];object value;
            switch(kind)
            {
                case "int32":
                    var i=int.Parse(text,NumberStyles.AllowLeadingSign,CultureInfo.InvariantCulture);if(i.ToString(CultureInfo.InvariantCulture)!=text)throw new ArgumentException("Noncanonical integer default.");value=i;BinaryPrimitives.WriteInt32LittleEndian(cell,i);break;
                case "int64":
                    var l=long.Parse(text,NumberStyles.AllowLeadingSign,CultureInfo.InvariantCulture);if(l.ToString(CultureInfo.InvariantCulture)!=text)throw new ArgumentException("Noncanonical Int64 default.");value=text;BinaryPrimitives.WriteInt64LittleEndian(cell,l);break;
                case "float32":
                    var f=float.Parse(text,NumberStyles.Float,CultureInfo.InvariantCulture);if(!float.IsFinite(f))throw new ArgumentException("Default must be a finite float32.");if(f==0)f=0;value=f;BinaryPrimitives.WriteSingleLittleEndian(cell,f);break;
                case "float64":
                    var d=double.Parse(text,NumberStyles.Float,CultureInfo.InvariantCulture);if(!double.IsFinite(d))throw new ArgumentException("Default must be a finite float64.");if(d==0)d=0;value=d;BinaryPrimitives.WriteDoubleLittleEndian(cell,d);break;
                default:
                    if(text!=new string('0',32))throw new ArgumentException("Entity defaults must be the unset all-zero ID.");value=text;break;
            }
            fields.Add(new(fieldId,label,field.Name,kind,unit,value,cell));
        }
        if(fields.Count is <1 or >32)throw new ArgumentException("A component requires 1..32 instance fields.");
        if(fields.Select(f=>f.Id).Distinct(StringComparer.Ordinal).Count()!=fields.Count || fields.Select(f=>f.Name).Distinct(StringComparer.Ordinal).Count()!=fields.Count)
            throw new ArgumentException("Duplicate field ID or display name.");
        fields.Sort((a,b)=>StringComparer.Ordinal.Compare(a.Id,b.Id));
        if(fields.Sum(f=>f.Bytes)>512)throw new ArgumentException("Component wire payload exceeds 512 bytes including buffer capacities.");
        var version=fields.Any(f=>f.Buffer is not null)?2:1;
        var semantic=new StringBuilder("poima.component.v").Append(version).Append('\n').Append(id).Append('\n').Append(version).Append('\n');
        foreach(var field in fields)
        {
            semantic.Append(field.Id).Append(':').Append(field.Kind).Append(':');
            if(field.Buffer is { } buffer)semantic.Append(buffer.Kind).Append(':').Append(buffer.Capacity).Append(':');
            semantic.Append(Convert.ToHexStringLower(field.Cell)).Append('\n');
        }
        var fingerprint=Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(semantic.ToString())));
        return new(type,id,name,fields.ToArray(),fingerprint);
    }
    private static string Codec(Definition d)
    {
        var type=d.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);var b=new StringBuilder("// Generated native-owned component codec.\n#nullable enable\n");
        if(!d.Type.ContainingNamespace.IsGlobalNamespace)b.Append("namespace ").Append(d.Type.ContainingNamespace.ToDisplayString()).Append(";\n");
        b.Append("public partial struct @").Append(d.Type.Name).Append(" : global::Poima.IGameplayComponent<").Append(type).Append(">\n{\n");
        b.Append("    static global::Poima.GameplayComponentDescriptor global::Poima.IGameplayComponent<").Append(type).Append(">.Descriptor => new(new(0x").Append(d.Id[..16]).Append("UL,0x").Append(d.Id[16..]).Append("UL)");
        for(int i=0;i<4;++i)b.Append(",0x").Append(d.Fingerprint.Substring(i*16,16)).Append("UL");
        b.Append(',').Append(d.Bytes).Append("u);\n");
        b.Append("    static void global::Poima.IGameplayComponent<").Append(type).Append(">.Encode(in ").Append(type).Append(" value,global::System.Span<byte> bytes)\n    {\n");
        b.Append("        if(bytes.Length!=").Append(d.Bytes).Append(")throw new global::System.ArgumentException(\"Component wire size mismatch.\");\n        bytes.Clear();\n");
        for(int i=0,offset=0;i<d.Fields.Length;offset+=d.Fields[i].Bytes,++i)
        {
            var f=d.Fields[i];var v="value.@"+f.Member;var slice="bytes["+offset+"..]";
            if(f.Buffer is not null) {b.Append("        ").Append(v).Append(".PoimaEncode(bytes.Slice(").Append(offset).Append(',').Append(f.Bytes).Append("));\n");continue;}
            if(f.Kind is "float32" or "float64")b.Append("        if(!").Append(f.Kind=="float32"?"float":"double").Append(".IsFinite(").Append(v).Append("))throw new global::System.ArgumentException(\"Component value must be finite.\");\n");
            if(f.Kind=="entity")
            {
                b.Append("        global::System.Buffers.Binary.BinaryPrimitives.WriteUInt64LittleEndian(").Append(slice).Append(',').Append(v).Append(".High);\n");
                b.Append("        global::System.Buffers.Binary.BinaryPrimitives.WriteUInt64LittleEndian(bytes[").Append(offset+8).Append("..],").Append(v).Append(".Low);\n");
            }
            else b.Append("        global::System.Buffers.Binary.BinaryPrimitives.Write").Append(Wire(f.Kind)).Append("LittleEndian(").Append(slice).Append(',').Append(f.Kind is "float32" or "float64"?v+"==0?0:"+v:v).Append(");\n");
        }
        b.Append("    }\n    static ").Append(type).Append(" global::Poima.IGameplayComponent<").Append(type).Append(">.Decode(global::System.ReadOnlySpan<byte> bytes)\n    {\n");
        b.Append("        if(bytes.Length!=").Append(d.Bytes).Append(")throw new global::System.ArgumentException(\"Component wire size mismatch.\");\n        ").Append(type).Append(" value=default;\n");
        for(int i=0,offset=0;i<d.Fields.Length;offset+=d.Fields[i].Bytes,++i)
        {
            var f=d.Fields[i];b.Append("        value.@").Append(f.Member).Append('=');
            if(f.Buffer is { } buffer) {b.Append(buffer.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)).Append(".PoimaDecode(bytes.Slice(").Append(offset).Append(',').Append(f.Bytes).Append("));\n");continue;}
            if(f.Kind=="entity")b.Append("new(global::System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(bytes[").Append(offset).Append("..]),global::System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(bytes[").Append(offset+8).Append("..]));\n");
            else b.Append("global::System.Buffers.Binary.BinaryPrimitives.Read").Append(Wire(f.Kind)).Append("LittleEndian(bytes[").Append(offset).Append("..]);\n");
        }
        return b.Append("        return value;\n    }\n}\n").ToString();
    }
    private static string Wire(string kind)=>kind switch {"int32"=>"Int32","int64"=>"Int64","float32"=>"Single","float64"=>"Double",_=>throw new ArgumentException("Unknown wire kind.")};
}
