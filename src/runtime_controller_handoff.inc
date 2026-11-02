// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. A callback sees one joined planned input
// observation; physics, hierarchy caches and final-pose history stay committed.
    std::map<std::string,detail::CharacterInputPlan> deferred_controller_inputs;
    std::map<std::string,RuntimeTransform> callback_pose_locals;
    std::map<std::string,Matrix4> callback_pose_worlds;
    std::map<std::pair<std::string,std::uint32_t>,RuntimeIkState> callback_ik_states;
    bool callback_pose_preview=false;

    bool linked_controller(const std::string& actor) const {
        for(const auto& [id,physical]:physical_owners)
            if(topology->identities.contains(id) && physical->handoff && physical->handoff->actor==actor)return true;
        return false;
    }
    void clear_controller_preview() noexcept {
        callback_pose_preview=false;callback_pose_locals.clear();callback_pose_worlds.clear();callback_ik_states.clear();
    }
    const RuntimeTransform& observation_local(const std::string& id) const {
        if(callback_pose_preview && game_phase==GamePhase::tick) {
            const auto found=callback_pose_locals.find(id);if(found!=callback_pose_locals.end())return found->second;
        }
        return registry.get<Node>(find(id)).local;
    }
    const Matrix4& observation_world(const std::string& id) const {
        if(callback_pose_preview && game_phase==GamePhase::tick) {
            const auto found=callback_pose_worlds.find(id);if(found!=callback_pose_worlds.end())return found->second;
        }
        return registry.get<Node>(find(id)).world;
    }
    std::optional<RuntimeIkState> observation_ik(const std::string& id,std::uint32_t slot) const {
        if(callback_pose_preview && game_phase==GamePhase::tick) {
            const auto found=callback_ik_states.find({id,slot});
            if(found!=callback_ik_states.end())return found->second;
        }
        return animations->ik_state(id,slot);
    }
    const detail::CharacterInputPlan* observation_character(const std::string& id) const {
        if(!callback_pose_preview || game_phase!=GamePhase::tick)return nullptr;
        const auto found=deferred_controller_inputs.find(id);return found==deferred_controller_inputs.end()?nullptr:&found->second;
    }
    void prepare_linked_controller(entt::entity e,const RuntimeInput& input,bool edges) {
        auto& c=registry.get<Controller>(e);if(c.character->IsSuppressed())return;
        const auto speed=c.crouched ? c.settings.stance->crouched_speed : c.settings.speed;
        deferred_controller_inputs.emplace(registry.get<Node>(e).id,detail::prepare_character_input(c.yaw,c.pitch,
            c.character->GetRotation(),c.character->GetIntegrationVelocity().GetY(),
            c.character->GetGroundState()==JPH::CharacterBase::EGroundState::OnGround,
            speed,c.settings.jump_speed,input,edges));
    }
    void build_controller_preview() {
        clear_controller_preview();
        if(deferred_controller_inputs.empty())return;
        for(const auto& [id,plan]:deferred_controller_inputs) {
            const auto e=find(id);const auto& c=registry.get<Controller>(e);auto actor=registry.get<Node>(e).local;
            const auto p=c.character->GetPosition();actor.position={p.GetX(),p.GetY(),p.GetZ()};
            actor.rotation={plan.rotation.GetX(),plan.rotation.GetY(),plan.rotation.GetZ(),plan.rotation.GetW()};
            callback_pose_locals.emplace(id,actor);
            if(c.settings.camera.empty())continue;
            const auto& camera=registry.get<Node>(find(c.settings.camera));auto local=camera.local;const auto& initial=camera.initial.rotation;
            const auto q=JPH::Quat(static_cast<float>(initial[0]),static_cast<float>(initial[1]),static_cast<float>(initial[2]),static_cast<float>(initial[3])).Normalized()
                * JPH::Quat::sRotation(JPH::Vec3::sAxisX(),static_cast<float>(plan.pitch*radians));
            local.rotation={q.GetX(),q.GetY(),q.GetZ(),q.GetW()};
            if(c.settings.stance) {local.position=camera.initial.position;if(c.crouched)local.position[1]-=c.settings.stance->camera_drop;}
            callback_pose_locals.emplace(c.settings.camera,local);
        }
        const auto rebuild=[&] {
            callback_pose_worlds.clear();
            for(const auto e:topology->hierarchy) {
                const auto& node=registry.get<Node>(e);const auto local=callback_pose_locals.find(node.id);
                const auto parent=callback_pose_worlds.find(node.parent);
                if(local==callback_pose_locals.end() && parent==callback_pose_worlds.end())continue;
                const auto& value=local==callback_pose_locals.end()?node.local:local->second;
                auto world=local_matrix(value.position,value.rotation,value.scale);
                if(!node.parent.empty())world=multiply(parent==callback_pose_worlds.end()?registry.get<Node>(find(node.parent)).world:parent->second,world);
                if(const auto* camera=registry.try_get<RuntimeCamera>(e);camera && camera->world_pose!=RuntimeCameraPose{})
                    world=multiply(world,local_matrix(camera->world_pose.position,camera->world_pose.rotation,{1,1,1}));
                for(double v:world)require(std::isfinite(v) && std::abs(v)<=1e12,"Planned callback hierarchy exceeds the supported range.");
                callback_pose_worlds.emplace(node.id,world);
            }
        };
        rebuild();
        for(const auto& [id,physical]:physical_owners) {
            if(!topology->identities.contains(id) || !physical->handoff ||
                !deferred_controller_inputs.contains(physical->handoff->actor) ||
                !animations->procedural_enabled(id) || animations->procedural_suspended(id))continue;
            const auto world=callback_pose_worlds.find(id);
            const auto& wrapper=world==callback_pose_worlds.end()?registry.get<Node>(find(id)).world:world->second;
            const auto preview=animations->preview_procedural(id,tick,wrapper);
            for(const auto& pose:preview.poses)callback_pose_locals.insert_or_assign(pose.entity,pose.local);
            for(const auto& state:preview.states)callback_ik_states.insert_or_assign({id,state.slot},state);
        }
        if(!callback_ik_states.empty())rebuild();
        callback_pose_preview=true;
    }
    void commit_linked_controller_inputs() {
        bool changed=false;
        for(const auto& [id,plan]:deferred_controller_inputs) {
            if(!topology->identities.contains(id))continue;
            auto& c=registry.get<Controller>(find(id));if(c.character->IsSuppressed())continue;
            c.yaw=plan.yaw;c.pitch=plan.pitch;
            if(plan.write_rotation)c.character->SetRotation(plan.rotation);
            if(plan.request_jump)c.character->RequestJump(c.settings.jump_speed);
            c.character->SetLinearVelocity(plan.requested_velocity);changed=true;
        }
        deferred_controller_inputs.clear();
        if(changed)sync();
    }
    void restore_controller_handoff_membership(const PhysicalCheckpoint& saved) {
        if(!saved.enabled)return;
        for(const auto& [id,row]:saved.rigs) {
            (void)id;if(!row.handoff)continue;
            auto& motor=*registry.get<Controller>(find(row.handoff->actor)).character;
            motor.RestoreSuppression(row.handoff->passive ? std::optional{row.handoff->passive->support} : std::nullopt);
        }
    }
