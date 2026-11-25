// SPDX-License-Identifier: Apache-2.0
// Included after Runtime::Impl: the save adapter uses typed engine state and
// public Jolt setters. The optional physical extension reconstructs a trusted
// recorder from strictly validated typed fields; arbitrary bytes are not accepted.
namespace {
using SaveJson=nlohmann::json;
constexpr std::size_t snapshot_bytes_limit=64*1024*1024;
constexpr std::uint64_t snapshot_integer_limit=9007199254740991ULL;
void save_fields(const SaveJson& v,std::initializer_list<const char*> keys) {
    require(v.is_object() && v.size()==keys.size(),"Invalid runtime snapshot object fields.");
    for(const auto* key:keys)require(v.contains(key),"Missing runtime snapshot field.");
}
bool save_digest(const std::string& s) { return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos; }
std::string save_hash(const std::string& s) { return sha256(std::as_bytes(std::span(s.data(),s.size()))); }
std::uint64_t save_integer(const SaveJson& v,std::uint64_t maximum=snapshot_integer_limit) {
    require(v.is_number_integer() && v>=0 && v<=maximum,"Runtime snapshot integer is out of range.");return v.get<std::uint64_t>();
}
double save_number(const SaveJson& v,double limit) {
    require(v.is_number(),"Runtime snapshot requires a number.");const auto result=v.get<double>();
    require(std::isfinite(result) && std::abs(result)<=limit,"Runtime snapshot number is out of range.");return result;
}
SolarLightingSample read_solar_lighting(const SaveJson& data) {
    save_fields(data,{"version","world_milliseconds","toward_sun","sun_gain","sun_tint","sky_gain","ambient"});
    require(save_integer(data.at("version"))==1,"Unsupported solar lighting snapshot version.");
    const auto magnitude=[](const SaveJson& value,double limit) {
        const auto number=save_number(value,limit);
        require(number>=0,"Solar lighting snapshot values must be nonnegative.");
        return static_cast<float>(number);
    };
    const auto& direction=data.at("toward_sun");
    const auto& tint=data.at("sun_tint");
    const auto& ambient=data.at("ambient");
    require(direction.is_array() && direction.size()==3 && tint.is_array() && tint.size()==3 &&
        ambient.is_array() && ambient.size()==3,"Solar lighting snapshot vectors need three values.");
    SolarLightingSample result;
    result.world_milliseconds=save_integer(data.at("world_milliseconds"),max_solar_world_milliseconds);
    result.sun_gain=magnitude(data.at("sun_gain"),1);
    result.sky_gain=magnitude(data.at("sky_gain"),1);
    for(std::size_t axis=0;axis<3;++axis) {
        result.toward_sun[axis]=save_number(direction[axis],1.000001);
        result.sun_tint[axis]=magnitude(tint[axis],1);
        result.ambient[axis]=magnitude(ambient[axis],1e6);
    }
    return result;
}
std::string save_text(const SaveJson& v,std::size_t bound=256,bool empty=false) {
    require(v.is_string(),"Runtime snapshot requires a string.");const auto s=v.get<std::string>();
    require((empty || !s.empty()) && s.size()<=bound && s.find('\0')==std::string::npos,"Runtime snapshot string is out of range.");return s;
}
std::map<std::string,RuntimeAttachmentOutcome> read_attachment_outcomes(const SaveJson& data,std::uint64_t tick) {
    save_fields(data,{"version","tick","outcomes"});
    const auto version=save_integer(data.at("version"),5);
    require(version>=1 && tick>0 && save_integer(data.at("tick"))==tick,
        "Saved attachment outcomes must match the committed tick.");
    const auto& rows=data.at("outcomes");
    require(rows.is_array() && !rows.empty() && rows.size()<=max_runtime_attachments,
        "Saved attachment outcomes require 1..64 entries.");
    const auto entity=[](const SaveJson& value) {
        auto id=save_text(value,32);
        require(id.size()==32 && id!=std::string(32,'0') && id.find_first_not_of("0123456789abcdef")==std::string::npos,
            "Saved attachment outcome identity is invalid.");
        return id;
    };
    const auto magnitude=[](const SaveJson& value,double max) {
        const auto number=save_number(value,max);
        require(number>=0,"Saved attachment measurement must be nonnegative.");
        return number;
    };
    std::map<std::string,RuntimeAttachmentOutcome> result;
    unsigned witness_version=1;
    for(const auto& row:rows) {
        const auto id=entity(row.at("entity")),anchor=entity(row.at("anchor"));
        require(id!=anchor && (result.empty() || result.rbegin()->first<id),
            "Saved attachment outcomes must be unique and sorted with distinct anchors.");
        const auto status=save_text(row.at("status"),16);
        RuntimeAttachmentOutcome outcome{anchor,{}};
        if(status=="frame_miss") {
            save_fields(row,{"entity","anchor","status","position_error_m","position_limit_m","rotation_error_deg","rotation_limit_deg"});
            RuntimeAttachmentFrameMiss detail{magnitude(row.at("position_error_m"),4e12),
                magnitude(row.at("position_limit_m"),1e9),magnitude(row.at("rotation_limit_deg"),180.),{}};
            if(!row.at("rotation_error_deg").is_null())detail.rotation_error_deg=magnitude(row.at("rotation_error_deg"),180.);
            require(detail.position_error_m>detail.position_limit_m || !detail.rotation_error_deg ||
                *detail.rotation_error_deg>detail.rotation_limit_deg,"Saved frame outcome does not describe a miss.");
            outcome.detail=detail;
        } else if(status=="surface_miss") {
            save_fields(row,{"entity","anchor","status","probe_index","target_triangle","distance_m","distance_limit_m"});
            RuntimeAttachmentSurfaceMiss detail{static_cast<std::size_t>(save_integer(row.at("probe_index"),max_attachment_surface_probes-1)),
                static_cast<std::size_t>(save_integer(row.at("target_triangle"),UINT32_MAX)),
                magnitude(row.at("distance_m"),4e12),magnitude(row.at("distance_limit_m"),1e9)};
            require(detail.distance_m>detail.distance_limit_m,"Saved surface outcome does not describe a miss.");
            outcome.detail=detail;
        } else if(status=="release_blocked") {
            save_fields(row,{"entity","anchor","status","obstacle","penetration_m"});
            const auto depth=magnitude(row.at("penetration_m"),4e12);
            require(depth>.001f,"Saved release obstruction must exceed the clearance threshold.");
            outcome.detail=RuntimeAttachmentReleaseBlocked{entity(row.at("obstacle")),depth};
        } else if(status=="reach_blocked") {
            using Pose=RuntimeAttachmentReachBlocked::Pose;
            auto pose=Pose::Unchanged;
            if(row.contains("pose")) {
                save_fields(row,{"entity","anchor","status","obstacle","segment","interval","fraction","penetration_m","pose"});
                const auto name=save_text(row.at("pose"),10);
                if(name=="previous")pose=Pose::Previous;
                else if(name=="animation")pose=Pose::Animation;
                else if(name=="unresolved")pose=Pose::Unresolved;
                else if(name=="blended")pose=Pose::Blended;
                else if(name=="steered")pose=Pose::Steered;
                else throw std::runtime_error("Invalid saved arm fallback pose.");
                witness_version=std::max(witness_version,pose==Pose::Steered ? 5u : pose==Pose::Blended ? 4u : 3u);
            } else {
                save_fields(row,{"entity","anchor","status","obstacle","segment","interval","fraction","penetration_m"});
                witness_version=std::max(witness_version,2u);
            }
            const auto segment=save_integer(row.at("segment"),2);
            const auto interval=save_integer(row.at("interval"),ik_clearance_intervals);
            const auto fraction=magnitude(row.at("fraction"),1);
            const auto depth=magnitude(row.at("penetration_m"),1e6);
            require(segment>0 && interval>0 && (fraction==0 || depth==0),
                "Invalid saved arm clearance witness.");
            outcome.detail=RuntimeAttachmentReachBlocked{entity(row.at("obstacle")),
                static_cast<std::uint32_t>(segment),static_cast<std::uint32_t>(interval),fraction,depth,pose};
        } else {
            save_fields(row,{"entity","anchor","status"});
            require(status=="transferred" || status=="released","Unknown saved attachment outcome.");
            if(status=="released")outcome.detail=RuntimeAttachmentReleased{};
        }
        result.emplace(id,std::move(outcome));
    }
    require(version==witness_version,"Saved attachment outcome version differs from its witness types.");
    return result;
}
template<std::size_t N>std::array<double,N> save_vector(const SaveJson& v,double bound) {
    require(v.is_array() && v.size()==N,"Invalid runtime snapshot vector.");std::array<double,N> out{};
    for(std::size_t i=0;i<N;++i)out[i]=save_number(v[i],bound);
    return out;
}
std::array<double,4> save_rotation(const SaveJson& v) {
    const auto out=save_vector<4>(v,1.00001);double norm=0;for(auto x:out)norm+=x*x;
    require(std::abs(norm-1)<=1e-5,"Runtime snapshot rotation must be normalized.");return out;
}
JPH::Quat save_quat(const std::array<double,4>& v) { return JPH::Quat(static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2]),static_cast<float>(v[3])); }
JPH::RVec3 save_position(const std::array<double,3>& v) { return JPH::RVec3(v[0],v[1],v[2]); }
JPH::Vec3 save_velocity(const std::array<double,3>& v) { return JPH::Vec3(static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2])); }
template<class T>SaveJson save_xyz(const T& v) { return SaveJson::array({v.GetX(),v.GetY(),v.GetZ()}); }
SaveJson save_xyzw(JPH::QuatArg v) { return SaveJson::array({v.GetX(),v.GetY(),v.GetZ(),v.GetW()}); }
SaveJson save_local(const RuntimeTransform& t) { return {{"position",t.position},{"rotation",t.rotation},{"scale",t.scale}}; }
RuntimeTransform save_transform(const SaveJson& v) {
    save_fields(v,{"position","rotation","scale"});RuntimeTransform t;
    t.position=save_vector<3>(v.at("position"),1e9);t.rotation=save_rotation(v.at("rotation"));t.scale=save_vector<3>(v.at("scale"),1e9);
    for(auto x:t.scale)require(x>0,"Runtime snapshot scale must be positive.");
    return t;
}
template<std::size_t N>bool save_near(const std::array<double,N>& a,const std::array<double,N>& b,double tolerance=1e-6) {
    for(std::size_t i=0;i<N;++i)if(std::abs(a[i]-b[i])>tolerance)return false;
    return true;
}
void save_budget(const SaveJson& value,std::size_t depth,std::size_t& tokens) {
    tokens+=value.is_structured() ? 2 : 1;
    if(value.is_object())tokens+=value.size();
    require(depth<=32 && tokens<=2000000,"Runtime snapshot nesting/token budget exceeded.");
    if(value.is_structured())for(const auto& child:value)save_budget(child,depth+1,tokens);
}
SaveJson save_parse(const std::string& bytes) {
    require(!bytes.empty() && bytes.size()<=snapshot_bytes_limit,"Runtime snapshot exceeds 64 MiB.");
    std::vector<std::set<std::string>> keys;std::size_t tokens=0;
    auto callback=[&](int depth,SaveJson::parse_event_t event,SaveJson& parsed) {
        require(depth<=32 && ++tokens<=2000000,"Runtime snapshot nesting/token budget exceeded.");
        if(event==SaveJson::parse_event_t::object_start)keys.emplace_back();
        if(event==SaveJson::parse_event_t::key)require(!keys.empty() && keys.back().insert(parsed.get<std::string>()).second,"Duplicate runtime snapshot key.");
        if(event==SaveJson::parse_event_t::object_end)keys.pop_back();
        return true;
    };
    return SaveJson::parse(bytes,callback);
}
void save_game_values(const SaveJson& g,const std::map<std::string,entt::entity>& ids) {
    save_fields(g,{"backend","assembly_sha256","type","schema","values"});
    require(g.at("backend")=="coreclr" || g.at("backend")=="native_aot","Unsupported saved gameplay backend.");
    require(save_digest(save_text(g.at("assembly_sha256"),64)),"Invalid saved gameplay image hash.");
    (void)save_text(g.at("type"),512,true);
    const auto& schema=g.at("schema");validate_gameplay_schema(schema.dump());
    const auto& values=g.at("values");require(values.is_object() && values.size()==schema.at("fields").size(),"Saved gameplay fields must be complete.");
    (void)validate_gameplay_values(schema.dump(),values.dump());
    for(const auto& field:schema.at("fields")) {
        const auto name=field.at("name").get<std::string>();require(values.contains(name),"Saved gameplay field is missing.");
        if(field.at("kind")=="entity") {
            const auto id=values.at(name).get<std::string>();
            require(id==std::string(32,'0') || ids.contains(id),"Saved gameplay entity reference does not resolve in this world.");
        }
    }
}
bool save_stance_definition(const RuntimeDefinition& definition) {
    const auto configured=[](const auto& entity) {return entity.character && entity.character->stance;};
    if(std::any_of(definition.entities.begin(),definition.entities.end(),configured))return true;
    for(const auto& recipe:definition.templates)
        if(std::any_of(recipe.entities.begin(),recipe.entities.end(),configured))return true;
    return false;
}
bool save_locomotion_definition(const RuntimeDefinition& definition) {
    const auto configured=[](const auto& entity) {return entity.character && entity.character->locomotion;};
    if(std::any_of(definition.entities.begin(),definition.entities.end(),configured))return true;
    for(const auto& recipe:definition.templates)
        if(std::any_of(recipe.entities.begin(),recipe.entities.end(),configured))return true;
    return false;
}
void save_stance_flags(bool requested,bool crouched,bool blocked) {
    require((!requested || crouched) && blocked==(!requested && crouched),
        "Snapshot character stance flags are inconsistent.");
}
}

