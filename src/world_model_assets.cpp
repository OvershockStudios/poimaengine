// SPDX-License-Identifier: Apache-2.0
#include "world_model_assets.hpp"

#include "asset_store.hpp"
#include "poima/physical_rig.hpp"
#include "poima/scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace poima::world_detail {

static AnimationReferencePose animation_reference_pose(const Json& value) {
    fields(value,{"kind","clip","time"},{"kind"});
    require(value.at("kind").is_string(),"Reference pose kind must be rest or sample.");
    const auto kind=value.at("kind").get<std::string>();
    AnimationReferencePose result;
    if(kind=="rest") {
        fields(value,{"kind"},{"kind"});return result;
    }
    require(kind=="sample","Reference pose kind must be rest or sample.");
    fields(value,{"kind","clip","time"},{"kind","clip","time"});
    const auto index=revision(value.at("clip"));require(index<256,"Reference take index must be 0..255.");
    const auto& time=value.at("time");
    require(time.is_number() && std::isfinite(time.get<double>()) && time>=0 && time<=3600,
        "Reference time must be finite and within 0..3600 seconds.");
    result.clip=static_cast<std::uint32_t>(index);result.time=time.get<double>();return result;
}
static AnimationFrameTransfer animation_frame_transfer(const Json& value) {
    fields(value,{"policy","source_pose","target_pose","alignment"},{"policy","source_pose","target_pose"});
    require(value.at("policy")=="reference-frame-v1","Unknown animation frame transfer policy.");
    AnimationFrameTransfer result;
    result.source_reference=animation_reference_pose(value.at("source_pose"));
    result.target_reference=animation_reference_pose(value.at("target_pose"));
    if(value.contains("alignment")) {
        const auto& alignment=value.at("alignment");fields(alignment,{"position","rotation"},{"position","rotation"});
        const auto& position=alignment.at("position");const auto& rotation=alignment.at("rotation");
        require(position.is_array() && position.size()==3 && rotation.is_array() && rotation.size()==4,
            "Frame alignment requires position XYZ and rotation XYZW arrays.");
        for(std::size_t i=0;i<3;++i) {
            const auto& number=position[i];
            require(number.is_number() && std::isfinite(number.get<double>()) && std::abs(number.get<double>())<=1e9,
                "Frame alignment position must be finite within +/-1e9 metres.");
            result.alignment_position[i]=number.get<double>();
        }
        double norm=0;
        for(std::size_t i=0;i<4;++i) {
            const auto& number=rotation[i];
            require(number.is_number() && std::isfinite(number.get<double>()) && number>=-1 && number<=1,
                "Frame alignment rotation components must be finite within [-1,1].");
            result.alignment_rotation[i]=number.get<double>();norm+=result.alignment_rotation[i]*result.alignment_rotation[i];
        }
        require(std::abs(norm-1)<=1e-6,"Frame alignment must be a normalized XYZW quaternion.");
    }
    return result;
}
static AnimationRotationRetarget animation_rotation_retarget(const Json& value) {
    fields(value,{"policy","source_pose","target_pose","alignment_rotation","positions","scales",
        "expected_source_model_sha256","expected_target_model_sha256","node_map"},
        {"policy","source_pose","target_pose","positions","scales","expected_source_model_sha256","expected_target_model_sha256"});
    require(value.at("policy")=="reference-rotation-v1","Unknown animation retarget policy.");
    require(value.at("scales")=="target_reference","Retarget scales must explicitly use the target reference.");
    AnimationRotationRetarget result;
    result.source_reference=animation_reference_pose(value.at("source_pose"));
    result.target_reference=animation_reference_pose(value.at("target_pose"));
    auto model_hash=[&](const char* key) {
        const auto& item=value.at(key);
        require(item.is_string() && valid_asset_id(item.get<std::string>()),"Expected model fingerprint must be 64 lowercase hexadecimal characters.");
        return item.get<std::string>();
    };
    result.expected_source_model_sha256=model_hash("expected_source_model_sha256");
    result.expected_target_model_sha256=model_hash("expected_target_model_sha256");
    if(value.contains("node_map")) {
        const auto& pairs=value.at("node_map");
        require(pairs.is_array() && !pairs.empty() && pairs.size()<=animation_retarget_node_map_limit,
            "Retarget node map requires 1..1024 pairs.");
        result.node_map.emplace();result.node_map->reserve(pairs.size());
        std::set<std::uint32_t> sources,targets;
        for(const auto& pair:pairs) {
            fields(pair,{"source_node","target_node"},{"source_node","target_node"});
            const auto source=revision(pair.at("source_node")),target=revision(pair.at("target_node"));
            require(source<10000 && target<10000,"Retarget node map index exceeds the model limit.");
            require(sources.insert(static_cast<std::uint32_t>(source)).second && targets.insert(static_cast<std::uint32_t>(target)).second,
                "Retarget node map must be bijective.");
            result.node_map->push_back({static_cast<std::uint32_t>(source),static_cast<std::uint32_t>(target)});
        }
        std::sort(result.node_map->begin(),result.node_map->end(),[](const auto& a,const auto& b){return a.source_node<b.source_node;});
    }
    if(value.contains("alignment_rotation")) {
        const auto& rotation=value.at("alignment_rotation");
        require(rotation.is_array() && rotation.size()==4,"Retarget alignment requires a XYZW quaternion.");
        double norm=0;
        for(std::size_t i=0;i<4;++i) {
            const auto& number=rotation[i];
            require(number.is_number() && std::isfinite(number.get<double>()) && number>=-1 && number<=1,"Retarget alignment components must be finite within [-1,1].");
            result.alignment_rotation[i]=number.get<double>();norm+=result.alignment_rotation[i]*result.alignment_rotation[i];
        }
        require(std::abs(norm-1)<=1e-6,"Retarget alignment must be normalized.");
    }
    const auto& positions=value.at("positions");fields(positions,{"kind","nodes","scale"},{"kind"});
    if(positions.at("kind")=="target_reference")fields(positions,{"kind"},{"kind"});
    else {
        require(positions.at("kind")=="reference_delta","Unknown retarget position policy.");
        fields(positions,{"kind","nodes","scale"},{"kind","nodes","scale"});
        const auto& nodes=positions.at("nodes");
        require(nodes.is_array() && !nodes.empty() && nodes.size()<=64,"Retarget translation requires 1..64 unique source nodes.");
        std::set<std::uint32_t> unique;
        for(const auto& source_node:nodes) {
            const auto index=revision(source_node);require(index<10000,"Retarget source node index exceeds the model limit.");
            require(unique.insert(static_cast<std::uint32_t>(index)).second,"Retarget source node index is repeated.");
            result.translation_nodes.push_back(static_cast<std::uint32_t>(index));
        }
        const auto& scale=positions.at("scale");
        require(scale.is_number() && std::isfinite(scale.get<double>()) && scale>0 && scale<=100,"Retarget translation scale must be finite within (0,100].");
        result.position_mode=AnimationRetargetPositionMode::reference_delta;result.translation_scale=scale.get<double>();
    }
    return result;
}

