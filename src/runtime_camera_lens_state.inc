// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. Frozen definitions are never mutated by lens
// controls; pending births expose authored values until membership publication.
    static void validate_camera_lens(const SceneLens& value) {
        require(std::isfinite(value.vertical_fov) && value.vertical_fov>=5 && value.vertical_fov<=150 &&
            std::isfinite(value.near_plane) && std::isfinite(value.far_plane) &&
            value.near_plane>=.001 && value.near_plane<value.far_plane && value.far_plane<=1e7,
            "Camera lens must be finite with FOV5..150 and .001<=near<far<=1e7.");
    }
    std::optional<RuntimeCameraLensState> camera_lens(const std::string& id) const {
        const auto e=find(id);const auto* camera=registry.try_get<RuntimeCamera>(e);
        if(!camera)return std::nullopt;
        const auto& authored=topology->definitions.at(id).camera;
        require(authored.has_value(),"Frozen Camera membership differs from runtime.");
        return RuntimeCameraLensState{id,{camera->vertical_fov,camera->near_plane,camera->far_plane},
            camera->viewmodel,{authored->vertical_fov,authored->near_plane,authored->far_plane}};
    }
    void validate_camera_lens_command(const RuntimeCameraLensCommand& command,bool allow_pending=false) const {
        require(command.world || command.viewmodel,"Camera lens command must select a channel.");
        if(command.world)validate_camera_lens(*command.world);
        if(command.viewmodel)validate_camera_lens(*command.viewmodel);
        const RuntimeCamera* camera=nullptr;
        if(allow_pending)if(const auto* pending=pending_entity(command.entity)) {
            require(pending->camera.has_value(),"Reserved lens target requires a Camera.");camera=&*pending->camera;
        }
        if(!camera)camera=registry.try_get<RuntimeCamera>(find(command.entity));
        require(camera,"Lens target requires a live Camera.");
        require(!command.viewmodel || camera->viewmodel.has_value(),"Lens controls cannot add a foreground lens.");
    }
    void apply_camera_lenses(const std::vector<RuntimeCameraLensCommand>& commands) {
        require(commands.size()<=64,"At most64 camera lens commands per Tick.");
        std::set<std::string> targets;
        for(const auto& command:commands) {
            require(targets.insert(command.entity).second,"Duplicate camera lens target.");
            validate_camera_lens_command(command);
        }
        for(const auto& command:commands) {
            auto& camera=registry.get<RuntimeCamera>(find(command.entity));
            if(command.world) {
                camera.vertical_fov=command.world->vertical_fov;camera.near_plane=command.world->near_plane;
                camera.far_plane=command.world->far_plane;
            }
            if(command.viewmodel)camera.viewmodel=*command.viewmodel;
        }
    }
