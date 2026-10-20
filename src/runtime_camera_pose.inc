// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. Setters stage; queries see committed state.
    static int32_t POIMA_CALL get_camera_pose(void* context,const PoimaEntityId* entity,
        PoimaGameCameraPoseStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && entity && output,"Camera pose query is absent.");
            require(output->version==1 && output->bytes>=sizeof(PoimaGameCameraPoseStateV1) && output->flags==0,
                "Camera pose state requires version1, at least272 bytes and zero input flags.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_camera_pose_reads<256,"Gameplay exceeded256 camera pose reads in one Tick.");
            const auto id=gameplay_id(*entity);std::optional<RuntimeCameraPoseState> state;
            if(const auto* pending=self.pending_entity(id)) {
                if(pending->camera)state=RuntimeCameraPoseState{id,{},pending->camera->viewmodel ? std::optional(RuntimeCameraPose{}) : std::nullopt,self.base_camera_world(id,true)};
            }else state=self.camera_pose(id);
            PoimaGameCameraPoseStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);candidate.entity=*entity;
            const auto pose=[](const RuntimeCameraPose& value) {PoimaGameCameraPoseV1 result{};std::copy(value.position.begin(),value.position.end(),result.position);std::copy(value.rotation.begin(),value.rotation.end(),result.rotation);return result;};
            if(state) {std::copy(state->base_world.begin(),state->base_world.end(),candidate.base_world);candidate.present=1;candidate.world=pose(state->world);if(state->viewmodel) {candidate.flags=1;candidate.viewmodel=pose(*state->viewmodel);}}
            ++self.game_camera_pose_reads;*output=candidate;
        });
    }
    static int32_t POIMA_CALL set_camera_pose(void* context,const PoimaGameCameraPoseCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context && source,"Camera pose command is absent.");
            require(source->version==1 && source->bytes>=sizeof(PoimaGameCameraPoseCommandV1) && source->reserved==0 && source->channels>=1 && source->channels<=3,
                "Camera pose command requires version1, at least144 bytes, channels1..3 and zero reserved.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_camera_pose_writes<64,"Gameplay exceeded64 camera pose writes in one Tick.");
            const auto pose=[](const PoimaGameCameraPoseV1& value) {RuntimeCameraPose result;std::copy_n(value.position,3,result.position.begin());std::copy_n(value.rotation,4,result.rotation.begin());return result;};
            RuntimeCameraPoseCommand command;command.entity=gameplay_id(source->entity);const PoimaGameCameraPoseV1 zero{};
            if(source->channels&1u)command.world=pose(source->world);else require(std::memcmp(&source->world,&zero,sizeof(zero))==0,"Unchanged world pose requires canonical zero values.");
            if(source->channels&2u)command.viewmodel=pose(source->viewmodel);else require(std::memcmp(&source->viewmodel,&zero,sizeof(zero))==0,"Unchanged viewmodel pose requires canonical zero values.");
            self.validate_camera_pose_command(command,true);
            require(std::none_of(self.game_camera_pose_commands.begin(),self.game_camera_pose_commands.end(),[&](const auto& value){return value.entity==command.entity;}),"Duplicate gameplay camera pose target in one Tick.");
            self.game_camera_pose_commands.push_back(std::move(command));++self.game_camera_pose_writes;
        });
    }
