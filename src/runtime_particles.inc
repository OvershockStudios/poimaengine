// SPDX-License-Identifier: Apache-2.0
// A frozen scene owns at most one particle system. Entity identities and
// emitter identities are separate from the system's reusable storage slots.
    struct ParticleBinding {
        std::string entity,template_id,template_node;
        RuntimeParticleEmitter definition;
        bool draining=false;
        RuntimeParticleRainSample rain{};
    };
    struct ParticleRuntime {
        ParticleRuntime()=default;
        ParticleRuntime(const ParticleRuntime& other)
            :system(other.system.clone()),sources(other.sources),next_id(other.next_id),membership(other.membership),
            rain(other.rain) {}
        particles::System system{runtime_particle_limits};
        std::map<particles::EmitterId,ParticleBinding> sources;
        particles::EmitterId next_id=1;
        std::shared_ptr<const Topology> membership;
        std::optional<RuntimeParticleRainWindow> rain;
    };
    std::unique_ptr<ParticleRuntime> particle_runtime;
    particles::SpriteCatalog particle_sprites;
    struct GameParticleBurst {
        std::string emitter;
        std::uint32_t count;
    };
    std::vector<GameParticleBurst> game_particle_bursts;
    std::size_t game_particle_burst_calls=0;

    static int32_t POIMA_CALL burst_game_particles(void* context,const PoimaGameParticleBurstV1* command,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Particle burst context is absent.");
            auto& self=*static_cast<Impl*>(context);
            self.require_tick();
            require(self.game_particle_burst_calls<max_gameplay_particle_burst_attempts,
                "Gameplay exceeded 32 particle burst requests in one tick.");
            ++self.game_particle_burst_calls;
            require(command && command->version==1 && command->bytes==sizeof(PoimaGameParticleBurstV1) &&
                command->reserved==0 && (command->emitter.high || command->emitter.low) &&
                command->count>=1 && command->count<=particles::max_burst,"Invalid particle burst request.");

            auto id=gameplay_id(command->emitter);
            const auto active=self.topology->definitions.find(id);
            const auto* target=active==self.topology->definitions.end() ? self.pending_entity(id) : &active->second;
            require(target && target->particle_emitter,"Particle burst requires a live or staged ParticleEmitter.");
            require(!target->particle_emitter->rain,"Rain emitters only accept weather-driven emission.");
            self.game_particle_bursts.push_back({std::move(id),command->count});
        });
    }

    void restore_particles(const RuntimeDefinition&,const nlohmann::json&);
    void restore_precipitation(const nlohmann::json&);
    nlohmann::json archive_precipitation() const;
    static bool definition_has_rain(const RuntimeDefinition& definition) {
        return std::ranges::any_of(definition.entities,[](const auto& entity) {
            return entity.particle_emitter && entity.particle_emitter->rain;
        });
    }
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
    void step_precipitation(std::optional<std::uint32_t> rain_nm) {
        auto& state=*particle_runtime;
        if(!state.rain)return;
        auto& window=*state.rain;
        if(rain_nm) {
            window.rain_nm=rain_nm;
            window.ticks_remaining=*rain_nm ? rain_particle_window_ticks : 0;
        }
        if(!window.ticks_remaining)return;
        --window.ticks_remaining;
        window.sample_tick=tick;
        for(auto& [id,source]:state.sources) {
            if(source.draining || !source.definition.rain || !source.definition.config.emitting)continue;
            const auto& profile=*source.definition.rain;
            auto& sample=source.rain;
            // One game-second depth funds a finite window at the simulation rate.
            // Covered ticks spend the same phase without placing deferred births.
            const auto denominator=profile.nm_per_particle*rain_particle_window_ticks;
            const auto numerator=std::min(*window.rain_nm,denominator*profile.max_particles_per_tick);
            const auto phase=sample.phase+numerator;
            sample.phase=phase%denominator;
            sample.scheduled_births=phase/denominator;
            const auto pose=state.system.emitter_status(id).pose;
            sample.blocker=rain_shelter(source.entity,pose.position_m);
            if(!sample.blocker && sample.scheduled_births)state.system.burst(id,sample.scheduled_births);
        }
    }
    void step_particles(std::span<const std::string_view> pressed,std::optional<std::uint32_t> rain_nm) {
        if(!particle_runtime)return;
        profiling::Scope profile("runtime.particles",static_cast<std::int64_t>(tick));
        synchronize_particles();
        auto& state=*particle_runtime;
        for(const auto& [id,source]:state.sources)if(!source.draining && source.definition.burst_count &&
            std::binary_search(pressed.begin(),pressed.end(),source.definition.burst_action))
            state.system.burst(id,source.definition.burst_count);
        // Synchronization above includes gameplay spawns and the finalized pose.
        // Both request paths feed the same native per-emitter budget.
        for(const auto& command:game_particle_bursts) {
            const auto found=std::find_if(state.sources.begin(),state.sources.end(),[&](const auto& entry) {
                return !entry.second.draining && entry.second.entity==command.emitter;
            });
            require(found!=state.sources.end(),"Particle burst target did not survive this tick.");
            const auto pending=state.system.emitter_status(found->first).pending_burst;
            require(command.count<=particles::max_burst-pending,"Particle burst exceeds the per-emitter tick budget.");
            state.system.burst(found->first,command.count);
        }
        game_particle_bursts.clear();
        step_precipitation(rain_nm);
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
        RuntimeParticleObservation result{true,state.system.status(),{},particle_instances(),state.rain};
        result.sources.reserve(state.sources.size());
        for(const auto& [id,source]:state.sources) {
            std::shared_ptr<const TextureImage> texture;
            const auto& sprite=source.definition.sprite;
            if(sprite) {
                const auto index=particle_sprites.indices.at({sprite->texture_sha256,sprite->depth_fade_m});
                texture=particle_sprites.styles.at(index).texture;
            }
            result.sources.push_back({id,source.entity,source.draining,state.system.emitter_status(id),sprite,
                std::move(texture),source.definition.rain ? std::optional(source.rain) : std::nullopt});
        }
        return result;
    }
