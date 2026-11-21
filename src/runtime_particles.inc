// SPDX-License-Identifier: Apache-2.0
// A frozen scene owns at most one particle system. Entity identities and
// emitter identities are separate from the system's reusable storage slots.
    struct ParticleBinding {
        std::string entity,template_id,template_node;
        RuntimeParticleEmitter definition;
        bool draining=false;
    };
    struct ParticleRuntime {
        ParticleRuntime()=default;
        ParticleRuntime(const ParticleRuntime& other)
            :system(other.system.clone()),sources(other.sources),next_id(other.next_id),membership(other.membership) {}
        particles::System system{runtime_particle_limits};
        std::map<particles::EmitterId,ParticleBinding> sources;
        particles::EmitterId next_id=1;
        std::shared_ptr<const Topology> membership;
    };
    std::unique_ptr<ParticleRuntime> particle_runtime;
    particles::SpriteCatalog particle_sprites;
    void restore_particles(const RuntimeDefinition&,const nlohmann::json&);
    static bool definition_has_particles(const RuntimeDefinition& definition) {
        const auto has_emitter=[](const auto& entry){return entry.particle_emitter.has_value();};
        return std::any_of(definition.entities.begin(),definition.entities.end(),has_emitter) ||
            std::any_of(definition.templates.begin(),definition.templates.end(),[&](const auto& recipe) {
                return has_emitter(recipe) || std::any_of(recipe.entities.begin(),recipe.entities.end(),has_emitter);
            });
    }
    static particles::Pose particle_pose(const Matrix4& world) {
        require(rigid_transform(world) && pose_frames::axes(world).basis==pose_frames::Basis::regular,
            "Particle emitter requires a rigid, proper world transform.");
        const auto rotation=runtime_quaternion(world);
        return {{world[12],world[13],world[14]},{rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()}};
    }
    std::pair<std::string,std::string> particle_origin(const std::string& entity) const {
        for(const auto& [root,origin]:topology->spawned) {
            (void)root;
            for(const auto& [local,id]:origin.nodes)if(id==entity)return {origin.template_id,local};
        }
        return {};
    }
    void synchronize_particles() {
        if(!particle_runtime)return;
        auto& state=*particle_runtime;
        if(state.membership!=topology) {
            std::set<std::string_view> active;
            for(auto entry=state.sources.begin();entry!=state.sources.end();) {
                auto& source=entry->second;
                if(source.draining) {++entry;continue;}
                if(topology->identities.contains(source.entity)) {
                    active.insert(source.entity);++entry;continue;
                }
                const auto id=entry->first;
                if(source.definition.config.simulation_space==particles::Space::World && state.system.emitter_status(id).particles.live) {
                    state.system.set_emitting(id,false);source.draining=true;++entry;
                } else {
                    state.system.remove_emitter(id);entry=state.sources.erase(entry);
                }
            }
            for(const auto& [id,entity]:topology->definitions)if(entity.particle_emitter && !active.contains(id)) {
                require(state.sources.size()<runtime_particle_limits.emitters,"Runtime particle emitter capacity exceeded, including draining sources.");
                require(state.next_id<9007199254740991ULL,"Runtime particle emitter identity space exhausted.");
                const auto& definition=*entity.particle_emitter;
                require(definition.burst_count<=particles::max_burst && (definition.burst_count ? valid_input_action_name(definition.burst_action) : definition.burst_action.empty()),
                    "Particle emitter requires a valid named action and positive bounded burst count together.");
                auto [template_id,template_node]=particle_origin(id);
                state.system.add_emitter(state.next_id,definition.config,particle_pose(registry.get<Node>(find(id)).world));
                state.sources.emplace(state.next_id,ParticleBinding{id,std::move(template_id),std::move(template_node),definition,false});
                ++state.next_id;
            }
            state.membership=topology;
        }
        for(const auto& [id,source]:state.sources)if(!source.draining)
            state.system.commit_pose(id,particle_pose(registry.get<Node>(find(source.entity)).world));
    }
    void step_particles(std::span<const std::string_view> pressed) {
        if(!particle_runtime)return;
        profiling::Scope profile("runtime.particles",static_cast<std::int64_t>(tick));
        synchronize_particles();
        auto& state=*particle_runtime;
        for(const auto& [id,source]:state.sources)if(!source.draining && source.definition.burst_count &&
            std::binary_search(pressed.begin(),pressed.end(),source.definition.burst_action))
            state.system.burst(id,source.definition.burst_count);
        state.system.step();
        for(auto entry=state.sources.begin();entry!=state.sources.end();) {
            if(entry->second.draining && !state.system.emitter_status(entry->first).particles.live) {
                state.system.remove_emitter(entry->first);entry=state.sources.erase(entry);
            } else ++entry;
        }
    }
    std::vector<particles::Instance> particle_instances() const {
        const auto& system=particle_runtime->system;
        std::vector<particles::Instance> result(system.status().particles.live);
        system.write_instances(result);
        return result;
    }
    RuntimeParticleObservation observe_particles() const {
        if(!particle_runtime)return {false,{runtime_particle_limits,tick,0,{}},{},{}};
        const auto& state=*particle_runtime;
        RuntimeParticleObservation result{true,state.system.status(),{},particle_instances()};
        result.sources.reserve(state.sources.size());
        for(const auto& [id,source]:state.sources)
            result.sources.push_back({id,source.entity,source.draining,state.system.emitter_status(id)});
        return result;
    }
