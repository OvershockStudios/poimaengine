// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl.
    struct AttachmentGraph {
        std::map<std::string,RuntimeAttachmentBinding> bindings;
        std::vector<entt::entity> order;
        std::set<std::string> world_draws;
    };
    std::shared_ptr<const AttachmentGraph> attachment_graph;
    using AttachmentOutcomes=std::map<std::string,RuntimeAttachmentOutcome>;
    std::shared_ptr<const AttachmentOutcomes> attachment_outcomes;
    struct StagedPickupClearance {
        std::string rig;
        std::uint32_t slot;
        std::array<std::string,3> joints;
        std::array<double,2> radii;
        ChainPoints start;
        std::optional<RuntimeIkCommand> fallback;
        bool blend=false;
        bool steer=false;
    };
    static constexpr unsigned pickup_blend_samples=4;
    static constexpr unsigned pickup_steer_samples=2;
    struct PickupClearanceRejected {
        RuntimeAttachmentReachBlocked detail;
    };

    static const RuntimeAttachmentBinding* attachment(const AttachmentGraph* graph,const std::string& id) {
        if(!graph)return nullptr;
        const auto found=graph->bindings.find(id);
        return found==graph->bindings.end() ? nullptr : &found->second;
    }
    static bool has_released_attachment(const AttachmentGraph* graph) {
        return graph && std::any_of(graph->bindings.begin(),graph->bindings.end(),
            [](const auto& row){return row.second.released;});
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
        graph.world_draws.clear();
        if(graph.bindings.empty()) {graph.order.clear();return;}
        std::set<std::string> release_bodies;
        for(const auto& [id,binding]:graph.bindings) {
            require(scene.identities.contains(id),"Attachment item must remain live. Reset it before removing its instance.");
            require(binding.anchor.empty() || scene.identities.contains(binding.anchor),
                "Attachment anchor must remain live. Transfer or reset its items before removal.");
            validate_attachment_offset(binding.offset);
            if(binding.released) {
                const auto spawned=scene.spawned.find(binding.anchor);
                require(spawned!=scene.spawned.end() && release_bodies.insert(binding.anchor).second &&
                    is_release_template(spawn_template(spawned->second.template_id)),
                    "A released item requires its own live dynamic collision proxy.");
            }
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
        if(!release_bodies.empty())for(const auto e:graph.order) {
            const auto& node=registry.get<Node>(e);
            const auto* binding=attachment(&graph,node.id);
            if(binding ? binding->released : graph.world_draws.contains(node.parent))
                graph.world_draws.insert(node.id);
        }
    }
    static bool is_release_template(const RuntimeSpawnTemplate& recipe) {
        return recipe.entities.empty() && !recipe.mesh && recipe.components.empty() &&
            recipe.collider && recipe.collider->motion==BodyMotion::Dynamic;
    }
    using AttachmentContacts=std::map<std::string,const RuntimeAttachmentContact*>;

    static Matrix4 attachment_contact_frame(const Matrix4& base,const RuntimeAttachmentFrame& offset) {
        for(const double x:offset.position)
            require(std::isfinite(x) && std::abs(x)<=1e9,"Attachment contact offset position must be finite and bounded.");
        const auto& q=offset.rotation;
        const double norm=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));
        require(std::abs(norm*norm-1)<=1e-6,"Attachment contact offset rotation must be a normalized XYZW quaternion.");
        auto frame=multiply(base,local_matrix(offset.position,offset.rotation,{1,1,1}));
        validate_attachment_world(frame);
        return frame;
    }
    static std::optional<RuntimeAttachmentContactReport> check_attachment_contact(const Node& node,const RuntimeAttachmentBinding& binding,
        const Matrix4& anchor,const RuntimeAttachmentContact& contact) {
        require(contact.position_limit_m>=0 && contact.position_limit_m<=1e9 &&
            contact.rotation_limit_deg>=0 && contact.rotation_limit_deg<=180,"Invalid attachment contact limits.");
        const auto item_frame=attachment_contact_frame(node.world,contact.item);
        const auto anchor_frame=attachment_contact_frame(anchor,contact.anchor);
        const auto item_axes=pose_frames::axes(item_frame),anchor_axes=pose_frames::axes(anchor_frame);
        const auto position=std::hypot(item_frame[12]-anchor_frame[12],item_frame[13]-anchor_frame[13],item_frame[14]-anchor_frame[14]);
        std::optional<double> rotation;
        if(item_axes.basis==pose_frames::Basis::regular && anchor_axes.basis==pose_frames::Basis::regular)
            rotation=pose_frames::rotation_error(item_axes,anchor_axes);
        if(position>contact.position_limit_m || !rotation || *rotation>contact.rotation_limit_deg)
            return RuntimeAttachmentContactReport{node.id,binding.anchor,position,contact.position_limit_m,contact.rotation_limit_deg,rotation};
        return {};
    }
    bool attachment_contains(const AttachmentGraph* graph,const std::string& root,const std::string& id) const {
        for(auto current=id;!current.empty();) {
            if(current==root)return true;
            const auto& node=registry.get<Node>(find(current));
            current=transform_parent(node,attachment(graph,current));
        }
        return false;
    }
    void check_attachment_surfaces(const AttachmentGraph& candidate,const AttachmentContacts& contacts,
        const std::map<std::string,Matrix4>& prepared,std::size_t& checks) const {
        std::map<std::string,SceneObject> drawables;
        const auto mesh=[&](const std::string& id)->const SceneObject& {
            if(const auto cached=drawables.find(id);cached!=drawables.end())return cached->second;
            const auto entity=find(id);
            const auto* geometry=registry.try_get<RuntimeMesh>(entity);
            require(geometry && geometry->mesh && geometry->visible,"Select a visible indexed mesh drawable.");
            return drawables.emplace(id,drawable(entity,*geometry,&prepared)).first->second;
        };
        std::optional<RuntimeAttachmentSurfaceReport> missed;
        for(const auto& [item,contact]:contacts) {
            for(std::size_t i=0;i<contact->surface.size();++i) {
                const auto& probe=contact->surface[i];
                require(probe.distance_limit_m && *probe.distance_limit_m>=0 && *probe.distance_limit_m<=1e9,
                    "Attachment surface probes require a limit within 0..1e9 metres.");
                require(attachment_contains(attachment_graph.get(),item,probe.target) &&
                    attachment_contains(&candidate,item,probe.target),
                    "Attachment surface target must stay inside the item's effective subtree.");
                require(!attachment_contains(attachment_graph.get(),item,probe.source) &&
                    !attachment_contains(&candidate,item,probe.source),
                    "Attachment surface source must stay outside the item's effective subtree.");
                const auto& source=mesh(probe.source);
                const auto& target=mesh(probe.target);
                const auto result=surface_measure::measure(probe,source,target,checks);
                if(result.distance_m>*probe.distance_limit_m && !missed)
                    missed=RuntimeAttachmentSurfaceReport{item,candidate.bindings.at(item).anchor,
                        probe.source,probe.target,i,result,*probe.distance_limit_m};
            }
        }
        if(missed)throw RuntimeAttachmentSurfaceRejected(std::move(*missed));
    }
    std::vector<std::string> pickup_clearance_exclusions(const std::string& item,const std::string& rig) const {
        const auto* dropped=attachment(attachment_graph.get(),item);
        require(dropped && dropped->released,"Pickup clearance requires a live release proxy.");
        std::vector<std::string> ignore{dropped->anchor};
        for(auto id=rig;!id.empty();id=registry.get<Node>(find(id)).parent) {
            if(registry.any_of<Body,Controller>(find(id))) {
                if(id!=dropped->anchor)ignore.push_back(id);
                break;
            }
        }
        return ignore;
    }
    void check_pickup_clearance(const std::string& item,const std::string& holder,const StagedPickupClearance& guard) const {
        require(animations->manual_ik_nodes(guard.rig,guard.slot)==guard.joints &&
            attachment_contains(attachment_graph.get(),guard.joints.back(),holder),
            "Pickup clearance must match the receiving hand's live manual chain.");
        const auto ignore=pickup_clearance_exclusions(item,guard.rig);
        if(auto hit=sweep_chain(guard.start,read_chain_points(guard.joints),guard.radii,ignore))
            throw PickupClearanceRejected{{std::move(hit->hit.entity),hit->segment,hit->interval,
                hit->hit.fraction,hit->hit.penetration_depth}};
    }
    static double pickup_pose_step(const ChainPoints& prior,const ChainPoints& candidate) {
        double step=0;
        for(std::size_t joint=1;joint<candidate.size();++joint) {
            std::array<double,3> delta{};
            for(std::size_t axis=0;axis<delta.size();++axis)
                delta[axis]=(candidate[joint][axis]-candidate[0][axis])-(prior[joint][axis]-prior[0][axis]);
            step=std::max(step,std::hypot(delta[0],delta[1],delta[2]));
        }
        return step;
    }
    static std::optional<std::array<double,3>> pickup_steering_offset(const ChainPoints& points,
        const RuntimeIkClearanceBlocker& blocker,double radius) {
        if(!blocker.hit.normal)return std::nullopt;
        double arm_length=0,lever=0;
        for(std::size_t i=0;i<3;++i) {
            const auto axis=points[2][i]-points[0][i];
            arm_length+=axis*axis;
            lever+=(blocker.hit.point_on_query[i]-points[0][i])*axis;
        }
        if(arm_length<1e-12)return std::nullopt;
        // A contact near the shoulder responds less to a wrist displacement.
        // Bound this estimate and let the following native casts decide.
        const auto margin=std::max(.001,.1*radius);
        const auto shift=(blocker.hit.penetration_depth+margin)/std::clamp(lever/arm_length,.25,1.);
        const auto metres=std::min(shift,.14*std::sqrt(arm_length));
        auto offset=*blocker.hit.normal;
        for(auto& coordinate:offset)coordinate*=metres;
        return offset;
    }
    RuntimeAttachmentReachBlocked::Pose resolve_pickup_pose(const std::string& item,const StagedPickupClearance& guard) {
        using Pose=RuntimeAttachmentReachBlocked::Pose;
        const auto ignore=pickup_clearance_exclusions(item,guard.rig);
        const auto& wrapper=registry.get<Node>(find(guard.rig)).world;
        const auto clear_pose=[&](const RuntimeIkCommand& control) -> std::optional<RuntimeAnimations::IkChainPreview> {
            auto preview=animations->preview_ik_chain(control,tick,wrapper);
            if(sweep_chain(preview.candidate,preview.candidate,guard.radii,ignore))return std::nullopt;
            return preview;
        };
        const auto commit=[&](const RuntimeIkCommand& control,RuntimeAnimations::IkChainPreview preview,Pose pose) {
            // Publish only the three validated, disjoint local joints. Other
            // rig clocks and cached physics/grounding samples already committed.
            animations->publish_pickup_fallback(control,std::move(preview.state));
            for(std::size_t i=0;i<preview.entities.size();++i)
                registry.get<Node>(find(preview.entities[i])).local=preview.locals[i];
            world_matrices();
            return pose;
        };
        const auto& previous=guard.fallback.value();
        auto prior_pose=animations->preview_ik_chain(previous,tick,wrapper);
        const auto prior_hit=sweep_chain(prior_pose.candidate,prior_pose.candidate,guard.radii,ignore);
        if(!prior_hit)return commit(previous,std::move(prior_pose),Pose::Previous);
        // Disabled controls already sampled the unmodified incoming pose.
        if(!previous.enabled)return Pose::Unresolved;
        auto neutral=previous;
        neutral.weight=0;
        if(!guard.blend)neutral.end_orientation.reset();
        neutral.enabled=neutral.end_orientation.has_value();
        if(neutral.enabled && previous.weight==0)return Pose::Unresolved;
        auto best=clear_pose(neutral);
        auto control=neutral;
        if(guard.blend && previous.weight>0) {
            double lower=0,upper=previous.weight;
            for(unsigned i=0;i<pickup_blend_samples;++i) {
                auto probe=previous;
                // If both endpoints are blocked, still look for a clear
                // interior solution. Every published candidate is queried.
                probe.weight=best ? std::midpoint(lower,upper)
                    : previous.weight*(pickup_blend_samples-i)/double(pickup_blend_samples+1);
                if(auto pose=clear_pose(probe)) {
                    lower=probe.weight;
                    control=probe;
                    best=std::move(pose);
                } else upper=probe.weight;
            }
        }
        auto kind=control.enabled ? Pose::Blended : Pose::Animation;
        if(guard.steer && previous.weight>0 && !animations->ik_state(guard.rig,guard.slot)->target_node) {
            if(auto offset=pickup_steering_offset(prior_pose.candidate,*prior_hit,guard.radii[prior_hit->segment-1])) {
                if(previous.space==IkSpace::Rig) {
                    const auto inverse=inverse_affine(wrapper);
                    const auto world_offset=*offset;
                    for(std::size_t i=0;i<3;++i)
                        (*offset)[i]=inverse[i]*world_offset[0]+inverse[4+i]*world_offset[1]+inverse[8+i]*world_offset[2];
                }
                auto best_step=best ? pickup_pose_step(guard.start,best->candidate) : std::numeric_limits<double>::infinity();
                for(unsigned i=0;i<pickup_steer_samples;++i) {
                    auto probe=previous;
                    const double multiplier=1.+.5*i;
                    for(std::size_t axis=0;axis<3;++axis)probe.target[axis]+=(*offset)[axis]*multiplier;
                    if(std::any_of(probe.target.begin(),probe.target.end(),[](double x){return !std::isfinite(x) || std::abs(x)>1e6;}))
                        continue;
                    if(auto pose=clear_pose(probe)) {
                        const auto step=pickup_pose_step(guard.start,pose->candidate);
                        if(step<best_step) {
                            best_step=step;
                            control=probe;
                            best=std::move(pose);
                            kind=Pose::Steered;
                        }
                    }
                }
            }
        }
        if(!best)return Pose::Unresolved;
        return commit(control,std::move(*best),kind);
    }
    void publish_attachment_transforms(std::shared_ptr<AttachmentGraph> graph,const std::set<std::string>& keep={},
        const AttachmentContacts& contacts={},std::size_t* tick_checks=nullptr,bool recover_miss=false,
        const StagedPickupClearance* clearance=nullptr) {
        validate_attachment_graph(*graph,*topology);
        const auto& order=graph->bindings.empty() ? topology->hierarchy : graph->order;
        std::map<std::string,Matrix4> prepared;
        std::optional<RuntimeAttachmentContactReport> frame_miss;
        for(const auto e:order) {
            const auto& node=registry.get<Node>(e);
            const auto item=graph->bindings.find(node.id);
            auto* binding=item==graph->bindings.end() ? nullptr : &item->second;
            const auto& parent=transform_parent(node,binding);
            const auto* anchor=parent.empty() ? nullptr : &prepared.at(parent);
            if(binding && keep.contains(node.id)) {
                if(const auto found=contacts.find(node.id);found!=contacts.end()) {
                    frame_miss=check_attachment_contact(node,*binding,*anchor,*found->second);
                    if(frame_miss && !recover_miss)
                        throw RuntimeAttachmentContactRejected(std::move(*frame_miss));
                }
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
        std::size_t own_checks=0;
        try {check_attachment_surfaces(*graph,contacts,prepared,tick_checks ? *tick_checks : own_checks);}
        catch(const RuntimeAttachmentSurfaceRejected&) {if(!frame_miss)throw;}
        if(frame_miss)throw RuntimeAttachmentContactRejected(std::move(*frame_miss));
        if(clearance) {
            const auto& item=contacts.begin()->first;
            check_pickup_clearance(item,graph->bindings.at(item).anchor,*clearance);
        }
        // All validation and allocation is complete. There is no partial graph
        // when a later command or matrix fails, and the old cache keys are stale.
        for(const auto e:order) {
            auto& node=registry.get<Node>(e);
            node.world=prepared.at(node.id);node.hierarchy_cache.invalidate();node.world_recomputed=true;
        }
        attachment_graph=graph->bindings.empty() ? nullptr : std::move(graph);
    }
    RuntimeStructureResult change_attachments(std::uint64_t expected,std::span<const RuntimeAttachmentCommand> commands,
        std::size_t* tick_checks=nullptr,bool recover_miss=false,const StagedPickupClearance* clearance=nullptr) {
        require(expected==structure_revision && structure_revision<9007199254740991ULL,
            "Attachment structure revision is stale or exhausted.");
        require(!commands.empty() && commands.size()<=max_runtime_attachments,"Submit 1..64 attachment commands.");
        require(!clearance || (commands.size()==1 && commands.front().mode==RuntimeAttachmentMode::Pickup),
            "Arm clearance applies to one staged pickup.");
        auto graph=std::make_shared<AttachmentGraph>();
        if(attachment_graph)graph->bindings=attachment_graph->bindings;
        std::set<std::string> seen,keep;
        std::vector<std::string> retired_bodies;
        std::map<std::string,Matrix4> pickup_poses;
        AttachmentContacts contacts;
        std::size_t surface_pairs=0;
        for(const auto& command:commands) {
            const auto e=find(command.entity);
            require(seen.insert(command.entity).second,"Duplicate attachment item in one transaction.");
            const bool pickup=command.mode==RuntimeAttachmentMode::Pickup;
            if(pickup)require(command.contact && !command.contact->surface.empty(),"Pickup requires sampled surface contact.");
            if(command.contact) {
                require((pickup || command.mode==RuntimeAttachmentMode::KeepWorld) && !command.anchor.empty(),
                    "Attachment contact requires a live keep-world destination anchor.");
                const auto count=command.contact->surface.size();
                require(count<=max_attachment_surface_probes && count<=max_surface_pairs-surface_pairs,
                    "Attachment contact supports up to 8 surface probes per item and 32 per batch.");
                surface_pairs+=count;
                contacts.emplace(command.entity,&*command.contact);
            }
            switch(command.mode) {
                case RuntimeAttachmentMode::Reset:
                    require(command.anchor.empty() && !command.offset,"Reset accepts only the item identity.");
                    require(graph->bindings.erase(command.entity)==1,"Reset requires an active attachment item.");
                    break;
                case RuntimeAttachmentMode::KeepWorld:
                case RuntimeAttachmentMode::Pickup:
                    require(!command.offset,"Keep-world computes its own attachment offset.");
                    if(pickup) {
                        const auto* dropped=attachment(attachment_graph.get(),command.entity);
                        require(dropped && dropped->released,"Pickup requires an active released item.");
                        require(command.anchor!=dropped->anchor,"Pickup cannot use the body it retires as a holder.");
                        retired_bodies.push_back(dropped->anchor);
                        pickup_poses.emplace(command.entity,registry.get<Node>(e).world);
                    }
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
        publish_attachment_transforms(std::move(graph),keep,contacts,tick_checks,recover_miss,clearance);
        if(retired_bodies.empty())return {++structure_revision,{}};
        // The enclosing owner transaction retains the original graph and live
        // physics until topology retirement and strong-reference checks succeed.
        auto result=publish_structure(expected,{},retired_bodies);
        for(const auto& [id,before]:pickup_poses) {
            const auto& after=registry.get<Node>(find(id)).world;
            for(std::size_t i=0;i<after.size();++i)
                require(std::abs(after[i]-before[i])<=1e-8*std::max(1.,std::abs(before[i])),
                    "Pickup could not preserve the current item matrix after retiring its body.");
        }
        return result;
    }