Json material_json(const PbrMaterial& m) {
    validate_pbr_material(m);
    Json result={{"base_color",m.base_color},{"emissive",m.emissive},{"metallic",m.metallic},{"roughness",m.roughness},{"double_sided",m.double_sided}};
    if(m.alpha_mode!=MaterialAlphaMode::opaque || m.opacity!=1 || m.alpha_cutoff!=.5f) {
        result["alpha_mode"]=material_alpha_mode_name(m.alpha_mode);result["opacity"]=m.opacity;result["alpha_cutoff"]=m.alpha_cutoff;
    }
    return result;
}

Json author_animation(const Json& params,const fs::path& assets) {
    fields(params,{"asset","clips"},{"asset","clips"});
    require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid model asset ID.");
    const auto& clips=params.at("clips");
    require(clips.is_array() && !clips.empty() && clips.size()<=8,"Author 1..8 rotation clips per request.");
    const auto number=[](const Json& value,double minimum,double maximum,const char* message) {
        require(value.is_number(),message);
        const auto result=value.get<double>();
        require(std::isfinite(result) && result>=minimum && result<=maximum,message);
        return result;
    };
    try {
        const auto source=read_model_asset(assets,params.at("asset"));
        ModelAsset model=*source.model;
        require(clips.size()<=max_model_clips-model.animations.size(),"Result exceeds the model clip limit.");
        std::set<std::string> names;
        for(const auto& clip:model.animations)names.insert(clip.name);
        auto appended=Json::array();
        std::size_t key_count=0;
        for(const auto& input:clips) {
            fields(input,{"name","tracks"},{"name","tracks"});
            require(input.at("name").is_string(),"Authored clip name must be a string.");
            AnimationClip clip;clip.name=input.at("name").get<std::string>();
            require(!clip.name.empty() && clip.name.size()<=256 && clip.name.find('\0')==std::string::npos,
                "Authored clip name must be 1..256 UTF-8 bytes without NUL.");
            require(names.insert(clip.name).second,"Authored clip name must be unique in the model.");
            const auto& tracks=input.at("tracks");
            require(tracks.is_array() && !tracks.empty() && tracks.size()<=128,"Author 1..128 rotation tracks per clip.");
            std::set<std::uint32_t> nodes;
            for(const auto& track:tracks) {
                fields(track,{"node","interpolation","hinge","keys"},{"node","keys"});
                const auto node=revision(track.at("node"));
                require(node<model.nodes.size(),"Rotation track node is outside the source model.");
                AnimationChannel channel;
                channel.node=static_cast<std::uint32_t>(node);
                channel.path=AnimationPath::rotation;
                require(nodes.insert(channel.node).second,"A clip permits one rotation track per node.");
                if(track.contains("interpolation")) {
                    const auto& interpolation=track.at("interpolation");
                    require(interpolation=="step" || interpolation=="linear","Rotation interpolation must be step or linear.");
                    if(interpolation=="step")channel.interpolation=AnimationInterpolation::step;
                }
                std::array<double,3> axis{};
                double minimum=0,maximum=0;
                const auto hinge=track.find("hinge");
                if(hinge!=track.end()) {
                    fields(*hinge,{"axis","minimum_degrees","maximum_degrees"},{"axis","minimum_degrees","maximum_degrees"});
                    minimum=number(hinge->at("minimum_degrees"),-180,180,"Hinge minimum must be within -180..180 degrees.");
                    maximum=number(hinge->at("maximum_degrees"),-180,180,"Hinge maximum must be within -180..180 degrees.");
                    require(maximum>=minimum && maximum-minimum<=179.9,"Hinge interval must be ordered and span at most 179.9 degrees.");
                    const auto& coordinates=hinge->at("axis");
                    require(coordinates.is_array() && coordinates.size()==3,"Hinge axis requires three components.");
                    for(std::size_t i=0;i<axis.size();++i)axis[i]=number(coordinates[i],-1.000001,1.000001,"Hinge axis must be a unit vector.");
                    const auto length=std::hypot(axis[0],axis[1],axis[2]);
                    require(std::abs(length-1)<=1e-6,"Hinge axis must have unit length within 1e-6.");
                    for(auto& component:axis)component/=length;
                }
                const auto& keys=track.at("keys");
                require(keys.is_array() && !keys.empty() && keys.size()<=256 && keys.size()<=4096-key_count,
                    "Author 1..256 keys per track and at most 4096 per request.");
                key_count+=keys.size();
                for(const auto& key:keys) {
                    const char* value_field=hinge==track.end()?"rotation":"degrees";
                    fields(key,{"time",value_field},{"time",value_field});
                    const auto time=number(key.at("time"),0,3600,"Rotation key time must be within 0..3600 seconds.");
                    const auto stored=static_cast<float>(time==0?0:time);
                    require(channel.times.empty() || stored>channel.times.back(),"Key times must increase after float32 encoding.");
                    channel.times.push_back(stored);
                    std::array<double,4> rotation{};
                    if(hinge==track.end()) {
                        const auto& q=key.at("rotation");
                        require(q.is_array() && q.size()==4,"Rotation key requires four XYZW components.");
                        for(std::size_t i=0;i<rotation.size();++i)rotation[i]=number(q[i],-1.000001,1.000001,"Rotation key must be a unit quaternion.");
                        const auto length=std::hypot(std::hypot(rotation[0],rotation[1]),std::hypot(rotation[2],rotation[3]));
                        require(std::abs(length-1)<=1e-6,"Rotation key must have unit length within 1e-6.");
                        rotation=physical_rig::canonical_quaternion(rotation);
                    } else {
                        const auto angle=number(key.at("degrees"),minimum,maximum,"Hinge key is outside its declared angle interval.");
                        const auto half=angle*(std::numbers::pi/360.0);
                        const auto sine=std::sin(half);
                        rotation=physical_rig::multiply(model.nodes[node].rotation,
                            {axis[0]*sine,axis[1]*sine,axis[2]*sine,std::cos(half)});
                    }
                    channel.values.push_back({static_cast<float>(rotation[0]),static_cast<float>(rotation[1]),
                        static_cast<float>(rotation[2]),static_cast<float>(rotation[3])});
                }
                clip.duration=std::max(clip.duration,static_cast<double>(channel.times.back()));
                clip.channels.push_back(std::move(channel));
            }
            std::sort(clip.channels.begin(),clip.channels.end(),[](const auto& a,const auto& b){return a.node<b.node;});
            appended.push_back({{"index",model.animations.size()},{"name",clip.name},{"duration",clip.duration},{"tracks",clip.channels.size()}});
            model.animations.push_back(std::move(clip));
        }
        const auto result=store_cooked_model(assets,model,"poima-animation-author-1");
        return {{"base_asset",source.id},{"asset",result.id},{"bytes",result.bytes},{"clips",std::move(appended)}};
    } catch(const Error&) { throw; }
    catch(const std::exception& error) { throw Error(-32050,error.what()); }
}

