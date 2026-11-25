// SPDX-License-Identifier: Apache-2.0
// A frozen scene owns at most one particle system. Entity identities and
// emitter identities are separate from the system's reusable storage slots.
    struct ParticleBinding {
        std::string entity,template_id,template_node;
        RuntimeParticleEmitter definition;
        bool draining=false;
        RuntimeParticleRainSample rain{};
        std::optional<RuntimeParticleContactSample> contacts{};
        RuntimeParticleWaterFlowSample water_flow{};
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
    void validate_water_flow_emitter(const RuntimeParticleEmitter& definition) const {
        if (const auto& flow=definition.water_flow) {
            require(!definition.rain && definition.config.particles_per_second==0 &&
                flow->microlitres_per_particle>=1 && flow->microlitres_per_particle<=max_microlitres_per_particle &&
                flow->max_particles_per_tick>=1 && flow->max_particles_per_tick<=max_water_flow_particles_per_tick &&
                (flow->direction==WaterFlowDirection::Forward || flow->direction==WaterFlowDirection::Reverse ||
                    flow->direction==WaterFlowDirection::Both),"Invalid water flow particle profile.");
            require(water_region && water_region->storage() && water_region->storage()->transfer_m3(flow->port),
                "Particle water flow needs a named port in the committed storage network.");
        }
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
                require(!definition.water_contact || definition.config.expire_on_contact,
                    "Particle water contact requires surface collision.");
                validate_water_flow_emitter(definition);
                require(definition.burst_count<=particles::max_burst && (definition.burst_count ? valid_input_action_name(definition.burst_action) : definition.burst_action.empty()),
                    "Particle emitter requires a valid named action and positive bounded burst count together.");
                if(const auto& profile=definition.impact_burst) {
                    require(definition.config.expire_on_contact && valid_particle_impact_profile(*profile),"Invalid particle impact profile.");
                    visit_impact_bursts(*profile,[&](const auto& burst) {
                        const auto target=topology->definitions.find(burst.emitter);
                        require(burst.count>=1 && burst.count<=max_particle_impact_burst && burst.emitter!=id &&
                            target!=topology->definitions.end() && target->second.particle_emitter &&
                            target->second.particle_emitter->config.simulation_space==particles::Space::World &&
                            !target->second.particle_emitter->rain, "Impact burst needs another live world emitter and a bounded count.");
                    });
                }
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
    static constexpr std::uint64_t microlitres_per_cubic_metre=1'000'000'000;
    static constexpr std::uint64_t max_water_flow_sample=static_cast<std::uint64_t>(
        water::max_storage_rate * water::step_seconds * microlitres_per_cubic_metre + .5);
    std::uint64_t water_flow_volume(const RuntimeParticleWaterFlowProfile& profile) const {
        const auto transfer=water_region->storage()->transfer_m3(profile.port).value();
        const double volume=selected_water_flow(transfer,profile.direction);
        return static_cast<std::uint64_t>(std::floor(volume*microlitres_per_cubic_metre+.5));
    }
    void step_water_flow_particles() {
        auto& state=*particle_runtime;
        for (auto& [id,source] : state.sources) {
            if (source.draining || !source.definition.water_flow || !source.definition.config.emitting) continue;
            const auto& profile=*source.definition.water_flow;
            auto& sample=source.water_flow;
            sample.tick=tick;
            sample.volume_microlitres=water_flow_volume(profile);
            const auto cap=std::uint64_t(profile.microlitres_per_particle)*profile.max_particles_per_tick;
            const auto credit=sample.phase_microlitres+std::min(sample.volume_microlitres,cap);
            sample.phase_microlitres=static_cast<std::uint32_t>(credit%profile.microlitres_per_particle);
            const auto pending=state.system.emitter_status(id).pending_burst;
            sample.scheduled_births=static_cast<std::uint32_t>(std::min<std::uint64_t>(
                credit/profile.microlitres_per_particle,particles::max_burst-pending));
            if (sample.scheduled_births) state.system.burst(id,sample.scheduled_births);
        }
    }
    static particles::Pose particle_impact_pose(const RuntimeRayHit& hit,const particles::Vec3& direction) {
        auto normal=*hit.normal;
        const auto length=std::hypot(normal[0],normal[1],normal[2]);
        require(std::isfinite(length) && length>0,"Particle impact hit has an invalid normal.");
        const auto facing=std::inner_product(normal.begin(),normal.end(),direction.begin(),0.)>0 ? -length : length;
        for(auto& axis:normal)axis/=facing;
        particles::Quaternion rotation{normal[2],0,-normal[0],1+normal[1]};
        const auto magnitude=std::hypot(rotation[0],rotation[2],rotation[3]);
        if(magnitude>0)for(auto& axis:rotation)axis/=magnitude;
        else rotation={1,0,0,0};
        return {hit.position,rotation};
    }
    bool particle_contact(ParticleBinding& source, std::uint64_t particle, const particles::Vec3& from,
        const particles::Vec3& to,std::optional<particles::PlacedBurst>& spawn) const {
        auto& sample=*source.contacts;
        const particles::Vec3 direction{to[0]-from[0],to[1]-from[1],to[2]-from[2]};
        const auto distance=std::hypot(direction[0],direction[1],direction[2]);
        const auto in_bounds=[](const particles::Vec3& point) {
            return std::ranges::all_of(point,[](double axis){return std::isfinite(axis) && std::abs(axis)<=1e6;});
        };
        if(!(distance>=.001 && distance<=10000) || !in_bounds(from) || !in_bounds(to)) {
            ++sample.skipped;
            return false;
        }
        ++sample.tested;
        RuntimeRay query{from,direction,distance,{}};
        if(!source.draining)query.ignore.push_back(source.entity);
        const auto& profile=source.definition.impact_burst;
        auto hit=physics_raycast(query,{},true,!sample.first || (profile && !profile->by_surface.empty()));
        if(source.definition.water_contact && water_region) {
            const auto liquid=water_region->surface_entry(from,to);
            if(liquid && (!hit || liquid->fraction*distance+1e-6<hit->distance))
                hit=RuntimeRayHit{"",liquid->fraction,liquid->fraction*distance,liquid->position_m,
                    liquid->normal,std::nullopt,"water"};
        }
        if(!hit)return false;
        ++sample.hits;
        const bool first=!sample.first;
        if(first)sample.first=RuntimeParticleContact{particle,hit->entity,hit->position,hit->normal,hit->triangle,hit->surface};
        if(profile && !source.draining && hit->normal) {
            const auto route=profile->by_surface.find(hit->surface);
            const auto& selected=route==profile->by_surface.end() ? profile->fallback : route->second;
            if(!selected)return true;
            const auto& burst=*selected;
            const auto target=std::ranges::find_if(particle_runtime->sources,[&](const auto& row) {
                return !row.second.draining && row.second.entity==burst.emitter;
            });
            require(target!=particle_runtime->sources.end(),"Impact burst target was not synchronized.");
            spawn=particles::PlacedBurst{target->first,burst.count,particle_impact_pose(*hit,direction)};
            sample.impact_requested+=burst.count;
            if(first && !profile->by_surface.empty())sample.first->impact_emitter=burst.emitter;
        }
        return true;
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
        step_water_flow_particles();
        for(auto& [id,source]:state.sources) {
            (void)id;
            if(source.definition.config.expire_on_contact)
                source.contacts=RuntimeParticleContactSample{.tick=tick,.first=std::nullopt};
        }
        struct ContactBatch {
            Impl& owner;
            std::optional<particles::PlacedBurst> spawn;
        } batch{*this,{}};
        state.system.step({&batch,[](void* context,particles::EmitterId emitter,std::uint64_t particle,
            const particles::Vec3& from,const particles::Vec3& to) {
            auto& pending=*static_cast<ContactBatch*>(context);
            return pending.owner.particle_contact(pending.owner.particle_runtime->sources.at(emitter),particle,from,to,pending.spawn);
        },[](void* context) {
            return std::exchange(static_cast<ContactBatch*>(context)->spawn,std::nullopt);
        }});
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
                std::move(texture),source.definition.rain ? std::optional(source.rain) : std::nullopt,
                source.definition.config.expire_on_contact,source.contacts,
                source.draining ? std::nullopt : source.definition.impact_burst,source.definition.water_contact,
                source.definition.water_flow ? std::optional(RuntimeParticleWaterFlowObservation{
                    *source.definition.water_flow,source.water_flow}) : std::nullopt});
        }
        return result;
    }
