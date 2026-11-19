// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl.
    struct AttachmentGraph {
        std::map<std::string,RuntimeAttachmentBinding> bindings;
        std::vector<entt::entity> order;
    };
    std::shared_ptr<const AttachmentGraph> attachment_graph;

    static const RuntimeAttachmentBinding* attachment(const AttachmentGraph* graph,const std::string& id) {
        if(!graph)return nullptr;
        const auto found=graph->bindings.find(id);
        return found==graph->bindings.end() ? nullptr : &found->second;
    }
    const std::vector<entt::entity>& transform_hierarchy() const {
        return attachment_graph && !attachment_graph->bindings.empty() ? attachment_graph->order : topology->hierarchy;
    }
    static const std::string& transform_parent(const Node& node,const RuntimeAttachmentBinding* binding) {
        return binding ? binding->anchor : node.parent;
    }
    static Matrix4 transform_local(const Node& node,const RuntimeAttachmentBinding* binding) {
        return binding ? binding->offset : local_matrix(node.local.position,node.local.rotation,node.local.scale);
    }
    static void validate_attachment_world(const Matrix4& value) {
        for(const double x:value)require(std::isfinite(x) && std::abs(x)<=1e12,"Attachment matrix exceeds the finite hierarchy bounds.");
    }
    static void validate_attachment_offset(const Matrix4& value) {
        validate_attachment_world(value);
        require(value[3]==0 && value[7]==0 && value[11]==0 && value[15]==1,"Attachment offset must be affine.");
        (void)inverse_affine(value);
    }
    void validate_attachment_graph(AttachmentGraph& graph,const Topology& scene) {
        require(graph.bindings.size()<=max_runtime_attachments,"At most 64 items can own attachment bindings.");
        if(graph.bindings.empty()) {graph.order.clear();return;}
        for(const auto& [id,binding]:graph.bindings) {
            require(scene.identities.contains(id),"Attachment item must remain live. Reset it before removing its instance.");
            require(binding.anchor.empty() || scene.identities.contains(binding.anchor),
                "Attachment anchor must remain live. Transfer or reset its items before removal.");
            validate_attachment_offset(binding.offset);
        }
        // Check the authored subtree even if a nested item currently follows a
        // different anchor. Returning it to its authored parent stays safe.
        std::set<std::string> moved;
        for(const auto e:scene.hierarchy) {
            const auto& node=registry.get<Node>(e);
            if(!graph.bindings.contains(node.id) && !moved.contains(node.parent))continue;
            moved.insert(node.id);
            const auto& d=scene.definitions.at(node.id);
            require(!d.collider && !d.mesh_collider && !d.character && !d.rig_node && !d.animation_rig &&
                !d.physical_rig && !d.skinned_mesh && !d.hit_region && !d.camera && !d.light && !d.environment &&
                !d.acoustics && !d.emitter,"Attachment items need an ordinary transform/rigid-mesh subtree without independent native owners.");
        }
        graph.order=sorted_hierarchy(scene,&graph);
    }
    void publish_attachment_transforms(std::shared_ptr<AttachmentGraph> graph,const std::set<std::string>& keep={}) {
        validate_attachment_graph(*graph,*topology);
        const auto& order=graph->bindings.empty() ? topology->hierarchy : graph->order;
        std::map<std::string,Matrix4> prepared;
        for(const auto e:order) {
            const auto& node=registry.get<Node>(e);
            const auto item=graph->bindings.find(node.id);
            auto* binding=item==graph->bindings.end() ? nullptr : &item->second;
            const auto& parent=transform_parent(node,binding);
            const auto* anchor=parent.empty() ? nullptr : &prepared.at(parent);
            if(binding && keep.contains(node.id)) {
                binding->offset=anchor ? multiply(inverse_affine(*anchor),node.world) : node.world;
                validate_attachment_offset(binding->offset);
            }
            auto world=transform_local(node,binding);
            if(anchor)world=multiply(*anchor,world);
            if(const auto* camera=registry.try_get<RuntimeCamera>(e);camera && camera->world_pose!=RuntimeCameraPose{})
                world=multiply(world,local_matrix(camera->world_pose.position,camera->world_pose.rotation,{1,1,1}));
            validate_attachment_world(world);
            if(keep.contains(node.id))for(std::size_t i=0;i<world.size();++i)
                require(std::abs(world[i]-node.world[i])<=1e-8*std::max(1.,std::abs(node.world[i])),
                    "Attachment anchor could not preserve the current item matrix.");
            prepared.emplace(node.id,world);
        }
        // All validation and allocation is complete. There is no partial graph
        // when a later command or matrix fails, and the old cache keys are stale.
        for(const auto e:order) {
            auto& node=registry.get<Node>(e);
            node.world=prepared.at(node.id);node.hierarchy_cache.invalidate();node.world_recomputed=true;
        }
        attachment_graph=graph->bindings.empty() ? nullptr : std::move(graph);
    }
    void change_attachments(std::uint64_t expected,const std::vector<RuntimeAttachmentCommand>& commands) {
        require(expected==structure_revision && structure_revision<9007199254740991ULL,
            "Attachment structure revision is stale or exhausted.");
        require(!commands.empty() && commands.size()<=max_runtime_attachments,"Submit 1..64 attachment commands.");
        auto graph=std::make_shared<AttachmentGraph>();
        if(attachment_graph)graph->bindings=attachment_graph->bindings;
        std::set<std::string> seen,keep;
        for(const auto& command:commands) {
            (void)find(command.entity);
            require(seen.insert(command.entity).second,"Duplicate attachment item in one transaction.");
            switch(command.mode) {
                case RuntimeAttachmentMode::Reset:
                    require(command.anchor.empty() && !command.offset,"Reset accepts only the item identity.");
                    require(graph->bindings.erase(command.entity)==1,"Reset requires an active attachment item.");
                    break;
                case RuntimeAttachmentMode::KeepWorld:
                    require(!command.offset,"Keep-world computes its own attachment offset.");
                    graph->bindings[command.entity]={command.anchor,identity_matrix()};
                    keep.insert(command.entity);
                    break;
                case RuntimeAttachmentMode::Snap: {
                    require(!command.anchor.empty() && command.offset.has_value(),"Snap requires an anchor and complete transform.");
                    const auto& offset=*command.offset;
                    for(const auto scale:offset.scale)require(scale>0 && scale<=1e9,"Attachment snap scale is outside finite positive bounds.");
                    for(const auto x:offset.position)require(std::isfinite(x) && std::abs(x)<=1e9,"Attachment snap position is outside finite bounds.");
                    const double norm=std::hypot(std::hypot(offset.rotation[0],offset.rotation[1]),std::hypot(offset.rotation[2],offset.rotation[3]));
                    require(std::abs(norm*norm-1)<=1e-6,"Attachment snap rotation must be a normalized XYZW quaternion.");
                    graph->bindings[command.entity]={command.anchor,local_matrix(offset.position,offset.rotation,offset.scale)};
                    break;
                }
                default: throw std::runtime_error("Unknown attachment transfer mode.");
            }
        }
        publish_attachment_transforms(std::move(graph),keep);
        ++structure_revision;
    }