#include "runtime_physical_save.inc"

std::optional<std::string> Runtime::Impl::restore_rain_shelter(const SaveJson& value,const std::string& source) const {
    if(value.is_null())return {};
    auto blocker=save_text(value,32);
    require(blocker.size()==32 && blocker!=source &&
        blocker.find_first_not_of("0123456789abcdef")==std::string::npos &&
        blocker.find_first_not_of('0')!=std::string::npos,"Invalid saved wetness shelter identity.");
    const auto live=topology->definitions.find(blocker);
    if(live!=topology->definitions.end()) {
        const auto& entity=live->second;
        require(entity.mesh_collider || (entity.collider && entity.collider->motion!=BodyMotion::Dynamic),
            "Saved wetness shelter must be static or kinematic collision.");
    } else require(entity_ids->allocated(gameplay_id(blocker)),
        "Saved wetness shelter has no allocated identity.");
    return blocker;
}

void Runtime::Impl::restore_wetness(const SaveJson& archive) {
    save_fields(archive,{"version","next_quantum","receivers"});
    require(save_integer(archive.at("version"))==1,"Unsupported wetness snapshot version.");
    auto state=std::make_shared<wetness::State>(*wetness_live);
    if(!archive.at("next_quantum").is_null()) {
        const auto next=save_integer(archive.at("next_quantum"),wetness::max_quantum+1);
        require(tick>0 && next>0,"Saved wetness clock must follow an accepted game second.");
        state->next_quantum=next;
    }
    const auto& rows=archive.at("receivers");
    require(rows.is_array() && rows.size()==state->receivers.size(),
        "Saved wetness receiver membership differs from frozen content.");
    auto expected=state->receivers.begin();
    for(const auto& row:rows) {
        save_fields(row,{"entity","film_nm_q16","blocker"});
        const auto id=save_text(row.at("entity"),32);
        require(id==expected->first,"Saved wetness receivers must match their frozen sorted IDs.");
        auto& receiver=expected++->second;
        const auto capacity=std::uint64_t(wetness_profile(id)->capacity_nm)*wetness::units_per_nm;
        receiver.film_nm_q16=save_integer(row.at("film_nm_q16"),capacity);
        receiver.blocker=restore_rain_shelter(row.at("blocker"),id);
        require(state->next_quantum || (!receiver.film_nm_q16 && !receiver.blocker),
            "A saved wetness receiver needs an accepted game second before it can record film or shelter.");
    }
    wetness_live=std::move(state);
}

SaveJson Runtime::Impl::archive_precipitation() const {
    const auto& state=*particle_runtime;
    const auto& window=*state.rain;
    auto sources=SaveJson::array();
    for(const auto& [id,source]:state.sources)if(source.definition.rain) {
        const auto& rain=source.rain;
        sources.push_back({{"id",id},{"phase",rain.phase},{"scheduled_births",rain.scheduled_births},
            {"blocker",rain.blocker ? SaveJson(*rain.blocker) : SaveJson(nullptr)}});
    }
    return {{"version",1},{"rain_nm",window.rain_nm ? SaveJson(*window.rain_nm) : SaveJson(nullptr)},
        {"ticks_remaining",window.ticks_remaining},
        {"sample_tick",window.sample_tick ? SaveJson(*window.sample_tick) : SaveJson(nullptr)},
        {"sources",std::move(sources)}};
}

void Runtime::Impl::restore_precipitation(const SaveJson& archive) {
    save_fields(archive,{"version","rain_nm","ticks_remaining","sample_tick","sources"});
    require(save_integer(archive.at("version"))==1,"Unsupported precipitation snapshot version.");
    auto& state=*particle_runtime;
    auto& window=*state.rain;
    if(!archive.at("rain_nm").is_null())
        window.rain_nm=static_cast<std::uint32_t>(save_integer(archive.at("rain_nm"),wetness::max_rain_nm));
    window.ticks_remaining=static_cast<std::uint32_t>(
        save_integer(archive.at("ticks_remaining"),rain_particle_window_ticks-1));
    if(!archive.at("sample_tick").is_null())window.sample_tick=save_integer(archive.at("sample_tick"),tick);
    require(window.rain_nm.has_value()==wetness_live->next_quantum.has_value() &&
        (!window.sample_tick || (window.rain_nm && *window.sample_tick>0)) &&
        (!window.rain_nm || !*window.rain_nm || window.sample_tick) &&
        (!window.ticks_remaining || (window.rain_nm && *window.rain_nm && window.sample_tick==tick)),
        "Saved precipitation window differs from its committed clock.");
    if(window.rain_nm && *window.rain_nm) {
        require(*window.sample_tick>=rain_particle_window_ticks-window.ticks_remaining,
            "Saved precipitation window could not have elapsed by its last sample.");
    }
    if(water_region && !water_region->rain_cells().empty()) {
        require(window.rain_nm.value_or(0)<=water_region->cumulative_rain_nm(),
            "Saved precipitation depth exceeds the water region's cumulative rainfall.");
    }
    const auto& rows=archive.at("sources");
    require(rows.is_array(),"Saved precipitation sources must be an array.");
    auto row=rows.begin();
    for(auto& [id,source]:state.sources)if(source.definition.rain) {
        require(row!=rows.end(),"Saved precipitation sources differ from frozen content.");
        save_fields(*row,{"id","phase","scheduled_births","blocker"});
        require(save_integer(row->at("id"))==id,"Saved precipitation sources differ from frozen emitter order.");
        const auto& profile=*source.definition.rain;
        auto& rain=source.rain;
        rain.phase=static_cast<std::uint32_t>(save_integer(row->at("phase"),
            profile.nm_per_particle*rain_particle_window_ticks-1));
        rain.scheduled_births=static_cast<std::uint32_t>(save_integer(row->at("scheduled_births"),
            profile.max_particles_per_tick));
        rain.blocker=restore_rain_shelter(row->at("blocker"),source.entity);
        require((window.sample_tick && source.definition.config.emitting) ||
            (!rain.phase && !rain.scheduled_births && !rain.blocker),
            "Saved precipitation needs a sampled window and an enabled source.");
        ++row;
    }
    require(row==rows.end(),"Saved precipitation sources differ from frozen content.");
}

