// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
namespace Poima;

[Flags] internal enum GameplayRequiredFeatures : uint { None=0,InertialAnimation=1,MaskedAnimation=2 }
internal static unsafe class GameplayRequirements
{
    internal const string Baseline="{\"call_version\":1,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":176,\"features\":[\"baseline_v7\"]}";
    internal const string Inertial="{\"call_version\":1,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":192,\"features\":[\"animation_inertial_v1\",\"baseline_v7\"]}";
    internal const string Masked="{\"call_version\":1,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":208,\"features\":[\"animation_inertial_v1\",\"animation_layers_v1\",\"baseline_v7\"]}";
    internal static string Json(bool inertial)=>inertial ? Inertial : Baseline;
    internal static GameplayRequiredFeatures Features(bool inertial)=>inertial ? GameplayRequiredFeatures.InertialAnimation : GameplayRequiredFeatures.None;
    internal static GameplayRequiredFeatures Features(bool inertial,bool masked)=>masked ? GameplayRequiredFeatures.InertialAnimation|GameplayRequiredFeatures.MaskedAnimation : Features(inertial);
    internal static string Json(GameplayRequiredFeatures features)
    {
        ValidateRequired(features);
        return (features & GameplayRequiredFeatures.MaskedAnimation)!=0 ? Masked : Json((features & GameplayRequiredFeatures.InertialAnimation)!=0);
    }
    private static void ValidateRequired(GameplayRequiredFeatures required)
    {
        if((required & ~(GameplayRequiredFeatures.InertialAnimation|GameplayRequiredFeatures.MaskedAnimation))!=0 ||
            ((required & GameplayRequiredFeatures.MaskedAnimation)!=0 && (required & GameplayRequiredFeatures.InertialAnimation)==0))
            throw new ArgumentException("Invalid gameplay required animation features.");
    }
    // Existing generated Bindings already carry Requirements. Reading this once
    // avoids adding a required member to their source-level contract.
    internal static GameplayRequiredFeatures DeclaredFeatures(string declaration)
    {
        using var document=JsonDocument.Parse(declaration);var value=document.RootElement;
        if(value.ValueKind!=JsonValueKind.Object)throw new ArgumentException("Generated gameplay requirements must be an object.");
        var keys=new HashSet<string>(StringComparer.Ordinal);
        foreach(var property in value.EnumerateObject())
            if(!keys.Add(property.Name) || property.Name is not ("call_version" or "call_bytes" or "services_version" or "services_bytes" or "features"))
                throw new ArgumentException("Invalid or duplicate generated gameplay requirement field.");
        if(keys.Count!=5 || Integer(value,"call_version")!=1 || Integer(value,"call_bytes")!=80 || Integer(value,"services_version")!=ServiceAbi.Epoch)
            throw new ArgumentException("Invalid generated gameplay requirement ABI.");
        if(!value.TryGetProperty("features",out var array) || array.ValueKind!=JsonValueKind.Array || array.GetArrayLength() is <1 or >3)
            throw new ArgumentException("Invalid generated gameplay required features.");
        var names=new HashSet<string>(StringComparer.Ordinal);GameplayRequiredFeatures result=GameplayRequiredFeatures.None;
        foreach(var feature in array.EnumerateArray())
        {
            if(feature.ValueKind!=JsonValueKind.String || !names.Add(feature.GetString()!))throw new ArgumentException("Invalid or duplicate generated gameplay required feature.");
            switch(feature.GetString())
            {
                case "baseline_v7":break;
                case "animation_inertial_v1":result|=GameplayRequiredFeatures.InertialAnimation;break;
                case "animation_layers_v1":result|=GameplayRequiredFeatures.MaskedAnimation;break;
                default:throw new ArgumentException("Unknown generated gameplay required feature.");
            }
        }
        ValidateRequired(result);
        uint bytes=(result & GameplayRequiredFeatures.MaskedAnimation)!=0 ? AnimationLayerServiceAbi.RequiredBytes :
            (result & GameplayRequiredFeatures.InertialAnimation)!=0 ? AnimationServiceAbi.RequiredBytes : ServiceAbi.RequiredBytes;
        if(!names.Contains("baseline_v7") || Integer(value,"services_bytes")!=bytes)
            throw new ArgumentException("Generated gameplay service prefix differs from its required features.");
        return result;
    }
    internal static void ValidateHost(JsonElement request,GameplayRequiredFeatures required)
    {
        ValidateRequired(required);
        if(request.ValueKind!=JsonValueKind.Object || request.EnumerateObject().Count(p=>p.Name=="host_contract")>1)
            throw new ArgumentException("Invalid or duplicate gameplay host_contract.");
        // Old hosts have no load-time negotiation and expose only the baseline.
        // A marked game must reject before its constructor or Initialize runs.
        if(!request.TryGetProperty("host_contract",out var host))
        {
            if(required!=GameplayRequiredFeatures.None)throw new ArgumentException("Host lacks required animation service negotiation.");
            return;
        }
        if(host.ValueKind!=JsonValueKind.Object)throw new ArgumentException("Gameplay host_contract must be an object.");
        var keys=new HashSet<string>(StringComparer.Ordinal);
        foreach(var property in host.EnumerateObject())
            if(!keys.Add(property.Name) || property.Name is not ("call_version" or "call_bytes" or "services_version" or "services_bytes" or "features"))
                throw new ArgumentException("Invalid or duplicate gameplay host_contract field.");
        if(keys.Count!=5 || Integer(host,"call_version")!=1 || Integer(host,"call_bytes")!=80 ||
            Integer(host,"services_version")!=ServiceAbi.Epoch || Integer(host,"services_bytes")<ServiceAbi.RequiredBytes)
            throw new ArgumentException("Gameplay host contract requires call 1/80 and services epoch 7 with at least 176 bytes.");
        var values=host.GetProperty("features");
        if(values.ValueKind!=JsonValueKind.Array || values.GetArrayLength()>64)throw new ArgumentException("Gameplay host features exceed bounds.");
        var features=new HashSet<string>(StringComparer.Ordinal);
        foreach(var value in values.EnumerateArray())
        {
            if(value.ValueKind!=JsonValueKind.String)throw new ArgumentException("Gameplay host feature must be text.");
            var feature=value.GetString()!;
            if(feature.Length is <1 or >64 || feature.Any(c=>!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_')) || !features.Add(feature))
                throw new ArgumentException("Invalid or duplicate gameplay host feature.");
        }
        if(!features.Contains("baseline_v7"))throw new ArgumentException("Gameplay host lacks baseline_v7.");
        if((required & GameplayRequiredFeatures.InertialAnimation)!=0 &&
            (Integer(host,"services_bytes")<AnimationServiceAbi.RequiredBytes || !features.Contains("animation_inertial_v1")))
            throw new ArgumentException("Gameplay host lacks animation_inertial_v1 with at least 192 service bytes.");
        if((required & GameplayRequiredFeatures.MaskedAnimation)!=0 &&
            (Integer(host,"services_bytes")<AnimationLayerServiceAbi.RequiredBytes || !features.Contains("animation_layers_v1")))
            throw new ArgumentException("Gameplay host lacks animation_layers_v1 with at least 208 service bytes.");
        // Unknown, well-formed available host features are intentionally ignored:
        // a later same-epoch host may append unrelated services safely.
    }
    private static uint Integer(JsonElement value,string name)
    {
        if(!value.TryGetProperty(name,out var property) || property.ValueKind!=JsonValueKind.Number || !property.TryGetUInt32(out uint result))
            throw new ArgumentException("Gameplay host contract numbers require nonnegative uint32 integers.");
        return result;
    }
    internal static void ValidateServices(NativeServices* services,GameplayRequiredFeatures required)
    {
        ValidateRequired(required);
        if((required & GameplayRequiredFeatures.MaskedAnimation)!=0)_=AnimationLayerServiceAbi.Validate(services);
        else if((required & GameplayRequiredFeatures.InertialAnimation)!=0)_=AnimationServiceAbi.Validate(services);
    }
}
