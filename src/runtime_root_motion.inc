// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. Sampling never consumes locomotion. The owner
// prepares one interval after staged commands, applies it through Jolt, then
// records collision-resolved observations only after the physical Tick.
    static int32_t POIMA_CALL get_root_motion(void* context,const PoimaEntityId* id,
        PoimaGameRootMotionStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && id && output,"Root-motion query is absent.");
            require(output->version==1 && output->bytes>=sizeof(PoimaGameRootMotionStateV1) &&
                output->reserved==0,"Root-motion state requires version 1, at least 176 bytes and zero reserved.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_root_motion_reads<1024,"Gameplay exceeded 1024 root-motion reads in one Tick.");
            const auto state=self.owner->root_motion(gameplay_id(*id));
            PoimaGameRootMotionStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);
            if(state) {
                candidate.present=1;candidate.entity=*id;
                if(!state->actor.empty())candidate.actor=gameplay_id(state->actor);
                candidate.node=state->node;candidate.mode=static_cast<std::uint32_t>(state->mode);
                candidate.enabled=state->enabled;candidate.yaw=state->yaw;
                candidate.from_tick=state->from_tick;candidate.to_tick=state->to_tick;
                std::copy(state->local_delta.begin(),state->local_delta.end(),candidate.local_delta);
                std::copy(state->world_requested.begin(),state->world_requested.end(),candidate.world_requested);
                std::copy(state->world_applied.begin(),state->world_applied.end(),candidate.world_applied);
                candidate.yaw_delta=state->yaw_delta;candidate.applied_yaw=state->applied_yaw;
                candidate.translation_error=state->translation_error;
            }
            ++self.game_root_motion_reads;*output=candidate;
        });
    }
    static int32_t POIMA_CALL set_root_motion(void* context,const PoimaGameRootMotionCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context && source,"Root-motion command is absent.");
            require(source->version==1 && source->bytes>=sizeof(PoimaGameRootMotionCommandV1) &&
                source->reserved==0 && source->enabled<=1,
                "Root-motion command requires version 1, at least 32 bytes, Boolean enabled and zero reserved.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_root_motion_writes<64,"Gameplay exceeded 64 root-motion writes in one Tick.");
            const auto id=gameplay_id(source->entity);
            if(const auto* pending=self.pending_entity(id))
                require(pending->animation_rig && pending->animation_rig->root_motion,
                    "Reserved root-motion target needs a configured AnimationRig.");
            else require(self.owner->root_motion(id).has_value(),"Root-motion target is not configured.");
            require(std::none_of(self.game_root_motion_commands.begin(),self.game_root_motion_commands.end(),
                [&](const auto& old){return old.entity==id;}),"Duplicate gameplay root-motion target in one Tick.");
            self.game_root_motion_commands.push_back({id,source->enabled!=0});++self.game_root_motion_writes;
        });
    }
    struct RootMotionInterval {
        RuntimeRootMotionState state;
        std::optional<entt::entity> actor;
        std::array<double,3> before_position{};
    };
    Matrix4 root_motion_actor_frame(const std::string& rig,const std::string& actor) const {
        Matrix4 frame=identity_matrix();std::string id=rig;std::size_t count=0;
        while(id!=actor) {
            require(++count<=topology->order.size(),"Root-motion actor is not a valid ancestor.");
            const auto& node=registry.get<Node>(find(id));
            require(!node.parent.empty(),"Root-motion actor must be self or an ancestor.");
            // Only frozen ordinary prefix transforms occur here. The native
            // definition validator rejects intermediate physical/animated owners.
            frame=multiply(local_matrix(node.initial.position,node.initial.rotation,node.initial.scale),frame);
            id=node.parent;
        }
        return frame;
    }
    static std::array<double,3> root_motion_vector(const Matrix4& matrix,const std::array<double,3>& value) {
        std::array<double,3> result{};
        for(std::size_t i=0;i<3;++i) {
            result[i]=matrix[i]*value[0]+matrix[4+i]*value[1]+matrix[8+i]*value[2];
            require(std::isfinite(result[i]) && std::abs(result[i])<=10000,"Root-motion displacement exceeds the supported range.");
        }
        return result;
    }
    std::vector<RootMotionInterval> prepare_root_motion_intervals(
        const std::map<entt::entity,const RuntimeInput*>& caller,
        const std::vector<RuntimeInput>& staged,bool caller_edges) {
        auto states=animations->prepare_root_motion(tick);
        std::vector<RootMotionInterval> result;result.reserve(states.size());
        if(states.empty())return result;
        sync();
        for(auto& state:states) {
            RootMotionInterval interval;interval.state=std::move(state);auto& motion=interval.state;
            if(!motion.enabled) { result.push_back(std::move(interval));continue; }
            if(motion.mode==RootMotionMode::Extract) {
                motion.world_requested=root_motion_vector(registry.get<Node>(find(motion.entity)).world,motion.local_delta);
                result.push_back(std::move(interval));continue;
            }
            const auto actor=find(motion.actor);auto& controller=registry.get<Controller>(actor);
            auto input_allowed=[&](const RuntimeInput& input,bool edges) {
                require(input.move[0]==0 && input.move[1]==0,"Character root motion conflicts with nonzero movement input.");
                require(!motion.yaw || !edges || input.look[0]==0,"Character root yaw conflicts with nonzero yaw input.");
            };
            if(const auto found=caller.find(actor);found!=caller.end())input_allowed(*found->second,caller_edges);
            for(const auto& input:staged)if(input.entity==motion.actor)input_allowed(input,true);
            const auto frame=root_motion_actor_frame(motion.entity,motion.actor);
            const std::array<double,4> yaw{0,std::sin(motion.yaw_delta*.5),0,std::cos(motion.yaw_delta*.5)};
            const auto local=local_matrix(motion.local_delta,yaw,{1,1,1});
            const auto actor_delta=multiply(multiply(frame,local),inverse_affine(frame));
            const std::array<double,3> translation{actor_delta[12],actor_delta[13],actor_delta[14]};
            require(std::abs(translation[1])<=1e-6,"Character root motion requires an upright actor/rig frame.");
            motion.world_requested=root_motion_vector(registry.get<Node>(actor).world,translation);
            require(std::abs(motion.world_requested[1])<=1e-6,"Character root-motion world displacement must be planar.");
            // Metric validation tolerates rounding; the Character contract is
            // canonical planar XZ, independently of tiny accepted mounting noise.
            motion.world_requested[1]=0;
            const auto position=controller.character->GetPosition();
            interval.before_position={position.GetX(),position.GetY(),position.GetZ()};interval.actor=actor;
            const auto velocity=controller.character->GetLinearVelocity();
            controller.character->SetLinearVelocity(JPH::Vec3(
                static_cast<float>(motion.world_requested[0]/Runtime::fixed_dt),velocity.GetY(),
                static_cast<float>(motion.world_requested[2]/Runtime::fixed_dt)));
            if(motion.yaw && motion.yaw_delta!=0) {
                controller.yaw=std::remainder(controller.yaw+motion.yaw_delta/radians,360.0);
                controller.character->SetRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(),static_cast<float>(controller.yaw*radians)));
                motion.applied_yaw=motion.yaw_delta;
            }
            result.push_back(std::move(interval));
        }
        return result;
    }
    void commit_root_motion_intervals(std::vector<RootMotionInterval>& intervals,std::uint64_t next_tick) {
        std::vector<RuntimeRootMotionState> states;states.reserve(intervals.size());
        for(auto& interval:intervals) {
            auto& state=interval.state;
            if(interval.actor) {
                const auto position=registry.get<Controller>(*interval.actor).character->GetPosition();
                const std::array<double,3> after{position.GetX(),position.GetY(),position.GetZ()};
                for(std::size_t i=0;i<3;++i)state.world_applied[i]=after[i]-interval.before_position[i];
                state.translation_error=std::hypot(state.world_applied[0]-state.world_requested[0],
                    state.world_applied[2]-state.world_requested[2]);
            }
            states.push_back(std::move(state));
        }
        animations->commit_root_motion(states,next_tick);
    }
