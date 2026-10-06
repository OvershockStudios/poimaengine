// SPDX-License-Identifier: Apache-2.0
// Included after Runtime::Impl: the save adapter uses typed engine state and
// public Jolt setters. No serialized bytes enter Jolt's StateRecorder.
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
std::string save_text(const SaveJson& v,std::size_t bound=256,bool empty=false) {
    require(v.is_string(),"Runtime snapshot requires a string.");const auto s=v.get<std::string>();
    require((empty || !s.empty()) && s.size()<=bound && s.find('\0')==std::string::npos,"Runtime snapshot string is out of range.");return s;
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
}

std::string Runtime::save_snapshot(const std::string& content_sha256) const {
    require(save_digest(content_sha256),"Snapshot content identity must be a lowercase SHA-256.");
    const auto& s=*impl_;
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
    const bool custom=!s.components->schemas().empty();
    const bool structural=s.structure_revision!=0;
    const bool logical_ui=!s.ui_model->definition().empty();
    if(custom || structural || logical_ui)payload["components"]=SaveJson::parse(s.components->save());
    if(logical_ui) {
        payload["ui"]=SaveJson::parse(s.ui_model->save());
        payload["control_sequence"]=s.control_sequence;
        payload["structure"]=nullptr;
    }
    if(structural) {
        SaveJson spawned=SaveJson::array();
        for(const auto& [id,origin]:s.topology->spawned)
            spawned.push_back({{"id",id},{"template_id",origin.template_id},{"initial_transform",save_local(origin.initial)}});
        const auto cursor=s.entity_ids->cursor();
        payload["structure"]={{"revision",s.structure_revision},{"next_entity_id",gameplay_id(cursor.next)},
            {"exhausted",cursor.exhausted},{"spawned",std::move(spawned)}};
    }
    const SaveJson envelope={{"format","poima.runtime-snapshot"},{"version",logical_ui ? 5 : structural ? 3 : custom ? 2 : 1},{"sha256",save_hash(payload.dump())},{"payload",payload}};
    std::size_t tokens=0;save_budget(envelope,0,tokens);const auto result=envelope.dump();
    require(result.size()<=snapshot_bytes_limit,"Runtime snapshot exceeds 64 MiB.");return result;
}

