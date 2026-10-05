// SPDX-License-Identifier: Apache-2.0
// Metadata-only extraction: no Assembly.Load, reflection constructors, or game initialization.
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Text;
using System.Text.Json;
namespace Poima.Build;
internal static class ComponentMetadata
{
    internal static JsonElement? ReadSchemas(byte[] image)
    {
        using var stream=new MemoryStream(image,false);using var pe=new PEReader(stream);
        var reader=pe.GetMetadataReader();string? manifest=null;
        foreach(var handle in reader.GetAssemblyDefinition().GetCustomAttributes())
        {
            var attribute=reader.GetCustomAttribute(handle);
            if(attribute.Constructor.Kind!=HandleKind.MemberReference)continue;
            var constructor=reader.GetMemberReference((MemberReferenceHandle)attribute.Constructor);
            if(constructor.Parent.Kind!=HandleKind.TypeReference)continue;
            var type=reader.GetTypeReference((TypeReferenceHandle)constructor.Parent);
            if(reader.GetString(type.Namespace)!="Poima" || reader.GetString(type.Name)!="GameplayComponentManifestAttribute")continue;
            if(type.ResolutionScope.Kind!=HandleKind.AssemblyReference || reader.GetString(reader.GetAssemblyReference((AssemblyReferenceHandle)type.ResolutionScope).Name)!="Poima.Gameplay")
                throw new ArgumentException("Component manifest attribute must come from the matching Poima SDK.");
            if(manifest is not null)throw new ArgumentException("Duplicate component manifest attribute.");
            var blob=reader.GetBlobReader(attribute.Value);
            if(blob.ReadUInt16()!=1)throw new ArgumentException("Invalid component metadata attribute.");
            manifest=blob.ReadSerializedString() ?? throw new ArgumentException("Null component manifest.");
            if(blob.ReadUInt16()!=0 || blob.RemainingBytes!=0)throw new ArgumentException("Unexpected component metadata arguments.");
        }
        if(manifest is null)return null;
        if(Encoding.UTF8.GetByteCount(manifest)>512*1024)throw new ArgumentException("Component manifest exceeds 512 KiB.");
        using var document=JsonDocument.Parse(manifest,new JsonDocumentOptions {MaxDepth=32});var root=document.RootElement;
        if(root.ValueKind!=JsonValueKind.Object || root.EnumerateObject().Count()!=3 || root.GetProperty("format").GetString()!="poima.components" || root.GetProperty("version").GetInt32()!=1)
            throw new ArgumentException("Invalid generated component manifest header.");
        var schemas=root.GetProperty("schemas");if(schemas.ValueKind!=JsonValueKind.Array || schemas.GetArrayLength()>64)throw new ArgumentException("Invalid generated schema count.");
        return schemas.GetArrayLength()==0?null:schemas.Clone();
    }
    internal static string Manifest(JsonElement? schemas)=>schemas is null?"{\"format\":\"poima.components\",\"version\":1,\"schemas\":[]}":
        "{\"format\":\"poima.components\",\"version\":1,\"schemas\":"+schemas.Value.GetRawText()+"}";
}
