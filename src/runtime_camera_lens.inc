// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. Tick setters stage; reads remain committed.
    static int32_t POIMA_CALL get_camera_lens(void* context,const PoimaEntityId* entity,
        PoimaGameCameraLensStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && entity && output,"Camera lens query is absent.");
            require(output->version==1 && output->bytes>=sizeof(PoimaGameCameraLensStateV1) && output->flags==0,
                "Camera lens state requires version1, at least104 bytes and zero input flags.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_camera_lens_reads<256,"Gameplay exceeded256 camera lens reads in one Tick.");
            const auto id=gameplay_id(*entity);std::optional<RuntimeCameraLensState> state;
            if(const auto* pending=self.pending_entity(id)) {
                if(pending->camera) {
                    const auto& c=*pending->camera;
                    state=RuntimeCameraLensState{id,{c.vertical_fov,c.near_plane,c.far_plane},c.viewmodel,{c.vertical_fov,c.near_plane,c.far_plane}};
                }
            }else state=self.camera_lens(id);
            PoimaGameCameraLensStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);
            const auto lens=[](const SceneLens& value) {return PoimaGameCameraLensV1{value.vertical_fov,value.near_plane,value.far_plane};};
            if(state) {
                candidate.present=1;candidate.entity=*entity;candidate.world=lens(state->world);candidate.authored_world=lens(state->authored_world);
                if(state->viewmodel) {candidate.flags=1;candidate.viewmodel=lens(*state->viewmodel);}
            }
            ++self.game_camera_lens_reads;*output=candidate;
        });
    }
    static int32_t POIMA_CALL set_camera_lens(void* context,const PoimaGameCameraLensCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context && source,"Camera lens command is absent.");
            require(source->version==1 && source->bytes>=sizeof(PoimaGameCameraLensCommandV1) && source->reserved==0 &&
                source->channels>=1 && source->channels<=3,"Camera lens command requires version1, at least80 bytes, channels1..3 and zero reserved.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_camera_lens_writes<64,"Gameplay exceeded64 camera lens writes in one Tick.");
            RuntimeCameraLensCommand command;command.entity=gameplay_id(source->entity);
            const auto lens=[](const PoimaGameCameraLensV1& value) {return SceneLens{value.vertical_fov,value.near_plane,value.far_plane};};
            const PoimaGameCameraLensV1 zero{};
            if(source->channels&1u)command.world=lens(source->world);
            else require(std::memcmp(&source->world,&zero,sizeof(zero))==0,"Unchanged world lens must have canonical zero values.");
            if(source->channels&2u)command.viewmodel=lens(source->viewmodel);
            else require(std::memcmp(&source->viewmodel,&zero,sizeof(zero))==0,"Unchanged viewmodel lens must have canonical zero values.");
            self.validate_camera_lens_command(command,true);
            require(std::none_of(self.game_camera_lens_commands.begin(),self.game_camera_lens_commands.end(),
                [&](const auto& previous){return previous.entity==command.entity;}),"Duplicate gameplay camera lens target in one Tick.");
            self.game_camera_lens_commands.push_back(std::move(command));++self.game_camera_lens_writes;
        });
    }
