// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl. Target-player controls join the physical command
// transaction; reads expose only the last committed tick.
    std::size_t game_physical_animation_reads=0;
    void physical_animation_read_attempt() {
        require_tick();
        require(game_physical_animation_reads<1024,"Gameplay exceeded1024 physical animation read attempts.");
        ++game_physical_animation_reads;
    }
    void physical_animation_write_attempt() {
        require_tick();
        require(game_physical_write_attempts<64,"Gameplay exceeded64 physical command attempts.");
        ++game_physical_write_attempts;
        require(game_physical_commands.size()+game_physical_caller_writes<64,"Caller and gameplay exceed64 physical writes.");
    }
    const RuntimePhysicalAnimation* physical_animation_player(const std::string& id) const {
        if(pending_entity(id))return nullptr;
        (void)find(id);
        const auto found=physical_owners.find(id);
        if(found==physical_owners.end()||!found->second->animation)return nullptr;
        const auto* player=found->second->animation.get();
        require(!player->pending()&&player->tick()==tick,"Physical animation read requires a committed boundary.");
        return player;
    }
    const RuntimeEntityDefinition& physical_animation_definition(const std::string& id) const {
        const auto* definition=pending_entity(id);
        if(!definition)definition=&topology->definitions.at(registry.get<Node>(find(id)).id);
        require(definition->physical_rig&&definition->physical_rig->animation_drive&&definition->animation_rig,
            "Physical animation controls require an opted-in PhysicalRig and AnimationRig.");
        return *definition;
    }
    static void physical_animation_layer_definition(const RuntimeEntityDefinition& definition,std::uint32_t slot) {
        require(slot<=4,"Physical animation layer must be0..4.");
        if(slot)require(std::any_of(definition.animation_rig->layers.begin(),definition.animation_rig->layers.end(),
            [&](const auto& layer){return layer.slot==slot;}),"Physical target layer is not configured.");
    }
    void stage_physical_animation(PhysicalRigCommand command) {
        validate_physical_commands({command});
        const auto channel=[](const PhysicalRigCommand& value)->std::optional<std::uint32_t> {
            if(value.animation)return value.animation->layer.value_or(0);
            if(value.animation_rate)return value.animation_rate->layer.value_or(0);
            return {};
        };
        for(const auto& old:game_physical_commands)if(old.entity==command.entity) {
            if(command.drive_source)require(
                !(old.drive&&old.drive->joint==command.drive_source->joint)&&
                !(old.drive_source&&old.drive_source->joint==command.drive_source->joint),
                "Duplicate gameplay physical source/drive write.");
            const auto a=channel(command),b=channel(old);
            require(!a||!b||a!=b,"Duplicate gameplay physical animation channel.");
        }
        game_physical_commands.push_back(std::move(command));
    }
    static int32_t POIMA_CALL get_physical_animation(void* context,const PoimaEntityId* entity,
        PoimaGameAnimationStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical animation context is absent.");auto& self=*static_cast<Impl*>(context);
            self.physical_animation_read_attempt();
            require(entity&&output,"Physical animation query is absent.");
            require(output->version==1&&output->bytes==sizeof(*output)&&output->reserved==0,
                "Physical animation output needs version1/exact136 bytes/zero reserved.");
            const auto* player=self.physical_animation_player(gameplay_id(*entity));
            const auto state=player?std::optional{player->state(false)}:std::nullopt;
            PoimaGameAnimationStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);
            project_animation(*entity,state,candidate.state);
            if(state&&state->transition)candidate.transition_mode=static_cast<std::uint32_t>(state->transition->mode);
            *output=candidate;
        });
    }
    static int32_t POIMA_CALL get_physical_animation_layer(void* context,const PoimaEntityId* entity,
        std::uint32_t slot,PoimaGameAnimationLayerStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical animation context is absent.");auto& self=*static_cast<Impl*>(context);
            self.physical_animation_read_attempt();
            require(entity&&output,"Physical animation layer query is absent.");
            require(output->version==1&&output->bytes==sizeof(*output)&&output->reserved==0&&slot>=1&&slot<=4,
                "Physical animation layer output needs version1/exact200 bytes/zero reserved/slot1..4.");
            const auto* player=self.physical_animation_player(gameplay_id(*entity));
            const auto layer=player?player->layer(slot):std::nullopt;
            *output=project_animation_layer(*entity,slot,layer);
        });
    }
    static int32_t POIMA_CALL get_physical_drive_source(void* context,const PoimaGamePhysicalRigDriveRequestV1* source,
        PoimaGamePhysicalRigDriveSourceStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical target source context is absent.");auto& self=*static_cast<Impl*>(context);
            self.physical_animation_read_attempt();
            require(source&&output,"Physical target source request/output is absent.");
            require(source->version==1&&source->bytes==sizeof(*source)&&source->reserved==0&&
                source->joint&&source->joint_bytes>=1&&source->joint_bytes<=64,"Invalid physical source request/name span.");
            require(output->version==1&&output->bytes==sizeof(*output)&&output->present==0&&output->flags==0&&
                output->joint_bytes==0&&output->reserved_name==0&&output->reserved_tail==0,
                "Physical source output needs version1/exact208 bytes/zero input flags and reserved.");
            const std::string_view joint(source->joint,source->joint_bytes);
            require(physical_part_name(joint),"Physical joint name must be portable ASCII.");
            const auto id=gameplay_id(source->entity);
            // Preserve known-joint versus unknown-joint admission even when a
            // physical rig has not opted into an animation target player.
            if(!self.pending_entity(id))(void)self.owner->physical_drive(id,std::string(joint));
            const auto* player=self.physical_animation_player(id);
            PoimaGamePhysicalRigDriveSourceStateV1 candidate{};
            candidate.version=1;candidate.bytes=sizeof(candidate);candidate.entity=source->entity;
            if(player) {
                const auto& joints=player->joints();
                const auto found=std::find_if(joints.begin(),joints.end(),[&](const auto& row){return row.name==joint;});
                if(found!=joints.end()) {
                    candidate.present=1;candidate.joint_bytes=source->joint_bytes;
                    std::copy(joint.begin(),joint.end(),candidate.joint);
                    candidate.source=found->source==RuntimePhysicalAnimation::Source::Manual?0u:1u;
                    candidate.limit_policy=static_cast<std::uint32_t>(self.topology->definitions.at(id).physical_rig->animation_drive->limit_policy);
                    candidate.sampled_tick=self.tick;
                    if(found->sample) {
                        const auto& sample=*found->sample;
                        candidate.flags=1u|(sample.hinge_projected?2u:0u)|(sample.twist_clamped?4u:0u)|(sample.swing_clamped?8u:0u);
                        std::copy(sample.requested.begin(),sample.requested.end(),candidate.requested_rotation);
                        std::copy(sample.target.begin(),sample.target.end(),candidate.target_rotation);
                        candidate.correction_radians=sample.correction_radians;candidate.chart_condition=sample.chart_condition;
                    }
                }
            }
            *output=candidate;
        });
    }
    static int32_t POIMA_CALL set_physical_drive_source(void* context,const PoimaGamePhysicalRigDriveSourceCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical target source context is absent.");auto& self=*static_cast<Impl*>(context);
            self.physical_animation_write_attempt();
            require(source&&source->version==1&&source->bytes==sizeof(*source)&&source->reserved==0&&
                source->reserved_source==0&&source->source<=1&&source->joint&&source->joint_bytes>=1&&source->joint_bytes<=64,
                "Invalid physical source command header/name/source.");
            const std::string_view joint(source->joint,source->joint_bytes);
            require(physical_part_name(joint),"Physical joint name must be portable ASCII.");
            const auto id=gameplay_id(source->entity);const auto& definition=self.physical_animation_definition(id);
            require(std::any_of(definition.physical_rig->profile.joints.begin(),definition.physical_rig->profile.joints.end(),
                [&](const auto& row){return row.name==joint&&row.drive.has_value();}),"Physical source joint has no authored drive.");
            PhysicalRigCommand command;command.entity=id;
            command.drive_source=PhysicalRigDriveSourceCommand{std::string(joint),static_cast<PhysicalRigDriveSource>(source->source),source->strength};
            self.stage_physical_animation(std::move(command));
        });
    }
    static int32_t POIMA_CALL set_physical_animation(void* context,const PoimaGameAnimationLayerCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical animation context is absent.");auto& self=*static_cast<Impl*>(context);
            self.physical_animation_write_attempt();
            require(source&&source->version==1&&source->bytes==sizeof(*source)&&source->reserved==0&&
                source->transition_mode<=1&&source->slot<=4,"Invalid physical playback command header/mode/slot.");
            const auto& input=source->command;
            require(input.clip>=-1&&input.loop<=1&&input.playing<=1&&input.blend_ticks<=3600&&
                std::isfinite(input.time)&&input.time>=0&&input.time<=1e9&&
                std::isfinite(input.speed)&&input.speed>=0&&input.speed<=8,"Invalid physical playback scalars.");
            require(std::isfinite(source->weight)&&source->weight>=0&&source->weight<=1&&source->weight_blend_ticks<=3600,
                "Invalid physical playback weight/duration.");
            require(source->slot!=0||(source->weight==1&&source->weight_blend_ticks==0),"Physical base playback has fixed weight1 and no weight fade.");
            const auto id=gameplay_id(input.entity);const auto& definition=self.physical_animation_definition(id);
            physical_animation_layer_definition(definition,source->slot);
            require(input.clip<0||(definition.animation_rig->model&&static_cast<std::size_t>(input.clip)<definition.animation_rig->model->animations.size()),
                "Physical target animation clip is absent.");
            AnimationCommand playback;playback.entity=id;
            if(input.clip>=0)playback.clip=static_cast<std::uint32_t>(input.clip);
            playback.time=input.time;playback.speed=input.speed;playback.loop=input.loop!=0;playback.playing=input.playing!=0;
            playback.blend_ticks=input.blend_ticks;playback.transition_mode=static_cast<AnimationTransitionMode>(source->transition_mode);
            if(source->slot)playback.layer=source->slot;
            playback.weight=source->weight;playback.weight_blend_ticks=source->weight_blend_ticks;
            PhysicalRigCommand command;command.entity=id;command.animation=std::move(playback);
            self.stage_physical_animation(std::move(command));
        });
    }
    static int32_t POIMA_CALL set_physical_animation_rate(void* context,const PoimaGameAnimationRateCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical animation context is absent.");auto& self=*static_cast<Impl*>(context);
            self.physical_animation_write_attempt();
            require(source&&source->version==1&&source->bytes==sizeof(*source)&&source->reserved==0&&source->layer<=4,
                "Invalid physical animation rate header/layer.");
            require(std::isfinite(source->speed)&&source->speed>=0&&source->speed<=8,"Physical animation speed must be0..8.");
            const auto id=gameplay_id(source->entity);const auto& definition=self.physical_animation_definition(id);
            physical_animation_layer_definition(definition,source->layer);
            AnimationRateCommand rate;rate.entity=id;rate.speed=source->speed;if(source->layer)rate.layer=source->layer;
            PhysicalRigCommand command;command.entity=id;command.animation_rate=rate;
            self.stage_physical_animation(std::move(command));
        });
    }
