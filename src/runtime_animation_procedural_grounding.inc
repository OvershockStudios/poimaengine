// SPDX-License-Identifier: Apache-2.0
// Included in the procedural codec's anonymous namespace.
void procedural_identity(const std::string& entity) {
    check(entity.size()==32 && entity.find_first_not_of("0123456789abcdef")==std::string::npos &&
        entity.find_first_not_of('0')!=std::string::npos,"Procedural support/query identity must be canonical and nonzero.");
}
StateJson procedural_support_json(const RuntimeAnimations::ProceduralSupportBinding& value) {
    procedural_identity(value.entity);
    // Native bit layout, allocator generation and lineage belong to the owner;
    // high sequence bits are not a portable invalid-ID mask.
    check(value.body_id!=UINT32_MAX,"Captured support body identity is invalid.");
    check(value.motion==BodyMotion::Static || value.motion==BodyMotion::Kinematic,"Captured support must be static or kinematic.");
    const auto& m=value.frame;
    for(double x:m)check(std::isfinite(x)&&std::abs(x)<=1e6,"Captured support rigid frame exceeds bounds.");
    check(m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1,"Captured support frame must be affine.");
    for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b) {
        double dot=0;for(unsigned row=0;row<3;++row)dot+=m[4*a+row]*m[4*b+row];
        check(a==b?std::abs(std::hypot(m[4*a],m[4*a+1],m[4*a+2])-1)<=1e-8:std::abs(dot)<=1e-8,
            "Captured support frame must exclude render/shape scale.");
    }
    const double determinant=m[0]*(m[5]*m[10]-m[6]*m[9])-m[4]*(m[1]*m[10]-m[2]*m[9])+m[8]*(m[1]*m[6]-m[2]*m[5]);
    check(determinant>0,"Captured support frame must be right handed.");
    return {{"entity",value.entity},{"body_id",value.body_id},{"motion",value.motion==BodyMotion::Static?"static":"kinematic"},{"frame",m}};
}
RuntimeAnimations::ProceduralSupportBinding read_procedural_support(const StateJson& value) {
    state_fields(value,{"entity","body_id","motion","frame"});
    check(value.at("entity").is_string() && (value.at("motion")=="static"||value.at("motion")=="kinematic"),"Captured support identity/kind differs.");
    RuntimeAnimations::ProceduralSupportBinding result;
    result.entity=value.at("entity").get<std::string>();result.body_id=static_cast<std::uint32_t>(state_uint(value.at("body_id"),UINT32_MAX));
    result.motion=value.at("motion")=="static"?BodyMotion::Static:BodyMotion::Kinematic;
    result.frame=state_vector<16>(value.at("frame"));
    check(procedural_support_json(result).dump()==value.dump(),"Captured support frame/identity is not canonical.");return result;
}
StateJson procedural_ray_json(const RuntimeRay& ray) {
    return {{"origin",ray.origin},{"direction",ray.direction},{"distance",ray.distance},{"ignore",ray.ignore}};
}
StateJson procedural_hit_json(const std::optional<RuntimeRayHit>& hit) {
    if(!hit)return nullptr;
    return {{"entity",hit->entity},{"fraction",hit->fraction},{"distance",hit->distance},{"position",hit->position},
        {"normal",hit->normal?StateJson(*hit->normal):StateJson(nullptr)},{"triangle",hit->triangle?StateJson(*hit->triangle):StateJson(nullptr)}};
}
StateJson procedural_ground_cache_json(const std::optional<RuntimeGroundingState>& value) {
    if(!value)return nullptr;
    return {{"state",state_grounding(*value)},{"planting",value->planting?state_planting(*value->planting):StateJson(nullptr)}};
}
std::optional<RuntimeGroundingState> read_procedural_ground_cache(const StateJson& value,
    const RuntimeTwoBoneIk& chain,std::uint64_t tick) {
    check(value.is_null()==!chain.grounding,"Captured prepare-input grounding membership differs.");
    if(value.is_null())return {};
    state_fields(value,{"state","planting"});auto result=read_grounding(value.at("state"),tick);
    check(result.casts<=result.decision_tick,"Captured prepare-input casts exceed their decision boundary.");
    if(!value.at("planting").is_null())result.planting=read_planting(value.at("planting"),result.decision_tick);
    validate_planting_cache(result,chain);
    const bool initial=result.status=="uninitialized",disabled=result.status=="disabled",lifted=result.status=="lifted";
    const bool empty=result.status=="miss"||result.status=="no-normal";
    const bool rejected=result.status=="slope"||result.status=="heading-degenerate"||result.status=="correction-limit"||result.status=="unreachable";
    const bool planted=chain.grounding->plant_lock;
    const bool lost=planted&&result.status=="plant-support-lost";
    const bool plant_rejected=planted&&(result.status=="plant-support-changed"||result.status=="plant-anchor-error");
    const bool contact=result.status=="contact";
    check(initial||disabled||lifted||empty||rejected||lost||plant_rejected||contact,"Captured previous grounding status is unknown.");
    check(result.cast==(empty||rejected||plant_rejected||contact) && (!result.cast||result.casts>0) &&
        result.hit.has_value()==(rejected||plant_rejected||contact) && result.position_error.has_value()==result.hit.has_value() &&
        result.normal_error.has_value()==result.hit.has_value(),"Captured previous grounding decision membership differs.");
    check(contact?(result.weight>0&&result.weight<=chain.weight):result.weight==0,"Captured previous effective grounding weight differs.");
    if(initial) {
        RuntimeGroundingState expected;
        if(planted)expected.planting=RuntimeFootPlantState{};
        check(procedural_ground_cache_json(result).dump()==procedural_ground_cache_json(expected).dump(),
            "Captured uninitialized prepare input must retain exact defaults.");
    }
    return result;
}
StateJson procedural_grounding_input_json(const RuntimeAnimations::ProceduralGroundingInput& input,
    const std::vector<RuntimeTwoBoneIk>& chains) {
    check(input.previous.size()==chains.size()&&input.requested.size()==chains.size()&&input.previous_controls.size()==chains.size()&&
        input.previous_requested.size()==chains.size()&&input.journal.size()<=32,
        "Captured grounding input membership/query budget differs.");
    auto previous=StateJson::array(),requested=StateJson::array(),prior_requests=StateJson::array(),journal=StateJson::array();
    for(std::size_t i=0;i<chains.size();++i) {
        validate_grounding_control(input.requested[i]);
        check(input.previous[i].has_value()==chains[i].grounding.has_value(),"Captured previous grounding slot differs.");
        previous.push_back({{"slot",chains[i].slot},{"grounding",procedural_ground_cache_json(input.previous[i])}});
        requested.push_back({{"slot",chains[i].slot},{"enabled",input.requested[i].enabled},{"weight",input.requested[i].weight}});
        validate_grounding_control(input.previous_requested[i]);
        prior_requests.push_back({{"slot",chains[i].slot},{"enabled",input.previous_requested[i].enabled},{"weight",input.previous_requested[i].weight}});
    }
    for(const auto& entry:input.journal) {
        check(!entry.owner.empty()&&entry.owner.size()<=4096,"Captured query owner identity exceeds bounds.");
        const auto binding=entry.binding?procedural_support_json(*entry.binding):StateJson(nullptr);
        if(entry.support)journal.push_back({{"kind","support"},{"owner",entry.owner},{"entity",entry.entity},
            {"frame",entry.frame?StateJson(*entry.frame):StateJson(nullptr)},{"binding",binding}});
        else journal.push_back({{"kind","ray"},{"owner",entry.owner},{"ray",procedural_ray_json(entry.ray)},
            {"hit",procedural_hit_json(entry.hit)},{"binding",binding}});
    }
    StateJson result={{"prepared",input.prepared},{"requested",requested},{"previous_requested",prior_requests},
        {"previous_controls",procedural_controls_json(input.previous_controls)},{"previous",previous},{"journal",journal}};
    procedural_finite_json(result);return result;
}
RuntimeAnimations::ProceduralGroundingInput read_procedural_grounding_input(const StateJson& value,
    const std::vector<RuntimeTwoBoneIk>& chains,const std::string& entity,std::uint64_t tick,const Matrix4& wrapper) {
    state_fields(value,{"prepared","requested","previous_requested","previous_controls","previous","journal"});
    RuntimeAnimations::ProceduralGroundingInput input;input.tick=tick;input.wrapper=wrapper;input.prepared=state_bool(value.at("prepared"));
    const auto& previous=value.at("previous");const auto& requested=value.at("requested");const auto& journal=value.at("journal");
    const auto& prior_requests=value.at("previous_requested");const auto& prior_controls=value.at("previous_controls");
    check(previous.is_array()&&requested.is_array()&&previous.size()==chains.size()&&requested.size()==chains.size()&&
        prior_requests.is_array()&&prior_requests.size()==chains.size()&&prior_controls.is_array()&&prior_controls.size()==chains.size()&&journal.is_array()&&journal.size()<=32,
        "Captured grounding input slot/query inventory differs.");
    for(std::size_t i=0;i<chains.size();++i) {
        state_fields(previous[i],{"slot","grounding"});state_fields(requested[i],{"slot","enabled","weight"});
        check(state_uint(previous[i].at("slot"),8)==chains[i].slot&&state_uint(requested[i].at("slot"),8)==chains[i].slot,
            "Captured grounding input slot order differs.");
        input.previous.push_back(read_procedural_ground_cache(previous[i].at("grounding"),chains[i],tick));
        if(input.prepared && input.previous.back() && input.previous.back()->status!="uninitialized")
            check(tick>0 && input.previous.back()->decision_tick==tick-1,
                "Prepared grounding capture must consume the previous completed tick's decision.");
        input.requested.push_back({state_bool(requested[i].at("enabled")),state_number(requested[i].at("weight"))});
        validate_grounding_control(input.requested.back());
        check(chains[i].grounding || input.requested.back()==RuntimeGroundingControl{},"Manual slots cannot retain requested grounding controls.");
        state_fields(prior_requests[i],{"slot","enabled","weight"});
        check(state_uint(prior_requests[i].at("slot"),8)==chains[i].slot,"Captured previous requested slot order differs.");
        input.previous_requested.push_back({state_bool(prior_requests[i].at("enabled")),state_number(prior_requests[i].at("weight"))});
        validate_grounding_control(input.previous_requested.back());
        check(chains[i].grounding || input.previous_requested.back()==RuntimeGroundingControl{},"Manual slots cannot retain prior requested grounding controls.");
        const auto& control=prior_controls[i];
        state_fields(control,{"entity","slot","enabled","space","target","pole","weight","end_orientation"});
        check(control.at("entity")==entity&&state_uint(control.at("slot"),8)==chains[i].slot&&
            (control.at("space")=="world"||control.at("space")=="rig"),"Captured previous generated IK identity/space differs.");
        RuntimeIkCommand command{entity,chains[i].slot,state_bool(control.at("enabled")),control.at("space")=="world"?IkSpace::World:IkSpace::Rig,
            state_vector<3>(control.at("target")),state_vector<3>(control.at("pole")),state_number(control.at("weight"))};
        if(!control.at("end_orientation").is_null()) {
            const auto& goal=control.at("end_orientation");state_fields(goal,{"rotation","weight"});
            command.end_orientation=canonical_ik_end_orientation(RuntimeIkEndOrientation{state_vector<4>(goal.at("rotation")),state_number(goal.at("weight"))});
        }
        validate_ik_command(command);input.previous_controls.push_back(std::move(command));
        if(chains[i].grounding) {
            const auto& state=*input.previous.back();
            const auto effective=state.status=="uninitialized"?chains[i]:effective_grounding_chain(chains[i],input.previous_requested.back());
            validate_grounding_cache(state,effective,input.previous_controls.back(),state.decision_tick);
            check(state.status!="uninitialized" || input.previous_requested.back()==RuntimeGroundingControl{},
                "Uninitialized previous grounding requires identity requested controls.");
        }
    }
    std::map<std::string,RuntimeAnimations::ProceduralSupportBinding> support_bindings;
    for(const auto& value:journal) {
        RuntimeAnimations::ProceduralGroundingQuery entry;
        check(value.at("kind")=="ray"||value.at("kind")=="support","Unknown captured grounding query kind.");
        check(value.at("owner").is_string()&&value.at("owner")==entity,"Captured query owner differs from its rig.");
        entry.owner=entity;entry.support=value.at("kind")=="support";
        if(entry.support) {
            state_fields(value,{"kind","owner","entity","frame","binding"});
            check(value.at("entity").is_string(),"Captured support lookup identity is absent.");entry.entity=value.at("entity").get<std::string>();procedural_identity(entry.entity);
            if(!value.at("frame").is_null())entry.frame=state_vector<16>(value.at("frame"));
        } else {
            state_fields(value,{"kind","owner","ray","hit","binding"});
            const auto& ray=value.at("ray");state_fields(ray,{"origin","direction","distance","ignore"});
            entry.ray.origin=state_vector<3>(ray.at("origin"));entry.ray.direction=state_vector<3>(ray.at("direction"));
            entry.ray.distance=state_number(ray.at("distance"));
            check(entry.ray.distance>=.001&&entry.ray.distance<=10000&&ray.at("ignore").is_array()&&ray.at("ignore").size()<=128,"Captured native ray extent/ignore list exceeds bounds.");
            for(const auto& ignored:ray.at("ignore")) {check(ignored.is_string(),"Captured ignored identity is not a string.");auto id=ignored.get<std::string>();procedural_identity(id);entry.ray.ignore.push_back(std::move(id));}
            if(!value.at("hit").is_null()) {
                const auto& saved=value.at("hit");state_fields(saved,{"entity","fraction","distance","position","normal","triangle"});
                RuntimeRayHit hit;check(saved.at("entity").is_string(),"Captured ray hit identity is absent.");hit.entity=saved.at("entity").get<std::string>();procedural_identity(hit.entity);
                hit.fraction=state_number(saved.at("fraction"));hit.distance=state_number(saved.at("distance"));hit.position=state_vector<3>(saved.at("position"));
                check(hit.fraction>=0&&hit.fraction<=1&&hit.distance>=0&&hit.distance<=entry.ray.distance,"Captured ray hit extent differs.");
                check(hit.fraction==static_cast<double>(static_cast<float>(hit.fraction)) &&
                    hit.distance==entry.ray.distance*hit.fraction,"Captured native hit fraction/distance relation differs.");
                double length=0;
                for(double component:entry.ray.direction)length+=component*component;
                check(std::isfinite(length)&&length>=1e-24,"Captured native ray direction is degenerate.");length=std::sqrt(length);
                // physics_raycast uses a float Vec3 delta, then Jolt's
                // RRayCast::GetPointOnRay computes float fraction*delta before
                // adding that float offset to its double-precision origin.
                // Keep those rounding boundaries explicit; no epsilon repair.
                for(std::size_t component=0;component<3;++component) {
                    check(std::abs(entry.ray.origin[component])<=1e6,"Captured native ray origin exceeds bounds.");
                    const float delta=static_cast<float>(entry.ray.direction[component]/length*entry.ray.distance);
                    const float offset=static_cast<float>(hit.fraction)*delta;
                    check(hit.position[component]==entry.ray.origin[component]+static_cast<double>(offset),
                        "Captured hit position differs from the native float-delta/double-origin ray.");
                }
                if(!saved.at("normal").is_null()) {
                    hit.normal=state_vector<3>(saved.at("normal"));
                    check(std::abs(std::hypot((*hit.normal)[0],(*hit.normal)[1],(*hit.normal)[2])-1)<=1e-6,
                        "Captured native surface normal must retain unit length within1e-6.");
                    for(double component:*hit.normal)
                        check(component==static_cast<double>(static_cast<float>(component)),
                            "Captured native surface normal components must retain exact float32 values.");
                }
                for(double coordinate:hit.position)check(std::abs(coordinate)<=1e6,"Captured native hit exceeds world bounds.");
                if(!saved.at("triangle").is_null())hit.triangle=static_cast<std::uint32_t>(state_uint(saved.at("triangle"),UINT32_MAX));
                entry.hit=std::move(hit);
            }
        }
        if(!value.at("binding").is_null())entry.binding=read_procedural_support(value.at("binding"));
        const auto support=entry.support?entry.entity:entry.hit?entry.hit->entity:std::string{};
        check(entry.binding.has_value()==(entry.support?entry.frame.has_value():entry.hit.has_value()) &&
            (!entry.binding || (entry.binding->entity==support && support!=entity && (!entry.frame || *entry.frame==entry.binding->frame))),
            "Captured support binding differs from its actual query result.");
        if(entry.binding) {
            const auto [old,inserted]=support_bindings.emplace(entry.binding->entity,*entry.binding);
            check(inserted || procedural_support_json(old->second).dump()==procedural_support_json(*entry.binding).dump(),
                "Captured native support changed identity/frame within one owner-thread decision.");
        }
        input.journal.push_back(std::move(entry));
    }
    check(procedural_grounding_input_json(input,chains).dump()==value.dump(),"Captured grounding input is not canonical/complete.");return input;
}
