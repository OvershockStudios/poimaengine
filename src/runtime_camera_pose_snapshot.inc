// SPDX-License-Identifier: Apache-2.0
// Pure presentation extraction. Never borrow a mutable hierarchy/cache or
// temporarily write an offset into the authoritative Runtime.
SceneSnapshot Runtime::snapshot(const std::string& camera,const RuntimeCameraPose& world,
    const std::optional<RuntimeCameraPose>& viewmodel) const {
    Impl::validate_camera_pose(world);
    if(viewmodel)Impl::validate_camera_pose(*viewmodel);
    const auto entity=impl_->find(camera);
    const auto* lens=impl_->registry.try_get<RuntimeCamera>(entity);
    require(lens,"Presentation pose target requires a Camera.");
    require(viewmodel.has_value()==lens->viewmodel.has_value(),"Presentation pose foreground membership must match its camera.");
    if(world==lens->world_pose && (!viewmodel || *viewmodel==lens->viewmodel_pose))return snapshot(camera);

    // Identity/current observations remain valid for legacy physical/animated
    // cameras; only a changed presentation may introduce a new effect.
    RuntimeCameraPoseCommand command{camera,{},viewmodel};
    if(world!=lens->world_pose)command.world=world;
    impl_->validate_camera_pose_command(command);
    const auto projection=impl_->camera_projection(camera);

    std::map<std::string,Matrix4> worlds;
    const auto bounded=[](const Matrix4& matrix) {
        for(double value:matrix)require(std::isfinite(value) && std::abs(value)<=1e12,"Presentation hierarchy matrix exceeds the supported range.");
    };
    for(const auto e:impl_->topology->hierarchy) {
        const auto& node=impl_->registry.get<Node>(e);
        const auto local=local_matrix(node.local.position,node.local.rotation,node.local.scale);
        auto matrix=node.parent.empty() ? local : multiply(worlds.at(node.parent),local);
        if(const auto* other=impl_->registry.try_get<RuntimeCamera>(e)) {
            const auto& offset=node.id==camera ? world : other->world_pose;
            if(offset!=RuntimeCameraPose{})matrix=multiply(matrix,local_matrix(offset.position,offset.rotation,{1,1,1}));
        }
        bounded(matrix);worlds.emplace(node.id,std::move(matrix));
    }
    const auto matrix=[&](const std::string& id)->const Matrix4& {return worlds.at(id);};
    require(rigid_transform(matrix(camera)),"Presentation camera hierarchy must not scale or shear the camera.");

    SceneSnapshot result;
    result.presentation_source_id=impl_->presentation_source_id;result.world_id=impl_->world_id;
    result.revision=impl_->revision;result.material_revision=impl_->material_revision;
    result.presentation_generation=impl_->material_revision;result.camera_id=camera;
    result.camera_world=matrix(camera);result.vertical_fov=projection.vertical_fov;
    result.authored_vertical_fov=projection.authored_vertical_fov;
    result.near_plane=projection.near_plane;result.far_plane=projection.far_plane;
    if(!impl_->ui_model->definition().empty())result.logical_ui=impl_->ui_model->presentation();
    for(const auto e:impl_->topology->order) {
        const auto& node=impl_->registry.get<Node>(e);
        if(const auto* light=impl_->registry.try_get<Light>(e))append_light(result.lighting,node.id,*light,matrix(node.id));
        if(const auto* environment=impl_->registry.try_get<LightingEnvironment>(e)) {result.lighting.environment=*environment;result.lighting.preview=false;}
        if(const auto* mesh=impl_->registry.try_get<RuntimeMesh>(e);mesh && mesh->visible) {
            result.objects.push_back({node.id,matrix(node.id),mesh->albedo,mesh->mesh,mesh->material,mesh->textures,
                impl_->animations->skin(node.id,matrix),1,mesh->cast_shadows,mesh->receive_shadows,true,
                impl_->topology->definitions.at(node.id).camera_draw});
            auto& object=result.objects.back();
            if(object.camera_draw && object.camera_draw->mode==CameraDrawMode::Viewmodel)object.cast_shadows=false;
        }
    }
    finalize_lighting(result.lighting);
    select_scene_camera_draw(result,projection.viewmodel);
    if(result.viewmodel && *viewmodel!=RuntimeCameraPose{}) {
        // The skin palette already uses the derived hierarchy. This last rigid
        // camera-space presentation moves each complete foreground mesh once.
        const auto delta=multiply(multiply(result.camera_world,local_matrix(viewmodel->position,viewmodel->rotation,{1,1,1})),inverse_affine(result.camera_world));
        for(auto& object:result.viewmodel->objects) {object.world=multiply(delta,object.world);bounded(object.world);}
    }
    return result;
}