std::unique_ptr<Runtime> Runtime::from_snapshot(const RuntimeDefinition& definition,const std::string& content_sha256,
    const std::string& bytes,const std::optional<GameplayConfig>& gameplay) {
    require(save_digest(content_sha256),"Snapshot content identity must be a lowercase SHA-256.");
    const auto envelope=save_parse(bytes);save_fields(envelope,{"format","version","sha256","payload"});
    const bool custom=!definition.component_schemas.empty();
    const bool logical_ui=!definition.ui.empty();
    const auto version=save_integer(envelope.at("version"));
    require(envelope.at("format")=="poima.runtime-snapshot" &&
        (logical_ui ? (version==4 || version==5) : (version==3 || version==(custom ? 2u : 1u))),"Unsupported runtime snapshot format/version for this definition.");
    const auto& data=envelope.at("payload");const auto digest=save_text(envelope.at("sha256"),64);
    require(save_digest(digest) && digest==save_hash(data.dump()),"Runtime snapshot checksum mismatch.");
    if(version==5)save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay","components","structure","ui","control_sequence"});
    else if(logical_ui)save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay","components","structure","ui"});
    else if(version==3)save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay","components","structure"});
    else if(custom)save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay","components"});
    else save_fields(data,{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay"});
    const bool structural=version==3 || (logical_ui && !data.at("structure").is_null());
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
        expanded=definition;std::string previous_birth;
        for(const auto& birth:births) {
            save_fields(birth,{"id","template_id","initial_transform"});const auto id=save_text(birth.at("id"),32);
            require(id.size()==32 && id.find_first_not_of("0123456789abcdef")==std::string::npos && (previous_birth.empty() || previous_birth<id),
                "Spawned snapshot identities must be canonical, unique and sorted.");previous_birth=id;
            require(!authored.contains(id) && restored_ids->allocated(gameplay_id(id)),"Saved spawned identity is outside its allocator lineage.");
            const auto template_id=save_text(birth.at("template_id"),32);
            const auto found=std::find_if(definition.templates.begin(),definition.templates.end(),[&](const auto& recipe){return recipe.id==template_id;});
            require(found!=definition.templates.end(),"Saved spawn template does not exist in frozen content.");
            account_components(found->components);
            const auto initial=save_transform(birth.at("initial_transform"));
            // Frozen assets and payload schemas were validated once above.
            // Only the saved transform and its collider scaling are variable.
            RuntimeDefinition validation;
            validation.templates.push_back({found->id,found->name,initial,found->collider,std::nullopt,{}});
            validate_runtime_templates(validation);
            RuntimeEntityDefinition entity;entity.id=id;entity.transform=initial;entity.collider=found->collider;
            entity.mesh=found->mesh;entity.components=found->components;expanded->entities.push_back(std::move(entity));
            origins.emplace(id,Impl::SpawnOrigin{template_id,initial});
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
    require(data.at("gameplay").is_null()==!gameplay.has_value(),"Snapshot gameplay needs a matching trusted host configuration.");
    require(data.at("gameplay").is_null() ? game_revision==0 : game_revision>0,"Snapshot gameplay revision does not match module presence.");
    // Construct from the original definition: animation baselines and camera
    // initial rotations must not be replaced by the saved presentation pose.
    auto candidate=std::make_unique<Runtime>(restored_definition);auto& s=*candidate->impl_;s.tick=tick;
    if(structural) {
        // Bootstrap may see generated objects as definitions, but exclusions
        // must come exclusively from the complete original authored inventory.
        s.entity_ids=std::move(restored_ids);s.structure_revision=structure_revision;
        auto topology=std::make_shared<Impl::Topology>(*s.topology);topology->spawned=std::move(origins);s.topology=std::move(topology);
    }
    if(custom || structural || logical_ui)s.components->load(data.at("components").dump());
    if(logical_ui)s.ui_model->load(data.at("ui").dump());
    if(version==5)s.control_sequence=save_integer(data.at("control_sequence"));
    if(!data.at("gameplay").is_null())save_game_values(data.at("gameplay"),s.topology->identities);
    s.animations->load_state(data.at("animation").dump(),tick);s.animation_locals();
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
            const auto& ctrl=row.at("controller");save_fields(ctrl,{"yaw","pitch"});
            c->yaw=save_number(ctrl.at("yaw"),180.00001);c->pitch=save_number(ctrl.at("pitch"),85);
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
    // Rebuild support from current geometry only after every body is restored.
    s.physics.OptimizeBroadPhase();for(auto e:s.topology->characters)s.registry.get<Controller>(e).character->PostSimulation(.05f);s.sync();
    for(const auto& [id,saved]:locals) {
        const auto& actual=s.registry.get<Node>(s.find(id)).local;
        require(save_near(saved.position,actual.position) && save_near(saved.rotation,actual.rotation) && save_near(saved.scale,actual.scale),"Saved local transform does not match reconstructed state/content.");
    }
    std::map<std::string,AudioEmitter> emitters;
    for(auto e:s.topology->order)if(const auto* emitter=s.registry.try_get<AudioEmitter>(e))emitters.emplace(s.registry.get<Node>(e).id,*emitter);
    s.sounds.load_state(data.at("sound").dump(),tick,emitters);
    // Executable selection comes exclusively from the host. No path, type name
    // or constructor selected by the snapshot is ever invoked. Fresh game
    // constructors/statics may execute while registering the trusted module;
    // their external side effects cannot roll back. Initialize is skipped.
    if(gameplay) {
        const auto& saved=data.at("gameplay");
        auto game=std::make_unique<Gameplay>(*gameplay,nullptr,GameplayInitialization::restore);const auto current=SaveJson::parse(game->inspect());
        for(const auto* key:{"backend","assembly_sha256","type","schema"})require(current.at(key)==saved.at(key),"Snapshot gameplay module/schema differs from the trusted module.");
        game->edit(saved.at("values").dump());s.components->validate_module(game->component_schemas());
        game->validate_entity_references([](void* context,PoimaEntityId id) {
            return static_cast<RuntimeComponents*>(context)->alive(id);
        },s.components.get());
        s.game=std::move(game);s.game_revision=game_revision;
    }
    return candidate;
}