void Runtime::Impl::restore_particles(const RuntimeDefinition& definition,const SaveJson& particle_extension) {
    const auto typed=particles::parse_snapshot_json(particle_extension.at("state"));
    require(typed.status.limits==runtime_particle_limits && typed.status.tick==tick,
        "Saved particle limits or tick differ from their runtime.");
    std::map<particles::EmitterId,const particles::SnapshotEmitter*> emitters;
    for(const auto& emitter:typed.emitters)if(emitter.id)
        require(emitters.emplace(emitter.id,&emitter).second,"Saved particle emitter IDs must be unique.");
    const auto& source_rows=particle_extension.at("sources");
    require(source_rows.is_array() && source_rows.size()==emitters.size() && source_rows.size()<=runtime_particle_limits.emitters,
        "Saved particle sources differ from their emitter slots.");
    std::map<std::string,particles::EmitterId> authored;
    for(const auto& entity:definition.entities)if(entity.particle_emitter)authored.emplace(entity.id,0);
    particles::EmitterId authored_count=0;
    for(auto& [name,id]:authored) { (void)name;id=++authored_count; }
    const auto frontier=save_integer(particle_extension.at("next_emitter_id"));
    require(frontier>authored_count && (structure_revision!=0 || frontier==authored_count+1),
        "Saved particle emitter identity frontier differs from its source history.");
    std::set<std::string> source_entities;
    particles::EmitterId previous_id=0;
    std::size_t active_count=0;
    bool has_water_flow=false;
    std::map<particles::EmitterId,ParticleBinding> sources;
    for(const auto& row:source_rows) {
        if (row.contains("water_flow")) save_fields(row,{"id","entity","template_id","template_node","draining","water_flow"});
        else save_fields(row,{"id","entity","template_id","template_node","draining"});
        const auto id=save_integer(row.at("id"));
        require(id>previous_id && id<frontier && emitters.contains(id),"Saved particle sources must be in allocated emitter order.");
        previous_id=id;
        const auto entity=save_text(row.at("entity"));
        const auto template_id=save_text(row.at("template_id"),256,true);
        const auto template_node=save_text(row.at("template_node"),256,true);
        require(source_entities.insert(entity).second && row.at("draining").is_boolean(),"Invalid or duplicate saved particle source.");
        const auto draining=row.at("draining").get<bool>();
        const auto& emitter=*emitters.at(id);
        const auto active=topology->definitions.find(entity);
        const auto original=authored.find(entity);
        require(original==authored.end() ? id>authored_count : id==original->second,
            "Saved particle emitter identity differs from its authored allocation.");
        const RuntimeParticleEmitter* frozen=nullptr;
        if(draining) {
            const auto retired=gameplay_id(entity);
            require(entity==gameplay_id(retired) && active==topology->definitions.end() && original==authored.end() &&
                entity_ids->allocated(retired),"Draining particle source needs a retired allocated entity.");
            const auto& recipe=spawn_template(template_id);
            if(recipe.entities.empty()) {
                if(template_node==recipe.id && recipe.particle_emitter)frozen=&*recipe.particle_emitter;
            } else {
                const auto found=std::find_if(recipe.entities.begin(),recipe.entities.end(),[&](const auto& node){return node.id==template_node;});
                if(found!=recipe.entities.end() && found->particle_emitter)frozen=&*found->particle_emitter;
            }
            require(frozen && frozen->config.simulation_space==particles::Space::World && emitter.status.particles.live,
                "Saved draining particle source must retain live world particles from a frozen template node.");
        } else {
            require(active!=topology->definitions.end() && active->second.particle_emitter &&
                particle_origin(entity)==std::pair(template_id,template_node),
                "Saved active particle source differs from frozen entity or template membership.");
            frozen=&*active->second.particle_emitter;
            auto pose=particle_pose(registry.get<Node>(find(entity)).world);
            if(std::inner_product(pose.rotation.begin(),pose.rotation.end(),emitter.status.pose.rotation.begin(),0.)<0)
                for(auto& value:pose.rotation)value=-value;
            require(save_near(pose.position_m,emitter.status.pose.position_m) && save_near(pose.rotation,emitter.status.pose.rotation),
                "Saved particle source pose differs from its reconstructed entity.");
            ++active_count;
        }
        require(emitter.config==frozen->config && emitter.status.emitting==(!draining && frozen->config.emitting) &&
            emitter.status.pending_burst==0 && emitter.next_pose==emitter.status.pose,
            "Saved particle source configuration, rate state or pose differs from its committed definition.");
        auto& source=sources.emplace(id,ParticleBinding{entity,template_id,template_node,*frozen,draining}).first->second;
        require(row.contains("water_flow")==frozen->water_flow.has_value(),"Saved particle water flow differs from frozen content.");
        if (const auto& profile=frozen->water_flow) {
            validate_water_flow_emitter(*frozen);
            has_water_flow=true;
            const auto& flow=row.at("water_flow");
            save_fields(flow,{"tick","volume_microlitres","phase_microlitres","scheduled_births"});
            auto& sample=source.water_flow;
            sample.tick=save_integer(flow.at("tick"),tick);
            sample.volume_microlitres=save_integer(flow.at("volume_microlitres"),max_water_flow_sample);
            sample.phase_microlitres=static_cast<std::uint32_t>(save_integer(flow.at("phase_microlitres"),
                profile->microlitres_per_particle-1));
            sample.scheduled_births=static_cast<std::uint32_t>(save_integer(flow.at("scheduled_births"),profile->max_particles_per_tick));
            const auto max_births=(sample.volume_microlitres+profile->microlitres_per_particle-1)/profile->microlitres_per_particle;
            require(sample.scheduled_births<=max_births &&
                (!sample.tick ? (!sample.volume_microlitres && !sample.phase_microlitres && !sample.scheduled_births) : frozen->config.emitting),
                "Saved particle water flow exceeds its completed emission budget.");
            if (!draining && frozen->config.emitting) {
                if (sample.tick || template_id.empty())
                    require(sample.tick==tick && sample.volume_microlitres==water_flow_volume(*profile),
                        "Saved particle water flow differs from the committed port or tick.");
            }
        }
    }
    require(save_integer(particle_extension.at("version"))==(has_water_flow ? 2u : 1u),
        "Saved particle extension version differs from its flow source membership.");
    require(active_count==static_cast<std::size_t>(std::count_if(topology->definitions.begin(),topology->definitions.end(),
        [](const auto& row){return row.second.particle_emitter.has_value();})),"Saved active particle source membership is incomplete.");
    auto& state=*particle_runtime;
    state.system=particles::System::restore(typed);
    state.sources=std::move(sources);state.next_id=frontier;state.membership=topology;
    if(state.rain)restore_precipitation(particle_extension.at("precipitation"));
}

std::string Runtime::save_snapshot(const std::string& content_sha256) const {
    require(save_digest(content_sha256),"Snapshot content identity must be a lowercase SHA-256.");
    const auto& s=*impl_;
    const bool joined_physics=s.topology->live.physics_checkpoint || s.physical_content || Impl::has_released_attachment(s.attachment_graph.get());
    const bool stance_state=save_stance_definition(s.topology->live);
    const bool motor_state=save_locomotion_definition(s.topology->live);
    (void)save_text(s.world_id);(void)save_integer(s.revision);(void)save_integer(s.tick);(void)save_integer(s.game_revision);
    SaveJson entities=SaveJson::array();auto& bodies=s.physics.GetBodyInterface();
    for(const auto e:s.topology->order) {
        const auto& node=s.registry.get<Node>(e);
        (void)save_text(node.id);(void)save_text(node.parent,256,true);(void)save_transform(save_local(node.local));
        SaveJson body=nullptr,controller=nullptr;
        const auto* b=s.registry.try_get<Body>(e);const auto* c=s.registry.try_get<Controller>(e);
        if(b || c) {
            const auto id=b ? b->id : c->character->GetBodyID();JPH::RVec3 p;JPH::Quat q;bodies.GetPositionAndRotation(id,p,q);
            SaveJson motion=nullptr;
            if(b && b->target) {
                const auto& m=*b->target;
                motion={{"start_position",save_xyz(m.start_position)},{"start_rotation",save_xyzw(m.start_rotation)},
                    {"target_position",m.target.position},{"target_rotation",m.target.rotation},
                    {"duration_ticks",m.target.duration_ticks},{"elapsed_ticks",m.elapsed}};
            }
            body={{"motion",c ? "character" : b->motion==BodyMotion::Static ? "static" : b->motion==BodyMotion::Dynamic ? "dynamic" : "kinematic"},
                {"position",save_xyz(p)},{"rotation",save_xyzw(q)},{"linear_velocity",save_xyz(bodies.GetLinearVelocity(id))},
                {"angular_velocity",save_xyz(bodies.GetAngularVelocity(id))},{"active",bodies.IsActive(id)},{"motion_state",motion}};
            (void)save_vector<3>(body.at("position"),1e6);(void)save_rotation(body.at("rotation"));
            (void)save_vector<3>(body.at("linear_velocity"),1e6);(void)save_vector<3>(body.at("angular_velocity"),1e6);
            if(c) {
                (void)save_number(c->yaw,180.00001);(void)save_number(c->pitch,85);
                controller={{"yaw",c->yaw},{"pitch",c->pitch}};
                if(c->settings.stance) {
                    save_stance_flags(c->requested_crouch,c->crouched,c->stand_blocked);
                    controller["requested_crouch"]=c->requested_crouch;controller["crouched"]=c->crouched;
                    controller["stand_blocked"]=c->stand_blocked;
                }
                require(c->character->IsVirtual()==c->settings.locomotion.has_value(),"Character motor differs from frozen profile.");
                if(c->character->IsVirtual()) {
                    const auto position=c->character->GetPosition();const auto rotation=c->character->GetRotation();
                    const auto velocity=c->character->GetLinearVelocity();const auto integration=c->character->GetIntegrationVelocity();
                    const float maximum=bodies.GetMaxLinearVelocity(id);
                    const auto& profile=*c->settings.locomotion;
                    const float reported_maximum=maximum+(profile.step_height+profile.ground_snap+.04f)*60+1;
                    require(integration.Length()<=maximum+1e-5f && velocity.Length()<=reported_maximum+1e-5f,
                        "Motor snapshot velocity exceeds the configured limits.");
                    controller["motor"]={{"position",save_xyz(position)},{"rotation",save_xyzw(rotation)},
                        {"linear_velocity",save_xyz(velocity)},{"integration_velocity",save_xyz(integration)}};
                    (void)save_vector<3>(controller["motor"]["position"],1e6);(void)save_rotation(controller["motor"]["rotation"]);
                    (void)save_vector<3>(controller["motor"]["linear_velocity"],1e6);
                    (void)save_vector<3>(controller["motor"]["integration_velocity"],1e6);
                    require(bodies.GetLinearVelocity(id)==JPH::Vec3::sZero() && bodies.GetAngularVelocity(id)==JPH::Vec3::sZero(),
                        "Virtual query proxy must not have independent velocity.");
                    const auto proxy_position=save_xyz(p).get<std::array<double,3>>();
                    const auto logical_proxy=save_xyz(position+JPH::Vec3(0,CharacterMotor::query_padding,0)).get<std::array<double,3>>();
                    const auto proxy_rotation=save_xyzw(q).get<std::array<double,4>>(),logical_rotation=save_xyzw(rotation).get<std::array<double,4>>();
                    bool proxy_agrees;
                    if(c->character->IsSuppressed()) {
                        const auto shape=bodies.GetShape(id);
                        const auto reported=save_xyz(CharacterMotor::ProxyOriginForMotor(position,rotation,shape.GetPtr())).get<std::array<double,3>>();
                        proxy_agrees=c->character->ProxyMatchesMotor() && proxy_position==reported && proxy_rotation==logical_rotation;
                    }else proxy_agrees=save_near(proxy_position,logical_proxy) && save_near(proxy_rotation,logical_rotation);
                    require(proxy_agrees,"Virtual query proxy differs from logical motor pose.");
                }
            }
        }
        entities.push_back({{"id",node.id},{"parent",node.parent},{"local",save_local(node.local)},{"body",body},{"controller",controller}});
    }
    SaveJson game=nullptr;
    if(s.game) {
        const auto inspected=SaveJson::parse(s.game->inspect());game=SaveJson::object();
        for(const auto* key:{"backend","assembly_sha256","type","schema","values"})game[key]=inspected.at(key);
        save_game_values(game,s.topology->identities);
    }
    SaveJson payload={{"content_sha256",content_sha256},{"world_id",s.world_id},{"authored_revision",s.revision},{"tick",s.tick},
        {"gameplay_revision",s.game_revision},{"entities",entities},{"animation",SaveJson::parse(s.animations->save_state(s.tick))},
        {"sound",SaveJson::parse(s.sounds.save_state(s.tick))},{"gameplay",game}};
    if(!joined_physics && !s.topology->kinematics.empty()) {
        SaveJson trackers=SaveJson::array();
        for(const auto e:s.topology->kinematics) {
            const auto& node=s.registry.get<Node>(e);
            const auto& body=s.registry.get<Body>(e);
            trackers.push_back({{"entity",node.id},{"state",capture_runtime_kinematic_sleep(s.physics,body.id)}});
        }
        payload["kinematic_sleep"]={{"version",1},{"trackers",std::move(trackers)}};
    }
    if(s.attachment_graph) {
        SaveJson bindings=SaveJson::array();
        for(const auto& [id,binding]:s.attachment_graph->bindings) {
            SaveJson row={{"entity",id},{"anchor",binding.anchor.empty() ? SaveJson(nullptr) : SaveJson(binding.anchor)},
                {"offset_matrix",binding.offset}};
            if(binding.released)row["released"]=true;
            bindings.push_back(std::move(row));
        }
        payload["attachments"]={{"version",1},{"bindings",std::move(bindings)}};
    }
    if(s.attachment_outcomes) {
        auto rows=attachment_outcome_rows(*s.attachment_outcomes);
        unsigned version=1;
        for(const auto& row:rows)if(row["status"]=="reach_blocked") {
            const auto pose=row.value("pose","");
            version=std::max(version,pose=="steered" ? 5u : pose=="blended" ? 4u : row.contains("pose") ? 3u : 2u);
        }
        payload["attachment_outcomes"]={{"version",version},{"tick",s.tick},{"outcomes",std::move(rows)}};
    }
    SaveJson cameras=SaveJson::array();
    for(const auto e:s.topology->order)if(const auto* camera=s.registry.try_get<RuntimeCamera>(e)) {
        const SceneLens world{camera->vertical_fov,camera->near_plane,camera->far_plane};Impl::validate_camera_lens(world);
        const auto lens_json=[](const SceneLens& lens) { return SaveJson{{"vertical_fov",lens.vertical_fov},{"near",lens.near_plane},{"far",lens.far_plane}}; };
        SaveJson foreground=nullptr;if(camera->viewmodel) {Impl::validate_camera_lens(*camera->viewmodel);foreground=lens_json(*camera->viewmodel);}
        cameras.push_back({{"entity",s.registry.get<Node>(e).id},{"world",lens_json(world)},{"viewmodel",std::move(foreground)}});
    }
    SaveJson poses=SaveJson::array();
    const auto pose_json=[](const RuntimeCameraPose& pose) {Impl::validate_camera_pose(pose);return SaveJson{{"position",pose.position},{"rotation",pose.rotation}};};
    for(const auto e:s.topology->order)if(const auto* camera=s.registry.try_get<RuntimeCamera>(e))
        if(camera->world_pose!=RuntimeCameraPose{} || camera->viewmodel_pose!=RuntimeCameraPose{}) {
            require(camera->viewmodel || camera->viewmodel_pose==RuntimeCameraPose{},"Absent foreground cannot own a pose.");
            RuntimeCameraPoseCommand command{s.registry.get<Node>(e).id,camera->world_pose,camera->viewmodel ? std::optional(camera->viewmodel_pose) : std::nullopt};
            auto validation=command;if(*validation.world==RuntimeCameraPose{})validation.world.reset();
            s.validate_camera_pose_command(validation);
            poses.push_back({{"entity",command.entity},{"world",pose_json(camera->world_pose)},
                {"viewmodel",camera->viewmodel ? pose_json(camera->viewmodel_pose) : SaveJson(nullptr)}});
        }
    if(!poses.empty())payload["camera_poses"]={{"version",1},{"states",std::move(poses)}};
    const bool camera_state=!cameras.empty();
    const bool custom=!s.components->schemas().empty();
    const bool structural=s.structure_revision!=0;
    const bool logical_ui=!s.ui_model->definition().empty();
    const bool hierarchical=structural && std::any_of(s.templates.begin(),s.templates.end(),[](const auto& recipe){return !recipe.entities.empty();});
    if(custom || structural || logical_ui || camera_state || stance_state || motor_state)payload["components"]=SaveJson::parse(s.components->save());
    if(logical_ui) {
        payload["ui"]=SaveJson::parse(s.ui_model->save());
        payload["control_sequence"]=s.control_sequence;
        payload["structure"]=nullptr;
    }
    if(structural) {
        SaveJson spawned=SaveJson::array();
        for(const auto& [id,origin]:s.topology->spawned) {
            SaveJson row={{"id",id},{"template_id",origin.template_id},{"initial_transform",save_local(origin.initial)}};
            if(hierarchical || camera_state || stance_state || motor_state)row["nodes"]=origin.nodes;
            spawned.push_back(std::move(row));
        }
        const auto cursor=s.entity_ids->cursor();
        payload["structure"]={{"revision",s.structure_revision},{"next_entity_id",gameplay_id(cursor.next)},
            {"exhausted",cursor.exhausted},{"spawned",std::move(spawned)}};
    }
    if(hierarchical && !logical_ui) {payload["ui"]=nullptr;payload["control_sequence"]=s.control_sequence;}
    if(camera_state || stance_state || motor_state) {
        if(!payload.contains("structure"))payload["structure"]=nullptr;
        if(!payload.contains("ui"))payload["ui"]=nullptr;
        payload["control_sequence"]=s.control_sequence;payload["cameras"]=std::move(cameras);
    }
    if(joined_physics)payload["physical"]=save_physical_extension(s);
    if(s.water_region) {
        require(s.water_region->grid().tick()==s.tick,"Water region tick differs from its runtime.");
        payload["water"]=s.water_region->save();
    }
    if(s.snow_live)payload["snow"]=s.archive_snow();
    if(s.wetness_live)payload["wetness"]=s.archive_wetness();
    if(s.solar_lighting_live)payload["solar_lighting"]=solar_lighting_json(*s.solar_lighting_live);
    if(const auto* particles=s.particle_runtime.get()) {
        require(particles->system.status().tick==s.tick,"Particle system tick differs from its runtime.");
        SaveJson sources=SaveJson::array();
        bool has_water_flow=false;
        for(const auto& [id,source]:particles->sources) {
            sources.push_back({{"id",id},{"entity",source.entity},{"template_id",source.template_id},
                {"template_node",source.template_node},{"draining",source.draining}});
            if (source.definition.water_flow) {
                has_water_flow=true;
                const auto& flow=source.water_flow;
                sources.back()["water_flow"]={{"tick",flow.tick},{"volume_microlitres",flow.volume_microlitres},
                    {"phase_microlitres",flow.phase_microlitres},{"scheduled_births",flow.scheduled_births}};
            }
        }
        payload["particles"]={{"version",has_water_flow ? 2u : 1u},{"next_emitter_id",particles->next_id},{"sources",std::move(sources)},
            {"state",poima::particles::snapshot_json(particles->system.snapshot())}};
        if(particles->rain)payload["particles"]["precipitation"]=s.archive_precipitation();
    }
    if(s.surface_marks) {
        SaveJson rows=SaveJson::array();
        bool textured=false,relief=false;
        for(const auto& row:s.surface_marks->rows) {
            textured|=!row.profile.empty();
            relief|=row.mark.relief_depth_m>0;
            rows.push_back(surface_mark_json(row));
        }
        const bool timed=s.surface_marks->has_lifecycle();
        auto extension=SaveJson{{"version",relief ? 4 : timed ? 3 : textured ? 2 : 1},
            {"next_id",s.surface_marks->next_id},{"marks",std::move(rows)}};
        if(timed)extension["lifecycle"]=surface_mark_lifecycle_json(s.surface_marks->lifecycle);
        payload["surface_marks"]=std::move(extension);
    }
    const SaveJson envelope={{"format","poima.runtime-snapshot"},{"version",motor_state ? 9 : stance_state ? 8 : camera_state ? 7 : hierarchical ? 6 : logical_ui ? 5 : structural ? 3 : custom ? 2 : 1},{"sha256",save_hash(payload.dump())},{"payload",payload}};
    std::size_t tokens=0;save_budget(envelope,0,tokens);const auto result=envelope.dump();
    require(result.size()<=snapshot_bytes_limit,"Runtime snapshot exceeds 64 MiB.");return result;
}

