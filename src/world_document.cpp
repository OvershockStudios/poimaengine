// SPDX-License-Identifier: Apache-2.0
#include "world_document.hpp"

#include "asset_provenance.hpp"
#include "asset_store.hpp"
#include "foliage_motion_json.hpp"
#include "particle_emitter_json.hpp"
#include "physical_rig_json.hpp"
#include "poima/navigation.hpp"
#include "poima/runtime_grounding.hpp"
#include "poima/runtime_ik.hpp"
#include "poima/snow.hpp"
#include "pose_frames.hpp"
#include "surface_mark_profiles.hpp"
#include "surface_mark_validation.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string_view>

namespace poima::world_detail {
void validate_name(const Json& value) {
    require(value.is_string() && !value.get_ref<const std::string&>().empty() &&
        value.get_ref<const std::string&>().size() <= 256, "Name must contain 1..256 UTF-8 bytes.");
}
void validate_transform(const Json& value) {
    fields(value, {"position", "rotation", "scale"}, {"position", "rotation", "scale"});
    for (const auto* key : {"position", "rotation", "scale"}) {
        const auto& v = value.at(key);
        require(v.is_array() && v.size() == (std::string_view(key) == "rotation" ? 4 : 3), "Invalid transform vector size.");
        for (const auto& component : v) {
            require(component.is_number() && std::isfinite(component.get<double>()) &&
                std::abs(component.get<double>()) <= 1e9, "Transform contains an invalid or out-of-range number.");
            if (std::string_view(key) == "scale") require(component.get<double>() > 0, "Scale must be positive.");
        }
    }
    double norm = 0;
    for (const auto& v : value.at("rotation")) norm += v.get<double>() * v.get<double>();
    require(std::abs(norm - 1) <= 1e-6, "Rotation must be a normalized XYZW quaternion.");
}
Light light_value(const Json& value) {
    fields(value,{"kind","color","intensity","enabled","range","inner_angle","outer_angle","shadow"},{"kind","color","intensity","enabled"});
    require(value.at("kind")=="directional" || value.at("kind")=="point" || value.at("kind")=="spot","Light kind must be directional, point or spot.");
    Light light;light.kind=value.at("kind")=="directional" ? LightKind::directional : value.at("kind")=="point" ? LightKind::point : LightKind::spot;
    require(light.kind!=LightKind::directional || !value.contains("range"),"Directional lights do not accept range.");
    require(light.kind==LightKind::spot || (!value.contains("inner_angle") && !value.contains("outer_angle")),"Cone angles apply only to spot lights.");
    require(value.at("color").is_array() && value.at("color").size()==3,"Light color needs three values.");
    for(const auto& x:value.at("color"))require(x.is_number() && std::isfinite(x.get<double>()) && x>=0 && x<=1,"Light color must be in [0,1].");
    for(const auto* key:{"intensity","range","inner_angle","outer_angle"})if(value.contains(key))require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Invalid light number.");
    for(const auto* key:{"intensity","range"})if(value.contains(key))require(value.at(key)>=0 && value.at(key)<=1e9,"Light intensity/range must be in [0,1e9].");
    if(light.kind==LightKind::spot) { const auto inner=value.value("inner_angle",0.0),outer=value.value("outer_angle",45.0);require(inner>=0 && inner<outer && outer<=90,"Spot half-angles need 0 <= inner < outer <= 90 degrees."); }
    require(value.at("enabled").is_boolean(),"Light enabled must be boolean.");
    light.color=value.at("color").get<std::array<float,3>>();light.intensity=value.at("intensity");light.enabled=value.at("enabled");
    light.range=value.value("range",0.0f);light.inner_angle=value.value("inner_angle",0.0f);light.outer_angle=value.value("outer_angle",45.0f);
    if(value.contains("shadow")) {
        const auto& v=value.at("shadow");fields(v,{"enabled","near","distance","bias","normal_bias","angular_size_degrees"},{"enabled"});
        require(v.at("enabled").is_boolean(),"Shadow enabled must be boolean.");light.shadow.enabled=v.at("enabled");
        for(const auto* key:{"near","distance","bias","normal_bias"})if(v.contains(key))require(v.at(key).is_number() && std::isfinite(v.at(key).get<double>()),"Invalid shadow number.");
        const double shadow_near=v.value("near",.05),distance=v.value("distance",80.0),bias=v.value("bias",.0005),normal=v.value("normal_bias",.01);
        require(shadow_near>=.001 && shadow_near<=100 && distance>shadow_near && distance<=100000 && bias>=0 && bias<=.1 && normal>=0 && normal<=1,"Invalid shadow near/distance/bias/normal_bias bounds.");
        light.shadow.near_plane=static_cast<float>(shadow_near);light.shadow.distance=static_cast<float>(distance);light.shadow.bias=static_cast<float>(bias);light.shadow.normal_bias=static_cast<float>(normal);
        if(v.contains("angular_size_degrees")) {
            const auto& angular=v.at("angular_size_degrees");
            require(light.kind==LightKind::directional,"Shadow angular_size_degrees applies only to directional lights.");
            require(angular.is_number() && std::isfinite(angular.get<double>()) && angular>=0 && angular<=20,"Shadow angular_size_degrees must be a finite number in [0,20] degrees.");
            light.shadow.angular_size_degrees=angular.get<float>();
        }
    }
    try { validate_light(light); }catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }return light;
}
Json sky_json(const SkySettings& sky) {
    return {{"enabled",sky.enabled},{"zenith",sky.zenith},{"horizon",sky.horizon},{"ground",sky.ground},
        {"horizon_falloff",sky.horizon_falloff},{"sun",sky.sun.empty() ? Json(nullptr) : Json(sky.sun)},
        {"sun_size_degrees",sky.sun_size_degrees},{"sun_intensity",sky.sun_intensity}};
}
static SkySettings sky_value(const Json& value) {
    fields(value,{"enabled","zenith","horizon","ground","horizon_falloff","sun","sun_size_degrees","sun_intensity"},
        {"enabled","zenith","horizon","ground","horizon_falloff","sun","sun_size_degrees","sun_intensity"});
    require(value.at("enabled").is_boolean(),"Sky enabled must be Boolean.");
    SkySettings result;result.enabled=value.at("enabled").get<bool>();
    for(const auto* key:{"zenith","horizon","ground"}) {
        const auto& color=value.at(key);require(color.is_array() && color.size()==3,"Sky colors require three values.");
        for(const auto& channel:color)require(channel.is_number() && std::isfinite(channel.get<double>()) && channel>=0 && channel<=1,"Sky color must be in [0,1].");
    }
    result.zenith=value.at("zenith").get<std::array<float,3>>();result.horizon=value.at("horizon").get<std::array<float,3>>();result.ground=value.at("ground").get<std::array<float,3>>();
    auto number=[&](const char* key,double minimum,double maximum) {
        const auto& v=value.at(key);require(v.is_number() && std::isfinite(v.get<double>()) && v>=minimum && v<=maximum,std::string("Sky ")+key+" is out of bounds.");return v.get<float>();
    };
    result.horizon_falloff=number("horizon_falloff",.1,16);result.sun_size_degrees=number("sun_size_degrees",.1,20);result.sun_intensity=number("sun_intensity",0,1e6);
    if(!value.at("sun").is_null())result.sun=identifier(value.at("sun"));
    return result;
}
static CloudSettings cloud_value(const Json& value) {
    fields(value,{"base_height_m","thickness_m","coverage","extinction_per_km","wind_m_per_s","shadow_strength"},
        {"base_height_m","thickness_m","coverage","extinction_per_km","wind_m_per_s"});
    const auto number=[&](const char* name,double minimum,double maximum) {
        const auto& field=value.at(name);
        require(field.is_number() && std::isfinite(field.get<double>()) && field>=minimum && field<=maximum,
            std::string("Cloud ")+name+" is outside its finite range.");
        return field.get<float>();
    };
    const auto& wind=value.at("wind_m_per_s");
    require(wind.is_array() && wind.size()==2,"Cloud wind_m_per_s requires [x,z] in metres per second.");
    for(const auto& axis:wind)
        require(axis.is_number() && std::isfinite(axis.get<double>()) && axis>=-100 && axis<=100,
            "Cloud wind_m_per_s must be finite and in [-100,100] on each axis.");
    CloudSettings result{number("base_height_m",250,12000),number("thickness_m",100,6000),number("coverage",0,1),
        number("extinction_per_km",0,32),wind.get<std::array<float,2>>(),std::nullopt};
    if(value.contains("shadow_strength"))result.shadow_strength=number("shadow_strength",0,1);
    return result;
}
LightingEnvironment environment_value(const Json& value) {
    fields(value,{"ambient","exposure","shadow_resolution","shadow_filter","sky","ibl","bloom","post_effect","output_grade",
        "tone_mapper","screen_reflections","screen_contact_shadows","screen_diffuse_bounce","height_fog","depth_of_field",
        "auto_exposure","atmosphere"},{"ambient","exposure"});
    require(value.at("ambient").is_array() && value.at("ambient").size()==3,"Ambient fill needs three values.");
    for(const auto& x:value.at("ambient"))require(x.is_number() && std::isfinite(x.get<double>()) && x>=0 && x<=1e6,"Ambient fill must be in [0,1e6].");
    require(value.at("exposure").is_number() && std::isfinite(value.at("exposure").get<double>()) && value.at("exposure")>=0 && value.at("exposure")<=1e6,"Exposure must be in [0,1e6].");
    if(value.contains("shadow_resolution"))require(value.at("shadow_resolution").is_number_integer() && (value.at("shadow_resolution")==256 || value.at("shadow_resolution")==512 || value.at("shadow_resolution")==1024 || value.at("shadow_resolution")==2048),"Shadow resolution must be 256, 512, 1024 or 2048.");
    LightingEnvironment result;result.ambient=value.at("ambient").get<std::array<float,3>>();result.exposure=value.at("exposure").get<float>();result.shadow_resolution=value.value("shadow_resolution",1024u);
    if(value.contains("tone_mapper")) {
        const auto& authored=value.at("tone_mapper");
        if(authored=="reinhard")result.tone_mapper=ToneMapper::reinhard;
        else if(authored=="agx")result.tone_mapper=ToneMapper::agx;
        else if(authored=="pbr_neutral")result.tone_mapper=ToneMapper::pbr_neutral;
        else if(authored=="aces2_sdr")result.tone_mapper=ToneMapper::aces2_sdr;
        else throw Error(-32602,"Tone mapper must be reinhard, agx, pbr_neutral or aces2_sdr.");
    }
    if(value.contains("auto_exposure")) {
        const auto& authored=value.at("auto_exposure");
        fields(authored,{"min_stops","max_stops","brighten_stops_per_second","darken_stops_per_second"});
        const auto read_field=[&](const char* key,double fallback,double minimum,double maximum) {
            if(!authored.contains(key))return fallback;
            const auto& member=authored.at(key);
            require(member.is_number() && std::isfinite(member.get<double>()) && member>=minimum && member<=maximum,
                std::string("Auto exposure ")+key+" is outside its finite range.");
            return member.get<double>();
        };
        const AutoExposureSettings defaults;
        const auto minimum=read_field("min_stops",defaults.min_stops,-8,8);
        const auto maximum=read_field("max_stops",defaults.max_stops,-8,8);
        require(minimum<=maximum,"Auto exposure min_stops must not exceed max_stops.");
        result.auto_exposure=AutoExposureSettings{
            static_cast<float>(minimum),static_cast<float>(maximum),
            static_cast<float>(read_field("brighten_stops_per_second",defaults.brighten_stops_per_second,.125,32)),
            static_cast<float>(read_field("darken_stops_per_second",defaults.darken_stops_per_second,.125,32))};
    }
    if(value.contains("shadow_filter")) {
        const auto& filter=value.at("shadow_filter");
        require(filter.is_string() && (filter=="pcf3_point" || filter=="pcf3_fractional" || filter=="pcss_directional"),"Environment shadow_filter must be pcf3_point, pcf3_fractional or pcss_directional.");
        result.shadow_filter=filter=="pcss_directional" ? ShadowFilter::pcss_directional : filter=="pcf3_fractional" ? ShadowFilter::pcf3_fractional : ShadowFilter::pcf3_point;
    }
    if(value.contains("sky"))result.sky=sky_value(value.at("sky"));
    if(value.contains("atmosphere")) {
        const auto& profile=value.at("atmosphere");
        fields(profile,{"sea_level_m","planet_center_m","aerosol_strength","radiance_scale","aerial_perspective_m","clouds"},
            {"aerosol_strength","radiance_scale"});
        const auto number=[&](const char* name,double minimum,double maximum) {
            const auto& field=profile.at(name);
            require(field.is_number() && std::isfinite(field.get<double>()) && field>=minimum && field<=maximum,
                std::string("Atmosphere ")+name+" is outside its finite range.");
            return field.get<double>();
        };
        auto& atmosphere=result.atmosphere.emplace();
        if(profile.contains("planet_center_m")) {
            require(!profile.contains("sea_level_m") && !profile.contains("aerial_perspective_m") && !profile.contains("clouds"),
                "Atmosphere planet_center_m is incompatible with sea_level_m, aerial_perspective_m and clouds.");
            const auto& center=profile.at("planet_center_m");
            require(center.is_array() && center.size()==3,"Atmosphere planet_center_m requires [x,y,z] in metres.");
            for(const auto& coordinate:center)
                require(coordinate.is_number() && std::isfinite(coordinate.get<double>()) && coordinate>=-1e9 && coordinate<=1e9,
                    "Atmosphere planet_center_m coordinates must be finite and in [-1e9,1e9].");
            atmosphere.planet_center_m=center.get<std::array<double,3>>();
        } else {
            require(profile.contains("sea_level_m"),"Atmosphere requires sea_level_m or planet_center_m.");
            atmosphere.sea_level_m=number("sea_level_m",-1e6,1e6);
        }
        atmosphere.aerosol_strength=static_cast<float>(number("aerosol_strength",0,4));
        atmosphere.radiance_scale=static_cast<float>(number("radiance_scale",0,64));
        if(profile.contains("aerial_perspective_m"))
            atmosphere.aerial_perspective_m=static_cast<float>(number("aerial_perspective_m",128,65536));
        if(profile.contains("clouds"))atmosphere.clouds=cloud_value(profile.at("clouds"));
    }
    if(value.contains("ibl")) {
        const auto& ibl=value.at("ibl");
        fields(ibl,{"enabled","diffuse_intensity","specular_intensity"},{"enabled","diffuse_intensity","specular_intensity"});
        require(ibl.at("enabled").is_boolean(),"IBL enabled must be Boolean.");
        for(const auto* key:{"diffuse_intensity","specular_intensity"}) {
            const auto& gain=ibl.at(key);
            require(gain.is_number() && std::isfinite(gain.get<double>()) && gain>=0 && gain<=64,
                    std::string("IBL ")+key+" must be a finite number in [0,64].");
        }
        result.ibl=IblSettings{ibl.at("enabled").get<bool>(),ibl.at("diffuse_intensity").get<float>(),ibl.at("specular_intensity").get<float>()};
    }
    if(value.contains("bloom")) {
        const auto& bloom=value.at("bloom");
        fields(bloom,{"intensity","threshold","radius"});
        const auto read_field=[&](const char* name,float fallback,double minimum,double maximum) {
            if(!bloom.contains(name))return fallback;
            const auto& field=bloom.at(name);
            require(field.is_number() && field>=minimum && field<=maximum,
                    std::string("Bloom ")+name+" is outside its finite range.");
            return field.get<float>();
        };
        result.bloom.intensity=read_field("intensity",result.bloom.intensity,0,1);
        result.bloom.threshold=read_field("threshold",result.bloom.threshold,0,65504);
        result.bloom.radius=read_field("radius",result.bloom.radius,2,32);
    }
    if(value.contains("output_grade")) {
        const auto& authored=value.at("output_grade");
        fields(authored,{"input_max","strength","knots"},{"input_max","strength","knots"});
        const auto read_number=[](const Json& field,double minimum,double maximum,const char* message) {
            require(field.is_number(),message);
            const auto number=field.get<double>();
            require(std::isfinite(number) && number>=minimum && number<=maximum,message);
            return static_cast<float>(number);
        };
        OutputGrade grade;
        grade.input_max=read_number(authored.at("input_max"),.125,64,
            "Output grade input_max must be finite and in [0.125,64].");
        grade.strength=read_number(authored.at("strength"),0,1,
            "Output grade strength must be finite and in [0,1].");
        const auto& knots=authored.at("knots");
        require(knots.is_array() && knots.size()==output_grade_knot_count,"Output grade needs exactly 33 RGB knots.");
        // Test the original numbers so float rounding cannot hide a nonzero first knot.
        require(knots.front()==Json::array({0,0,0}),"Output grade first knot must be [0,0,0].");
        for(std::size_t i=0;i<grade.knots.size();++i) {
            const auto& knot=knots.at(i);
            require(knot.is_array() && knot.size()==3,"An output grade knot needs exactly three RGB channels.");
            for(std::size_t channel=0;channel<grade.knots[i].size();++channel)
                grade.knots[i][channel]=read_number(knot.at(channel),0,16,
                    "Output grade knot channels must be finite and in [0,16].");
        }
        result.output_grade=grade;
    }
    if(value.contains("screen_reflections")) {
        const auto& authored=value.at("screen_reflections");
        fields(authored,{"strength","max_distance_m","thickness_m"},{"strength","max_distance_m","thickness_m"});
        const auto read_number=[&](const char* name,double minimum,double maximum) {
            const auto& field=authored.at(name);
            require(field.is_number() && std::isfinite(field.get<double>()) && field>=minimum && field<=maximum,
                    std::string("Screen reflections ")+name+" is outside its finite range.");
            return field.get<float>();
        };
        result.screen_reflections=ScreenReflections{
            read_number("strength",0,1),read_number("max_distance_m",.25,32),read_number("thickness_m",.01,.5)};
    }
    if(value.contains("screen_contact_shadows")) {
        const auto& authored=value.at("screen_contact_shadows");
        fields(authored,{"light","length_m","thickness_m","strength"},{"light","length_m","thickness_m","strength"});
        const auto number=[&](const char* name,double low,double high) {
            const auto& field=authored.at(name);
            require(field.is_number() && std::isfinite(field.get<double>()) && field>=low && field<=high,
                std::string("Screen contact shadows ")+name+" is outside its finite range.");
            return field.get<double>();
        };
        const auto length=number("length_m",.02,2);
        const auto thickness=number("thickness_m",.001,.15);
        require(thickness<=length,"Screen contact shadows thickness_m must not exceed length_m.");
        result.screen_contact_shadows=ScreenContactShadows{identifier(authored.at("light")),
            static_cast<float>(length),static_cast<float>(thickness),static_cast<float>(number("strength",0,1))};
    }
    if(value.contains("screen_diffuse_bounce")) {
        const auto& authored=value.at("screen_diffuse_bounce");
        fields(authored,{"radius_m","thickness_m","strength","resolution"},{"radius_m","thickness_m","strength"});
        const auto number=[&](const char* name,double low,double high) {
            const auto& field=authored.at(name);
            require(field.is_number() && std::isfinite(field.get<double>()) && field>=low && field<=high,
                std::string("Screen diffuse bounce ")+name+" is outside its finite range.");
            return field.get<double>();
        };
        const auto radius=number("radius_m",.2,4);
        const auto thickness=number("thickness_m",.005,.25);
        require(thickness<=radius,"Screen diffuse bounce thickness_m must not exceed radius_m.");
        ScreenDiffuseBounce bounce{static_cast<float>(radius),static_cast<float>(thickness),
            static_cast<float>(number("strength",0,1)),std::nullopt};
        if(authored.contains("resolution")) {
            const auto& resolution=authored.at("resolution");
            require(resolution=="full" || resolution=="half","Screen diffuse bounce resolution must be full or half.");
            bounce.resolution=resolution=="half" ? DiffuseBounceResolution::half : DiffuseBounceResolution::full;
        }
        result.screen_diffuse_bounce=bounce;
    }
    if(value.contains("depth_of_field")) {
        const auto& profile=value.at("depth_of_field");
        fields(profile,{"focus_distance_m","focus_band_m","radius_at_infinity_px","max_radius_px"},
                       {"focus_distance_m","focus_band_m","radius_at_infinity_px","max_radius_px"});
        const auto number=[&](const char* name,double low,double high) {
            const auto& field=profile.at(name);
            require(field.is_number() && std::isfinite(field.get<double>()) && field>=low && field<=high,
                    std::string("Depth of field ")+name+" is outside its finite range.");
            return field.get<float>();
        };
        result.depth_of_field=DepthOfField{number("focus_distance_m",.01,1000000),
            number("focus_band_m",0,1000000),number("radius_at_infinity_px",0,24),number("max_radius_px",0,24)};
    }
    if(value.contains("height_fog")) {
        const auto& authored=value.at("height_fog");
        fields(authored,{"density_per_m","base_height_m","height_falloff_per_m","start_distance_m","max_distance_m",
            "ambient_radiance","anisotropy","spot_lights"},{"density_per_m","base_height_m","height_falloff_per_m",
            "start_distance_m","max_distance_m","ambient_radiance","anisotropy"});
        const auto read_number=[](const Json& field,double minimum,double maximum,const char* name) {
            require(field.is_number() && std::isfinite(field.get<double>()) && field>=minimum && field<=maximum,
                std::string("Height fog ")+name+" is outside its finite range.");
            return field.get<float>();
        };
        HeightFog fog;
        fog.density_per_m=read_number(authored.at("density_per_m"),0,.75,"density_per_m");
        fog.base_height_m=read_number(authored.at("base_height_m"),-8192,8192,"base_height_m");
        fog.height_falloff_per_m=read_number(authored.at("height_falloff_per_m"),0,4,"height_falloff_per_m");
        fog.start_distance_m=read_number(authored.at("start_distance_m"),0,300,"start_distance_m");
        fog.max_distance_m=read_number(authored.at("max_distance_m"),.1,512,"max_distance_m");
        fog.anisotropy=read_number(authored.at("anisotropy"),-.8,.8,"anisotropy");
        const auto& rgb=authored.at("ambient_radiance");
        require(rgb.is_array() && rgb.size()==3,"Height fog ambient_radiance needs three RGB values.");
        for(std::size_t channel=0;channel<fog.ambient_radiance.size();++channel)
            fog.ambient_radiance[channel]=read_number(rgb.at(channel),0,32,"ambient_radiance");
        if(authored.contains("spot_lights")) {
            const auto& spots=authored.at("spot_lights");
            require(spots.is_array() && spots.size()<=2,"Height fog accepts at most two spot lights.");
            for(const auto& spot:spots) {
                fields(spot,{"entity","scattering"},{"entity","scattering"});
                fog.spot_lights.push_back({identifier(spot.at("entity")),
                    read_number(spot.at("scattering"),0,1,"spot light scattering")});
            }
        }
        result.height_fog=std::move(fog);
    }
    if(value.contains("post_effect")) {
        const auto& digest=value.at("post_effect");
        require(digest.is_string() && valid_asset_id(digest.get_ref<const std::string&>()),
            "Post effect must be a 64-character lowercase SHA-256.");
        result.post_effect_asset=digest.get<std::string>();
    }
    try { validate_environment(result); }catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
    return result;
}
AcousticMaterial acoustic_value(const Json& v) {
    fields(v,{"absorption","transmission","scattering","enabled"},{"absorption","transmission","scattering","enabled"});
    for(const auto* key:{"absorption","transmission"}) {
        require(v.at(key).is_array() && v.at(key).size()==3,"Acoustic bands need three values.");
        for(const auto& x:v.at(key))require(x.is_number() && std::isfinite(x.get<double>()) && x>=0 && x<=1,"Acoustic bands must be in [0,1].");
    }
    require(v.at("scattering").is_number() && std::isfinite(v.at("scattering").get<double>()) && v.at("scattering")>=0 && v.at("scattering")<=1,"Scattering must be in [0,1].");
    require(v.at("enabled").is_boolean(),"Acoustic enabled must be Boolean.");
    return {v.at("absorption").get<std::array<float,3>>(),v.at("transmission").get<std::array<float,3>>(),v.at("scattering").get<float>(),v.at("enabled").get<bool>()};
}
AudioRoom audio_room_value(const Json& value) {
    fields(value,{"half_extents","decay_seconds","high_frequency_decay_seconds","wet_gain","priority","portal_send"},
        {"half_extents","decay_seconds","high_frequency_decay_seconds","wet_gain"});
    const auto& extents=value.at("half_extents");
    require(extents.is_array() && extents.size()==3,"AudioRoom needs three half extents.");
    const auto number=[](const Json& field) {
        require(field.is_number(),"AudioRoom dimensions, decay and gain must be numbers.");
        return field.get<double>();
    };
    AudioRoom room;
    for (std::size_t axis=0;axis<3;++axis)
        room.half_extents[axis]=number(extents.at(axis));
    room.decay_seconds=number(value.at("decay_seconds"));
    room.high_frequency_decay_seconds=number(value.at("high_frequency_decay_seconds"));
    room.wet_gain=number(value.at("wet_gain"));
    if(value.contains("portal_send"))room.portal_send=number(value.at("portal_send"));
    const auto priority=revision(value.value("priority",Json(0)));
    require(priority<=255,"AudioRoom priority must be in [0,255].");
    room.priority=static_cast<std::uint8_t>(priority);
    try { validate_audio_room(room); }
    catch(const std::invalid_argument& error) { throw Error(-32602,error.what()); }
    return room;
}
AudioPortal audio_portal_value(const Json& value) {
    fields(value,{"half_extents","gain"},{"half_extents"});
    const auto& half=value.at("half_extents");
    const auto gain=value.value("gain",Json(1));
    require(half.is_array() && half.size()==2 && half[0].is_number() && half[1].is_number() && gain.is_number(),
        "AudioPortal needs two numeric half extents and optional numeric gain.");
    const AudioPortal portal{half.get<std::array<double,2>>(),gain.get<double>()};
    try { validate_audio_portal(portal); }
    catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
    return portal;
}
AudioEmitter emitter_value(const Json& v) {
    fields(v,{"asset","gain","loop","enabled","bus","water_flow"},{"asset","gain","loop","enabled"});
    require(v.at("asset").is_string() && valid_asset_id(v.at("asset").get<std::string>()),"AudioEmitter needs a clip content hash.");
    require(v.at("gain").is_number() && std::isfinite(v.at("gain").get<double>()) && v.at("gain")>=0 && v.at("gain")<=4,"Audio gain must be in [0,4].");
    require(v.at("loop").is_boolean() && v.at("enabled").is_boolean(),"Audio loop/enabled must be Boolean.");
    AudioEmitter emitter{v.at("asset").get<std::string>(),{},v.at("gain").get<float>(),
        v.at("loop").get<bool>(),v.at("enabled").get<bool>(),{}};
    if (v.contains("bus")) {
        const auto& bus=v.at("bus");
        require(bus.is_string() && audio_buses::valid_name(bus.get_ref<const std::string&>()),
            "AudioEmitter bus must be a valid named route.");
        emitter.bus=AudioBusRoute{bus.get<std::string>()};
    }
    if (v.contains("water_flow")) {
        const auto& flow=v.at("water_flow");
        fields(flow,{"port","full_discharge_m3_s","direction"},{"port","full_discharge_m3_s"});
        const auto& name=flow.at("port");
        const auto& full=flow.at("full_discharge_m3_s");
        require(name.is_string() && valid_input_action_name(name.get_ref<const std::string&>()),
            "Water-driven audio needs a named storage port.");
        require(full.is_number() && std::isfinite(full.get<double>()) &&
            full>=min_audio_full_discharge_m3_s && full<=max_audio_full_discharge_m3_s,
            "Water-driven audio full discharge must be in [0.000001,1000000] m³/s.");
        const auto direction=flow.value("direction",Json("forward"));
        require(direction=="forward" || direction=="reverse" || direction=="both",
            "Water flow direction must be forward, reverse or both.");
        emitter.water_flow=AudioWaterFlowProfile{name.get<std::string>(),full.get<double>(),
            direction=="both" ? WaterFlowDirection::Both :
            direction=="reverse" ? WaterFlowDirection::Reverse : WaterFlowDirection::Forward};
    }
    return emitter;
}
audio_buses::Catalog authored_audio_buses(const Json& document) {
    const auto catalog=document.find("audio_buses");
    if(catalog==document.end())return {};
    try { return audio_buses::parse(*catalog); }
    catch(const std::invalid_argument& error) { throw Error(-32602,error.what()); }
}
AnimationCommand animation_value(const Json& value) {
    AnimationCommand state;
    if(!value.at("clip").is_null()) {
        const auto clip=revision(value.at("clip"));require(clip<max_model_clips,"Animation clip index exceeds the model limit.");
        state.clip=static_cast<std::uint32_t>(clip);
    }
    for(const auto* key:{"time","speed"})require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Animation time/speed must be finite numbers.");
    state.time=value.at("time");state.speed=value.at("speed");
    require(state.time>=0 && state.time<=1e9 && state.speed>=0 && state.speed<=8,"Animation time must be 0..1e9 and speed 0..8.");
    require(value.at("loop").is_boolean() && value.at("playing").is_boolean(),"Animation loop/playing must be Boolean.");
    state.loop=value.at("loop");state.playing=value.at("playing");return state;
}
std::vector<RuntimeAnimationLayer> animation_layers_value(const Json& values) {
    require(values.is_array() && values.size()<=4,"AnimationRig layers must contain at most four authored slots.");
    std::vector<RuntimeAnimationLayer> result;std::set<std::uint32_t> slots;
    for(const auto& value:values) {
        fields(value,{"slot","mode","clip","time","speed","loop","playing","weight","mask","reference_clip","reference_time"},
            {"slot","mode","clip","time","speed","loop","playing","weight","mask"});
        const auto slot=revision(value.at("slot"));require(slot>=1 && slot<=4,"Animation layer slot must be 1..4.");
        RuntimeAnimationLayer layer;layer.slot=static_cast<std::uint32_t>(slot);
        require(slots.insert(layer.slot).second,"Duplicate authored animation layer slot.");
        require(value.at("mode").is_string() && (value.at("mode")=="override" || value.at("mode")=="additive"),"Animation layer mode must be override or additive.");
        layer.mode=value.at("mode")=="additive" ? AnimationLayerMode::Additive : AnimationLayerMode::Override;
        const auto playback=animation_value(value);layer.clip=playback.clip;layer.time=playback.time;layer.speed=playback.speed;layer.loop=playback.loop;layer.playing=playback.playing;
        require(value.at("weight").is_number() && std::isfinite(value.at("weight").get<double>()) && value.at("weight")>=0 && value.at("weight")<=1,"Animation layer weight must be within 0..1.");
        layer.weight=value.at("weight").get<double>();
        const auto& mask=value.at("mask");require(mask.is_array() && mask.size()<=10000,"Animation layer mask exceeds 10000 nodes.");
        std::set<std::uint32_t> nodes;
        for(const auto& item:mask) {
            fields(item,{"node","weight"},{"node","weight"});const auto node=revision(item.at("node"));require(node<10000,"Animation mask node must be 0..9999.");
            require(item.at("weight").is_number() && std::isfinite(item.at("weight").get<double>()) && item.at("weight")>=0 && item.at("weight")<=1,"Animation mask weight must be within 0..1.");
            const auto index=static_cast<std::uint32_t>(node);require(nodes.insert(index).second,"Duplicate animation mask node.");
            layer.mask.push_back({index,item.at("weight").get<double>()});
        }
        std::sort(layer.mask.begin(),layer.mask.end(),[](const auto& left,const auto& right) { return left.node<right.node; });
        if(value.contains("reference_clip") && !value.at("reference_clip").is_null()) {
            const auto clip=revision(value.at("reference_clip"));require(clip<max_model_clips,"Animation layer reference clip exceeds model limit.");layer.reference_clip=static_cast<std::uint32_t>(clip);
        }
        if(value.contains("reference_time")) {
            require(value.at("reference_time").is_number() && std::isfinite(value.at("reference_time").get<double>()) && value.at("reference_time")>=0 && value.at("reference_time")<=1e9,"Animation reference time must be within 0..1e9.");
            layer.reference_time=value.at("reference_time").get<double>();
        }
        require(layer.reference_clip || layer.reference_time==0,"A rest-pose animation reference must use time zero.");
        require(layer.mode==AnimationLayerMode::Additive || (!layer.reference_clip && layer.reference_time==0),"Override layers do not use a separate reference clip.");
        result.push_back(std::move(layer));
    }
    std::sort(result.begin(),result.end(),[](const auto& left,const auto& right) { return left.slot<right.slot; });return result;
}
RuntimeIkCommand ik_control_value(const Json& value) {
    RuntimeIkCommand result;
    const auto slot=revision(value.at("slot"));require(slot>=1 && slot<=8,"IK slot must be1..8.");result.slot=static_cast<std::uint32_t>(slot);
    for(const auto* key:{"target","pole"}) {
        const auto& vector=value.at(key);require(vector.is_array() && vector.size()==3,"IK target/pole requires three coordinates.");
        for(const auto& x:vector)require(x.is_number() && std::isfinite(x.get<double>()) && std::abs(x.get<double>())<=1e6,"IK target/pole must be finite and within1e6.");
    }
    result.target=value.at("target").get<std::array<double,3>>();result.pole=value.at("pole").get<std::array<double,3>>();
    if(value.contains("enabled")) {require(value.at("enabled").is_boolean(),"IK enabled must be boolean.");result.enabled=value.at("enabled").get<bool>();}
    if(value.contains("space")) {
        require(value.at("space").is_string() && (value.at("space")=="world" || value.at("space")=="rig"),"IK space must be world or rig.");
        result.space=value.at("space")=="rig" ? IkSpace::Rig : IkSpace::World;
    }
    if(value.contains("weight")) {
        require(value.at("weight").is_number() && std::isfinite(value.at("weight").get<double>()) && value.at("weight")>=0 && value.at("weight")<=1,"IK weight must be within0..1.");
        result.weight=value.at("weight").get<double>();
    }
    if(value.contains("end_orientation")) {
        const auto& goal=value.at("end_orientation");
        fields(goal,{"rotation","weight"},{"rotation","weight"});
        const auto& rotation=goal.at("rotation");
        require(rotation.is_array() && rotation.size()==4,"IK end orientation rotation requires four XYZW components.");
        for(const auto& x:rotation)require(x.is_number() && std::isfinite(x.get<double>()),"IK end orientation rotation must contain finite numbers.");
        require(goal.at("weight").is_number() && std::isfinite(goal.at("weight").get<double>()) && goal.at("weight")>=0 && goal.at("weight")<=1,"IK end orientation weight must be within0..1.");
        RuntimeIkEndOrientation orientation;
        orientation.rotation=rotation.get<std::array<double,4>>();orientation.weight=goal.at("weight").get<double>();
        const auto& q=orientation.rotation;
        const auto norm=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));
        require(std::isfinite(norm) && norm>0,"IK end orientation requires a nonzero finite quaternion norm.");
        result.end_orientation=canonical_ik_end_orientation(orientation);
    }
    return result;
}
std::vector<RuntimeTwoBoneIk> animation_ik_value(const Json& values) {
    require(values.is_array() && values.size()<=8,"AnimationRig IK permits at most eight chains.");
    std::vector<RuntimeTwoBoneIk> result;std::set<std::uint32_t> slots;
    for(const auto& value:values) {
        fields(value,{"slot","root","middle","end","target","pole","enabled","space","weight","end_orientation","metric","grounding","target_node"},{"slot","root","middle","end","target","pole"});
        const auto control=ik_control_value(value);require(slots.insert(control.slot).second,"Duplicate authored IK slot.");
        RuntimeTwoBoneIk chain;chain.slot=control.slot;chain.enabled=control.enabled;chain.space=control.space;chain.target=control.target;chain.pole=control.pole;chain.weight=control.weight;
        chain.end_orientation=control.end_orientation;
        if(value.contains("target_node")) {
            const auto node=revision(value.at("target_node"));
            require(node<10000,"IK target_node must be a model index 0..9999.");
            chain.target_node=static_cast<std::uint32_t>(node);
        }
        if(value.contains("grounding")) {
            const auto& g=value.at("grounding");
            fields(g,{"sole_offset","sole_up","sole_forward","reference_height","plant_height","lift_height","probe_above","probe_below","max_correction","max_slope_degrees","plant_lock","plant_release_height"},
                {"sole_offset","sole_up","sole_forward","reference_height"});
            RuntimeGroundingDefinition definition;
            auto vector=[&](const char* key) {
                require(g.at(key).is_array() && g.at(key).size()==3,"Grounding vectors require three numbers.");
                std::array<double,3> result{};
                for(std::size_t i=0;i<3;++i){require(g.at(key)[i].is_number(),"Grounding vectors require numbers.");result[i]=g.at(key)[i].get<double>();}
                return result;
            };
            definition.sole_offset=vector("sole_offset");definition.sole_up=vector("sole_up");definition.sole_forward=vector("sole_forward");
            auto number=[&](const char* key,double& output){if(g.contains(key)){require(g.at(key).is_number(),"Grounding parameters require numbers.");output=g.at(key).get<double>();}};
            number("reference_height",definition.reference_height);number("plant_height",definition.plant_height);number("lift_height",definition.lift_height);
            number("probe_above",definition.probe_above);number("probe_below",definition.probe_below);number("max_correction",definition.max_correction);number("max_slope_degrees",definition.max_slope_degrees);
            if(g.contains("plant_lock")) {
                require(g.at("plant_lock").is_boolean(),"Grounding plant_lock must be boolean.");
                definition.plant_lock=g.at("plant_lock").get<bool>();
            }
            if(g.contains("plant_release_height")) {
                require(g.at("plant_release_height").is_number(),"Grounding plant_release_height must be numeric.");
                definition.plant_release_height=g.at("plant_release_height").get<double>();
            }
            require(!value.contains("end_orientation"),"Grounding owns the end orientation; do not supply end_orientation.");
            try { grounding::validate(definition); }catch(const std::runtime_error& error){throw Error(-32602,error.what());}
            chain.grounding=definition;
        }
        if(value.contains("metric")) {
            require(value.at("metric").is_string() && (value.at("metric")=="uniform" || value.at("metric")=="near-uniform-v1"),
                "IK metric must be uniform or near-uniform-v1.");
            chain.metric_policy=value.at("metric")=="near-uniform-v1" ? RuntimeIkMetricPolicy::NearUniformV1 : RuntimeIkMetricPolicy::Uniform;
        }
        const auto root=revision(value.at("root")),middle=revision(value.at("middle")),end=revision(value.at("end"));
        require(root<10000 && middle<10000 && end<10000 && root!=middle && root!=end && middle!=end,"IK chain requires three distinct model indices0..9999.");
        chain.root=static_cast<std::uint32_t>(root);chain.middle=static_cast<std::uint32_t>(middle);chain.end=static_cast<std::uint32_t>(end);result.push_back(chain);
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.slot<b.slot;});return result;
}
RuntimeCharacterLocomotionProfile character_locomotion_value(const Json& value) {
    fields(value,{"step_height","ground_snap","max_slope_degrees"},{});
    for(const auto* key:{"step_height","ground_snap","max_slope_degrees"})if(value.contains(key))
        require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Character locomotion parameters must be finite numbers.");
    const double step=value.value("step_height",0.4),snap=value.value("ground_snap",0.5),slope=value.value("max_slope_degrees",45.0);
    require(step>=0 && step<=1 && snap>=0 && snap<=1 && slope>=0 && slope<=80,"Character locomotion parameters are outside their bounds.");
    RuntimeCharacterLocomotionProfile result{static_cast<float>(step),static_cast<float>(snap),static_cast<float>(slope)};
    require(std::isfinite(result.step_height) && result.step_height>=0 && result.step_height<=1 &&
        std::isfinite(result.ground_snap) && result.ground_snap>=0 && result.ground_snap<=1 &&
        std::isfinite(result.max_slope_degrees) && result.max_slope_degrees>=0 && result.max_slope_degrees<=80,
        "Character locomotion parameters are invalid at native precision.");
    return result;
}
RuntimeCharacterStanceProfile character_stance_value(const Json& value,double radius,double height) {
    fields(value,{"crouched_height","crouched_speed","camera_drop"},{});
    for(const auto* key:{"crouched_height","crouched_speed","camera_drop"})if(value.contains(key))
        require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Character stance parameters must be finite numbers.");
    const double crouched_height=value.value("crouched_height",1.0),speed=value.value("crouched_speed",2.0),drop=value.value("camera_drop",0.8);
    require(crouched_height>2*radius && crouched_height<height,"Crouched height must exceed the capsule diameter and be below standing height.");
    require(speed>0 && speed<=30,"Crouched speed must be within (0,30].");
    require(drop>=0 && drop<=height-crouched_height+1e-6,"Camera drop must fit the standing-to-crouched height difference.");
    RuntimeCharacterStanceProfile result{static_cast<float>(crouched_height),static_cast<float>(speed),static_cast<float>(drop)};
    require(result.crouched_height>2*static_cast<float>(radius) && result.crouched_height<static_cast<float>(height) &&
        result.crouched_speed>0 && result.crouched_speed<=30 && result.camera_drop>=0 &&
        static_cast<double>(result.camera_drop)<=static_cast<double>(static_cast<float>(height))-result.crouched_height+1e-6,
        "Character stance parameters are invalid at native precision.");
    return result;
}
RuntimeRootMotionDefinition root_motion_value(const Json& value) {
    fields(value,{"node","mode","actor","enabled","yaw"},{"node"});
    RuntimeRootMotionDefinition result;const auto node=revision(value.at("node"));
    require(node<10000,"Root-motion node must be a model index0..9999.");result.node=static_cast<std::uint32_t>(node);
    if(value.contains("mode")) {
        require(value.at("mode").is_string() && (value.at("mode")=="extract" || value.at("mode")=="character"),"Root-motion mode must be extract or character.");
        result.mode=value.at("mode")=="character" ? RootMotionMode::Character : RootMotionMode::Extract;
    }
    if(result.mode==RootMotionMode::Character) {
        require(value.contains("actor"),"Character root motion requires an actor.");result.actor=identifier(value.at("actor"));
    }else require(!value.contains("actor") || value.at("actor").is_null(),"Extract root motion requires an absent or null actor.");
    for(const auto* key:{"enabled","yaw"})if(value.contains(key))require(value.at(key).is_boolean(),"Root-motion enabled/yaw must be boolean.");
    if(value.contains("enabled"))result.enabled=value.at("enabled").get<bool>();
    if(value.contains("yaw"))result.yaw=value.at("yaw").get<bool>();
    return result;
}
RuntimeHitRegion hit_region_value(const Json& value) {
    require(value.is_object() && value.contains("shape") && value.at("shape").is_string(),"HitRegion requires a shape.");
    const auto shape=value.at("shape").get<std::string>();
    if(shape=="sphere")fields(value,{"owner","name","shape","center","enabled","radius"},{"owner","name","shape","radius"});
    else if(shape=="box")fields(value,{"owner","name","shape","center","enabled","half_extents"},{"owner","name","shape","half_extents"});
    else if(shape=="capsule")fields(value,{"owner","name","shape","center","enabled","radius","half_height"},{"owner","name","shape","radius","half_height"});
    else throw Error(-32602,"Unknown HitRegion shape.");
    RuntimeHitRegion result;result.owner=identifier(value.at("owner"));
    require(value.at("name").is_string(),"HitRegion name must be text.");result.name=value.at("name").get<std::string>();
    require(!result.name.empty() && result.name.size()<=64 && result.name[0]>='a' && result.name[0]<='z' &&
        result.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.-")==std::string::npos,"Invalid HitRegion name.");
    result.shape=shape=="sphere" ? HitRegionShape::Sphere : shape=="box" ? HitRegionShape::Box : HitRegionShape::Capsule;
    auto number=[](const Json& value,double minimum,double maximum) {
        require(value.is_number() && std::isfinite(value.get<double>()) && value.get<double>()>=minimum && value.get<double>()<=maximum,"HitRegion dimension/center is out of bounds.");return value.get<double>();
    };
    if(value.contains("center")) {
        const auto& center=value.at("center");require(center.is_array() && center.size()==3,"HitRegion center needs three coordinates.");
        for(std::size_t k=0;k<3;++k)result.center[k]=number(center[k],-10000,10000);
    }
    if(value.contains("enabled")) {require(value.at("enabled").is_boolean(),"HitRegion enabled must be boolean.");result.enabled=value.at("enabled").get<bool>();}
    if(shape=="box") {
        const auto& extent=value.at("half_extents");require(extent.is_array() && extent.size()==3,"HitRegion box needs three half extents.");
        for(std::size_t k=0;k<3;++k)result.half_extents[k]=number(extent[k],.001,10000);
    }else result.radius=number(value.at("radius"),.001,10000);
    if(shape=="capsule")result.half_height=number(value.at("half_height"),0,10000);
    return result;
}
SceneLens scene_lens_value(const Json& value) {
    fields(value,{"vertical_fov","near","far"},{"vertical_fov","near","far"});
    for(const auto* key:{"vertical_fov","near","far"})
        require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Invalid camera lens number.");
    SceneLens result{value.at("vertical_fov"),value.at("near"),value.at("far")};
    require(result.vertical_fov>=5 && result.vertical_fov<=150,"Camera field of view must be 5..150 degrees.");
    require(result.near_plane>=.001 && result.far_plane<=1e7 && result.far_plane>result.near_plane,
        "Camera needs 0.001 <= near < far <= 10000000.");
    return result;
}
ViewmodelProjection viewmodel_projection_value(const Json& camera) {
    if(camera.value("viewmodel_world_depth",false))return ViewmodelProjection::WorldDepth;
    if(camera.value("viewmodel_match_world_fov",false))return ViewmodelProjection::MatchWorldFov;
    return ViewmodelProjection::Independent;
}
CameraDrawBinding camera_draw_value(const Json& value) {
    fields(value,{"camera","mode","scene_anchored"},{"camera","mode"});
    const auto camera=identifier(value.at("camera"));require(camera!=std::string(32,'0'),"CameraDraw camera cannot be zero.");
    require(value.at("mode").is_string() && (value.at("mode")=="viewmodel" || value.at("mode")=="hidden"),
        "CameraDraw mode must be viewmodel or hidden.");
    const auto anchored=value.value("scene_anchored",Json(false));
    require(anchored.is_boolean() && (anchored==false || value.at("mode")=="viewmodel"),
        "Scene-anchored CameraDraw requires a Boolean and viewmodel mode.");
    return {camera,value.at("mode")=="viewmodel" ? CameraDrawMode::Viewmodel : CameraDrawMode::Hidden,
        anchored.get<bool>()};
}
static void camera_draw_references(const Json& entities) {
    std::map<std::pair<std::string,CameraDrawMode>,std::size_t> counts;
    for(const auto& [id,entity]:entities.items()) {
        const auto& bag=entity.at("components");if(!bag.contains("CameraDraw"))continue;
        const auto binding=camera_draw_value(bag.at("CameraDraw"));
        require(int(bag.contains("MeshRenderer"))+int(bag.contains("StaticMesh"))+int(bag.contains("SkinnedMesh"))==1,
            "CameraDraw requires exactly one drawable component.");
        require(entities.contains(binding.camera) && entities.at(binding.camera).at("components").contains("Camera"),
            "CameraDraw camera does not exist.");
        require(++counts[{binding.camera,binding.mode}]<=32,"CameraDraw permits at most 32 bindings per mode per camera, including invisible meshes.");
        if(binding.mode==CameraDrawMode::Viewmodel) {
            require(entities.at(binding.camera).at("components").at("Camera").contains("viewmodel"),"Viewmodel CameraDraw requires a configured camera lens.");
            if(binding.scene_anchored) {
                require(id!=binding.camera,"A Camera cannot be its own viewmodel.");
                continue;
            }
            auto ancestor=entity.at("parent").is_null() ? std::string{} : identifier(entity.at("parent"));
            while(!ancestor.empty() && ancestor!=binding.camera) {
                const auto& parent=entities.at(ancestor).at("parent");ancestor=parent.is_null() ? std::string{} : identifier(parent);
            }
            require(ancestor==binding.camera,"Viewmodel drawable must be a strict descendant of its camera.");
        }
    }
}
ViewmodelClearanceProfile viewmodel_clearance_value(const Json& value) {
    fields(value,{"position","rotation","radius","half_height","max_retraction","skin"},
        {"position","rotation","radius","half_height","max_retraction","skin"});
    ViewmodelClearanceProfile profile;
    const auto vector=[&](const char* name,auto& target) {
        const auto& array=value.at(name);require(array.is_array() && array.size()==target.size(),"Clearance vector has an invalid size.");
        for(std::size_t i=0;i<target.size();++i) {
            require(array[i].is_number(),"Clearance vector must be numeric.");target[i]=array[i].get<double>();
        }
    };
    vector("position",profile.position);vector("rotation",profile.rotation);
    for(const char* name:{"radius","half_height","max_retraction","skin"})require(value.at(name).is_number(),"Clearance scalar must be numeric.");
    profile.radius=value.at("radius").get<double>();profile.half_height=value.at("half_height").get<double>();
    profile.max_retraction=value.at("max_retraction").get<double>();profile.skin=value.at("skin").get<double>();
    try{validate_viewmodel_clearance_profile(profile);}catch(const std::exception& error){throw Error(-32602,error.what());}
    return profile;
}
physical_rig::Configuration physical_rig_value(const Json& value) {
    try { return physical_rig::parse_configuration(value); }
    catch(const std::exception& error) { throw Error(-32602,error.what()); }
}
double surface_mark_scalar(const Json& value,double low,double high) {
    require(value.is_number(),"Surface mark values must be numeric.");
    const auto number=value.get<double>();
    require(std::isfinite(number) && number>=low && number<=high,"Surface mark value is outside its range.");
    return number;
}
SceneSurfaceMark surface_mark_style(const Json& row) {
    const auto& half_extent=row.at("half_extent_m");
    const auto& color=row.at("linear_rgb");
    require(half_extent.is_array() && half_extent.size()==2 && color.is_array() && color.size()==3,
        "Surface marks require two half extents and three linear color channels.");
    SceneSurfaceMark style;
    for(std::size_t i=0;i<2;++i)style.half_extent_m[i]=static_cast<float>(surface_mark_scalar(half_extent[i],.005,4));
    for(std::size_t i=0;i<3;++i)style.linear_rgb[i]=static_cast<float>(surface_mark_scalar(color[i],0,1));
    style.half_depth_m=static_cast<float>(surface_mark_scalar(row.at("half_depth_m"),.001,.25));
    style.minimum_normal_dot=static_cast<float>(surface_mark_scalar(row.at("minimum_normal_dot"),0,1));
    style.edge_fraction=static_cast<float>(surface_mark_scalar(row.at("edge_fraction"),0,1));
    style.opacity=static_cast<float>(surface_mark_scalar(row.at("opacity"),0,1));
    style.roughness=static_cast<float>(surface_mark_scalar(row.at("roughness"),.045,1));
    if(row.contains("relief_depth_m"))
        style.relief_depth_m=static_cast<float>(surface_mark_scalar(row.at("relief_depth_m"),
            0,surface_mark_validation::max_authored_relief_depth_m));
    return style;
}
std::string surface_mark_profile_id(const Json& value) {
    auto profile=identifier(value);
    require(profile!=std::string(32,'0'),"Surface mark profile identity cannot be zero.");
    return profile;
}
std::uint32_t surface_mark_lifetime(const Json& value) {
    const auto lifetime=revision(value);
    require(lifetime>0 && lifetime<=max_surface_mark_lifetime_ticks,"Surface mark lifetime must be 1..216000 ticks.");
    return static_cast<std::uint32_t>(lifetime);
}
RuntimeFootprintSurface footprint_surface_value(const Json& value) {
    fields(value,{"half_extent_m","half_depth_m","minimum_normal_dot","edge_fraction","linear_rgb","opacity",
        "roughness","lifetime_ticks","profile","sole_tolerance_m","relief_depth_m"},
        {"half_extent_m","half_depth_m","minimum_normal_dot","edge_fraction","linear_rgb","opacity","roughness","lifetime_ticks"});
    RuntimeFootprintSurface surface;
    surface.style=surface_mark_style(value);
    surface.lifetime_ticks=surface_mark_lifetime(value.at("lifetime_ticks"));
    if(value.contains("profile"))surface.profile=surface_mark_profile_id(value.at("profile"));
    if(value.contains("sole_tolerance_m"))surface.sole_tolerance_m=surface_mark_scalar(value.at("sole_tolerance_m"),.001,.1);
    return surface;
}
wetness::Profile surface_wetness_value(const Json& value) {
    const std::initializer_list<const char*> required{"capacity_nm","retention_q16","drying_nm_per_second",
        "probe_local_m","darkening","roughness_factor"};
    fields(value,required,required);
    const auto integer=[&](const char* key,std::uint32_t minimum,std::uint32_t maximum) {
        const auto& number=value.at(key);
        require(number.is_number_integer() && number>=minimum && number<=maximum,
            std::string("SurfaceWetness ")+key+" must be an integer in its allowed range.");
        return number.get<std::uint32_t>();
    };
    const auto bounded=[](const Json& number,double minimum,double maximum) {
        require(number.is_number() && std::isfinite(number.get<double>()) && number>=minimum && number<=maximum,
            "SurfaceWetness probe or material response is outside its numeric range.");
        return number.get<double>();
    };
    wetness::Profile profile;
    profile.capacity_nm=integer("capacity_nm",1,wetness::max_capacity_nm);
    profile.retention_q16=integer("retention_q16",0,wetness::units_per_nm);
    profile.drying_nm_per_second=integer("drying_nm_per_second",0,profile.capacity_nm);
    const auto& probe=value.at("probe_local_m");
    require(probe.is_array() && probe.size()==profile.probe_local_m.size(),
        "SurfaceWetness probe_local_m requires three numbers.");
    for(std::size_t axis=0;axis<profile.probe_local_m.size();++axis)
        profile.probe_local_m[axis]=bounded(probe.at(axis),-wetness::probe_bound_m,wetness::probe_bound_m);
    profile.darkening=static_cast<float>(bounded(value.at("darkening"),0,1));
    profile.roughness_factor=static_cast<float>(bounded(value.at("roughness_factor"),0,1));
    return profile;
}
FoliageMotion foliage_motion_value(const Json& value) {
    fields(value,{"root_y","tip_y","wind","touch"},{"root_y","tip_y"});
    const auto number=[](const Json& field) {
        require(field.is_number() && std::isfinite(field.get<double>()),"Foliage motion requires finite numbers.");
        return field.get<double>();
    };
    FoliageMotion motion;
    motion.root_y=number(value.at("root_y"));
    motion.tip_y=number(value.at("tip_y"));
    if(value.contains("wind")) {
        const auto& wind=value.at("wind");
        fields(wind,{"direction","tip_m","period_ticks","wavelength_m"},{"direction","tip_m","period_ticks","wavelength_m"});
        const auto& direction=wind.at("direction");
        require(direction.is_array() && direction.size()==2,"Foliage wind direction needs two XZ coefficients.");
        const auto& ticks=wind.at("period_ticks");
        require(ticks.is_number_integer() && ticks>=2 && ticks<=3600,"Foliage wind period_ticks must be an integer in [2,3600].");
        motion.wind=FoliageWindMotion{{number(direction.at(0)),number(direction.at(1))},
            number(wind.at("tip_m")),ticks.get<std::uint32_t>(),number(wind.at("wavelength_m"))};
    }
    if(value.contains("touch")) {
        const auto& touch=value.at("touch");
        fields(touch,{"actor","radius_m","tip_m"},{"actor","radius_m","tip_m"});
        motion.touch=FoliageTouchMotion{identifier(touch.at("actor")),number(touch.at("radius_m")),number(touch.at("tip_m"))};
    }
    component_checked([&]{validate_foliage_motion(motion);});
    return motion;
}
static const Json* authored_foliage_motion(const Json& components) {
    const auto mesh=components.find("StaticMesh");
    if(mesh==components.end())return nullptr;
    const auto motion=mesh->find("foliage_motion");
    return motion==mesh->end() ? nullptr : &*motion;
}
static void validate_template_foliage_motion(const Json& components) {
    const auto* motion=authored_foliage_motion(components);
    require(!motion || !motion->contains("touch"),"Foliage touch requires an authored world drawable outside the spawn catalog.");
}
FocusTarget focus_target_value(const Json& value) {
    fields(value,{"entity","local_offset_m"},{"entity"});
    FocusTarget target{identifier(value.at("entity"))};
    if(value.contains("local_offset_m")) {
        const auto& offset=value.at("local_offset_m");
        require(offset.is_array() && offset.size()==3,"Camera focus target needs a three-axis offset in metres.");
        for(std::size_t axis=0;axis<target.local_offset_m.size();++axis) {
            require(offset.at(axis).is_number(),"Camera focus target offsets must be numeric.");
            target.local_offset_m[axis]=offset.at(axis).get<double>();
        }
    }
    component_checked([&]{validate_focus_target(target);});
    return target;
}
void validate_component(const std::string& type, const Json& value) {
    if(type=="PhysicalRig") { (void)physical_rig_value(value);return; }
    if(type=="HitRegion") { (void)hit_region_value(value);return; }
    if(type=="FootprintSurface") { (void)footprint_surface_value(value);return; }
    if(type=="SurfaceWetness") { (void)surface_wetness_value(value);return; }
    if(type=="MeshRenderer" || type=="StaticMesh" || type=="SkinnedMesh")
        for(const auto* key:{"cast_shadows","receive_shadows"})if(value.contains(key))
            require(value.at(key).is_boolean(),"Mesh shadow controls must be boolean.");
    if(type=="AnimationRig") {
        fields(value,{"asset","clip","time","speed","loop","playing","layers","ik","root_motion"},{"asset","clip","time","speed","loop","playing"});
        require(value.at("asset").is_string() && valid_asset_id(value.at("asset").get<std::string>()),"AnimationRig requires a model content hash.");
        (void)animation_value(value);if(value.contains("layers"))(void)animation_layers_value(value.at("layers"));if(value.contains("ik"))(void)animation_ik_value(value.at("ik"));if(value.contains("root_motion"))(void)root_motion_value(value.at("root_motion"));return;
    }
    if(type=="RigNode") {
        fields(value,{"rig","node"},{"rig","node"});identifier(value.at("rig"));require(revision(value.at("node"))<10000,"RigNode index must be 0..9999.");return;
    }
    if(type=="SkinnedMesh") {
        fields(value,{"asset","primitive","visible","rig","node","cast_shadows","receive_shadows"},{"asset","primitive","visible","rig","node"});
        require(value.at("asset").is_string() && valid_asset_id(value.at("asset").get<std::string>()),"SkinnedMesh requires a model content hash.");
        identifier(value.at("rig"));require(revision(value.at("node"))<10000 && revision(value.at("primitive"))<10000 && value.at("visible").is_boolean(),"Invalid skinned mesh node/primitive/visibility.");return;
    }
    if(type=="AcousticMaterial") { (void)acoustic_value(value);return; }
    if(type=="AudioRoom") { (void)audio_room_value(value);return; }
    if(type=="AudioPortal") { (void)audio_portal_value(value);return; }
    if(type=="AudioEmitter") { (void)emitter_value(value);return; }
    if(type=="Light") { (void)light_value(value);return; }
    if(type=="LightingEnvironment") { (void)environment_value(value);return; }
    if (type == "Transform") { validate_transform(value); return; }
    if (type == "Camera") {
        fields(value,{"vertical_fov","near","far","viewmodel","viewmodel_clearance","viewmodel_match_world_fov","viewmodel_world_depth","focus_target"},{"vertical_fov","near","far"});
        (void)scene_lens_value(Json{{"vertical_fov",value.at("vertical_fov")},{"near",value.at("near")},{"far",value.at("far")}});
        if(value.contains("focus_target"))(void)focus_target_value(value.at("focus_target"));
        if(value.contains("viewmodel"))(void)scene_lens_value(value.at("viewmodel"));
        for(const auto* option:{"viewmodel_match_world_fov","viewmodel_world_depth"})
            if(value.contains(option))require(value.contains("viewmodel") && value.at(option).is_boolean(),
                "Viewmodel projection and depth options require a foreground lens and boolean values.");
        if(value.contains("viewmodel_clearance")) {
            require(value.contains("viewmodel"),"Viewmodel clearance requires a foreground lens.");
            (void)viewmodel_clearance_value(value.at("viewmodel_clearance"));
        }
        return;
    }
    if(type=="CameraDraw") { (void)camera_draw_value(value);return; }
    if (type == "MeshRenderer") {
        fields(value, {"primitive", "albedo", "visible", "cast_shadows", "receive_shadows"}, {"primitive", "albedo", "visible"});
        require(value.at("primitive") == "box" && value.at("visible").is_boolean(), "MeshRenderer currently supports box primitives and boolean visibility.");
        const auto& color = value.at("albedo");
        require(color.is_array() && color.size() == 3, "Albedo needs three linear RGB values.");
        for (const auto& item : color) require(item.is_number() && std::isfinite(item.get<double>()) && item >= 0 && item <= 1, "Albedo must be in [0, 1].");
        return;
    }
    if(type=="StaticMesh") {
        fields(value,{"asset","primitive","visible","cast_shadows","receive_shadows","lods","foliage_motion"},{"asset","primitive","visible"});
        require(value.at("asset").is_string() && valid_asset_id(value.at("asset").get<std::string>()),"StaticMesh requires a 64-character lowercase content hash.");
        require(revision(value.at("primitive"))<10000 && value.at("visible").is_boolean(),"Invalid StaticMesh primitive/visibility.");
        if(value.contains("lods")) {
            const auto& lods=value.at("lods");
            require(lods.is_array() && !lods.empty() && lods.size()<=max_mesh_lod_levels,"StaticMesh.lods requires 1..4 alternate levels.");
            double previous=0;
            for(const auto& lod:lods) {
                fields(lod,{"asset","primitive","min_distance_m"},{"asset","primitive","min_distance_m"});
                require(lod.at("asset").is_string() && valid_asset_id(lod.at("asset").get_ref<const std::string&>()),"Mesh LOD requires a 64-character lowercase content hash.");
                require(revision(lod.at("primitive"))<10000,"Invalid Mesh LOD primitive index.");
                require(lod.at("min_distance_m").is_number(),"Mesh LOD min_distance_m must be numeric.");
                const auto distance=lod.at("min_distance_m").get<double>();
                require(std::isfinite(distance) && distance>previous && distance<=max_mesh_lod_distance_m,
                    "Mesh LOD distances must be strictly increasing in (0, 1000000] meters.");
                previous=distance;
            }
        }
        if(value.contains("foliage_motion"))(void)foliage_motion_value(value.at("foliage_motion"));
        return;
    }
    if(type=="PbrTextures") {
        fields(value,{"base_color","metallic_roughness","emissive","occlusion","normal","occlusion_strength","normal_scale"});
        for(const auto* key:{"base_color","metallic_roughness","emissive","occlusion","normal"})if(value.contains(key) && !value.at(key).is_null()) {
            const auto& map=value.at(key);fields(map,{"asset","image","wrap_s","wrap_t","min_filter","mag_filter"},{"asset"});
            require(map.at("asset").is_string() && valid_asset_id(map.at("asset").get<std::string>()),"Texture map requires an asset content hash.");
            if(map.contains("image"))require(revision(map.at("image"))<256,"Model image index must be 0..255.");
            for(const auto* field:{"wrap_s","wrap_t","min_filter","mag_filter"})if(map.contains(field))require(map.at(field).is_number_integer() && map.at(field)>=0 && map.at(field)<=33648,"Invalid sampler integer.");
            TextureMap sampler;sampler.wrap_s=map.value("wrap_s",10497);sampler.wrap_t=map.value("wrap_t",10497);sampler.min_filter=map.value("min_filter",9987);sampler.mag_filter=map.value("mag_filter",9729);
            require(valid_texture_sampler(sampler),"Unsupported texture sampler enum.");
        }
        for(const auto* key:{"occlusion_strength","normal_scale"})if(value.contains(key))require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()) && value.at(key)>=0 && value.at(key)<=(std::string_view(key)=="normal_scale" ? 16 : 1),"Texture strength/scale out of range.");
        return;
    }
    if(type=="PbrMaterial") {
        fields(value,{"base_color","emissive","metallic","roughness","double_sided","alpha_mode","opacity","alpha_cutoff"},{"base_color","emissive","metallic","roughness","double_sided"});
        for(const auto* key:{"base_color","emissive"}) {
            require(value.at(key).is_array() && value.at(key).size()==3,"Material needs three color components.");
            for(const auto& x:value.at(key))require(x.is_number() && std::isfinite(x.get<double>()) && x>=0 && x<=1,"Material colors must be in [0,1].");
        }
        for(const auto* key:{"metallic","roughness"})require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()) && value.at(key)>=0 && value.at(key)<=1,"Material factors must be in [0,1].");
        require(value.at("double_sided").is_boolean(),"double_sided must be boolean.");
        if(value.contains("alpha_mode"))require(value.at("alpha_mode").is_string() && (value.at("alpha_mode")=="opaque" || value.at("alpha_mode")=="mask"),"alpha_mode must be opaque or mask.");
        if(value.contains("opacity"))require(value.at("opacity").is_number() && std::isfinite(value.at("opacity").get<double>()) && value.at("opacity")>=0 && value.at("opacity")<=1,"opacity must be in [0,1].");
        if(value.contains("alpha_cutoff"))require(value.at("alpha_cutoff").is_number() && std::isfinite(value.at("alpha_cutoff").get<double>()) && value.at("alpha_cutoff")>=0 && value.at("alpha_cutoff")<=max_material_alpha_cutoff,"alpha_cutoff must be in [0,1000000].");return;
    }
    if (type == "MeshCollider") {
        fields(value,{"asset","primitive","friction","restitution","surface"},{"asset","primitive","friction","restitution"});
        if(value.contains("surface"))require(value.at("surface").is_string() &&
            valid_collision_surface(value.at("surface").get_ref<const std::string&>()),"Invalid collision surface name.");
        require(value.at("asset").is_string() && valid_asset_id(value.at("asset").get<std::string>()),"MeshCollider requires a 64-character lowercase content hash.");
        require(revision(value.at("primitive"))<10000,"Invalid MeshCollider primitive index.");
        for(const auto* key:{"friction","restitution"})require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Invalid mesh collider material.");
        require(value.at("friction")>=0 && value.at("friction")<=2 && value.at("restitution")>=0 && value.at("restitution")<=1,"Mesh collider material out of range.");
        return;
    }
    if (type == "BoxCollider") {
        fields(value, {"half_extents", "motion", "mass", "friction", "restitution", "navigation_obstacle", "surface", "buoyancy"}, {"half_extents", "motion", "mass", "friction", "restitution"});
        if(value.contains("surface"))require(value.at("surface").is_string() &&
            valid_collision_surface(value.at("surface").get_ref<const std::string&>()),"Invalid collision surface name.");
        const auto& extent=value.at("half_extents");
        require(extent.is_array() && extent.size()==3,"BoxCollider needs three half extents.");
        for(const auto& v:extent) require(v.is_number() && std::isfinite(v.get<double>()) && v>=0.001 && v<=10000,"Collider half extents must be 0.001..10000 meters.");
        require(value.at("motion")=="static" || value.at("motion")=="dynamic" || value.at("motion")=="kinematic","Collider motion must be static, dynamic or kinematic.");
        if (value.contains("buoyancy")) {
            const auto& settings=value.at("buoyancy");
            fields(settings,{"linear_drag","angular_drag"});
            require(value.at("motion")=="dynamic","Buoyancy requires a dynamic BoxCollider.");
            require(value.at("mass").is_number() && value.at("mass")>=.001,
                "Buoyancy requires mass of at least 1 g.");
            for (const auto& drag : settings)
                require(drag.is_number() && drag>=0 && drag<=2,"Buoyancy drag coefficients must be in [0,2].");
        }
        if(value.contains("navigation_obstacle")) {
            require(value.at("navigation_obstacle").is_boolean(),"BoxCollider navigation_obstacle must be Boolean.");
            require(!value.at("navigation_obstacle").get<bool>() || value.at("motion")!="dynamic","Navigation obstacles must be static or kinematic boxes.");
        }
        for(const auto* key:{"mass","friction","restitution"}) require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Invalid collider parameter.");
        require(value.at("mass")>0 && value.at("mass")<=1e6 && value.at("friction")>=0 && value.at("friction")<=2 && value.at("restitution")>=0 && value.at("restitution")<=1,"Collider material/mass out of range.");
        return;
    }
    if (type == "ParticleEmitter") {
        try { (void)parse_particle_emitter(value); }
        catch (const std::invalid_argument& error) { throw Error(-32602, error.what()); }
        return;
    }
    if (type == "CharacterController") {
        fields(value, {"radius","height","speed","jump_speed","camera","stance","locomotion"}, {"radius","height","speed","jump_speed","camera"});
        if(!value.at("camera").is_null())identifier(value.at("camera"));
        for(const auto* key:{"radius","height","speed","jump_speed"}) require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Invalid controller parameter.");
        require(value.at("radius")>=0.05 && value.at("radius")<=2 && value.at("height")>2*value.at("radius").get<double>() && value.at("height")<=4,"Invalid capsule dimensions.");
        require(value.at("speed")>0 && value.at("speed")<=30 && value.at("jump_speed")>=0 && value.at("jump_speed")<=20,"Controller speed out of range.");
        if(value.contains("stance"))(void)character_stance_value(value.at("stance"),value.at("radius").get<double>(),value.at("height").get<double>());
        if(value.contains("locomotion"))(void)character_locomotion_value(value.at("locomotion"));
        return;
    }
    throw Error(-32602, "Unknown component type.");
}
std::map<std::string, Matrix4> world_matrices(const Json& entities) {
    std::map<std::string, Matrix4> result;
    for (const auto& [id, unused] : entities.items()) {
        (void)unused;
        std::vector<std::string> chain;
        auto current = id;
        while (!current.empty() && !result.contains(current)) {
            chain.push_back(current);
            const auto& parent = entities.at(current).at("parent");
            current = parent.is_null() ? "" : parent.get<std::string>();
        }
        auto matrix = current.empty() ? identity_matrix() : result.at(current);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto& t = entities.at(*it).at("components").at("Transform");
            matrix = multiply(matrix, local_matrix(t.at("position").get<std::array<double,3>>(),
                t.at("rotation").get<std::array<double,4>>(), t.at("scale").get<std::array<double,3>>()));
            result.emplace(*it, matrix);
        }
    }
    return result;
}
static void validate_particle_pose(const Matrix4& world, const std::string& id) {
    require(rigid_transform(world) && pose_frames::axes(world).basis == pose_frames::Basis::regular,
        "Particle emitter " + id + " requires a rigid, proper world transform.");
    require(std::all_of(world.begin() + 12, world.begin() + 15, [](double value) {
        return std::isfinite(value) && std::abs(value) <= 1e9;
    }), "Particle emitter " + id + " exceeds the world position bound.");
}
bool custom_component(const std::string& type) { return type.starts_with("game:"); }
std::string custom_type(const std::string& type) {
    require(custom_component(type),"Custom component type needs game: prefix.");return identifier(type.substr(5));
}
std::vector<components::Schema> authored_component_schemas(const Json& doc) {
    if(!doc.contains("component_schemas"))return {};
    const auto& registry=doc.at("component_schemas");require(registry.is_object() && registry.size()<=components::max_types,"Invalid authored component schema registry.");
    Json schemas=Json::array();for(const auto& [id,value]:registry.items()) { require(value.is_object() && value.contains("id") && value.at("id")==id,"Component schema registry key differs from its identity.");schemas.push_back(value); }
    return component_checked([&]{return components::parse_manifest(Json{{"format","poima.components"},{"version",1},{"schemas",schemas}}.dump());});
}
const components::Schema& authored_schema(const std::vector<components::Schema>& schemas,const std::string& id) {
    const auto found=std::lower_bound(schemas.begin(),schemas.end(),id,[](const auto& schema,const auto& key){return schema.id<key;});
    require(found!=schemas.end() && found->id==id,"Custom component type is not registered.");return *found;
}
static bool authored_reference_exists(void* context,PoimaEntityId entity) { return static_cast<const Json*>(context)->contains(gameplay_id(entity)); }
Json history_state(const Json& doc) {
    Json result={{"entities",doc.at("entities")},{"component_schemas",doc.value("component_schemas",Json::object())},{"templates",doc.value("templates",Json::object())},{"ui",doc.value("ui",Json::object())}};
    if(doc.contains("navigation"))result["navigation"]=doc.at("navigation");
    if(doc.contains("water"))result["water"]=doc.at("water");
    if(doc.contains("snow"))result["snow"]=doc.at("snow");
    if(doc.contains("physics_checkpoint"))result["physics_checkpoint"]=doc.at("physics_checkpoint");
    if(doc.contains("asset_provenance"))result["asset_provenance"]=doc.at("asset_provenance");
    if(doc.contains("surface_mark_profiles"))result["surface_mark_profiles"]=doc.at("surface_mark_profiles");
    if(doc.contains("audio_buses"))result["audio_buses"]=doc.at("audio_buses");
    return result;
}
bool optional_authored_value_differs(const Json& a,const Json& b,const char* key) {
    const auto left=a.find(key),right=b.find(key);
    return left==a.end() ? right!=b.end() : right==b.end() || *left!=*right;
}
bool same_authored_state(const Json& a,const Json& b) {
    if(a.at("entities")!=b.at("entities"))return false;
    for(const auto* key:{"asset_provenance","navigation","water","snow","physics_checkpoint","surface_mark_profiles","audio_buses"}) {
        if(optional_authored_value_differs(a,b,key))return false;
    }
    for(const auto* key:{"component_schemas","templates","ui"}) {
        const auto left=a.find(key),right=b.find(key);
        if(left==a.end()) { if(right!=b.end() && !right->empty())return false; }
        else if(right==b.end() ? !left->empty() : *left!=*right)return false;
    }
    return true;
}
void upgrade_components(Json& doc) {
    if(doc.at("version")==1) {doc["version"]=2;doc["component_schemas"]=Json::object();doc["retired_component_schemas"]=Json::array();}
}
void upgrade_templates(Json& doc) {
    upgrade_components(doc);
    if(doc.at("version")==2) {doc["version"]=3;doc["templates"]=Json::object();doc["retired_template_ids"]=Json::array();}
}
void upgrade_ui(Json& doc) {
    upgrade_templates(doc);
    if(doc.at("version")==3) {doc["version"]=4;doc["ui"]=Json::object();doc["retired_ui_ids"]=Json::array();}
}
static bool template_component(const std::string& type) {
    return type=="Transform" || type=="BoxCollider" || type=="MeshRenderer" || type=="StaticMesh" || type=="PbrMaterial" || type=="PbrTextures" || type=="ParticleEmitter" || custom_component(type);
}
Json template_document(const Json& document,const Json& entities) {
    Json result={{"format","poima.authored-world"},{"version",2},{"world_id",document.at("world_id")},
        {"revision",document.at("revision")},{"entities",entities},{"retired_ids",Json::array()},
        {"receipts",Json::array()},{"component_schemas",document.value("component_schemas",Json::object())},
        {"retired_component_schemas",Json::array()}};
    if(document.contains("audio_buses"))result["audio_buses"]=document.at("audio_buses");
    if(document.contains("physics_checkpoint"))result["physics_checkpoint"]=document.at("physics_checkpoint");
    return result;
}
static void validate_template_graph(const Json& document,const Json& recipe,
    const std::vector<components::Schema>& schemas,std::size_t& payload_bytes) {
    fields(recipe,{"name","root","entities"},{"name","root","entities"});validate_name(recipe.at("name"));
    const auto root=identifier(recipe.at("root"));require(root!=std::string(32,'0'),"Template root cannot be zero.");
    const auto& members=recipe.at("entities");
    require(members.is_object() && !members.empty() && members.size()<=max_runtime_template_entities,
        "Hierarchical template requires 1..1024 local entities.");
    require(members.contains(root),"Template root does not name a local entity.");
    auto validation=members;
    for(const auto& [id,member]:members.items()) {
        require(identifier(id)!=std::string(32,'0'),"Template local identity cannot be zero.");
        fields(member,{"name","parent","components"},{"name","parent","components"});
        require(id==root ? member.at("parent").is_null() : !member.at("parent").is_null(),
            "Template requires exactly one connected root.");
        if(!member.at("parent").is_null())require(members.contains(identifier(member.at("parent"))),"Template parent must be local.");
        const auto& bag=member.at("components");require(bag.is_object(),"Template member components must be an object.");
        require(!bag.contains("FootprintSurface"),"FootprintSurface requires an authored world receiver outside the spawn catalog.");
        require(!bag.contains("SurfaceWetness"),"SurfaceWetness requires an authored world receiver outside the spawn catalog.");
        require(!bag.contains("ParticleEmitter") || !bag.at("ParticleEmitter").contains("rain"),
            "Rain emitters require an authored world entity outside the spawn catalog.");
        for(const auto& [type,value]:bag.items())if(!custom_component(type))validate_component(type,value);
        validate_template_foliage_motion(bag);
        if(bag.contains("BoxCollider"))require(!bag.at("BoxCollider").value("navigation_obstacle",false),"Navigation obstacles require authored world entities.");
        auto local_reference=[&](const Json& target,const char* expected) {
            if(target.is_null())return;
            const auto value=identifier(target);require(members.contains(value),"Template native entity reference must be local.");
            require(members.at(value).at("components").contains(expected),"Template native reference targets the wrong component.");
        };
        if(bag.contains("CharacterController"))local_reference(bag.at("CharacterController").at("camera"),"Camera");
        if(bag.contains("CameraDraw"))local_reference(bag.at("CameraDraw").at("camera"),"Camera");
        if(bag.contains("Camera") && bag.at("Camera").contains("focus_target")) {
            const auto target=focus_target_value(bag.at("Camera").at("focus_target"));
            require(target.entity!=id && members.contains(target.entity),"Template camera focus target must reference another local entity.");
        }
        if(bag.contains("RigNode"))local_reference(bag.at("RigNode").at("rig"),"AnimationRig");
        if(bag.contains("AnimationRig") && bag.at("AnimationRig").contains("root_motion") && bag.at("AnimationRig").at("root_motion").contains("actor"))
            local_reference(bag.at("AnimationRig").at("root_motion").at("actor"),"CharacterController");
        if(bag.contains("PhysicalRig") && bag.at("PhysicalRig").contains("controller_handoff"))
            local_reference(bag.at("PhysicalRig").at("controller_handoff").at("actor"),"CharacterController");
        if(bag.contains("SkinnedMesh"))local_reference(bag.at("SkinnedMesh").at("rig"),"AnimationRig");
        if(bag.contains("LightingEnvironment")) {
            const auto& environment=bag.at("LightingEnvironment");
            if(environment.contains("sky"))local_reference(environment.at("sky").at("sun"),"Light");
            if(environment.contains("screen_contact_shadows"))
                local_reference(environment.at("screen_contact_shadows").at("light"),"Light");
            if(environment.contains("height_fog") && environment.at("height_fog").contains("spot_lights"))
                for(const auto& spot:environment.at("height_fog").at("spot_lights"))
                    local_reference(spot.at("entity"),"Light");
        }
        for(const auto& [type,value]:bag.items())if(custom_component(type)) {
            const auto& schema=authored_schema(schemas,custom_type(type));
            const auto payload=component_checked([&]{return components::parse_values(schema,value.dump());});
            require(payload.size()<=max_runtime_template_payload_bytes-payload_bytes,"Template custom payload budget exceeded.");
            payload_bytes+=payload.size();
            // Native graph validation must not apply world liveness to local
            // or explicitly preserved external custom handles. Final candidate
            // liveness belongs to atomic instantiation, as with legacy recipes.
            validation[id]["components"].erase(type);
        }
    }
    // Reuses ordinary structural/native component validation without recursive
    // catalogs, borrowed definitions or collision between world and local IDs.
    validate(template_document(document,validation));
}
void validate_physics_checkpoint_profile(const Json& profile) {
    fields(profile,{"version"},{"version"});
    require(profile.at("version").is_number_integer() && profile.at("version")==1,
        "Physics checkpoint profile requires integer version 1.");
}
void validate(const Json& doc) {
    require(doc.is_object() && doc.contains("version") && doc.at("version").is_number_integer(),"Unsupported world format/version.");
    const bool user_interface=doc.at("version")==4,catalog=user_interface || doc.at("version")==3,custom=catalog || doc.at("version")==2;
    if(user_interface)fields(doc,{"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","templates","retired_template_ids","ui","retired_ui_ids","navigation","asset_provenance","water","snow","physics_checkpoint","surface_mark_profiles","audio_buses"},
        {"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","templates","retired_template_ids","ui","retired_ui_ids"});
    else if(catalog)fields(doc,{"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","templates","retired_template_ids","navigation","asset_provenance","water","snow","physics_checkpoint","surface_mark_profiles","audio_buses"},
        {"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","templates","retired_template_ids"});
    else if(custom)fields(doc,{"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","navigation","asset_provenance","water","snow","physics_checkpoint","surface_mark_profiles","audio_buses"},
        {"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas"});
    else fields(doc,{"format","version","world_id","revision","entities","retired_ids","receipts","navigation","asset_provenance","water","snow","physics_checkpoint","surface_mark_profiles","audio_buses"},{"format","version","world_id","revision","entities","retired_ids","receipts"});
    require(doc.at("format")=="poima.authored-world" && (doc.at("version")==1 || custom),"Unsupported world format/version.");
    const bool physics_checkpoint=doc.contains("physics_checkpoint");
    if(physics_checkpoint)validate_physics_checkpoint_profile(doc.at("physics_checkpoint"));
    const auto buses=authored_audio_buses(doc);
    if(doc.contains("navigation")) {
        const auto& binding=doc.at("navigation");fields(binding,{"asset"},{"asset"});
        require(binding.at("asset").is_string() && valid_asset_id(binding.at("asset")),"Navigation binding asset must be64 lowercase hexadecimal digits.");
    }
    if(doc.contains("water")) {
        const auto& binding=doc.at("water");fields(binding,{"asset"},{"asset"});
        require(binding.at("asset").is_string() && valid_asset_id(binding.at("asset")),"Water binding asset must be a lowercase SHA-256.");
    }
    if(doc.contains("surface_mark_profiles")) {
        try { surface_mark_profiles::validate_catalog(doc.at("surface_mark_profiles")); }
        catch(const std::invalid_argument& error) { throw Error(-32602,error.what()); }
    }
    if(doc.contains("asset_provenance"))component_checked([&]{provenance::validate_bindings(doc.at("asset_provenance"));return true;});
    const auto schemas=authored_component_schemas(doc);
    if(user_interface) {
        component_checked([&]{return ui::parse_definition(doc.at("ui").dump());});
        require(doc.at("retired_ui_ids").is_array(),"Invalid retired UI identities.");std::set<std::string> retired;
        for(const auto& value:doc.at("retired_ui_ids")) {const auto id=identifier(value);require(id!=std::string(32,'0') && !doc.at("ui").contains(id) && retired.insert(id).second,"Reused or duplicate retired UI identity.");}
    }
    if(custom) {
        require(doc.at("retired_component_schemas").is_array(),"Invalid retired component schema identities.");std::set<std::string> retired;
        for(const auto& value:doc.at("retired_component_schemas")) {const auto id=identifier(value);require(id!=std::string(32,'0') && !doc.at("component_schemas").contains(id) && retired.insert(id).second,"Reused or duplicate retired component schema identity.");}
    }
    if(catalog) {
        const auto& templates=doc.at("templates");require(templates.is_object() && templates.size()<=max_runtime_spawn_templates,"Invalid template catalog or recipe count.");
        require(doc.at("retired_template_ids").is_array(),"Invalid retired template identities.");std::set<std::string> retired;
        for(const auto& value:doc.at("retired_template_ids")) {const auto id=identifier(value);require(id!=std::string(32,'0') && !templates.contains(id) && retired.insert(id).second,"Reused or duplicate retired template identity.");}
        std::size_t bytes=0,members=0;
        for(const auto& [id,recipe]:templates.items()) {
            require(identifier(id)!=std::string(32,'0'),"Template identity cannot be zero.");
            const auto count=recipe.contains("entities") ? recipe.at("entities").size():1;
            require(count<=max_runtime_template_total_entities-members,"Template catalog exceeds 4096 local entities.");members+=count;
            if(recipe.contains("entities") || recipe.contains("root")) {
                validate_template_graph(doc,recipe,schemas,bytes);continue;
            }
            fields(recipe,{"name","components"},{"name","components"});validate_name(recipe.at("name"));
            const auto& bag=recipe.at("components");require(bag.is_object() && bag.contains("Transform"),"Template requires components with Transform.");
            require(!(bag.contains("StaticMesh") && bag.contains("MeshRenderer")),"Template permits one mesh component.");
            require(!(bag.contains("PbrMaterial") || bag.contains("PbrTextures")) || bag.contains("StaticMesh") || bag.contains("MeshRenderer"),"Template material requires a mesh component.");
            for(const auto& [type,value]:bag.items()) {
                require(template_component(type),"Unsupported spawn template component: "+type);
                if(!custom_component(type))validate_component(type,value);
                else {
                    const auto type_id=custom_type(type);const auto& schema=authored_schema(schemas,type_id);const auto payload=component_checked([&]{return components::parse_values(schema,value.dump());});
                    require(payload.size()<=max_runtime_template_payload_bytes-bytes,"Template custom payload budget exceeded.");bytes+=payload.size();
                }
            }
            validate_template_foliage_motion(bag);
            if(bag.contains("BoxCollider"))require(!bag.at("BoxCollider").value("navigation_obstacle",false),"Navigation obstacles require authored world entities.");
            if(bag.contains("ParticleEmitter")) {
                require(!bag.at("ParticleEmitter").contains("rain"),
                    "Rain emitters require an authored world entity outside the spawn catalog.");
                require(!bag.at("ParticleEmitter").contains("impact_burst"),
                    "Impact bursts in templates need a local receiving node in a hierarchical template.");
                const auto& transform=bag.at("Transform");
                validate_particle_pose(local_matrix(transform.at("position").get<std::array<double,3>>(),
                    transform.at("rotation").get<std::array<double,4>>(), transform.at("scale").get<std::array<double,3>>()), id);
            }
        }
    }
    std::size_t component_count=0,component_bytes=0,navigation_obstacles=0;
    identifier(doc.at("world_id")); revision(doc.at("revision"));
    const auto& entities = doc.at("entities");
    require(entities.is_object() && entities.size() <= 10000, "World must contain at most 10,000 entities.");
    if(doc.contains("snow")) {
        require(!doc.contains("navigation"),"Snow cannot be combined with authored navigation.");
        const auto snow_binding=component_checked([&]{return snow::parse_definition(doc.at("snow"));});
        for(const auto& tile:snow_binding.tiles) {
            require(entities.contains(tile),"Snow tile entity does not exist.");
            const auto& entity=entities.at(tile);
            const auto components=entity.find("components");
            require(components==entity.end() || !authored_foliage_motion(*components),
                "Snow tiles cannot use foliage motion.");
        }
    }
    std::size_t audio_sources=0,wetness_receivers=0;
    std::vector<std::string> particle_sources, audio_rooms, audio_portals;
    std::size_t audio_room_sends=0;
    for (const auto& [id, entity] : entities.items()) {
        identifier(id);require(schemas.empty() || id!=std::string(32,'0'),"Custom component worlds reserve the zero entity identity as unset.");
        fields(entity, {"name", "parent", "components"}, {"name", "parent", "components"});
        validate_name(entity.at("name"));
        if (!entity.at("parent").is_null())
            require(entities.contains(identifier(entity.at("parent"))), "Parent entity does not exist.");
        require(entity.at("components").is_object() && entity.at("components").contains("Transform"),"Entity requires a component object with Transform.");
        const auto& audio_components=entity.at("components");
        if(physics_checkpoint) {
            require(!audio_components.contains("PhysicalRig"),"Physics checkpoint version 1 cannot include PhysicalRig content.");
            const auto character=audio_components.find("CharacterController");
            require(character==audio_components.end() || !character->contains("locomotion"),
                "Physics checkpoint version 1 requires rigid-body character controllers.");
        }
        require(!audio_components.contains("CameraDraw") || !audio_components.contains("StaticMesh") ||
            !audio_components.at("StaticMesh").contains("lods"),"Mesh LOD requires a world drawable without CameraDraw.");
        require(!audio_components.contains("MeshCollider") || (!audio_components.contains("BoxCollider") && !audio_components.contains("CharacterController")),"MeshCollider cannot combine with BoxCollider or CharacterController.");
        if(audio_components.contains("AcousticMaterial"))require(audio_components.contains("BoxCollider") || audio_components.contains("MeshRenderer") || audio_components.contains("StaticMesh"),"AcousticMaterial requires box collider or mesh geometry.");
        if(audio_components.contains("AudioRoom")) {
            require(audio_rooms.size()<max_audio_rooms,"World exceeds the audio room limit.");
            audio_rooms.push_back(id);
            const auto& room=audio_components.at("AudioRoom");
            if(room.contains("portal_send") && room.at("portal_send").is_number() && room.at("portal_send").get<double>()>0)
                require(++audio_room_sends<=max_audio_room_sends,"World exceeds the audio room send limit.");
        }
        if(audio_components.contains("AudioPortal")) {
            require(audio_portals.size()<max_audio_portals,"World exceeds the audio portal limit.");
            audio_portals.push_back(id);
        }
        if(audio_components.contains("ParticleEmitter")) {
            require(particle_sources.size() < runtime_particle_limits.emitters, "Scene exceeds the particle emitter capacity.");
            particle_sources.push_back(id);
            const auto& emitter=audio_components.at("ParticleEmitter");
            if(emitter.contains("impact_burst")) {
                const auto source=component_checked([&]{return parse_particle_emitter(emitter);});
                visit_impact_bursts(*source.impact_burst,[&](const auto& burst) {
                    const auto& reference=burst.emitter;
                    require(reference!=id && entities.contains(reference),"Impact burst must reference another emitter in the scene or template.");
                    const auto& target=entities.at(reference).at("components");
                    require(target.contains("ParticleEmitter"),"Impact burst target needs a ParticleEmitter component.");
                    const auto definition=component_checked([&]{return parse_particle_emitter(target.at("ParticleEmitter"));});
                    require(definition.config.simulation_space==particles::Space::World && !definition.rain,
                        "Impact burst target needs world simulation and ordinary emission.");
                });
            }
        }
        require(int(audio_components.contains("MeshRenderer"))+int(audio_components.contains("StaticMesh"))+int(audio_components.contains("SkinnedMesh"))<=1,"An entity can have only one mesh component.");
        require(!audio_components.contains("SkinnedMesh") || !audio_components.contains("AcousticMaterial"),"Skinned mesh acoustics are not supported; use a separate explicit collision/acoustic proxy.");
        for (const auto& [type,value]:audio_components.items()) {
            if(!custom_component(type))validate_component(type,value);
            else {
                const auto type_id=custom_type(type);const auto& schema=authored_schema(schemas,type_id);
                const auto payload=component_checked([&]{return components::parse_values(schema,value.dump());});
                component_checked([&]{components::validate_payload(schema,payload,&authored_reference_exists,const_cast<Json*>(&entities));});
                require(++component_count<=components::max_instances && payload.size()<=components::max_payload_bytes-component_bytes,"Authored component instance/byte budget exceeded.");component_bytes+=payload.size();
            }
        }
        if(const auto* motion=authored_foliage_motion(audio_components)) {
            require(!audio_components.contains("CameraDraw"),"Foliage motion requires a world drawable without CameraDraw.");
            if(motion->contains("touch")) {
                const auto& actor=motion->at("touch").at("actor").get_ref<const std::string&>();
                require(actor!=id && entities.contains(actor),"Foliage touch requires a distinct authored CharacterController.");
                const auto& entity=entities.at(actor);
                const auto components=entity.find("components");
                require(components!=entity.end() && components->contains("CharacterController"),
                    "Foliage touch requires a distinct authored CharacterController.");
            }
        }
        if(audio_components.contains("AudioEmitter")) {
            const auto& emitter=audio_components.at("AudioEmitter");
            if(emitter.at("enabled")==true)require(++audio_sources<=max_audio_sources,"At most 64 enabled audio emitters.");
            if(emitter.contains("bus"))require(buses.contains(emitter.at("bus").get_ref<const std::string&>()),
                "AudioEmitter on "+id+" refers to an absent audio bus.");
        }
        if(audio_components.contains("BoxCollider") && audio_components.at("BoxCollider").value("navigation_obstacle",false))
            require(++navigation_obstacles<=navigation::max_obstacles,"At most 64 navigation obstacles are supported.");
        if(audio_components.contains("FootprintSurface")) {
            const auto& surface=audio_components.at("FootprintSurface");
            if(surface.contains("profile"))
                require(doc.contains("surface_mark_profiles") &&
                    doc.at("surface_mark_profiles").contains(surface.at("profile").get_ref<const std::string&>()),
                    "FootprintSurface on "+id+" refers to an absent surface mark profile.");
        }
        if(audio_components.contains("SurfaceWetness")) {
            require(++wetness_receivers<=wetness::max_receivers,"World exceeds the SurfaceWetness receiver capacity.");
            require(audio_components.contains("PbrMaterial") &&
                (audio_components.contains("MeshRenderer") || audio_components.contains("StaticMesh")) &&
                !audio_components.contains("SkinnedMesh") && !audio_components.contains("CameraDraw"),
                "SurfaceWetness requires a rigid PBR world drawable without CameraDraw.");
        }
    }
    std::size_t light_count=0,environment_count=0,shadow_count=0;std::uint32_t shadow_resolution=1024;
    for(const auto& [id,e]:entities.items()) {
        const auto& components=e.at("components");
        if(components.contains("Camera") && components.at("Camera").contains("focus_target")) {
            const auto target=focus_target_value(components.at("Camera").at("focus_target"));
            require(target.entity!=id && entities.contains(target.entity),"Camera focus target must reference another authored entity.");
        }
        if(components.contains("Light") && components.at("Light").at("enabled")==true)++light_count;
        if(components.contains("Light"))shadow_count+=shadow_view_count(light_value(components.at("Light")));
        if(components.contains("LightingEnvironment")) {
            ++environment_count;const auto environment=environment_value(components.at("LightingEnvironment"));shadow_resolution=environment.shadow_resolution;
            if(!environment.sky.sun.empty()) {
                require(entities.contains(environment.sky.sun),"Sky sun entity does not exist.");
                const auto& sun=entities.at(environment.sky.sun).at("components");
                require(sun.contains("Light") && sun.at("Light").at("kind")=="directional","Sky sun must reference a directional Light.");
            }
            if(const auto& contact=environment.screen_contact_shadows) {
                require(entities.contains(contact->light),"Screen contact shadow light entity does not exist.");
                const auto& target=entities.at(contact->light).at("components");
                require(target.contains("Light") && target.at("Light").at("kind")=="directional",
                    "Screen contact shadows must reference a directional Light.");
            }
            if(environment.height_fog)for(const auto& spot:environment.height_fog->spot_lights) {
                require(entities.contains(spot.entity),"Height fog spot light entity does not exist.");
                const auto& emitter=entities.at(spot.entity).at("components");
                require(emitter.contains("Light") && emitter.at("Light").at("kind")=="spot",
                    "Height fog spot light must reference an authored spot Light.");
            }
        }
    }
    require(light_count<=max_scene_lights && environment_count<=1,"World permits at most 1024 enabled lights and one LightingEnvironment.");
    try { validate_shadow_budget(shadow_count,shadow_resolution); }catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
    std::map<std::string, int> colors;
    for (const auto& [id, unused] : entities.items()) {
        (void)unused;
        std::string current = id;
        std::vector<std::string> chain;
        while (!current.empty() && colors[current] != 2) {
            require(colors[current] != 1, "Hierarchy contains a cycle.");
            colors[current] = 1; chain.push_back(current);
            const auto& parent = entities.at(current).at("parent");
            current = parent.is_null() ? "" : parent.get<std::string>();
        }
        for (const auto& item : chain) colors[item] = 2;
    }
    if(!particle_sources.empty() || !audio_rooms.empty() || !audio_portals.empty()) {
        const auto matrices=world_matrices(entities);
        for(const auto& id:particle_sources)validate_particle_pose(matrices.at(id), id);
        const auto validate_audio_transform=[&](const auto& ids,const std::string& type) {
            for(const auto& id:ids) {
                try { (void)inverse_affine(matrices.at(id)); }
                catch(const std::runtime_error&) { throw Error(-32602,type+" "+id+" requires an invertible world transform."); }
            }
        };
        validate_audio_transform(audio_rooms,"AudioRoom");
        validate_audio_transform(audio_portals,"AudioPortal");
    }
    for(const auto& [id,entity]:entities.items()) {
        const auto& bag=entity.at("components");
        if(!bag.contains("CharacterController") || !bag.at("CharacterController").contains("stance"))continue;
        const auto& camera=bag.at("CharacterController").at("camera");if(camera.is_null())continue;
        const auto target=identifier(camera);
        require(entities.contains(target) && entities.at(target).at("components").contains("Camera") && entities.at(target).at("parent")==id,
            "Stance controller camera must be an existing direct-child Camera.");
        require(!entities.at(target).at("components").contains("RigNode"),
            "Stance controller camera cannot be an animation-owned RigNode.");
    }
    camera_draw_references(entities);
    // Profiles reference model-local nodes, never a second asset or hidden
    // authoring graph. Validate exactly the same membership for world and
    // detached hierarchical-template documents before atomic publication.
    if(std::any_of(entities.begin(),entities.end(),[](const Json& entity){return entity.at("components").contains("PhysicalRig");})) {
        std::map<std::string,std::map<std::uint32_t,std::size_t>> physical_nodes;
        std::map<std::string,std::string> linked_actors;
        std::set<std::string> procedural_physical_rigs;
        std::set<std::string> camera_rig_owners;
        for(const auto& [id,entity]:entities.items()) {
            (void)id;const auto& bag=entity.at("components");
            if(bag.contains("BoxCollider") || bag.contains("MeshCollider") || bag.contains("CharacterController")) {
                std::size_t depth=0;auto parent=entity.at("parent");
                while(!parent.is_null()) {
                    const auto ancestor=identifier(parent);require(entities.contains(ancestor) && ++depth<=entities.size(),"Invalid physical attachment ancestor graph.");
                    const auto& value=entities.at(ancestor);
                    require(!value.at("components").contains("PhysicalRig"),"Ordinary colliders/controllers cannot descend from PhysicalRig until attachment ownership is supported.");
                    parent=value.at("parent");
                }
            }
            if(bag.contains("RigNode")) {
                const auto& ref=bag.at("RigNode");
                ++physical_nodes[identifier(ref.at("rig"))][static_cast<std::uint32_t>(revision(ref.at("node")))];
                if(bag.contains("Camera"))camera_rig_owners.insert(identifier(ref.at("rig")));
            }
        }
        for(const auto& [id,entity]:entities.items()) {
            const auto& bag=entity.at("components");if(!bag.contains("PhysicalRig"))continue;
            require(bag.contains("AnimationRig"),"PhysicalRig requires AnimationRig on the same entity.");
            const auto profile=physical_rig_value(bag.at("PhysicalRig"));
            if(profile.controller_handoff) {
                const auto actor=identifier(profile.controller_handoff->actor);
                require(actor!=id && entities.contains(actor) && entity.at("parent")==actor,
                    "Physical controller handoff actor must exist and directly parent its rig.");
                const auto& owner=entities.at(actor);const auto& actor_bag=owner.at("components");
                require(owner.at("parent").is_null() && actor_bag.contains("CharacterController") &&
                    actor_bag.at("CharacterController").contains("locomotion"),
                    "Physical controller handoff requires a root virtual-locomotion CharacterController.");
                require(!actor_bag.contains("PhysicalRig") && !actor_bag.contains("AnimationRig") && !actor_bag.contains("RigNode") &&
                    !actor_bag.contains("BoxCollider") && !actor_bag.contains("MeshCollider"),
                    "Physical controller actor cannot have independent collider or animation ownership.");
                require(linked_actors.emplace(actor,id).second,"A controller actor can link to only one PhysicalRig.");
            }
            for(const auto& part:profile.profile.parts) {
                const auto owner=physical_nodes.find(id);
                require(owner!=physical_nodes.end() && owner->second.contains(part.node) && owner->second.at(part.node)==1,
                    "PhysicalRig part requires exactly one matching model-local RigNode on its AnimationRig.");
            }
            if(profile.procedural_pose) {
                procedural_physical_rigs.insert(id);
                require(!bag.contains("Camera") &&
                    !entities.at(profile.controller_handoff->actor).at("components").contains("Camera") && !camera_rig_owners.contains(id),
                    "Physical procedural pose requires one transform owner. Place Camera on a separate child entity instead of the linked actor, rig wrapper or model RigNode.");
                const auto& animation=bag.at("AnimationRig");
                require(!animation.contains("root_motion"),"Physical procedural pose cannot configure root motion, including disabled profiles.");
                std::set<std::uint32_t> mapped;
                for(const auto& part:profile.profile.parts)mapped.insert(part.node);
                const bool grounded=*profile.procedural_pose==physical_rig::ProceduralPosePolicy::GroundedAnimatedOnly;
                std::size_t grounded_slots=0;
                if(animation.contains("ik"))for(const auto& chain:animation_ik_value(animation.at("ik"))) {
                    require(grounded || !chain.grounding,"Physical procedural pose permits only manual IK, never configured grounding.");
                    if(chain.grounding)++grounded_slots;
                    require(mapped.contains(chain.root) && mapped.contains(chain.middle) && mapped.contains(chain.end),
                        "Physical procedural IK root, middle and end must all map to physical parts.");
                }
                require(!grounded || grounded_slots>0,"Physical grounded_animated_only requires at least one configured grounding slot.");
            }
        }
        if(!linked_actors.empty()) {
            // Linked actors are roots, so each descendant has at most one
            // owner. Cache root membership instead of walking every subtree
            // once for each rig on large authored graphs.
            std::map<std::string,std::string> roots;
            for(const auto& [id,entity]:entities.items()) {
                (void)entity;if(roots.contains(id))continue;
                std::vector<std::string> chain;auto at=id;
                while(!roots.contains(at)) {
                    chain.push_back(at);const auto& parent=entities.at(at).at("parent");
                    if(parent.is_null())break;
                    at=identifier(parent);
                }
                const auto root=roots.contains(at)?roots.at(at):at;
                for(const auto& member:chain)roots.emplace(member,root);
            }
            for(const auto& [id,entity]:entities.items()) {
                const auto& bag=entity.at("components");
                if(bag.contains("AnimationRig") && bag.at("AnimationRig").contains("root_motion")) {
                    const auto motion=root_motion_value(bag.at("AnimationRig").at("root_motion"));
                    require(!linked_actors.contains(motion.actor),"A handoff controller cannot have another root-motion writer.");
                }
                const auto linked=linked_actors.find(roots.at(id));if(linked==linked_actors.end())continue;
                if(id!=linked->first)require(!bag.contains("CharacterController") && !bag.contains("BoxCollider") && !bag.contains("MeshCollider"),
                    "Linked physical actor subtree cannot contain independently simulated bodies or controllers.");
                require(!bag.contains("PhysicalRig") || id==linked->second,"Linked physical actor subtree cannot contain another PhysicalRig.");
                if(bag.contains("AnimationRig")) {
                    const auto& animation=bag.at("AnimationRig");
                    require(!animation.contains("root_motion"),"Linked physical actor subtree cannot contain root motion, IK or grounding ownership.");
                    require(!animation.contains("ik") || animation.at("ik").empty() ||
                        (id==linked->second && procedural_physical_rigs.contains(id)),
                        "Linked physical actor subtree cannot contain root motion, IK or grounding ownership.");
                }
            }
        }
    }
    std::set<std::pair<std::string,std::string>> regions;
    for(const auto& [id,entity]:entities.items())if(entity.at("components").contains("HitRegion")) {
        const auto region=hit_region_value(entity.at("components").at("HitRegion"));
        require(regions.size()<max_runtime_hit_regions && regions.emplace(region.owner,region.name).second,"HitRegion count exceeds 4096 or owner/name is duplicated.");
        require(entities.contains(region.owner),"HitRegion owner does not exist.");
        auto ancestor=id;
        while(ancestor!=region.owner && !ancestor.empty()) {
            const auto& parent=entities.at(ancestor).at("parent");ancestor=parent.is_null() ? std::string{} : parent.get<std::string>();
        }
        require(ancestor==region.owner,"HitRegion owner must be self or an ancestor.");
    }
    require(doc.at("retired_ids").is_array(), "Invalid retired IDs.");
    std::set<std::string> retired;
    for (const auto& id : doc.at("retired_ids")) {
        const auto text = identifier(id);
        require(!entities.contains(text) && retired.insert(text).second, "Reused or duplicate retired entity ID.");
    }
    require(doc.at("receipts").is_array() && doc.at("receipts").size() <= 128, "Invalid transaction receipts.");
    std::set<std::string> requests;
    for (const auto& receipt : doc.at("receipts")) {
        fields(receipt, {"params", "result"}, {"params", "result"});
        require(receipt.at("params").is_object() && receipt.at("result").is_object(), "Invalid transaction receipt.");
        require(requests.insert(identifier(receipt.at("params").at("request_id"))).second, "Duplicate receipt ID.");
        require(revision(receipt.at("result").at("revision")) <= revision(doc.at("revision")), "Receipt revision exceeds world revision.");
    }
}

} // namespace poima::world_detail