std::string asset_diagnostic(const char* message,std::size_t limit) {
    // Filesystem diagnostics can include arbitrary POSIX filename bytes.
    // Bound both input and repaired output without splitting UTF-8.
    auto text=Json::parse(Json(std::string(message).substr(0,limit)).dump(-1,' ',false,Json::error_handler_t::replace)).get<std::string>();
    auto length=std::min<std::size_t>(text.size(),limit);
    while(length<text.size() && length>0 && (static_cast<unsigned char>(text[length])&0xc0u)==0x80u)--length;
    text.resize(length);return text;
}
static Error fbx_texture_dependency_error(const FbxTextureDependencyError& error,bool inspection=false) {
    const auto& diagnostic=error.diagnostic();
    // Only the already-confined relative texture reference is reported,
    // never the resolved source directory or an OS exception's path.
    const auto relative=Json::parse(Json(diagnostic.relative_path).dump(-1,' ',false,Json::error_handler_t::replace));
    return Error(-32050,asset_diagnostic(error.what()),{
        {"type","fbx_texture_dependency"},{"version",1},
        {"reason",fbx_texture_dependency_reason_name(diagnostic.reason)},
        {"relative_path",relative},
        {"file_bytes",diagnostic.file_bytes ? Json(*diagnostic.file_bytes) : Json(nullptr)},
        {"byte_limit",diagnostic.byte_limit},
        {"source_role",inspection ? "inspection" : diagnostic.donor_index ? "animation_donor" : "base"},
        {"donor_index",!inspection && diagnostic.donor_index ? Json(*diagnostic.donor_index) : Json(nullptr)}});
}
static Error fbx_material_profile_error(const FbxMaterialProfileError& error,bool enabled,bool inspection=false) {
    if(!enabled)return Error(-32050,asset_diagnostic(error.what()));
    const auto& diagnostic=error.diagnostic();
    return Error(-32050,asset_diagnostic(error.what()),{
        {"type","fbx_material_profile"},{"version",1},
        {"reason",fbx_material_profile_reason_name(diagnostic.reason)},
        {"feature",diagnostic.feature},{"node",diagnostic.node},
        {"node_name",asset_diagnostic(diagnostic.node_name.c_str(),256)},
        {"material_slot",diagnostic.material_slot},
        {"material_name",asset_diagnostic(diagnostic.material_name.c_str(),256)},
        {"observed_count",diagnostic.observed_count ? Json(*diagnostic.observed_count) : Json(nullptr)},
        {"limit",diagnostic.limit ? Json(*diagnostic.limit) : Json(nullptr)},
        {"source_role",inspection ? "inspection" : diagnostic.donor_index ? "animation_donor" : "base"},
        {"donor_index",!inspection && diagnostic.donor_index ? Json(*diagnostic.donor_index) : Json(nullptr)}});
}
Json source_model_inspection(const Json& params,const fs::path& world_path) {
    fields(params,{"source","fbx_normal_map","fbx_geometry_only","fbx_profile_diagnostics","section","offset","limit","pose","expected_model_sha256","skin","clip"},{"source"});
    require(!params.contains("fbx_profile_diagnostics") || params.at("fbx_profile_diagnostics").is_boolean(),"fbx_profile_diagnostics must be Boolean.");
    require(!params.contains("fbx_geometry_only") || params.at("fbx_geometry_only").is_boolean(),"fbx_geometry_only must be Boolean.");
    const auto& path=params.at("source");
    require(path.is_string(),"Source inspection requires a path.");const auto text=path.get<std::string>();
    require(!text.empty() && text.size()<=4096 && text.find('\0')==std::string::npos,"Invalid source inspection path.");
    require(!params.contains("section") || params.at("section").is_string(),"Source inspection section must be a string.");
    const auto section=params.value("section",std::string("summary"));
    require(section=="summary" || section=="nodes" || section=="skins" || section=="animations" || section=="skin_joints" || section=="animation_channels","Invalid source inspection section.");
    require(!params.contains("pose") || section=="nodes","Source pose sampling requires the nodes section.");
    require(!params.contains("skin") || section=="skin_joints","Source skin selector requires the skin_joints section.");
    require(!params.contains("clip") || section=="animation_channels","Source clip selector requires the animation_channels section.");
    std::optional<std::uint64_t> selected_skin,selected_clip;
    if(section=="skin_joints") {
        require(params.contains("skin"),"Source skin_joints inspection requires a skin selector.");
        selected_skin=revision(params.at("skin"));
    }
    if(section=="animation_channels") {
        require(params.contains("clip"),"Source animation_channels inspection requires a clip selector.");
        selected_clip=revision(params.at("clip"));
    }
    const auto offset=params.contains("offset") ? revision(params.at("offset")) : 0;
    const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;
    require(limit>=1 && limit<=64,"Source inspection page limit must be 1..64.");
    std::optional<AnimationReferencePose> selector;
    if(params.contains("pose"))selector=animation_reference_pose(params.at("pose"));
    std::optional<std::string> expected;
    if(params.contains("expected_model_sha256")) {
        const auto& value=params.at("expected_model_sha256");
        require(value.is_string() && valid_asset_id(value.get<std::string>()),"Expected source fingerprint must be 64 lowercase hexadecimal characters.");
        expected=value.get<std::string>();
    }
    auto convention=FbxNormalConvention::opengl;
    if(params.contains("fbx_normal_map")) {
        const auto& value=params.at("fbx_normal_map");
        require(value=="opengl" || value=="directx","Invalid FBX normal map convention.");
        convention=value=="opengl" ? FbxNormalConvention::opengl : FbxNormalConvention::directx;
    }
    try {
        auto source=fs::path(std::u8string(text.begin(),text.end()));if(source.is_relative())source=world_path.parent_path()/source;
        if(fs::file_size(source)>128*1024*1024)throw std::runtime_error("Source inspection exceeds the 128 MiB source budget.");
        const auto imported=import_model(source,convention,params.value("fbx_geometry_only",false));const auto& model=*imported.model;
        if(params.contains("fbx_normal_map") && imported.importer.empty())throw std::runtime_error("fbx_normal_map applies only to FBX sources.");
        validate_animation_data(model);const auto retained=retained_model_import_bytes(model);
        const auto fingerprint=content_hash(encode_model(model));
        if(expected && *expected!=fingerprint)throw std::runtime_error("Source model fingerprint changed; inspect the current source before selecting nodes or takes.");
        if(selected_skin && *selected_skin>=model.skins.size())throw std::runtime_error("Source skin index is outside the original model.");
        if(selected_clip && *selected_clip>=model.animations.size())throw std::runtime_error("Source clip index is outside the original model.");
        std::optional<ModelPose> pose;
        if(selector) {
            if(selector->clip && (*selector->clip>=model.animations.size() || selector->time>model.animations[*selector->clip].duration))
                throw std::runtime_error("Source reference take or time is outside the original clip.");
            pose=CompiledAnimation(model).sample(selector->clip,selector->time,false);
        }
        std::size_t vertices=0,indices=0;for(const auto& mesh:model.primitives) {vertices+=mesh->vertices.size();indices+=mesh->indices.size();}
        Json diagnostics=Json::array();
        for(std::size_t i=0;i<std::min<std::size_t>(64,model.diagnostics.size());++i)diagnostics.push_back(asset_diagnostic(model.diagnostics[i].c_str()));
        Json out={{"format","poima.source-inspection.v1"},{"published",false},{"model_sha256",fingerprint},
            {"importer",imported.importer.empty() ? "cgltf-1.15/poima-animation-reference-1" : imported.importer},
            {"retained_bytes",retained},{"nodes",model.nodes.size()},{"primitives",model.primitives.size()},
            {"skins",model.skins.size()},{"animations",model.animations.size()},{"images",model.images.size()},
            {"vertices",vertices},{"triangles",indices/3},{"roots",model.roots},{"diagnostics",std::move(diagnostics)},
            {"diagnostic_count",model.diagnostics.size()},{"diagnostics_truncated",model.diagnostics.size()>64}};
        if(pose)out["pose"]=params.at("pose");
        if(section!="summary") {
            const auto total=selected_skin ? model.skins[*selected_skin].joints.size() : selected_clip ? model.animations[*selected_clip].channels.size() :
                section=="nodes" ? model.nodes.size() : section=="skins" ? model.skins.size() : model.animations.size();
            const auto end=offset>=total ? total:offset+std::min<std::uint64_t>(limit,total-offset);
            out["items"]=Json::array();out["section"]=section;out["total"]=total;
            if(selected_skin)out["skin"]=*selected_skin;
            if(selected_clip)out["clip"]=*selected_clip;
            for(auto i=offset;i<end;++i) {
                if(section=="nodes") {
                    const auto& node=model.nodes[i];
                    Json row={{"index",i},{"name",node.name},{"parent",node.parent},{"primitives",node.primitives},{"skin",node.skin}};
                    if(pose) {
                        const auto& local=pose->local[i];row["position"]=local.position;row["rotation"]=local.rotation;row["scale"]=local.scale;row["world"]=pose->world[i];
                    }else {row["position"]=node.position;row["rotation"]=node.rotation;row["scale"]=node.scale;}
                    out["items"].push_back(std::move(row));
                }else if(section=="skins") {
                    const auto& skin=model.skins[i];out["items"].push_back({{"index",i},{"name",skin.name},{"skeleton",skin.skeleton},{"joints",skin.joints.size()}});
                }else if(selected_skin) {
                    const auto& skin=model.skins[*selected_skin];out["items"].push_back({{"index",i},{"node",skin.joints[i]},{"inverse_bind",skin.inverse_bind[i]}});
                }else if(selected_clip) {
                    const auto& channel=model.animations[*selected_clip].channels[i];
                    const char* paths[]={"translation","rotation","scale"};const char* modes[]={"STEP","LINEAR","CUBICSPLINE"};
                    out["items"].push_back({{"index",i},{"node",channel.node},{"path",paths[std::size_t(channel.path)]},
                        {"interpolation",modes[std::size_t(channel.interpolation)]},{"keys",channel.times.size()}});
                }else {
                    const auto& clip=model.animations[i];out["items"].push_back({{"index",i},{"name",clip.name},{"duration",clip.duration},{"channels",clip.channels.size()}});
                }
            }
            out["next_offset"]=end<total ? Json(end) : Json(nullptr);
        }
        return out;
    }catch(const Error&) {throw;}
    catch(const FbxTextureDependencyError& error) {throw fbx_texture_dependency_error(error,true);}
    catch(const FbxMaterialProfileError& error) {throw fbx_material_profile_error(error,params.value("fbx_profile_diagnostics",false),true);}
    catch(const std::exception& error) {throw Error(-32050,asset_diagnostic(error.what()));}
}

