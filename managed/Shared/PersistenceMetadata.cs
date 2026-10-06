// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Reflection;
using System.Text.Json;
using Poima;
namespace Poima.Build;

// Build/load-time metadata only: no attribute constructors, state constructors,
// or Initialize calls. Return detached JSON rather than retaining game Types.
internal static class PersistenceMetadata
{
    private static CustomAttributeData? Attribute(MemberInfo member,Type expected)
    {
        var found=member.GetCustomAttributesData().Where(a=>a.AttributeType==expected).ToArray();
        if(found.Length>1)throw new ArgumentException($"Duplicate {expected.Name} on {member.Name}.");
        return found.SingleOrDefault();
    }
    internal static string GameIdentity(Type game)
    {
        var annotation=Attribute(game,typeof(GameModuleAttribute));
        if(annotation is null || annotation.ConstructorArguments.Count!=1 || annotation.ConstructorArguments[0].Value is not string identity ||
            string.IsNullOrWhiteSpace(identity) || identity.Length>128)
            throw new ArgumentException("GameModule needs an identity of 1..128 characters.");
        return identity;
    }
    private static string? Named(CustomAttributeData attribute,string name)
    {
        var found=attribute.NamedArguments.Where(a=>a.MemberName==name).ToArray();
        if(found.Length>1)throw new ArgumentException($"Duplicate {name} metadata.");
        if(found.Length==0)return null;
        if(found[0].TypedValue.ArgumentType!=typeof(string))throw new ArgumentException($"{name} metadata must be literal text.");
        return (string?)found[0].TypedValue.Value;
    }
    private static string Kind(Type type)=>type==typeof(int)?"int32":type==typeof(long)?"int64":type==typeof(float)?"float32":
        type==typeof(double)?"float64":type==typeof(EntityId)?"entity":throw new ArgumentException("Unsupported persistent global field kind.");
    private static object Default(string kind,string? literal)
    {
        var text=literal ?? (kind=="entity"?new string('0',32):"0");
        if(text.Length>256)throw new ArgumentException("Persistent default exceeds 256 characters.");
        switch(kind) {
            case "int32":
                var i=int.Parse(text,NumberStyles.AllowLeadingSign,CultureInfo.InvariantCulture);
                if(i.ToString(CultureInfo.InvariantCulture)!=text)throw new ArgumentException("Persistent int32 default must be canonical decimal.");return i;
            case "int64":
                var l=long.Parse(text,NumberStyles.AllowLeadingSign,CultureInfo.InvariantCulture);
                if(l.ToString(CultureInfo.InvariantCulture)!=text)throw new ArgumentException("Persistent int64 default must be canonical decimal.");return text;
            case "float32":
                var f=float.Parse(text,NumberStyles.Float,CultureInfo.InvariantCulture);
                if(!float.IsFinite(f))throw new ArgumentException("Persistent float32 default must be finite.");return f==0 ? 0f:f;
            case "float64":
                var d=double.Parse(text,NumberStyles.Float,CultureInfo.InvariantCulture);
                if(!double.IsFinite(d))throw new ArgumentException("Persistent float64 default must be finite.");return d==0 ? 0d:d;
            default:
                if(text!=new string('0',32))throw new ArgumentException("Persistent entity defaults must be the unset all-zero ID.");return text;
        }
    }
    internal static JsonElement? Read(Type state)
    {
        var annotation=Attribute(state,typeof(GameplayPersistenceAttribute));if(annotation is null)return null;
        if(annotation.ConstructorArguments.Count!=1 || annotation.ConstructorArguments[0].Value is not int revision || revision<=0 || annotation.NamedArguments.Count!=0)
            throw new ArgumentException("GameplayPersistence requires a positive int32 revision.");
        var fields=state.GetFields(BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic);
        if(fields.Length is <1 or >128 || fields.Any(f=>!f.IsPublic || f.IsInitOnly))throw new ArgumentException("Persistent state requires 1..128 public mutable fields.");
        var entries=new List<(string Id,string Name,string Kind,object Default)>();
        foreach(var field in fields) {
            var attribute=Attribute(field,typeof(GameplayFieldAttribute)) ?? throw new ArgumentException($"Persistent global field {field.Name} requires GameplayField.");
            if(attribute.ConstructorArguments.Count!=1 || attribute.ConstructorArguments[0].Value is not string id || id.Length!=32 || id.All(c=>c=='0') ||
                id.Any(c=>!(c>='0' && c<='9') && !(c>='a' && c<='f')))
                throw new ArgumentException("Persistent global IDs must be nonzero 32-character lowercase hexadecimal strings.");
            var name=Named(attribute,"Name");var unit=Named(attribute,"Unit");
            if(name is not null && name!=field.Name)throw new ArgumentException("Persistent global Name must match the reflected field name; labels are not part of this metadata format.");
            if(!string.IsNullOrEmpty(unit))throw new ArgumentException("Persistent global Unit is not supported by this metadata format.");
            if(attribute.NamedArguments.Any(a=>a.MemberName is not ("Name" or "Unit" or "Default")))throw new ArgumentException("Unknown persistent global field metadata.");
            var kind=Kind(field.FieldType);entries.Add((id,field.Name,kind,Default(kind,Named(attribute,"Default"))));
        }
        if(entries.Select(e=>e.Id).Distinct(StringComparer.Ordinal).Count()!=entries.Count)throw new ArgumentException("Duplicate persistent global field ID.");
        return JsonSerializer.SerializeToElement(new {format="poima.gameplay-persistence",version=1,revision,
            fields=entries.OrderBy(e=>e.Id,StringComparer.Ordinal).Select(e=>new {id=e.Id,name=e.Name,kind=e.Kind,@default=e.Default})});
    }
}
