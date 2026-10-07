// SPDX-License-Identifier: Apache-2.0
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
namespace Poima.Components.Generator;
public sealed partial class ComponentGenerator
{
    private sealed record BufferDefinition(INamedTypeSymbol Type,string Kind,int Capacity);
    private static string ScalarKind(ITypeSymbol type)=>type.SpecialType switch {
        SpecialType.System_Int32=>"int32",SpecialType.System_Int64=>"int64",SpecialType.System_Single=>"float32",SpecialType.System_Double=>"float64",
        _=>type.ToDisplayString()=="Poima.EntityId"?"entity":throw new ArgumentException("Component element kinds are int, long, float, double or EntityId.")};
    private static BufferDefinition ParseBuffer(INamedTypeSymbol type)
    {
        if(type.DeclaredAccessibility!=Accessibility.Public || type.ContainingType is not null || type.IsGenericType || type.IsReadOnly || type.IsRefLikeType || !type.IsUnmanagedType ||
            type.DeclaringSyntaxReferences.Any(r=>r.GetSyntax() is not StructDeclarationSyntax s || !s.Modifiers.Any(SyntaxKind.PartialKeyword) || s.Members.Count!=0 || s.ParameterList is not null || s.BaseList is not null))
            throw new ArgumentException("Gameplay buffers must be empty public, top-level, nongeneric, unmanaged mutable partial structs.");
        if(type.GetAttributes().Any(a=>a.AttributeClass?.ToDisplayString() is "Poima.GameplayComponentAttribute" or "System.Runtime.InteropServices.StructLayoutAttribute" or "System.Runtime.CompilerServices.InlineArrayAttribute"))
            throw new ArgumentException("Gameplay buffer storage/layout is generated; do not declare component or layout attributes on it.");
        var attribute=Attribute(type,"Poima.GameplayBufferAttribute");
        if(attribute.ConstructorArguments.Length!=2 || attribute.ConstructorArguments[0].Value is not ITypeSymbol element || attribute.ConstructorArguments[1].Value is not int capacity || capacity is <1 or >31)
            throw new ArgumentException("Gameplay buffers require a supported scalar element type and capacity 1..31.");
        return new(type,ScalarKind(element),capacity);
    }
    private static object FieldMetadata(Field f)=>f.Buffer is { } b ?
        new {id=f.Id,name=f.Name,kind="array",element_kind=b.Kind,capacity=b.Capacity,@default=f.Initial,unit=f.Unit} :
        new {id=f.Id,name=f.Name,kind=f.Kind,@default=f.Initial,unit=f.Unit};
    private static string BufferCodec(BufferDefinition d)
    {
        var element=d.Kind switch {"int32"=>"int","int64"=>"long","float32"=>"float","float64"=>"double",_=>"global::Poima.EntityId"};
        var type=d.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);var bytes=(d.Capacity+1)*16;
        var used=d.Kind=="entity"?16:d.Kind is "int32" or "float32"?4:8;
        var b=new StringBuilder("// Generated fixed-capacity gameplay buffer.\n#nullable enable\n");
        if(!d.Type.ContainingNamespace.IsGlobalNamespace)b.Append("namespace ").Append(d.Type.ContainingNamespace.ToDisplayString()).Append(";\n");
        b.Append("public partial struct @").Append(d.Type.Name).Append("\n{\n")
            .Append("    [global::System.Runtime.CompilerServices.InlineArray(").Append(d.Capacity).Append(")] private struct PoimaStorage {private ").Append(element).Append(" first;}\n")
            .Append("    private int poimaLength;private PoimaStorage poimaStorage;\n")
            .Append("    public readonly int Count{get{PoimaLengthCheck();return poimaLength;}}\n    public const int Capacity=").Append(d.Capacity).Append(";\n")
            .Append("    private readonly void PoimaLengthCheck(){if((uint)poimaLength>Capacity)throw new global::System.ArgumentException(\"Invalid buffer length.\");}\n    private readonly void PoimaCheck(int index){PoimaLengthCheck();if((uint)index>=(uint)poimaLength)throw new global::System.ArgumentOutOfRangeException(nameof(index));}\n")
            .Append("    private static ").Append(element).Append(" PoimaValue(").Append(element).Append(" value){");
        if(d.Kind is "float32" or "float64")b.Append("if(!").Append(element).Append(".IsFinite(value))throw new global::System.ArgumentException(\"Buffer element must be finite.\");return value==0?0:value;");
        else b.Append("return value;");
        b.Append("}\n    public ").Append(element).Append(" this[int index]{readonly get{PoimaCheck(index);return poimaStorage[index];}set{PoimaCheck(index);poimaStorage[index]=PoimaValue(value);}}\n")
            .Append("    public bool TryAdd(").Append(element).Append(" value){PoimaLengthCheck();if(poimaLength==Capacity)return false;value=PoimaValue(value);poimaStorage[poimaLength]=value;++poimaLength;return true;}\n")
            .Append("    public void RemoveAt(int index){PoimaCheck(index);for(int i=index+1;i<poimaLength;++i)poimaStorage[i-1]=poimaStorage[i];poimaStorage[--poimaLength]=default;}\n")
            .Append("    public void Clear(){this=default;}\n")
            .Append("    internal readonly void PoimaEncode(global::System.Span<byte> bytes)\n    {\n")
            .Append("        if(bytes.Length!=").Append(bytes).Append(" || (uint)poimaLength>Capacity)throw new global::System.ArgumentException(\"Buffer wire size/length differs.\");\n")
            .Append("        bytes.Clear();global::System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(bytes,(uint)poimaLength);\n")
            .Append("        for(int i=0;i<poimaLength;++i){var cell=bytes.Slice(16+i*16,16);var value=PoimaValue(poimaStorage[i]);");
        if(d.Kind=="entity")b.Append("global::System.Buffers.Binary.BinaryPrimitives.WriteUInt64LittleEndian(cell,value.High);global::System.Buffers.Binary.BinaryPrimitives.WriteUInt64LittleEndian(cell[8..],value.Low);");
        else b.Append("global::System.Buffers.Binary.BinaryPrimitives.Write").Append(Wire(d.Kind)).Append("LittleEndian(cell,value);");
        b.Append("}\n    }\n    internal static ").Append(type).Append(" PoimaDecode(global::System.ReadOnlySpan<byte> bytes)\n    {\n")
            .Append("        if(bytes.Length!=").Append(bytes).Append(")throw new global::System.ArgumentException(\"Buffer wire size differs.\");\n")
            .Append("        uint length=global::System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(bytes);if(length>Capacity)throw new global::System.ArgumentException(\"Buffer wire length exceeds capacity.\");\n")
            .Append("        for(int i=4;i<16;++i)if(bytes[i]!=0)throw new global::System.ArgumentException(\"Nonzero buffer header padding.\");\n")
            .Append("        for(int i=16+(int)length*16;i<bytes.Length;++i)if(bytes[i]!=0)throw new global::System.ArgumentException(\"Nonzero inactive buffer element.\");\n")
            .Append("        ").Append(type).Append(" result=default;for(int i=0;i<(int)length;++i){var cell=bytes.Slice(16+i*16,16);");
        if(used<16)b.Append("for(int j=").Append(used).Append(";j<16;++j)if(cell[j]!=0)throw new global::System.ArgumentException(\"Nonzero buffer element padding.\");");
        if(d.Kind=="entity")b.Append("var value=new global::Poima.EntityId(global::System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(cell),global::System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(cell[8..]));");
        else b.Append("var value=global::System.Buffers.Binary.BinaryPrimitives.Read").Append(Wire(d.Kind)).Append("LittleEndian(cell);");
        if(d.Kind is "float32" or "float64")b.Append("if(value==0 && global::System.BitConverter.").Append(d.Kind=="float32"?"SingleToInt32Bits(value)==int.MinValue":"DoubleToInt64Bits(value)==long.MinValue").Append(")throw new global::System.ArgumentException(\"Noncanonical buffer zero.\");");
        b.Append("result.TryAdd(value);}\n        return result;\n    }\n}\n");return b.ToString();
    }
}