std::unique_ptr<Runtime> Runtime::restore_snapshot_data(const RuntimeDefinition& definition,const std::string& content_sha256,
    const std::string& bytes,std::string& saved_gameplay,std::uint64_t& saved_gameplay_revision,std::shared_ptr<jobs::Executor> executor) {
    require(save_digest(content_sha256),"Snapshot content identity must be a lowercase SHA-256.");
    const auto envelope=save_parse(bytes);save_fields(envelope,{"format","version","sha256","payload"});
    const bool custom=!definition.component_schemas.empty();
    const bool logical_ui=!definition.ui.empty();
    const auto version=save_integer(envelope.at("version"));
    require(envelope.at("format")=="poima.runtime-snapshot" &&
        (version==9 || version==8 || version==7 || version==6 || (logical_ui ? (version==4 || version==5) : (version==3 || version==(custom ? 2u : 1u)))),"Unsupported runtime snapshot format/version for this definition.");
    require((version==9)==save_locomotion_definition(definition),"Virtual locomotion content requires snapshot version 9.");
    require(version!=8 || save_stance_definition(definition),"Snapshot version 8 requires frozen character stance content.");
    auto data=envelope.at("payload");const auto digest=save_text(envelope.at("sha256"),64);
    require(save_digest(digest) && digest==save_hash(data.dump()),"Runtime snapshot checksum mismatch.");
    // Authenticate the original complete payload before validating a separately
    // named optional extension; every legacy field and unknown-key rejection stays exact.
    require(data.contains("water")==definition.water.has_value(),"Snapshot water membership differs from frozen content.");
    SaveJson water_extension;
    if(definition.water) {
        water_extension=std::move(data.at("water"));
        data.erase("water");
    }
    require(data.contains("snow")==definition.snow.has_value(),"Snapshot snow membership differs from frozen content.");
    SaveJson snow_extension;
    if(definition.snow) {
        snow_extension=std::move(data.at("snow"));
        data.erase("snow");
    }
    const bool rain_content=Impl::definition_has_rain(definition);
    const bool wetness_content=rain_content || Impl::rain_water_content(definition) ||
        std::ranges::any_of(definition.entities,[](const auto& entity) {
            return entity.surface_wetness.has_value();
        });
    require(data.contains("wetness")==wetness_content,"Snapshot wetness membership differs from frozen content.");
    SaveJson wetness_extension;
    if(wetness_content) {
        wetness_extension=std::move(data.at("wetness"));
        data.erase("wetness");
    }
    std::optional<SolarLightingSample> solar_sample;
    if(data.contains("solar_lighting")) {
        require(!data.at("gameplay").is_null() && save_integer(data.at("tick"))>0,
            "A solar lighting sample requires a committed gameplay tick.");
        solar_sample=read_solar_lighting(data.at("solar_lighting"));
        data.erase("solar_lighting");
    }
    const bool particle_content=Impl::definition_has_particles(definition);
    require(data.contains("particles")==particle_content,"Snapshot particle membership differs from frozen content.");
    SaveJson particle_extension;
    if(particle_content) {
        particle_extension=std::move(data.at("particles"));data.erase("particles");
        if(rain_content)save_fields(particle_extension,{"version","next_emitter_id","sources","state","precipitation"});
        else save_fields(particle_extension,{"version","next_emitter_id","sources","state"});
        const auto particle_version=save_integer(particle_extension.at("version"),2);
        require(particle_version>=1,"Unsupported runtime particle extension version.");
    }
    SaveJson surface_extension=nullptr;
    if(data.contains("surface_marks")) {
        surface_extension=std::move(data.at("surface_marks"));data.erase("surface_marks");
        const auto surface_version=save_integer(surface_extension.at("version"));
        if(surface_version==3 || (surface_version==4 && surface_extension.contains("lifecycle")))
            save_fields(surface_extension,{"version","next_id","marks","lifecycle"});
        else save_fields(surface_extension,{"version","next_id","marks"});
        require(surface_version>=1 && surface_version<=4 && save_integer(surface_extension.at("next_id"))>1,
            "Invalid runtime surface mark extension version or identity frontier.");
        const auto& rows=surface_extension.at("marks");
        require(rows.is_array() && rows.size()<=max_scene_surface_marks,"Invalid saved surface mark count.");
    }
    const bool physical_content=Impl::definition_has_physical(definition);
    SaveJson pose_extension=nullptr;
    if(data.contains("camera_poses")) {
        require(version>=7,"Camera pose extension requires Camera snapshot membership.");
        pose_extension=data.at("camera_poses");data.erase("camera_poses");
        save_fields(pose_extension,{"version","states"});
        require(pose_extension.at("version").is_number_integer() && pose_extension.at("version")==1,
            "Unsupported camera pose extension version.");
        const auto& states=pose_extension.at("states");
        require(states.is_array() && !states.empty() && states.size()<=10000,"Camera pose extension requires1..10000 nonidentity states.");
    }
    SaveJson kinematic_sleep=nullptr;
    if(data.contains("kinematic_sleep")) {
        kinematic_sleep=std::move(data.at("kinematic_sleep"));data.erase("kinematic_sleep");
        save_fields(kinematic_sleep,{"version","trackers"});
        require(save_integer(kinematic_sleep.at("version"))==1,"Unsupported kinematic sleep extension version.");
        const auto& trackers=kinematic_sleep.at("trackers");
        require(trackers.is_array() && !trackers.empty() && trackers.size()<=RuntimeBodyIds::capacity,
            "Saved kinematic sleep tracker count exceeds its bound.");
    }
    std::shared_ptr<Impl::AttachmentGraph> attachment_graph;
    if(data.contains("attachments")) {
        const auto& extension=data.at("attachments");
        save_fields(extension,{"version","bindings"});
        require(save_integer(extension.at("version"))==1,"Unsupported attachment extension version.");
        attachment_graph=std::make_shared<Impl::AttachmentGraph>();
        const auto& rows=extension.at("bindings");
        require(rows.is_array() && !rows.empty() && rows.size()<=max_runtime_attachments,
            "Saved attachment extension requires 1..64 bindings.");
        std::string previous;
        for(const auto& row:rows) {
            const bool released=row.contains("released");
            if(released) {
                save_fields(row,{"entity","anchor","offset_matrix","released"});
                require(row.at("released")==true,"Saved release flag must be true.");
            } else save_fields(row,{"entity","anchor","offset_matrix"});
            const auto id=save_text(row.at("entity"));
            require(previous.empty() || previous<id,"Saved attachment items must be unique and sorted.");
            previous=id;
            const auto anchor=row.at("anchor").is_null() ? std::string{} : save_text(row.at("anchor"));
            attachment_graph->bindings.emplace(id,RuntimeAttachmentBinding{anchor,save_vector<16>(row.at("offset_matrix"),1e12),released});
        }
        data.erase("attachments");
    }
    const bool joined_physics=definition.physics_checkpoint || physical_content || Impl::has_released_attachment(attachment_graph.get());
    require(data.contains("physical")==joined_physics,
        definition.physics_checkpoint ? "Physics checkpoint profile requires its complete versioned physics extension." :
            "Physical rigs and released items require their complete versioned physics extension.");
    require(!joined_physics || kinematic_sleep.is_null(),"Joined physical snapshots already own kinematic sleep tracking.");
    SaveJson physical_extension=nullptr;
    if(joined_physics) {
        physical_extension=std::move(data.at("physical"));data.erase("physical");
        save_physical_extension_fields(physical_extension,definition);
    }
    std::shared_ptr<const Impl::AttachmentOutcomes> attachment_outcomes;
    if(data.contains("attachment_outcomes")) {
        attachment_outcomes=std::make_shared<const Impl::AttachmentOutcomes>(
            read_attachment_outcomes(data.at("attachment_outcomes"),save_integer(data.at("tick"))));
        data.erase("attachment_outcomes");
    }
    if(version>=7)save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay","components","structure","ui","control_sequence","cameras"});
    else if(version==6 || version==5)save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay","components","structure","ui","control_sequence"});
    else if(logical_ui)save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay","components","structure","ui"});
    else if(version==3)save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay","components","structure"});
    else if(custom)save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay","components"});
    else save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay"});
    const bool structural=version==6 || version==3 || ((version>=7 || logical_ui) && !data.at("structure").is_null());
    require(!attachment_graph || structural,"Saved attachments require a structure revision.");
    if(version>=6)require(data.at("ui").is_null()==!logical_ui,"Snapshot UI membership differs from frozen content.");
    require(data.at("content_sha256")==content_sha256 && save_text(data.at("world_id"))==definition.world_id && save_integer(data.at("authored_revision"))==definition.authored_revision,
        "Runtime snapshot does not match the frozen world content/revision.");
    const auto tick=save_integer(data.at("tick")),game_revision=save_integer(data.at("gameplay_revision"));
    const auto& rows=data.at("entities");require(rows.is_array() && rows.size()<=10000,"Runtime snapshot entity budget exceeded.");
    std::optional<RuntimeDefinition> expanded;
    std::optional<RuntimeEntityIds> restored_ids;
    std::map<std::string,Impl::SpawnOrigin> origins;
    std::uint64_t structure_revision=0;
    if(structural) {
        validate_runtime_templates(definition);
        const auto& structure=data.at("structure");save_fields(structure,{"revision","next_entity_id","exhausted","spawned"});
        structure_revision=save_integer(structure.at("revision"));require(structure_revision>0,"Structural snapshot needs a committed structure revision.");
        const auto next=save_text(structure.at("next_entity_id"),32);
        require(next.size()==32 && next.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid saved entity allocator frontier.");
        require(structure.at("exhausted").is_boolean(),"Saved entity allocator exhaustion must be Boolean.");
        std::vector<PoimaEntityId> exclusions;std::set<std::string> authored;
        for(const auto& entity:definition.entities) {
            require(authored.insert(entity.id).second,"Duplicate original authored identity.");
            if(entity.id.size()==32 && entity.id.find_first_not_of("0123456789abcdef")==std::string::npos) {
                const auto numeric=gameplay_id(entity.id);if(numeric.high || numeric.low)exclusions.push_back(numeric);
            }
        }
        restored_ids.emplace(exclusions,RuntimeEntityIds::Cursor{gameplay_id(next),structure.at("exhausted").get<bool>()});
        const auto& births=structure.at("spawned");
        require(definition.entities.size()<=10000 && births.is_array() && births.size()<=10000-definition.entities.size(),"Snapshot spawned entity budget exceeded.");
        std::size_t payload_bytes=0,component_instances=0;
        auto account_components=[&](const auto& values) {
            for(const auto& [type_id,payload]:values) {
                (void)type_id;
                require(component_instances<components::max_instances && payload.size()<=components::max_payload_bytes-payload_bytes,
                    "Restored component storage budget exceeded.");
                ++component_instances;payload_bytes+=payload.size();
            }
        };
        for(const auto& entity:definition.entities)account_components(entity.components);
        expanded=definition;std::string previous_birth;std::set<std::string> live_ids=authored;
        for(const auto& birth:births) {
            if(version>=6)save_fields(birth,{"id","template_id","initial_transform","nodes"});
            else save_fields(birth,{"id","template_id","initial_transform"});
            const auto id=save_text(birth.at("id"),32);
            require(id.size()==32 && id.find_first_not_of("0123456789abcdef")==std::string::npos && (previous_birth.empty() || previous_birth<id),
                "Spawned snapshot identities must be canonical, unique and sorted.");previous_birth=id;
            const auto template_id=save_text(birth.at("template_id"),32);
            const auto found=std::find_if(definition.templates.begin(),definition.templates.end(),[&](const auto& recipe){return recipe.id==template_id;});
            require(found!=definition.templates.end(),"Saved spawn template does not exist in frozen content.");
            require(version>=6 || found->entities.empty(),"Legacy snapshot cannot reconstruct a hierarchical recipe.");
            const auto initial=save_transform(birth.at("initial_transform"));
            validate_runtime_spawn_transform(*found,initial);
            RuntimeSpawnInstance origin;origin.root=id;origin.template_id=template_id;origin.initial=initial;
            const auto members=runtime_template_members(*found);
            if(version>=6) {
                const auto& nodes=birth.at("nodes");require(nodes.is_object() && nodes.size()==members.size(),"Saved instance node membership differs from frozen recipe.");
                for(const auto& local:members) {
                    require(nodes.contains(local),"Saved instance is missing a frozen local node.");
                    const auto live=save_text(nodes.at(local),32);
                    require(live.size()==32 && live.find_first_not_of("0123456789abcdef")==std::string::npos,
                        "Saved instance node identity is not canonical.");
                    origin.nodes.emplace(local,live);
                }
            }else origin.nodes.emplace(found->id,id);
            require(origin.nodes.at(members.front())==id,"Saved instance root differs from its node map.");
            RuntimeEntityIds sequential(exclusions,RuntimeEntityIds::Cursor{gameplay_id(id),false});
            for(const auto& local:members)require(gameplay_id(sequential.allocate())==origin.nodes.at(local),
                "Saved instance identities differ from root-first allocation order.");
            for(const auto& [local,live]:origin.nodes) {
                (void)local;require(restored_ids->allocated(gameplay_id(live)) && live_ids.insert(live).second,
                    "Saved instance identity aliases another entity or is outside its allocator lineage.");
            }
            auto entities=runtime_template_entities(*found,origin.nodes,initial,definition.component_schemas);
            require(entities.size()<=10000-expanded->entities.size(),"Snapshot instance node budget exceeded.");
            for(auto& entity:entities) {account_components(entity.components);expanded->entities.push_back(std::move(entity));}
            require(origins.emplace(id,std::move(origin)).second,"Duplicate saved instance root.");
        }
        // Validate complete saved membership against trusted layouts, then use
        // saved payloads for construction. Old defaults can reference an entity
        // legitimately retired after its incoming references were repaired.
        const auto schemas=components::parse_manifest(components::manifest_json(definition.component_schemas));
        const auto& state=data.at("components");save_fields(state,{"revision","types"});(void)save_integer(state.at("revision"));
        const auto& types=state.at("types");require(types.is_array() && types.size()==schemas.size(),"Saved component type membership differs from frozen content.");
        std::map<std::string,RuntimeEntityDefinition*> live;
        for(auto& entity:expanded->entities)require(live.emplace(entity.id,&entity).second,"Duplicate restored entity identity.");
        for(std::size_t i=0;i<schemas.size();++i) {
            const auto& schema=schemas[i];const auto& type=types[i];save_fields(type,{"id","fingerprint","instances"});
            require(type.at("id")==schema.id && type.at("fingerprint")==components::fingerprint_hex(schema),"Saved component schema differs from frozen content.");
            const auto& instances=type.at("instances");std::size_t count=0;
            for(const auto& [id,entity]:live) { (void)id;if(entity->components.contains(schema.id))++count; }
            require(instances.is_array() && instances.size()==count,"Saved component instance membership differs from restored layouts.");
            std::string previous_instance;
            for(const auto& instance:instances) {
                save_fields(instance,{"entity","values"});const auto id=save_text(instance.at("entity"),32);
                require(previous_instance.empty() || previous_instance<id,"Saved component instances must be unique and sorted.");previous_instance=id;
                const auto found=live.find(id);require(found!=live.end() && found->second->components.contains(schema.id),"Saved component does not belong to its restored entity layout.");
                auto payload=components::parse_values(schema,instance.at("values").dump(),true);
                components::validate_payload(schema,payload,[](void* context,PoimaEntityId target) {
                    return static_cast<std::map<std::string,RuntimeEntityDefinition*>*>(context)->contains(gameplay_id(target));
                },&live);
                found->second->components[schema.id]=std::move(payload);
            }
        }
    }
    const auto& restored_definition=expanded ? *expanded : definition;
    require(rows.size()==restored_definition.entities.size(),"Runtime snapshot entity set differs from restored content.");
    require(data.at("gameplay").is_null() ? game_revision==0 : game_revision>0,"Snapshot gameplay revision does not match module presence.");
    // Construct from the original definition: animation baselines and camera
    // initial rotations must not be replaced by the saved presentation pose.
    std::optional<RuntimePhysicsBootstrap> physical_bootstrap;
    if(joined_physics)physical_bootstrap=save_physical_parse_bootstrap(physical_extension,restored_definition,tick);
    // Exact IDs/generations are installed at first allocation, never remapped.
    // The private constructor also initializes Jolt before owner-vector parsing.
    auto candidate=std::unique_ptr<Runtime>(new Runtime(restored_definition,std::move(executor),physical_bootstrap ? &*physical_bootstrap : nullptr));auto& s=*candidate->impl_;s.tick=tick;
    if(s.snow_binding)s.restore_snow(std::move(snow_extension));
    std::optional<Impl::PhysicalCheckpoint> physical_checkpoint;
    if(joined_physics)physical_checkpoint=save_physical_checkpoint(s,physical_extension,*physical_bootstrap,tick);
    if(structural) {
        // Bootstrap may see generated objects as definitions, but exclusions
        // must come exclusively from the complete original authored inventory.
        s.entity_ids=std::move(restored_ids);s.structure_revision=structure_revision;
        auto topology=std::make_shared<Impl::Topology>(*s.topology);topology->spawned=std::move(origins);s.topology=std::move(topology);
    }
    if(s.wetness_live)s.restore_wetness(wetness_extension);
    if(expanded)s.has_solar_target=Impl::authored_solar_target(definition);
    if(solar_sample) {
        s.validate_solar_sample(*solar_sample);
        s.solar_lighting_live=solar_sample;
    }
    if(s.water_region)s.water_region->restore(water_extension,tick,
        s.wetness_live ? s.wetness_live->next_quantum : std::nullopt);
    if(custom || structural || logical_ui || version>=7)s.components->load(data.at("components").dump());
    if(logical_ui)s.ui_model->load(data.at("ui").dump());
    if(version>=5)s.control_sequence=save_integer(data.at("control_sequence"));
    if(version>=7) {
        const auto& cameras=data.at("cameras");std::size_t count=0;
        for(const auto e:s.topology->order)count+=s.registry.all_of<RuntimeCamera>(e);
        require((version>=8 || count>0) && cameras.is_array() && cameras.size()==count,"Snapshot Camera membership differs from frozen content.");
        const auto lens_read=[](const SaveJson& value) {
            save_fields(value,{"vertical_fov","near","far"});
            SceneLens result{save_number(value.at("vertical_fov"),150),save_number(value.at("near"),1e7),save_number(value.at("far"),1e7)};
            Impl::validate_camera_lens(result);return result;
        };
        std::string previous_camera;
        for(const auto& row:cameras) {
            save_fields(row,{"entity","world","viewmodel"});const auto id=save_text(row.at("entity"));
            require(previous_camera.empty() || previous_camera<id,"Snapshot Camera IDs must be unique and sorted.");previous_camera=id;
            const auto e=s.find(id);auto* camera=s.registry.try_get<RuntimeCamera>(e);
            require(camera,"Saved lens entity is not a frozen Camera.");
            require(row.at("viewmodel").is_null()==!camera->viewmodel,"Saved foreground lens presence differs from frozen Camera.");
            const auto world=lens_read(row.at("world"));camera->vertical_fov=world.vertical_fov;camera->near_plane=world.near_plane;camera->far_plane=world.far_plane;
            if(camera->viewmodel)camera->viewmodel=lens_read(row.at("viewmodel"));
        }
    }
    if(!pose_extension.is_null()) {
        const auto read_pose=[](const SaveJson& value) {
            save_fields(value,{"position","rotation"});RuntimeCameraPose result;
            result.position=save_vector<3>(value.at("position"),10);result.rotation=save_rotation(value.at("rotation"));
            Impl::validate_camera_pose(result);return result;
        };
        std::string previous_pose;
        for(const auto& row:pose_extension.at("states")) {
            save_fields(row,{"entity","world","viewmodel"});const auto id=save_text(row.at("entity"));
            require(previous_pose.empty() || previous_pose<id,"Camera pose extension IDs must be unique and sorted.");previous_pose=id;
            const auto e=s.find(id);auto* camera=s.registry.try_get<RuntimeCamera>(e);
            require(camera && row.at("viewmodel").is_null()==!camera->viewmodel,"Saved camera pose membership differs from frozen Camera.");
            RuntimeCameraPoseCommand command;command.entity=id;command.world=read_pose(row.at("world"));
            if(camera->viewmodel)command.viewmodel=read_pose(row.at("viewmodel"));
            require(*command.world!=RuntimeCameraPose{} || (command.viewmodel && *command.viewmodel!=RuntimeCameraPose{}),"Redundant identity camera pose extension row.");
            auto validation=command;if(*validation.world==RuntimeCameraPose{})validation.world.reset();
            s.validate_camera_pose_command(validation);
            camera->world_pose=*command.world;if(command.viewmodel)camera->viewmodel_pose=*command.viewmodel;
        }
    }
    if(!data.at("gameplay").is_null()) {
        const auto& source_game=data.at("gameplay");save_game_values(source_game,s.topology->identities);
        const auto& schema=source_game.at("schema");
        const auto declared=schema.contains("components")
            ? components::parse_manifest(SaveJson{{"format","poima.components"},{"version",1},{"schemas",schema.at("components")}}.dump())
            : std::vector<components::Schema>{};
        s.components->validate_module(declared);
    }
    // Validate the complete controller inventory before installing any shape.
    // Body poses below are foot-origin poses and must use the restored shape.
    struct StanceRestore {entt::entity entity;bool requested=false,crouched=false,blocked=false;};
    std::vector<StanceRestore> stances;
    struct MotorRestore {entt::entity entity;JPH::RVec3 position;JPH::Quat rotation;JPH::Vec3 integration,reported;};
    std::vector<MotorRestore> motors;
    std::string previous_stance_entity;
    for(const auto& row:rows) {
        save_fields(row,{"id","parent","local","body","controller"});
        const auto id=save_text(row.at("id"));
        require(previous_stance_entity.empty() || previous_stance_entity<id,"Runtime snapshot entity IDs must be unique and sorted.");
        previous_stance_entity=id;
        const auto e=s.find(id);auto* controller=s.registry.try_get<Controller>(e);
        require(row.at("controller").is_null()==!controller,"Runtime snapshot controller components differ.");
        if(!controller)continue;
        const auto& saved=row.at("controller");
        const bool virtual_motor=controller->character->IsVirtual();
        require(virtual_motor==controller->settings.locomotion.has_value(),"Character motor differs from frozen profile.");
        if(version>=8 && controller->settings.stance) {
            if(virtual_motor)save_fields(saved,{"yaw","pitch","requested_crouch","crouched","stand_blocked","motor"});
            else save_fields(saved,{"yaw","pitch","requested_crouch","crouched","stand_blocked"});
            require(saved.at("requested_crouch").is_boolean() && saved.at("crouched").is_boolean() && saved.at("stand_blocked").is_boolean(),
                "Snapshot character stance flags must be Boolean.");
            const bool requested=saved.at("requested_crouch"),crouched=saved.at("crouched"),blocked=saved.at("stand_blocked");
            save_stance_flags(requested,crouched,blocked);stances.push_back({e,requested,crouched,blocked});
        }else {
            if(virtual_motor)save_fields(saved,{"yaw","pitch","motor"});else save_fields(saved,{"yaw","pitch"});
            if(controller->settings.stance)stances.push_back({e,false,false,false});
        }
        (void)save_number(saved.at("yaw"),180.00001);(void)save_number(saved.at("pitch"),85);
        if(virtual_motor) {
            require(version==9,"Virtual motor requires snapshot version 9.");
            const auto& motor=saved.at("motor");save_fields(motor,{"position","rotation","linear_velocity","integration_velocity"});
            const auto p=save_vector<3>(motor.at("position"),1e6);const auto q=save_rotation(motor.at("rotation"));
            const auto v=save_vector<3>(motor.at("linear_velocity"),1e6);
            const auto integrated=save_vector<3>(motor.at("integration_velocity"),1e6);
            const auto position=save_position(p);const auto rotation=save_quat(q);const auto velocity=save_velocity(v);
            const auto integration=save_velocity(integrated);
            const float maximum=s.physics.GetBodyInterface().GetMaxLinearVelocity(controller->character->GetBodyID());
            const auto& profile=*controller->settings.locomotion;
            const float reported_maximum=maximum+(profile.step_height+profile.ground_snap+.04f)*60+1;
            require(std::isfinite(velocity.GetX()) && std::isfinite(velocity.GetY()) && std::isfinite(velocity.GetZ()) &&
                velocity.Length()<=reported_maximum+1e-5f && std::isfinite(integration.GetX()) &&
                std::isfinite(integration.GetY()) && std::isfinite(integration.GetZ()) && integration.Length()<=maximum+1e-5f,
                "Snapshot motor velocity exceeds the configured limits.");
            const auto& body=row.at("body");save_fields(body,{"motion","position","rotation","linear_velocity","angular_velocity","active","motion_state"});
            const auto proxy=save_vector<3>(body.at("position"),1e6);const auto proxy_rotation=save_rotation(body.at("rotation"));
            auto expected_proxy=save_xyz(position+JPH::Vec3(0,CharacterMotor::query_padding,0)).get<std::array<double,3>>();
            if(controller->character->IsSuppressed()) {
                // The saved stance has been validated but its shape has not
                // been installed yet. Use that selected frozen shape, rather
                // than the constructor's standing proxy, for native COM math.
                const bool crouched=controller->settings.stance && saved.at("crouched").get<bool>();
                const auto* shape=crouched ? controller->crouched_shape.GetPtr() : controller->standing_shape.GetPtr();
                expected_proxy=save_xyz(CharacterMotor::ProxyOriginForMotor(position,rotation,shape)).get<std::array<double,3>>();
            }
            require(body.at("motion")=="character" && body.at("motion_state").is_null() &&
                (controller->character->IsSuppressed() ? (proxy==expected_proxy && proxy_rotation==q) :
                    (save_near(proxy,expected_proxy) && save_near(proxy_rotation,q))),"Snapshot query proxy differs from logical motor pose.");
            require(save_vector<3>(body.at("linear_velocity"),1e6)==std::array<double,3>{} &&
                save_vector<3>(body.at("angular_velocity"),1e6)==std::array<double,3>{},"Virtual query proxy cannot have independent velocity.");
            const auto expected_yaw=JPH::Quat::sRotation(JPH::Vec3::sAxisY(),static_cast<float>(saved.at("yaw").get<double>()*radians));
            const auto local=save_transform(row.at("local"));
            require(std::abs(std::abs(rotation.Dot(expected_yaw))-1)<1e-5 &&
                (controller->character->IsSuppressed() || (save_near(local.position,p) && save_near(local.rotation,q))),
                "Snapshot logical motor pose differs from yaw/local state.");
            motors.push_back({e,position,rotation,integration,velocity});
        }
    }
    for(const auto& saved:stances)
        s.restore_character_stance(s.registry.get<Controller>(saved.entity),saved.requested,saved.crouched,saved.blocked);
    if(physical_bootstrap)for(const auto& [rig,sidecar]:physical_bootstrap->physical_returns)
        s.animations->load_physical_return_state(rig,sidecar,tick);
    s.animations->load_state(data.at("animation").dump(),tick);
    if(physical_bootstrap)for(const auto& [rig,source]:physical_bootstrap->recovery_sources)
        s.animations->load_recovery_source_state(rig,source.first,tick,source.second);
    if(!s.animations->has_ik() && !s.animations->has_root_motion())s.animation_locals();
    auto& bodies=s.physics.GetBodyInterface();std::map<std::string,RuntimeTransform> locals;
    std::string previous;
    for(const auto& row:rows) {
        save_fields(row,{"id","parent","local","body","controller"});const auto id=save_text(row.at("id"));
        require(previous.empty() || previous<id,"Runtime snapshot entity IDs must be unique and sorted.");previous=id;
        const auto e=s.find(id);auto& node=s.registry.get<Node>(e);
        require(save_text(row.at("parent"),256,true)==node.parent,"Runtime snapshot parent differs from the definition.");
        locals.emplace(id,save_transform(row.at("local")));
        auto* b=s.registry.try_get<Body>(e);auto* c=s.registry.try_get<Controller>(e);
        const auto& record=row.at("body");require(record.is_null()==!(b || c),"Runtime snapshot body components differ.");
        require(row.at("controller").is_null()==!c,"Runtime snapshot controller components differ.");
        if(c) {
            const auto& ctrl=row.at("controller");
            c->yaw=save_number(ctrl.at("yaw"),180.00001);c->pitch=save_number(ctrl.at("pitch"),85);
            // A suppressed motor remains frozen at its last locomotion pose;
            // the root actor is independently owned by physical presentation.
            // Preserve the saved candidate until solved publication verifies it.
            if(c->character->IsSuppressed())node.local=locals.at(id);
        }
        if(!b && !c)continue;
        save_fields(record,{"motion","position","rotation","linear_velocity","angular_velocity","active","motion_state"});
        const std::string kind=c ? "character" : b->motion==BodyMotion::Static ? "static" : b->motion==BodyMotion::Dynamic ? "dynamic" : "kinematic";
        require(record.at("motion")==kind,"Runtime snapshot body motion differs from the definition.");
        const auto p=save_vector<3>(record.at("position"),1e6);const auto q=save_rotation(record.at("rotation"));
        const auto linear=save_vector<3>(record.at("linear_velocity"),1e6),angular=save_vector<3>(record.at("angular_velocity"),1e6);
        require(record.at("active").is_boolean(),"Snapshot active flag must be Boolean.");const bool active=record.at("active");
        require(active || (linear==std::array<double,3>{} && angular==std::array<double,3>{}),"An inactive saved body cannot have velocity.");
        const auto body_id=b ? b->id : c->character->GetBodyID();
        if(kind=="static") {
            JPH::RVec3 original_p;JPH::Quat original_q;bodies.GetPositionAndRotation(body_id,original_p,original_q);
            require(!active && save_near(p,save_xyz(original_p).get<std::array<double,3>>()) && save_near(q,save_xyzw(original_q).get<std::array<double,4>>()),"Static snapshot body differs from frozen content.");
        } else {
            require(save_velocity(linear).Length()<=bodies.GetMaxLinearVelocity(body_id)+1e-5f &&
                save_velocity(angular).Length()<=bodies.GetMaxAngularVelocity(body_id)+1e-5f,"Snapshot velocity exceeds the configured body limits.");
            if(c) {
                const auto expected=JPH::Quat::sRotation(JPH::Vec3::sAxisY(),static_cast<float>(c->yaw*radians));
                const auto actual=save_quat(q);require(std::abs(std::abs(actual.Dot(expected))-1)<1e-5,"Snapshot character rotation does not match yaw.");
                require(angular==std::array<double,3>{},"Snapshot character angular velocity is unsupported.");
            }
            bodies.SetPositionAndRotation(body_id,save_position(p),save_quat(q),JPH::EActivation::DontActivate);
            bodies.SetLinearAndAngularVelocity(body_id,save_velocity(linear),save_velocity(angular));
            if(active)bodies.ActivateBody(body_id);else bodies.DeactivateBody(body_id);
        }
        const auto& motion=record.at("motion_state");
        if(!motion.is_null()) {
            require(b && b->motion==BodyMotion::Kinematic,"Snapshot motion state requires a kinematic body.");
            save_fields(motion,{"start_position","start_rotation","target_position","target_rotation","duration_ticks","elapsed_ticks"});
            Motion m;const auto start=save_vector<3>(motion.at("start_position"),1e6);m.start_position=save_position(start);
            m.start_rotation=save_quat(save_rotation(motion.at("start_rotation")));m.target.entity=id;
            m.target.position=save_vector<3>(motion.at("target_position"),1e6);m.target.rotation=save_rotation(motion.at("target_rotation"));m.target_rotation=save_quat(m.target.rotation).Normalized();
            m.target.duration_ticks=static_cast<std::uint32_t>(save_integer(motion.at("duration_ticks"),36000));
            m.elapsed=static_cast<std::uint32_t>(save_integer(motion.at("elapsed_ticks"),36000));
            require(m.target.duration_ticks>0 && m.elapsed<m.target.duration_ticks && m.elapsed<=tick,"Invalid snapshot motion clock.");
            const auto delta=save_position(m.target.position)-m.start_position;const auto seconds=m.target.duration_ticks*fixed_dt;
            require(delta.Length()/seconds<=100.00001,"Snapshot motion exceeds 100 meters per second.");
            JPH::Vec3 axis;float angle;(m.target_rotation*m.start_rotation.Conjugated()).GetAxisAngle(axis,angle);
            require(angle/seconds<=20.00001,"Snapshot motion exceeds 20 radians per second.");
            // Jolt integrates kinematic endpoints in float arithmetic. Retain
            // the actual saved pose while checking it against the authored path.
            const double fraction=static_cast<double>(m.elapsed)/m.target.duration_ticks;
            const auto expected=m.start_position+delta*fraction;
            require((save_position(p)-expected).Length()<.01,"Snapshot kinematic pose differs from its motion clock.");
            const auto expected_q=m.start_rotation.SLERP(m.target_rotation,static_cast<float>(fraction));
            require(std::abs(std::abs(save_quat(q).Dot(expected_q))-1)<1e-4,"Snapshot kinematic rotation differs from its motion clock.");
            b->target=std::move(m);
        }
    }
    // Ordinary sync reconstructs a live controller's owned camera from its
    // initial local pose and current float pitch. Suppressed sync deliberately
    // leaves that camera alone, so reconstruct and validate this one owned
    // channel explicitly before accepting the saved original local values.
    for(const auto e:s.topology->characters) {
        const auto& c=s.registry.get<Controller>(e);
        if(!c.character->IsSuppressed() || c.settings.camera.empty())continue;
        auto& camera=s.registry.get<Node>(s.find(c.settings.camera));auto expected=camera.initial;
        const auto& q=camera.initial.rotation;
        const auto rotation=JPH::Quat(static_cast<float>(q[0]),static_cast<float>(q[1]),static_cast<float>(q[2]),static_cast<float>(q[3])).Normalized()
            * JPH::Quat::sRotation(JPH::Vec3::sAxisX(),static_cast<float>(c.pitch*radians));
        expected.rotation={rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()};
        if(c.settings.stance && c.crouched)expected.position[1]-=c.settings.stance->camera_drop;
        const auto& saved=locals.at(c.settings.camera);
        require(saved.position==expected.position && saved.rotation==expected.rotation && saved.scale==expected.scale,
            "Suppressed controller camera differs from its frozen initial pose, pitch or stance.");
        camera.local=saved;
    }
    // Logical virtual poses are separate from their zero-velocity query proxies.
    // Restore them only after all platforms/body poses and motion clocks exist.
    for(const auto& motor:motors)s.registry.get<Controller>(motor.entity).character->RestoreMotorPose(motor.position,motor.rotation,motor.integration,motor.reported);
    // Enroll exact articulated identities before support reconstruction. These
    // detached candidate mutations are superseded by the joined native state.
    if(physical_checkpoint) {
        s.restore_physical_before_jolt(*physical_checkpoint);
        s.restore_suppressed_controller_proxies(*physical_checkpoint);
    }
    // Rebuild support from current geometry only after every body is restored.
    s.physics.OptimizeBroadPhase();for(auto e:s.topology->characters) {
        auto& motor=*s.registry.get<Controller>(e).character;
        if(!motor.IsSuppressed())motor.PostSimulation(.05f);
    }
    s.sync();
    if(physical_bootstrap)for(const auto& [rig,capture]:physical_bootstrap->procedural_captures) {
        s.animations->install_procedural_terminal(rig,capture,tick,false);
        const auto captured=save_vector<16>(SaveJson::parse(capture).at("wrapper"),1e12);
        require(detail::handoff_array_same_bits(captured,s.registry.template get<Node>(s.find(rig)).world),
            "Live grounded capture wrapper differs from its restored native actor frame.");
        s.validate_procedural_supports(rig);
    }
    s.validate_planting_membership();
    s.validate_restored_snow_foot_releases();
    if(s.animations->has_ik() || s.animations->has_root_motion()) { s.animation_locals();s.sync(); }
    if(physical_checkpoint) {
        // Broadphase membership/global constraint order already match. Restore
        // all GBC/contact bytes and exact active/invalidation owner state only
        // after legacy character/support setup, which may touch native caches.
        validate_runtime_physics_state_joined(s.physics,physical_extension.at("physics"),physical_checkpoint->owner);
        save_physical_lineage(physical_extension.at("physics"),physical_bootstrap->ledger);
        restore_runtime_physics_state_joined(s.physics,physical_extension.at("physics"),physical_checkpoint->owner);
        if(definition.physics_checkpoint)for(const auto& row:rows) {
            const auto& saved=row.at("body");
            if(saved.is_null())continue;
            const auto entity=s.find(row.at("id").get<std::string>());
            const auto* body=s.registry.try_get<Body>(entity);
            const auto id=body ? body->id : s.registry.get<Controller>(entity).character->GetBodyID();
            JPH::RVec3 position;JPH::Quat rotation;
            bodies.GetPositionAndRotation(id,position,rotation);
            require(saved.at("position")==save_xyz(position) && saved.at("rotation")==save_xyzw(rotation) &&
                saved.at("linear_velocity")==save_xyz(bodies.GetLinearVelocity(id)) &&
                saved.at("angular_velocity")==save_xyz(bodies.GetAngularVelocity(id)) &&
                saved.at("active")==bodies.IsActive(id),"Physics checkpoint body differs from its ordinary saved witness.");
        }
        s.sync(); // publishes solved physical locals before final comparisons
        for(const auto& id:s.topology->physical_rigs)
            if(s.animations->procedural_enabled(id) && !s.animations->procedural_suspended(id) &&
                s.topology->definitions.at(id).physical_rig->procedural_pose!=poima::physical_rig::ProceduralPosePolicy::GroundedAnimatedOnly)
                s.animations->record_procedural_final(id,tick,s.registry.template get<Node>(s.find(id)).world);
        save_physical_verify_restored(s,*physical_checkpoint,rows);
    }
    // Native bodies and saved physical history were joined and validated above.
    // Their presentation transforms are derived through host matrix/quaternion
    // arithmetic. Preserve the original locals of these explicit owners after
    // a much narrower comparison than the general legacy save_near check.
    std::map<std::string,std::string> restored_procedural_owners;
    if(physical_checkpoint)for(const auto& [rig_id,row]:physical_checkpoint->rigs) {
        const auto& rig=s.topology->definitions.at(rig_id);
        if(row.membership.mode==RuntimePhysicalRigMode::Animated ||
            rig.physical_rig->procedural_pose!=poima::physical_rig::ProceduralPosePolicy::GroundedAnimatedOnly)continue;
        auto enroll=[&](const std::string& id) {
            require(restored_procedural_owners.emplace(id,rig_id).second,
                "Restored procedural presentation transform has overlapping owners.");
        };
        enroll(rig_id);
        if(row.handoff)enroll(row.handoff->actor);
        for(const auto& [id,node]:s.topology->definitions)
            if(node.rig_node && node.rig_node->rig==rig_id)enroll(id);
    }
    bool retained_procedural_locals=false;
    for(const auto& [id,saved]:locals) {
        auto& actual=s.registry.get<Node>(s.find(id)).local;
        bool exact_physical_scale=false;
        if(physical_content) {
            const auto& node_definition=s.topology->definitions.at(id);
            if(node_definition.rig_node) {
                const auto& rig=s.topology->definitions.at(node_definition.rig_node->rig);
                exact_physical_scale=rig.physical_rig && rig.physical_rig->pose_frame==poima::physical_rig::PoseFramePolicy::SkeletalBoundedScale;

            }
        }
        if(const auto owner=restored_procedural_owners.find(id);owner!=restored_procedural_owners.end()) {
            require(s.animations->validate_restored_procedural_local(owner->second,tick,{id,saved},{id,actual}),
                "Restored procedural presentation owner lacks its current restore boundary.");
            actual=saved;retained_procedural_locals=true;
        }
        require(save_near(saved.position,actual.position) && save_near(saved.rotation,actual.rotation) &&
            (exact_physical_scale ? saved.scale==actual.scale : save_near(saved.scale,actual.scale)),
            "Saved local transform does not match reconstructed state/content.");
    }
    if(retained_procedural_locals) {
        s.world_matrices();
        // Recheck the final graph against the untouched native bodies and
        // calibrated physical history, without re-solving presentation locals.
        if(physical_checkpoint)save_physical_final_history(s,*physical_checkpoint);
    }
    if(attachment_graph)s.publish_attachment_transforms(std::move(attachment_graph));
    s.attachment_outcomes=std::move(attachment_outcomes);
    if(particle_content)s.restore_particles(definition,particle_extension);
    if(!surface_extension.is_null()) {
        auto state=std::make_shared<Impl::SurfaceMarksState>();
        state->next_id=save_integer(surface_extension.at("next_id"));
        const auto issued=state->next_id-1;
        const auto attempt_budget=s.tick*max_surface_mark_commands;
        require(issued<=attempt_budget,
            "Saved surface mark identity frontier exceeds the placement history.");
        if(surface_extension.contains("lifecycle")) {
            const auto& counts=surface_extension.at("lifecycle");
            save_fields(counts,{"expired","recycled","dropped"});
            state->lifecycle={save_integer(counts.at("expired")),save_integer(counts.at("recycled")),save_integer(counts.at("dropped"))};
            require(state->lifecycle.dropped<=attempt_budget-issued,
                "Saved dropped marks exceed the placement attempt history.");
        }
        std::map<std::string,std::size_t> receiver_counts;
        const auto floats=[]<std::size_t N>(const SaveJson& value,double limit) {
            const auto values=save_vector<N>(value,limit);
            std::array<float,N> result{};
            std::transform(values.begin(),values.end(),result.begin(),[](double member){return static_cast<float>(member);});
            return result;
        };
        bool textured=false,relief=false;
        for(const auto& saved:surface_extension.at("marks")) {
            RuntimeSurfaceMark row;
            auto values=saved;
            if(saved.contains("profile") || saved.contains("mask_sha256")) {
                require(saved.contains("profile") && saved.contains("mask_sha256"),"Saved surface mark profile and mask must be paired.");
                row.profile=save_text(saved.at("profile"),32);
                row.mark.mask_sha256=save_text(saved.at("mask_sha256"),64);
                require(surface_mark_profiles::valid_id(row.profile) && save_digest(row.mark.mask_sha256),
                    "Invalid saved surface mark profile or mask identity.");
                values.erase("profile");
                values.erase("mask_sha256");
                textured=true;
            }
            if(saved.contains("expires_tick")) {
                row.expires_tick=save_integer(saved.at("expires_tick"));
                values.erase("expires_tick");
            }
            if(saved.contains("relief_depth_m")) {
                row.mark.relief_depth_m=static_cast<float>(save_number(saved.at("relief_depth_m"),max_surface_mark_relief_depth_m));
                values.erase("relief_depth_m");
                relief=true;
            }
            save_fields(values,{"id","receiver","tick","center_m","normal","tangent","half_extent_m",
                "half_depth_m","minimum_normal_dot","edge_fraction","linear_rgb","opacity","roughness"});
            row.id=save_integer(saved.at("id"),state->next_id-1);
            row.tick=save_integer(saved.at("tick"),s.tick);
            row.receiver=save_text(saved.at("receiver"),32);
            require(state->rows.empty() || (state->rows.back().id<row.id && state->rows.back().tick<=row.tick),
                "Saved surface marks must retain their placement order.");
            require(++receiver_counts[row.receiver]<=max_surface_marks_per_receiver,"Saved surface mark receiver capacity exceeded.");
            auto& mark=row.mark;
            mark.center_m=floats.template operator()<3>(saved.at("center_m"),10000);
            mark.normal=floats.template operator()<3>(saved.at("normal"),1.001);
            mark.tangent=floats.template operator()<3>(saved.at("tangent"),1.001);
            mark.half_extent_m=floats.template operator()<2>(saved.at("half_extent_m"),4);
            mark.half_depth_m=static_cast<float>(save_number(saved.at("half_depth_m"),.25));
            mark.minimum_normal_dot=static_cast<float>(save_number(saved.at("minimum_normal_dot"),1));
            mark.edge_fraction=static_cast<float>(save_number(saved.at("edge_fraction"),1));
            mark.linear_rgb=floats.template operator()<3>(saved.at("linear_rgb"),1);
            mark.opacity=static_cast<float>(save_number(saved.at("opacity"),1));
            mark.roughness=static_cast<float>(save_number(saved.at("roughness"),1));
            s.validate_surface_mark(row);
            require(surface_mark_json(row)==saved,"Saved surface marks must use canonical native values.");
            state->rows.push_back(std::move(row));
        }
        require(state->lifecycle.expired+state->lifecycle.recycled+state->rows.size()<=issued,
            "Saved retired marks exceed the issued identities.");
        const bool timed=state->has_lifecycle();
        require(save_integer(surface_extension.at("version"))==(relief ? 4u : timed ? 3u : textured ? 2u : 1u) &&
            surface_extension.contains("lifecycle")==timed,
            "Saved surface mark extension version does not match its contents.");
        s.surface_marks=std::move(state);
    }
    if(!kinematic_sleep.is_null()) {
        // Install last: restoring public body pose/activation and rebuilding
        // controller support can otherwise reset the sleep test spheres/timer.
        const auto& trackers=kinematic_sleep.at("trackers");
        require(trackers.size()==s.topology->kinematics.size(),"Saved kinematic sleep tracker membership differs.");
        for(std::size_t i=0;i<trackers.size();++i) {
            const auto& tracker=trackers[i];
            save_fields(tracker,{"entity","state"});
            const auto e=s.topology->kinematics[i];
            require(tracker.at("entity")==s.registry.get<Node>(e).id,"Saved kinematic sleep trackers must match live items in ID order.");
            try {restore_runtime_kinematic_sleep(s.physics,s.registry.get<Body>(e).id,tracker.at("state"));}
            catch(const std::invalid_argument& error) {throw std::runtime_error(error.what());}
        }
    }
    std::map<std::string,AudioEmitter> emitters;
    for(auto e:s.topology->order)if(const auto* emitter=s.registry.try_get<AudioEmitter>(e))emitters.emplace(s.registry.get<Node>(e).id,*emitter);
    s.sounds.load_state(data.at("sound").dump(),tick,emitters);
    for(const auto& [rig,owner]:s.physical_owners)if(owner->recovery)s.validate_recovery_history(rig,*owner->recovery);
    saved_gameplay=data.at("gameplay").dump();saved_gameplay_revision=game_revision;
    return candidate;
}
void Runtime::validate_snapshot(const RuntimeDefinition& definition,const std::string& content_sha256,const std::string& bytes,std::shared_ptr<jobs::Executor> executor) {
    std::string saved_gameplay;std::uint64_t saved_gameplay_revision=0;
    (void)restore_snapshot_data(definition,content_sha256,bytes,saved_gameplay,saved_gameplay_revision,std::move(executor));
}
std::unique_ptr<Runtime> Runtime::from_snapshot(const RuntimeDefinition& definition,const std::string& content_sha256,
    const std::string& bytes,const std::optional<GameplayConfig>& gameplay,std::shared_ptr<jobs::Executor> executor) {
    std::string saved_gameplay;std::uint64_t game_revision=0;
    auto candidate=restore_snapshot_data(definition,content_sha256,bytes,saved_gameplay,game_revision,std::move(executor));
    const auto saved=SaveJson::parse(saved_gameplay);
    require(saved.is_null()==!gameplay.has_value(),"Snapshot gameplay needs a matching trusted host configuration.");
    std::unique_ptr<Gameplay> game;
    if(gameplay)game=std::make_unique<Gameplay>(*gameplay,nullptr,GameplayInitialization::restore,gameplay_abi::available_contract(bool(candidate->impl_->navigation_binding)));
    return bind_snapshot_gameplay(std::move(candidate),saved_gameplay,game_revision,std::move(game));
}
std::unique_ptr<Runtime> Runtime::from_snapshot_with_gameplay(const RuntimeDefinition& definition,const std::string& content_sha256,
    const std::string& bytes,std::unique_ptr<Gameplay> game,std::shared_ptr<jobs::Executor> executor) {
    std::string saved_gameplay;std::uint64_t game_revision=0;
    auto candidate=restore_snapshot_data(definition,content_sha256,bytes,saved_gameplay,game_revision,std::move(executor));
    return bind_snapshot_gameplay(std::move(candidate),saved_gameplay,game_revision,std::move(game));
}
std::unique_ptr<Runtime> Runtime::bind_snapshot_gameplay(std::unique_ptr<Runtime> candidate,const std::string& saved_gameplay,
    std::uint64_t game_revision,std::unique_ptr<Gameplay> game) {
    const auto saved=SaveJson::parse(saved_gameplay);
    require(saved.is_null()==!bool(game),"Snapshot gameplay needs a matching trusted host instance.");
    auto& s=*candidate->impl_;
    // Executable selection comes exclusively from the host. No path, type name
    // or constructor selected by the snapshot is ever invoked. Fresh game
    // constructors/statics may execute while registering the trusted module;
    // their external side effects cannot roll back. Initialize is skipped.
    if(game) {
        game->validate_services(gameplay_abi::available_contract(bool(s.navigation_binding)));
        const auto current=SaveJson::parse(game->inspect());
        for(const auto* key:{"backend","assembly_sha256","type","schema"})require(current.at(key)==saved.at(key),"Snapshot gameplay module/schema differs from the trusted module.");
        game->edit(saved.at("values").dump());s.components->validate_module(game->component_schemas());
        game->validate_entity_references([](void* context,PoimaEntityId id) {
            return static_cast<RuntimeComponents*>(context)->alive(id);
        },s.components.get());
        s.game=std::move(game);s.game_revision=game_revision;
    }
    return candidate;
}