Json model_asset_dispatch(const std::string& method,const Json& params,const fs::path& world_path,
                          const fs::path& assets) {
    if(method=="asset.mesh.triangles") {
        fields(params,{"asset","primitive","offset","limit"},{"asset","primitive"});
        require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid model asset ID.");
        const auto primitive=revision(params.at("primitive"));
        const auto offset=params.contains("offset")?revision(params.at("offset")):0;
        const auto limit=params.contains("limit")?revision(params.at("limit")):64;
        require(limit>=1 && limit<=64,"Triangle page limit must be 1..64.");
        try {
            const auto loaded=read_model_asset(assets,params.at("asset"));
            require(primitive<loaded.model->primitives.size(),"Primitive index is outside the cooked model.");
            const auto& indices=loaded.model->primitives[primitive]->indices;
            const auto total=indices.size()/3;
            const auto end=std::min<std::uint64_t>(total,offset+limit);
            auto items=Json::array();
            for(auto i=offset;i<end;++i)
                items.push_back({{"index",i},{"vertices",{indices[3*i],indices[3*i+1],indices[3*i+2]}}});
            return {{"asset",loaded.id},{"primitive",primitive},{"total",total},{"items",std::move(items)},
                {"next_offset",end<total?Json(end):Json(nullptr)}};
        } catch(const Error&) {throw;}
        catch(const std::exception& error) {throw Error(-32050,error.what());}
    }
    try {
        LoadedModel loaded;
        if(method=="asset.import") {
            fields(params,{"source","animations","fbx_normal_map","fbx_geometry_only","fbx_profile_diagnostics","expected_model_sha256"},{"source"});require(params.at("source").is_string(),"Source path must be a string.");
            require(!params.contains("fbx_profile_diagnostics") || params.at("fbx_profile_diagnostics").is_boolean(),"fbx_profile_diagnostics must be Boolean.");
            require(!params.contains("fbx_geometry_only") || params.at("fbx_geometry_only").is_boolean(),"fbx_geometry_only must be Boolean.");
            std::optional<std::string> expected_model;
            if(params.contains("expected_model_sha256")) {
                const auto& value=params.at("expected_model_sha256");
                require(value.is_string() && valid_asset_id(value.get<std::string>()),"Expected source model SHA256 must be 64 lowercase hexadecimal characters.");
                expected_model=value.get<std::string>();
            }
            const auto text=params.at("source").get<std::string>();require(!text.empty() && text.find('\0')==std::string::npos,"Invalid source path.");
            auto source=fs::path(std::u8string(text.begin(),text.end()));if(source.is_relative())source=world_path.parent_path()/source;
            std::optional<FbxNormalConvention> normal_map;
            if(params.contains("fbx_normal_map")) {
                require(params.at("fbx_normal_map").is_string(),"FBX normal map convention must be opengl or directx.");
                const auto convention=params.at("fbx_normal_map").get<std::string>();require(convention=="opengl" || convention=="directx","Invalid FBX normal map convention.");
                normal_map=convention=="opengl" ? FbxNormalConvention::opengl : FbxNormalConvention::directx;
            }
            std::vector<ModelAnimationSource> animations;
            if(params.contains("animations")) {
                const auto& files=params.at("animations");require(files.is_array() && !files.empty() && files.size()<=32,"Animations must contain 1..32 source paths.");
                for(const auto& file:files) {
                    ModelAnimationSource selected;std::string value;
                    if(file.is_object()) {
                        fields(file,{"source","clip","name","frame_transfer","retarget","expected_model_sha256"},{"source"});require(file.at("source").is_string(),"Animation source path must be a string.");value=file.at("source").get<std::string>();
                        if(file.contains("expected_model_sha256")) {
                            const auto& expected=file.at("expected_model_sha256");
                            require(expected.is_string() && valid_asset_id(expected.get<std::string>()),"Expected animation source model SHA256 must be 64 lowercase hexadecimal characters.");
                            selected.expected_model_sha256=expected.get<std::string>();
                        }
                        require(!file.contains("frame_transfer") || !file.contains("retarget"),"Frame transfer and retarget policies are mutually exclusive.");
                        if(file.contains("clip")) {const auto index=revision(file.at("clip"));require(index<256,"Animation take index must be 0..255.");selected.clip=static_cast<std::uint32_t>(index);}
                        if(file.contains("name")) {
                            require(file.at("name").is_string(),"Animation take name must be a string.");selected.name=file.at("name").get<std::string>();
                            require(!selected.name.empty() && selected.name.size()<=256 && selected.name.find('\0')==std::string::npos,"Animation take name must be 1..256 bytes without NUL.");
                        }
                        if(file.contains("frame_transfer"))selected.frame_transfer=animation_frame_transfer(file.at("frame_transfer"));
                        if(file.contains("retarget"))selected.rotation_retarget=animation_rotation_retarget(file.at("retarget"));
                    }else {require(file.is_string(),"Animation source must be a path or a selection object.");value=file.get<std::string>();}
                    require(!value.empty() && value.find('\0')==std::string::npos,"Invalid animation source path.");
                    auto path=fs::path(std::u8string(value.begin(),value.end()));if(path.is_relative())path=world_path.parent_path()/path;
                    selected.source=std::move(path);animations.push_back(std::move(selected));
                }
            }
            loaded=store_model_asset(assets,source,animations,normal_map,expected_model,params.value("fbx_geometry_only",false));
        } else if(method=="asset.inspect") {
            fields(params,{"asset","section","offset","limit"},{"asset"});
            require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid asset ID.");
            loaded=read_model_asset(assets,params.at("asset"));
        } else throw Error(-32601,"Unknown asset method.");
        std::size_t vertices=0,indices=0;for(const auto& mesh:loaded.model->primitives) { vertices+=mesh->vertices.size();indices+=mesh->indices.size(); }
        Json result={{"asset",loaded.id},{"bytes",loaded.bytes},{"nodes",loaded.model->nodes.size()},{"primitives",loaded.model->primitives.size()},
            {"vertices",vertices},{"triangles",indices/3},{"roots",loaded.model->roots},{"diagnostics",loaded.model->diagnostics},{"images",loaded.model->images.size()},{"skins",loaded.model->skins.size()},{"animations",loaded.model->animations.size()},{"format",std::string(loaded.model->package_version>=4 ? "poima.model.v" : "poima.static-model.v")+std::to_string(loaded.model->package_version)}};
        if(method=="asset.inspect") {
            const auto section=params.value("section",std::string("summary"));require(section=="summary" || section=="nodes" || section=="primitives" || section=="images" || section=="skins" || section=="animations","Invalid asset section.");
            const auto offset=params.contains("offset") ? revision(params.at("offset")) : 0;
            const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;require(limit>=1 && limit<=64,"Asset page limit must be 1..64.");
            if(section!="summary") {
                result["items"]=Json::array();const auto total=section=="skins" ? loaded.model->skins.size() : section=="animations" ? loaded.model->animations.size() : section=="nodes" ? loaded.model->nodes.size() : section=="images" ? loaded.model->images.size() : loaded.model->primitives.size();
                const auto end=std::min<std::uint64_t>(total,offset+limit);
                for(auto i=offset;i<end;++i) {
                    if(section=="skins") { const auto& skin=loaded.model->skins[i];result["items"].push_back({{"index",i},{"name",skin.name},{"skeleton",skin.skeleton},{"joints",skin.joints.size()}}); }
                    else if(section=="animations") { const auto& clip=loaded.model->animations[i];result["items"].push_back({{"index",i},{"name",clip.name},{"duration",clip.duration},{"channels",clip.channels.size()}}); }
                    else if(section=="nodes") { const auto& node=loaded.model->nodes[i];result["items"].push_back({{"index",i},{"name",node.name},{"parent",node.parent},{"position",node.position},{"rotation",node.rotation},{"scale",node.scale},{"primitives",node.primitives},{"skin",node.skin}}); }
                    else if(section=="images") {
                        const auto& image=*loaded.model->images[i];Json mips=Json::array();std::size_t bytes=0;
                        for(const auto& mip:image.mips) { bytes+=mip.rgba.size();mips.push_back({{"width",mip.width},{"height",mip.height},{"bytes",mip.rgba.size()}}); }
                        result["items"].push_back({{"index",i},{"color_space",image.srgb ? "srgb" : "linear"},{"mips",mips},{"bytes",bytes}});
                    } else {
                        const auto& mesh=*loaded.model->primitives[i];Json maps=Json::object();const char* names[]={"base_color","metallic_roughness","emissive","occlusion","normal"};
                        for(std::size_t slot=0;slot<5;++slot) {
                            const auto& map=mesh.textures[slot];maps[names[slot]]=nullptr;if(!map.image)continue;
                            maps[names[slot]]={{"image",std::size_t(std::find(loaded.model->images.begin(),loaded.model->images.end(),map.image)-loaded.model->images.begin())},
                                {"wrap_s",map.wrap_s},{"wrap_t",map.wrap_t},{"min_filter",map.min_filter},{"mag_filter",map.mag_filter}};
                        }
                        result["items"].push_back({{"index",i},{"vertices",mesh.vertices.size()},{"triangles",mesh.indices.size()/3},{"material",material_json(mesh.material)},{"textures",maps},{"occlusion_strength",mesh.occlusion_strength},{"normal_scale",mesh.normal_scale},{"has_uv",mesh.has_uv},{"skinned",!mesh.influences.empty()},{"tangent_frames",std::all_of(mesh.vertices.begin(),mesh.vertices.end(),valid_tangent)}});
                    }
                }
                result["next_offset"]=end<total ? Json(end) : Json(nullptr);
            }
        }
        return result;
    } catch(const Error&) { throw; }
    catch(const ModelSourceFingerprintError& error) {
        const auto& diagnostic=error.diagnostic();
        throw Error(-32050,error.what(),{{"type","model_source_changed"},{"version",1},
            {"source_role",diagnostic.role==ModelSourceRole::base ? "base" : "animation_donor"},
            {"donor_index",diagnostic.donor_index ? Json(*diagnostic.donor_index) : Json(nullptr)},
            {"expected_model_sha256",diagnostic.expected_model_sha256},
            {"actual_model_sha256",diagnostic.actual_model_sha256}});
    }
    catch(const FbxTextureDependencyError& error) { throw fbx_texture_dependency_error(error); }
    catch(const FbxMaterialProfileError& error) { throw fbx_material_profile_error(error,params.value("fbx_profile_diagnostics",false)); }
    catch(const AnimationCompositionError& error) {
        const auto& report=error.report();
        Json issues=Json::array();
        auto diagnostic_name=[](const std::string& value) {
            return Json::parse(Json(value).dump(-1,' ',false,Json::error_handler_t::replace));
        };
        for(const auto& issue:report.issues) {
            const auto kind=issue.kind==AnimationCompositionIssueKind::rest_frame ? "rest_frame" :
                issue.kind==AnimationCompositionIssueKind::missing_joint_or_ancestor ? "missing_joint_or_ancestor" : "missing_animation_target";
            Json row={{"kind",kind},{"base_node",issue.base_node ? Json(*issue.base_node) : Json(nullptr)},
                {"donor_node",issue.donor_node ? Json(*issue.donor_node) : Json(nullptr)},
                {"base_name",issue.base_node ? diagnostic_name(issue.base_name) : Json(nullptr)},
                {"donor_name",issue.donor_node ? diagnostic_name(issue.donor_name) : Json(nullptr)},
                {"required_skeleton",issue.required_skeleton},{"animated_ancestry",issue.animated_ancestry}};
            if(issue.kind==AnimationCompositionIssueKind::rest_frame) {
                row["metrics"]={{"max_translation_meters",issue.max_translation_meters},
                    {"max_scale_absolute",issue.max_scale_absolute},{"absolute_quaternion_dot",issue.absolute_quaternion_dot},
                    {"rotation_degrees",issue.rotation_degrees}};
                row["mismatches"]={{"translation",issue.translation_mismatch},{"rotation",issue.rotation_mismatch},{"scale",issue.scale_mismatch}};
            }
            issues.push_back(std::move(row));
        }
        throw Error(-32050,asset_diagnostic(error.what()),
            {{"type","animation_composition"},{"version",1},{"policy","exact-skeleton-v1"},
             {"comparison","normalized_local_rest"},{"donor_index",report.donor_index},
             {"required_base_nodes",report.required_base_nodes},{"checked_source_nodes",report.checked_source_nodes},
             {"matched_nodes",report.matched_nodes},{"issue_count",report.issue_count},
             {"limit",animation_composition_issue_limit},{"truncated",report.truncated},
             {"thresholds",{{"translation_meters",animation_composition_translation_tolerance},
                 {"scale_absolute",animation_composition_scale_tolerance},
                 {"absolute_quaternion_dot_min",animation_composition_quaternion_dot_min}}},
             {"issues",std::move(issues)}});
    }
    catch(const std::exception& error) { throw Error(-32050,asset_diagnostic(error.what())); }
}

} // namespace poima::world_detail
