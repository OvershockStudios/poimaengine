// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. Base local poses remain owned by animation,
// native look and stance; effects are evaluated once, without accumulation.
    static void validate_camera_pose(const RuntimeCameraPose& pose) {
        double norm=0;
        for(double v:pose.position)require(std::isfinite(v) && std::abs(v)<=10,"Camera pose translation must be finite and within10 metres per axis.");
        for(double v:pose.rotation) {require(std::isfinite(v) && std::abs(v)<=1.00001,"Camera pose rotation must be finite and normalized.");norm+=v*v;}
        require(std::abs(norm-1)<=1e-5,"Camera pose quaternion must be normalized.");
    }
    Matrix4 base_camera_world(const std::string& id,bool allow_pending=false) const {
        std::vector<RuntimeTransform> ancestry;std::string current=id;
        while(!current.empty()) {
            require(ancestry.size()<10000,"Base camera hierarchy exceeds10000 nodes.");
            if(allow_pending)if(const auto* pending=pending_entity(current)) {ancestry.push_back(pending->transform);current=pending->parent;continue;}
            const auto& node=registry.get<Node>(find(current));ancestry.push_back(node.local);current=node.parent;
        }
        auto result=identity_matrix();bool first=true;
        for(auto i=ancestry.rbegin();i!=ancestry.rend();++i) {
            const auto local=local_matrix(i->position,i->rotation,i->scale);
            result=first ? local : multiply(result,local);first=false;
        }
        for(double v:result)require(std::isfinite(v) && std::abs(v)<=1e12,"Base camera matrix exceeds supported range.");
        return result;
    }
    std::optional<RuntimeCameraPoseState> camera_pose(const std::string& id) const {
        const auto* camera=registry.try_get<RuntimeCamera>(find(id));if(!camera)return std::nullopt;
        return RuntimeCameraPoseState{id,camera->world_pose,camera->viewmodel ? std::optional(camera->viewmodel_pose) : std::nullopt,base_camera_world(id)};
    }
    void validate_camera_physical_subtree(const std::string& id) const {
        std::set<std::string> subtree;
        for(const auto e:topology->hierarchy) {
            const auto& node=registry.get<Node>(e);
            if(node.id==id || subtree.contains(node.parent)) {
                subtree.insert(node.id);
                require(!registry.any_of<Body,Controller>(e),"World camera effects cannot move physical descendants.");
            }
        }
        std::map<std::string,const RuntimeEntityDefinition*> reserved;
        for(const auto& birth:game_spawns)if(!birth.canceled)for(const auto& entity:birth.entities)reserved.emplace(entity.id,&entity);
        for(const auto& [name,entity]:reserved)if(entity->collider || entity->mesh_collider || entity->character) {
            auto current=name;std::size_t depth=0;
            while(!current.empty()) {
                require(current!=id,"World camera effects cannot move reserved physical descendants.");
                require(++depth<=10000,"Reserved camera hierarchy exceeds10000 nodes.");
                const auto pending=reserved.find(current);
                if(pending!=reserved.end())current=pending->second->parent;
                else current=registry.get<Node>(find(current)).parent;
            }
        }
    }
    void validate_camera_pose_command(const RuntimeCameraPoseCommand& command,bool allow_pending=false) const {
        require(command.world || command.viewmodel,"Camera pose command must select a channel.");
        if(command.world)validate_camera_pose(*command.world);if(command.viewmodel)validate_camera_pose(*command.viewmodel);
        if(command.world)validate_camera_physical_subtree(command.entity);
        if(allow_pending)if(const auto* pending=pending_entity(command.entity)) {
            require(pending->camera.has_value(),"Reserved pose target requires a Camera.");
            require(!pending->rig_node && !pending->collider && !pending->mesh_collider && !pending->character,"Camera pose target cannot be animation-owned or physical.");
            require(!command.viewmodel || pending->camera->viewmodel.has_value(),"Camera pose cannot add a foreground channel.");return;
        }
        const auto e=find(command.entity);const auto* camera=registry.try_get<RuntimeCamera>(e);
        require(camera,"Pose target requires a live Camera.");
        require(!registry.any_of<RuntimeRigNode,Body,Controller>(e),"Camera pose target cannot be animation-owned or physical.");
        require(!command.viewmodel || camera->viewmodel.has_value(),"Camera pose cannot add a foreground channel.");
    }
    void apply_camera_poses(const std::vector<RuntimeCameraPoseCommand>& commands) {
        require(commands.size()<=64,"At most64 camera pose commands per Tick.");std::set<std::string> targets;
        for(const auto& command:commands) {require(targets.insert(command.entity).second,"Duplicate camera pose target.");validate_camera_pose_command(command);}
        for(const auto& command:commands) {
            auto& camera=registry.get<RuntimeCamera>(find(command.entity));
            if(command.world)camera.world_pose=*command.world;if(command.viewmodel)camera.viewmodel_pose=*command.viewmodel;
        }
    }
