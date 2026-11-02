// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. All ownership/physics calls are owner-thread.
    struct PhysicalOwner {
        std::unique_ptr<RuntimePhysicalRigPoseBinding> binding;
        std::unique_ptr<RuntimePhysicalRigBackend> backend;
        std::uint32_t collision_group=0;
        std::optional<RuntimeControllerHandoffState> handoff;
    };
    struct PhysicalSavedRig {
        RuntimePhysicalRigMembership membership;
        RuntimePhysicalRigPoseState pose;
        std::vector<JPH::BodyID> ids;
        std::uint32_t collision_group=0;
        std::optional<RuntimeControllerHandoffState> handoff;
    };
    struct PhysicalCheckpoint {
        bool enabled=false;
        std::map<std::string,PhysicalSavedRig> rigs;
        std::vector<std::pair<std::string,std::string>> constraint_order;
        JPH::PoimaBodyOwnerState owner;
        std::uint32_t next_collision_group=1;
    };
    std::map<std::string,std::unique_ptr<PhysicalOwner>> physical_owners;
    std::uint32_t physical_next_collision_group=1;
    bool physical_content=false,physical_overlay_busy=false;
    std::vector<PhysicalRigCommand> game_physical_commands;

    RuntimePhysicalRigPoseBinding::World physical_world() const {
        return [this](const std::string& id){return registry.get<Node>(find(id)).world;};
    }
    RuntimePhysicalRigPoseBinding::Local physical_local() const {
        return [this](const std::string& id){return registry.get<Node>(find(id)).local;};
    }
    static std::size_t physical_part_count(const RuntimeEntityDefinition& d) {
        return d.physical_rig ? d.physical_rig->profile.parts.size() : 0;
    }
    static bool definition_has_physical(const RuntimeDefinition& d) {
        for(const auto& e:d.entities)if(e.physical_rig)return true;
        for(const auto& recipe:d.templates)for(const auto& e:recipe.entities)if(e.physical_rig)return true;
        return false;
    }
    void validate_physical_ownership(const RuntimeDefinition& d) const try {
        std::map<std::string,const RuntimeEntityDefinition*> entities;
        for(const auto& e:d.entities)require(entities.emplace(e.id,&e).second,"Duplicate physical runtime entity identity.");
        if(topology)for(const auto& [rig,owner]:physical_owners)if(owner->handoff && topology->identities.contains(rig))
            require(entities.contains(rig)==entities.contains(owner->handoff->actor),"A linked controller and physical rig must retire together.");
        if(std::none_of(d.entities.begin(),d.entities.end(),[](const auto& e){return bool(e.physical_rig);}))return;
        std::map<std::string,std::string> linked;
        // Ordinary attached colliders do not follow the solved hierarchy. Until
        // an explicit attachment-body owner exists, reject the ghost obstacle
        // rather than leave its Jolt pose frozen while its rendered node moves.
        for(const auto& e:d.entities)if(e.collider || e.mesh_collider || e.character) {
            std::size_t depth=0;
            for(auto parent=e.parent;!parent.empty();) {
                const auto found=entities.find(parent);require(found!=entities.end() && ++depth<=entities.size(),"Invalid physical attachment ancestor graph.");
                const auto& ancestor=*found->second;
                require(!ancestor.physical_rig,"Ordinary colliders/controllers cannot descend from PhysicalRig until attachment ownership is supported.");
                parent=ancestor.parent;
            }
        }
        for(const auto& e:d.entities)if(e.physical_rig) {
            poima::physical_rig::validate(*e.physical_rig);
            require(e.animation_rig.has_value() && e.animation_rig->model && !e.rig_node,
                "PhysicalRig requires an ordinary AnimationRig wrapper.");
            require(!e.collider && !e.character && !e.mesh_collider,"PhysicalRig wrapper cannot also own an ordinary collider/controller.");
            require(!e.animation_rig->root_motion && (e.animation_rig->ik.empty() || e.physical_rig->procedural_pose),
                "PhysicalRig requires explicit procedural_pose ownership for manual IK; root motion remains unsupported.");
            if(e.physical_rig->procedural_pose) {
                const bool grounded=*e.physical_rig->procedural_pose==poima::physical_rig::ProceduralPosePolicy::GroundedAnimatedOnly;
                require(!e.camera,"Physical procedural pose requires Camera on a separate child entity, not the rig wrapper.");
                for(const auto& node:d.entities)if(node.rig_node && node.rig_node->rig==e.id)
                    require(!node.camera,"Physical procedural pose requires Camera on a separate child entity, not a model RigNode.");
                std::set<std::uint32_t> mapped;
                for(const auto& part:e.physical_rig->profile.parts)mapped.insert(part.node);
                for(const auto& chain:e.animation_rig->ik) {
                    require(grounded || !chain.grounding,"Manual physical procedural_pose does not coordinate automatic grounding/planting.");
                    require(mapped.contains(chain.root) && mapped.contains(chain.middle) && mapped.contains(chain.end),
                        "Physical procedural_pose requires every IK root/middle/end to be physically mapped.");
                }
                require(!grounded || std::any_of(e.animation_rig->ik.begin(),e.animation_rig->ik.end(),
                    [](const auto& chain){return chain.grounding.has_value();}),
                    "Grounded physical procedural_pose requires at least one configured grounding slot.");
            }
            if(e.physical_rig->controller_handoff) {
                const auto& link=*e.physical_rig->controller_handoff;
                const auto found=entities.find(link.actor);
                require(found!=entities.end() && e.parent==link.actor,"Physical handoff needs its direct controller parent.");
                const auto& actor=*found->second;
                require(!e.physical_rig->procedural_pose || !actor.camera,
                    "Physical procedural pose requires Camera on a separate child entity, not the linked actor.");
                require(actor.parent.empty() && actor.character && actor.character->locomotion && !actor.animation_rig && !actor.rig_node && !actor.physical_rig && !actor.collider && !actor.mesh_collider,
                    "Physical handoff requires an otherwise unowned root virtual controller.");
                require(linked.emplace(link.actor,e.id).second,"A controller can link to only one physical rig.");
            }
            std::size_t depth=0;
            for(auto parent=e.parent;!parent.empty();) {
                const auto found=entities.find(parent);require(found!=entities.end() && ++depth<=entities.size(),"Invalid PhysicalRig ancestor graph.");
                const auto& a=*found->second;
                const bool linked_parent=e.physical_rig->controller_handoff && parent==e.physical_rig->controller_handoff->actor && parent==e.parent;
                require((!a.character || linked_parent) && !a.rig_node && !a.physical_rig && (!a.collider || a.collider->motion==BodyMotion::Static),
                    "Initial PhysicalRig cannot inherit a controller, moving body, animated node or another physical rig; coordinated transfer remains required.");
                parent=a.parent;
            }
        }
        if(linked.empty())return;
        for(const auto& e:d.entities) {
            if(e.animation_rig && e.animation_rig->root_motion)
                require(!linked.contains(e.animation_rig->root_motion->actor),"A handoff controller cannot have another root-motion writer.");
            auto ancestor=e.id;std::size_t depth=0;
            while(!ancestor.empty() && !linked.contains(ancestor)) {
                const auto found=entities.find(ancestor);require(found!=entities.end() && ++depth<=entities.size(),"Invalid linked controller graph.");ancestor=found->second->parent;
            }
            if(ancestor.empty())continue;
            const bool actor=e.id==ancestor;const bool rig=e.id==linked.at(ancestor);
            require(actor || (!e.character && !e.collider && !e.mesh_collider),"A linked actor subtree cannot own additional independent bodies/controllers.");
            require(!e.physical_rig || rig,"A linked actor subtree cannot have another physical rig.");
            require(!e.animation_rig || (!e.animation_rig->root_motion &&
                (e.animation_rig->ik.empty() || (rig && e.physical_rig->procedural_pose))),
                "Linked controller permits manual IK only on its explicitly coordinated own physical rig.");
        }
    } catch(const std::invalid_argument& error) { throw std::runtime_error(std::string("PhysicalRig: ")+error.what()); }
    void validate_physical_bootstrap(const RuntimeDefinition& d,const RuntimePhysicsBootstrap& restored) const try {
        require(restored.ledger.snapshot().sealed,"Cold physical reconstruction requires a sealed exact body-ID ledger.");
        require(restored.next_collision_group>=1,"Cold physical collision-group cursor is invalid.");
        std::set<std::uint32_t> slots,groups;std::size_t ordinary=0,rigs=0;
        const auto claim=[&](JPH::BodyID id) {
            require(restored.ledger.owns(id) && slots.insert(id.GetIndex()).second,"Cold physical body IDs are absent, stale or repeated.");
        };
        for(const auto& e:d.entities) {
            if(e.collider || e.character || e.mesh_collider) {
                const auto found=restored.bodies.find(e.id);require(found!=restored.bodies.end(),"Cold ordinary body ID is missing.");claim(found->second);++ordinary;
            }
            if(e.physical_rig) {
                const auto found=restored.rigs.find(e.id);require(found!=restored.rigs.end(),"Cold physical rig identity is missing.");const auto& row=found->second;
                require(row.parts.size()==e.physical_rig->profile.parts.size() && row.collision_group>=1 &&
                    row.collision_group<restored.next_collision_group && row.collision_group!=JPH::CollisionGroup::cInvalidGroup && groups.insert(row.collision_group).second,
                    "Cold physical part IDs or collision-group identity are invalid.");
                for(auto id:row.parts)claim(id);++rigs;
            }
        }
        require(ordinary==restored.bodies.size() && rigs==restored.rigs.size() && slots.size()==restored.ledger.live(),
            "Cold body ledger must exactly match all ordinary and physical live definitions; no contact-ID remapping.");
    } catch(const std::invalid_argument& error) { throw std::runtime_error(std::string("PhysicalRig: ")+error.what()); }
    void create_physical(const RuntimeEntityDefinition& d,Topology& candidate,const RuntimePhysicsBootstrap* restored=nullptr) try {
        if(!d.physical_rig)return;
        require(!physical_owners.contains(d.id),"Physical rig owner already exists.");
        auto owner=std::make_unique<PhysicalOwner>();
        if(d.physical_rig->controller_handoff) {
            owner->handoff=RuntimeControllerHandoffState{d.physical_rig->controller_handoff->actor,{}};
            if(restored) {
                const auto found=restored->handoffs.find(d.id);require(found!=restored->handoffs.end() && found->second.actor==owner->handoff->actor,"Cold handoff ownership differs from authored link.");
                owner->handoff=found->second;
            }
        }
        owner->binding=std::make_unique<RuntimePhysicalRigPoseBinding>(candidate.live,d.id,d.physical_rig->profile,physical_world(),physical_local(),d.physical_rig->pose_frame,d.physical_rig->max_scale_deviation);
        const auto poses=owner->binding->record_final(tick,physical_world(),physical_local());
        std::span<const JPH::BodyID> ids;
        if(restored) {
            const auto& row=restored->rigs.at(d.id);owner->collision_group=row.collision_group;ids=row.parts;
        }else {
            require(physical_next_collision_group!=JPH::CollisionGroup::cInvalidGroup,"Physical collision-group identities exhausted.");
            owner->collision_group=physical_next_collision_group++;
        }
        owner->backend=std::make_unique<RuntimePhysicalRigBackend>(physics,body_ids,d.physical_rig->profile,poses,1,owner->collision_group,0,ids);
        // Cold mode/poses arrive later from the fully checked saved extension;
        // do not enroll an authored initial mode in front of that restoration.
        if(d.physical_rig->initially_passive && !restored) {
            owner->binding->capture_passive_reference(physical_world(),physical_local());owner->backend->enter_passive(poses);
        }
        for(const auto& part:d.physical_rig->profile.parts) {
            const auto raw=owner->backend->body_id(part.name).GetIndexAndSequenceNumber();
            require(candidate.body_names.emplace(raw,d.id).second && candidate.physical_bodies.insert(raw).second,"Duplicate physical body route.");
        }
        physical_owners.emplace(d.id,std::move(owner));candidate.physical_rigs.push_back(d.id);
        std::sort(candidate.physical_rigs.begin(),candidate.physical_rigs.end());
    } catch(const std::invalid_argument& error) { throw std::runtime_error(std::string("PhysicalRig: ")+error.what()); }
    std::vector<RuntimePhysicalRigPose> physical_native_poses(const PhysicalOwner& owner) const {
        std::vector<RuntimePhysicalRigPose> rows;rows.reserve(owner.backend->definition().parts.size());
        for(const auto& part:owner.backend->definition().parts) {
            JPH::BodyLockRead lock(physics.GetBodyLockInterface(),owner.backend->body_id(part.name));require(lock.Succeeded(),"Cannot publish owned physical body.");
            const auto& b=lock.GetBody();const auto p=b.GetCenterOfMassPosition();const auto q=b.GetRotation();const auto v=b.GetLinearVelocity(),w=b.GetAngularVelocity();
            rows.push_back({part.name,{p.GetX(),p.GetY(),p.GetZ()},{q.GetX(),q.GetY(),q.GetZ(),q.GetW()},{v.GetX(),v.GetY(),v.GetZ()},{w.GetX(),w.GetY(),w.GetZ()}});
        }
        return rows;
    }
    void physical_overlay() try {
        if(physical_overlay_busy || topology->physical_rigs.empty())return;
        physical_overlay_busy=true;
        try {
            // Independent nonnested rigs can prepare one complete publication
            // against the same coherent graph; never perform R whole-world
            // matrix rebuilds or evaluate joint potentials during publication.
            world_matrices();std::size_t count=0;
            for(const auto& id:topology->physical_rigs) {
                const auto found=physical_owners.find(id);if(found!=physical_owners.end() && found->second->backend->mode()==RuntimePhysicalRigMode::Passive)
                    count+=found->second->backend->definition().parts.size()+1+(found->second->handoff?1:0);
            }
            std::vector<RuntimePhysicalRigLocalUpdate> prepared;prepared.reserve(count);
            for(const auto& id:topology->physical_rigs) {
                const auto found=physical_owners.find(id);if(found==physical_owners.end())continue;auto& owner=*found->second;
                if(owner.backend->mode()!=RuntimePhysicalRigMode::Passive)continue;
                const auto bodies=physical_native_poses(owner);
                auto world=physical_world();auto local=physical_local();
                std::map<std::string,Matrix4> candidate_worlds;
                std::optional<RuntimePhysicalActorFollowCandidate> follow;
                if(owner.handoff) {
                    require(owner.handoff->passive.has_value(),"Passive linked rig requires transferred controller ownership.");
                    const auto& actor=owner.handoff->actor;
                    follow=plan_actor_follow(owner.handoff->passive->follow,owner.binding->solved_wrapper_world(bodies),identity_matrix(),
                        registry.get<Node>(find(actor)).local,registry.get<Node>(find(id)).local);
                    candidate_worlds.emplace(actor,follow->actor_world);
                    // Rebuild a coherent pre-overlay source hierarchy against
                    // the candidate parent. Pose binding still checks every
                    // actual sampled scale; only rigid ownership moves here.
                    for(const auto e:topology->hierarchy) {
                        const auto& node=registry.get<Node>(e);
                        if(node.id==actor || !candidate_worlds.contains(node.parent))continue;
                        auto value=multiply(candidate_worlds.at(node.parent),local_matrix(node.local.position,node.local.rotation,node.local.scale));
                        if(const auto* camera=registry.try_get<RuntimeCamera>(e);camera && camera->world_pose!=RuntimeCameraPose{})
                            value=multiply(value,local_matrix(camera->world_pose.position,camera->world_pose.rotation,{1,1,1}));
                        candidate_worlds.emplace(node.id,value);
                    }
                    world=[&,fallback=physical_world()](const std::string& name){const auto found=candidate_worlds.find(name);return found==candidate_worlds.end()?fallback(name):found->second;};
                }
                auto updates=owner.binding->solved_locals(bodies,world,local);
                if(follow) {
                    prepared.push_back({owner.handoff->actor,follow->actor_local});
                    for(auto& update:updates)if(update.entity==id)update.local=follow->wrapper_local;
                }
                for(auto& update:updates)prepared.push_back(std::move(update));
            }
            for(const auto& update:prepared)registry.get<Node>(find(update.entity)).local=update.local;
            if(!prepared.empty())world_matrices();
            physical_overlay_busy=false;
        }catch(...){physical_overlay_busy=false;throw;}
    } catch(const std::invalid_argument& error) { throw std::runtime_error(std::string("PhysicalRig: ")+error.what()); }
    void physical_record_final() try {
        for(const auto& id:topology->physical_rigs) {
            physical_owners.at(id)->binding->record_final(tick,physical_world(),physical_local());
            if(animations->procedural_enabled(id) && !animations->procedural_suspended(id))
                animations->record_procedural_final(id,tick,registry.get<Node>(find(id)).world);
        }
    } catch(const std::invalid_argument& error) { throw std::runtime_error(std::string("PhysicalRig: ")+error.what()); }
    void physical_before_step() try {
        for(const auto& id:topology->physical_rigs)physical_owners.at(id)->backend->before_step(Runtime::fixed_dt);
    } catch(const std::invalid_argument& error) { throw std::runtime_error(std::string("PhysicalRig: ")+error.what()); }
    void physical_ignore(const std::string& id,JPH::IgnoreMultipleBodiesFilter& filter) const {
        for(const auto& [rig,owner]:physical_owners) {
            if(!topology->identities.contains(rig) || (rig!=id && (!owner->handoff || owner->handoff->actor!=id)))continue;
            for(const auto& part:owner->backend->definition().parts)filter.IgnoreBody(owner->backend->body_id(part.name));
        }
    }
    void validate_physical_commands(const std::vector<PhysicalRigCommand>& commands) const {
        require(commands.size()<=64,"At most64 physical rig commands per tick.");std::set<std::string> activations;
        for(const auto& c:commands) {
            require(!c.entity.empty() && c.entity.size()<=256 && (c.activate || c.impulse.has_value()),"Physical command needs a rig and activation or a localized impulse.");
            if(c.activate)require(activations.insert(c.entity).second,"At most one activation per physical rig in a tick.");
            if(c.impulse) {
                require(!c.impulse->part.empty() && c.impulse->part.size()<=64,"Physical impulse part is invalid.");double magnitude=0;
                for(std::size_t k=0;k<3;++k) {
                    const auto p=c.impulse->world_point[k],v=c.impulse->impulse[k];
                    require(std::isfinite(p) && std::abs(p)<=runtime_physical_rig_world_limit && std::isfinite(v) && std::abs(v)<=runtime_physical_rig_impulse_limit,"Physical impulse has invalid finite point/vector bounds.");magnitude+=v*v;
                }
                require(magnitude<=runtime_physical_rig_impulse_limit*runtime_physical_rig_impulse_limit,"Physical impulse magnitude exceeds bounds.");
            }
        }
    }
    template<class Commands> static bool physical_targets(const Commands& commands,const std::string& entity) {
        return std::any_of(commands.begin(),commands.end(),[&](const auto& c){return c.entity==entity;});
    }
    void reject_physical_pose_conflicts(const std::vector<PhysicalRigCommand>& commands,
        const std::vector<AnimationCommand>& bases,const std::vector<AnimationRateCommand>& rates,
        const std::vector<RuntimeIkCommand>& ik,const std::vector<RuntimeRootMotionCommand>& roots,
        const std::vector<RuntimeGroundingCommand>& grounding) const {
        for(const auto& c:commands) {
            const auto found=physical_owners.find(c.entity);
            if(c.activate || (found!=physical_owners.end() && found->second->backend->mode()==RuntimePhysicalRigMode::Passive))
                require(!physical_targets(bases,c.entity) && !physical_targets(rates,c.entity) && !physical_targets(ik,c.entity) && !physical_targets(roots,c.entity) && !physical_targets(grounding,c.entity),
                    "Physical ownership conflicts with a same-tick animation/rate/IK/grounding/root-motion edit.");
        }
        for(const auto& [id,owner]:physical_owners)if(topology->identities.contains(id) && owner->backend->mode()==RuntimePhysicalRigMode::Passive)
            require(!physical_targets(bases,id) && !physical_targets(rates,id) && !physical_targets(ik,id) && !physical_targets(roots,id) && !physical_targets(grounding,id),"Passive rig does not accept animation/rate/IK/grounding/root-motion writes before a recovery policy exists.");
    }
    void apply_physical_commands(const std::vector<PhysicalRigCommand>& caller,const std::vector<PhysicalRigCommand>& gameplay) try {
        require(caller.size()+gameplay.size()<=64,"Caller and gameplay exceed64 physical rig commands.");
        // Preserve literal caller-then-gameplay ordering. Repeated impulse-only
        // commands are distinct applications (including repeated part names);
        // never average/drop them or silently activate/reorder an impulse.
        std::set<std::string> activations;
        const auto validate_activations=[&](const auto& commands){for(const auto& c:commands)if(c.activate)
            require(activations.insert(c.entity).second,"Caller and gameplay requested duplicate physical activation.");};
        validate_activations(caller);validate_activations(gameplay);
        const auto apply=[&](const auto& commands) {for(const auto& command:commands) {
            const auto found=physical_owners.find(command.entity);require(found!=physical_owners.end() && topology->identities.contains(command.entity),"Physical command needs a live PhysicalRig owner.");auto& owner=*found->second;
            if(command.activate) {
                require(owner.backend->mode()==RuntimePhysicalRigMode::Animated && owner.binding->state().current && owner.binding->state().current->tick==tick,"Physical activation requires the current committed animated pose.");
                if(owner.handoff) {
                    require(owner.binding->state().previous.has_value(),"Controller handoff requires a completed committed locomotion tick; birth plus activation in one tick is unsupported.");
                    auto& c=registry.get<Controller>(find(owner.handoff->actor));
                    require(!owner.handoff->passive && !c.character->IsSuppressed(),"Controller ownership was already transferred.");
                    RuntimeControllerSuppression passive;passive.takeover_tick=tick;
                    passive.follow=capture_actor_follow(registry.get<Node>(find(owner.handoff->actor)).world,registry.get<Node>(find(command.entity)).world);
                    passive.support=c.character->CaptureSupport();
                    // History already includes resolved actor/platform motion.
                    // Never add motor velocity a second time.
                    if(animations->procedural_enabled(command.entity))animations->suspend_procedural(command.entity,tick);
                    owner.binding->capture_passive_reference(physical_world(),physical_local());
                    c.character->Suppress(passive.support);passive.proxy=c.character->CaptureProxy();owner.handoff->passive=passive;
                    owner.backend->enter_passive(owner.binding->state().current->bodies);
                }else {
                    owner.binding->capture_passive_reference(physical_world(),physical_local());owner.backend->enter_passive(owner.binding->state().current->bodies);
                }
            }
            if(command.impulse)owner.backend->impulse(command.impulse->part,command.impulse->world_point,command.impulse->impulse);
        }};
        apply(caller);apply(gameplay);
    } catch(const std::invalid_argument& error) { throw std::runtime_error(std::string("PhysicalRig: ")+error.what()); }
    PhysicalCheckpoint checkpoint_physical() const try {
        PhysicalCheckpoint saved;saved.enabled=physical_content;saved.next_collision_group=physical_next_collision_group;
        if(!saved.enabled)return saved;
        require(physics.CapturePoimaOwnerState(saved.owner),"Cannot capture exact physics owner state.");
        std::map<const JPH::Constraint*,std::pair<std::string,std::string>> names;
        for(const auto& id:topology->physical_rigs) {
            const auto& owner=*physical_owners.at(id);PhysicalSavedRig row;
            row.membership=owner.backend->snapshot_membership();row.pose=owner.binding->snapshot();row.collision_group=owner.collision_group;row.handoff=owner.handoff;
            for(const auto& part:owner.backend->definition().parts)row.ids.push_back(owner.backend->body_id(part.name));
            saved.rigs.emplace(id,std::move(row));
            if(owner.backend->mode()==RuntimePhysicalRigMode::Passive)for(const auto& [joint,ref]:owner.backend->constraints())require(names.emplace(ref.GetPtr(),std::pair{id,joint}).second,"Duplicate physical constraint identity.");
        }
        for(const auto& constraint:physics.GetConstraints()) {
            const auto found=names.find(constraint.GetPtr());require(found!=names.end(),"Unowned native constraint in PhysicalRig checkpoint.");saved.constraint_order.push_back(found->second);
        }
        require(saved.constraint_order.size()==names.size(),"Physical checkpoint constraint inventory is incomplete.");return saved;
    } catch(const std::invalid_argument& error) { throw std::runtime_error(std::string("PhysicalRig: ")+error.what()); }
    void restore_physical_before_jolt(const PhysicalCheckpoint& saved) try {
        if(!saved.enabled)return;
        require(saved.rigs.size()==physical_owners.size(),"Physical rollback owner inventory differs.");
        for(const auto& [id,row]:saved.rigs) {
            auto& owner=*physical_owners.at(id);require(owner.collision_group==row.collision_group && row.ids.size()==owner.backend->definition().parts.size(),"Physical rollback allocation/profile differs.");
            for(std::size_t n=0;n<row.ids.size();++n)require(owner.backend->body_id(owner.backend->definition().parts[n].name)==row.ids[n],"Physical rollback must preserve exact body IDs.");
            owner.backend->restore_membership(row.membership);owner.binding->restore(row.pose);owner.handoff=row.handoff;
            if(row.handoff) {
                auto& motor=*registry.get<Controller>(find(row.handoff->actor)).character;
                motor.RestoreSuppression(row.handoff->passive ? std::optional{row.handoff->passive->support} : std::nullopt);
                if(row.handoff->passive)motor.RestoreProxy(row.handoff->passive->proxy);
            }
        }
        std::vector<JPH::Ref<JPH::Constraint>> constraints;constraints.reserve(saved.constraint_order.size());
        for(const auto& [rig,joint]:saved.constraint_order)constraints.push_back(physical_owners.at(rig)->backend->constraint(joint));
        runtime_physical_rig_restore_constraint_order(physics,constraints);physical_next_collision_group=saved.next_collision_group;
    } catch(const std::invalid_argument& error) { throw std::runtime_error(std::string("PhysicalRig: ")+error.what()); }
    void restore_physical_after_jolt(const PhysicalCheckpoint& saved) {
        if(saved.enabled)require(physics.RestorePoimaOwnerState(saved.owner),"Cannot restore exact physics owner active/invalidation state.");
    }
    RuntimePhysicsBootstrap physical_bootstrap() const {
        RuntimePhysicsBootstrap value;value.ledger=body_ids;value.next_collision_group=physical_next_collision_group;
        for(const auto e:topology->order) {
            const auto& id=registry.get<Node>(e).id;
            if(const auto* b=registry.try_get<Body>(e))value.bodies.emplace(id,b->id);
            if(const auto* c=registry.try_get<Controller>(e))value.bodies.emplace(id,c->character->GetBodyID());
        }
        for(const auto& id:topology->physical_rigs) {
            const auto& owner=*physical_owners.at(id);RuntimePhysicalRigIdentity row;row.collision_group=owner.collision_group;
            if(owner.handoff)value.handoffs.emplace(id,*owner.handoff);
            for(const auto& part:owner.backend->definition().parts)row.parts.push_back(owner.backend->body_id(part.name));value.rigs.emplace(id,std::move(row));
        }
        return value;
    }
    PhysicalRigObservation observe_physical(const std::string& id,bool parts,bool joints) const try {
        const auto& owner=*physical_owners.at(id);const auto& definition=owner.backend->definition();
        PhysicalRigObservation result;result.entity=id;result.mode=owner.backend->mode()==RuntimePhysicalRigMode::Passive?PhysicalRigMode::Passive:PhysicalRigMode::Animated;
        result.body_count=static_cast<std::uint32_t>(definition.parts.size());result.joint_count=static_cast<std::uint32_t>(definition.joints.size());
        if(parts)for(const auto& d:definition.parts) {
            PhysicalRigPartObservation row;row.name=d.name;row.node=d.node;row.node_entity=owner.binding->node_entities().at(d.node);const auto id=owner.backend->body_id(d.name);row.body_id=id.GetIndexAndSequenceNumber();
            JPH::BodyLockRead lock(physics.GetBodyLockInterface(),id);require(lock.Succeeded(),"Cannot inspect physical part.");const auto& b=lock.GetBody();const auto* motion=b.GetMotionProperties();
            const auto p=b.GetCenterOfMassPosition();const auto q=b.GetRotation();const auto v=b.GetLinearVelocity(),w=b.GetAngularVelocity();
            row.pose={{p.GetX(),p.GetY(),p.GetZ()},poima::physical_rig::canonical_quaternion({q.GetX(),q.GetY(),q.GetZ(),q.GetW()})};row.linear_velocity={v.GetX(),v.GetY(),v.GetZ()};row.angular_velocity={w.GetX(),w.GetY(),w.GetZ()};row.mass=1.0/static_cast<double>(motion->GetInverseMass());
            const auto inv=motion->GetInverseInertiaDiagonal();const auto rotation=JPH::Mat44::sRotation(motion->GetInertiaRotation());
            const auto tensor=rotation*JPH::Mat44::sScale({1/inv.GetX(),1/inv.GetY(),1/inv.GetZ()})*rotation.Transposed3x3();
            for(JPH::uint r=0;r<3;++r)for(JPH::uint c=0;c<3;++c)row.inertia[r*3+c]=tensor.GetColumn3(c)[r];
            row.enrolled=b.IsInBroadPhase();row.active=b.IsActive();row.sleeping=row.enrolled&&!row.active;row.max_linear_velocity=motion->GetMaxLinearVelocity();row.max_angular_velocity=motion->GetMaxAngularVelocity();row.linear_damping=motion->GetLinearDamping();row.angular_damping=motion->GetAngularDamping();row.friction=b.GetFriction();row.restitution=b.GetRestitution();row.at_linear_velocity_limit=v.Length()>=row.max_linear_velocity*(1-1e-5);row.at_angular_velocity_limit=w.Length()>=row.max_angular_velocity*(1-1e-5);result.parts.push_back(std::move(row));
        }
        if(joints)for(const auto& d:owner.backend->observe(false,true).joints) {
            PhysicalRigJointObservation row;row.name=d.name;row.parent=d.parent;row.child=d.child;row.hinge=d.hinge;row.relative_rotation=d.relative_rotation;row.relative_angular_velocity=d.relative_angular_velocity;row.passive_child_torque=d.passive_child_torque;row.twist_angle=d.twist_angle;row.swing_y_angle=d.swing_y_angle;row.swing_z_angle=d.swing_z_angle;row.twist_min=d.twist_min;row.twist_max=d.twist_max;row.swing_y_limit=d.swing_y_limit;row.swing_z_limit=d.swing_z_limit;row.friction_torque=d.friction_torque;result.joints.push_back(std::move(row));
        }
        return result;
    } catch(const std::invalid_argument& error) { throw std::runtime_error(std::string("PhysicalRig: ")+error.what()); }

    // physical_rig_v1 callback-local budgets; authoritative writes stay in the
    // coordinator's existing ordered caller-then-gameplay transaction.
    std::size_t game_physical_summary_reads=0,game_physical_part_reads=0;
    std::size_t game_physical_write_attempts=0,game_physical_caller_writes=0;
    static bool physical_part_name(std::string_view value) {
        const auto alnum=[](unsigned char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');};
        if(value.empty()||value.size()>64||!alnum(static_cast<unsigned char>(value.front())))return false;
        for(unsigned char c:value)if(!alnum(c)&&c!='_'&&c!='.'&&c!='-')return false;return true;
    }
    std::optional<PhysicalRigPartObservation> observe_physical_part(const std::string& id,const std::string& part) const try {
        const auto found=physical_owners.find(id);if(found==physical_owners.end())return {};
        const auto& owner=*found->second;const auto& definitions=owner.backend->definition().parts;
        const auto definition=std::find_if(definitions.begin(),definitions.end(),[&](const auto& p){return p.name==part;});if(definition==definitions.end())return {};
        const auto native=owner.backend->observe_part(part);PhysicalRigPartObservation row;row.name=native.pose.part;row.node=definition->node;row.node_entity=owner.binding->node_entities().at(row.node);row.body_id=native.body.GetIndexAndSequenceNumber();
        row.pose={native.pose.position,native.pose.rotation};row.linear_velocity=native.pose.linear_velocity;row.angular_velocity=native.pose.angular_velocity;row.mass=native.mass;row.inertia=native.inertia;
        row.enrolled=native.enrolled;row.active=native.active;row.sleeping=native.sleeping;row.max_linear_velocity=native.max_linear_velocity;row.max_angular_velocity=native.max_angular_velocity;
        row.linear_damping=native.linear_damping;row.angular_damping=native.angular_damping;row.friction=native.friction;row.restitution=native.restitution;row.at_linear_velocity_limit=native.at_linear_velocity_limit;row.at_angular_velocity_limit=native.at_angular_velocity_limit;return row;
    } catch(const std::invalid_argument& error) {throw std::runtime_error(std::string("PhysicalRig: ")+error.what());}
    static int32_t POIMA_CALL get_physical_summary(void* context,const PoimaEntityId* entity,PoimaGamePhysicalRigSummaryV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical summary context is absent.");auto& self=*static_cast<Impl*>(context);self.require_tick();require(self.game_physical_summary_reads<256,"Gameplay exceeded256 physical summary read attempts in one Tick.");++self.game_physical_summary_reads;
            require(entity&&output,"Physical summary entity/output is absent.");require(output->version==1&&output->bytes==sizeof(*output)&&output->present==0&&output->mode==0&&output->reserved==0,"Physical summary needs version1/exact48 bytes/zero input payload flags and reserved.");
            const auto id=gameplay_id(*entity);std::optional<PhysicalRigObservation> state;if(!self.pending_entity(id))state=self.owner->physical_rig(id,false,false);
            PoimaGamePhysicalRigSummaryV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);candidate.entity=*entity;
            if(state){candidate.present=1;candidate.mode=state->mode==PhysicalRigMode::Passive?1u:0u;candidate.body_count=state->body_count;candidate.joint_count=state->joint_count;}*output=candidate;
        });
    }
    static int32_t POIMA_CALL get_physical_part(void* context,const PoimaGamePhysicalRigPartRequestV1* source,PoimaGamePhysicalRigPartStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical part context is absent.");auto& self=*static_cast<Impl*>(context);self.require_tick();require(self.game_physical_part_reads<1024,"Gameplay exceeded1024 selective physical part read attempts in one Tick.");++self.game_physical_part_reads;
            require(source&&output,"Physical part request/output is absent.");require(source->version==1&&source->bytes==sizeof(*source)&&source->reserved==0&&source->part&&source->part_bytes>=1&&source->part_bytes<=64,"Invalid physical part V1 request header/name span.");
            require(output->version==1&&output->bytes==sizeof(*output)&&output->present==0&&output->flags==0&&output->reserved==0&&output->reserved_tail==0,"Physical part output needs version1/exact368 bytes and zero input flags/reserved.");
            const std::string_view part(source->part,source->part_bytes);require(physical_part_name(part),"Physical part name must be portable ASCII1..64 bytes.");const auto id=gameplay_id(source->entity);std::optional<PhysicalRigPartObservation> state;
            if(!self.pending_entity(id))state=self.owner->physical_part(id,std::string(part));
            PoimaGamePhysicalRigPartStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);candidate.entity=source->entity;
            if(state) {
                candidate.present=1;candidate.flags=(state->enrolled?1u:0u)|(state->active?2u:0u)|(state->sleeping?4u:0u)|(state->at_linear_velocity_limit?8u:0u)|(state->at_angular_velocity_limit?16u:0u);candidate.node_entity=gameplay_id(state->node_entity);candidate.node=state->node;candidate.body_id=state->body_id;
                require(state->name==part,"Selective physical response part differs.");candidate.part_bytes=source->part_bytes;std::copy(part.begin(),part.end(),candidate.part);
                std::copy(state->pose.position.begin(),state->pose.position.end(),candidate.position);std::copy(state->pose.rotation.begin(),state->pose.rotation.end(),candidate.rotation);std::copy(state->linear_velocity.begin(),state->linear_velocity.end(),candidate.linear_velocity);std::copy(state->angular_velocity.begin(),state->angular_velocity.end(),candidate.angular_velocity);candidate.mass=state->mass;std::copy(state->inertia.begin(),state->inertia.end(),candidate.inertia);
                candidate.max_linear_velocity=state->max_linear_velocity;candidate.max_angular_velocity=state->max_angular_velocity;candidate.linear_damping=state->linear_damping;candidate.angular_damping=state->angular_damping;candidate.friction=state->friction;candidate.restitution=state->restitution;
            }*output=candidate;
        });
    }
    static int32_t POIMA_CALL set_physical_command(void* context,const PoimaGamePhysicalRigCommandV1* source,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical command context is absent.");auto& self=*static_cast<Impl*>(context);self.require_tick();require(self.game_physical_write_attempts<64,"Gameplay exceeded64 physical command attempts in one Tick.");++self.game_physical_write_attempts;
            require(source,"Physical command is absent.");require(source->version==1&&source->bytes==sizeof(*source)&&source->reserved==0&&source->reserved_name==0&&source->flags>=1&&(source->flags&~3u)==0,"Invalid physical command V1 header/flags/reserved.");
            require(self.game_physical_commands.size()+self.game_physical_caller_writes<64,"Caller and gameplay exceed64 physical writes in this Tick.");const auto id=gameplay_id(source->entity);PhysicalRigCommand command;command.entity=id;command.activate=(source->flags&1u)!=0;
            const physical_rig::Configuration* configuration=nullptr;if(const auto* pending=self.pending_entity(id)){require(pending->physical_rig.has_value(),"Pending physical command target has no PhysicalRig.");configuration=&*pending->physical_rig;}
            else {const auto& definition=self.topology->definitions.at(self.registry.get<Node>(self.find(id)).id);require(definition.physical_rig.has_value(),"Physical command target has no PhysicalRig.");configuration=&*definition.physical_rig;}
            if(source->flags&2u) {
                require(source->part&&source->part_bytes>=1&&source->part_bytes<=64,"Physical impulse needs a borrowed1..64-byte part name.");const std::string_view part(source->part,source->part_bytes);require(physical_part_name(part),"Physical impulse part name must be portable ASCII.");
                require(std::any_of(configuration->profile.parts.begin(),configuration->profile.parts.end(),[&](const auto& p){return p.name==part;}),"Physical impulse part is absent from the frozen profile.");PhysicalRigImpulse impulse;impulse.part=std::string(part);std::copy_n(source->world_point,3,impulse.world_point.begin());std::copy_n(source->impulse,3,impulse.impulse.begin());command.impulse=std::move(impulse);
            } else {
                require(source->part==nullptr&&source->part_bytes==0,"Absent physical impulse must not carry a name span.");for(double x:source->world_point)require(x==0&&!std::signbit(x),"Absent impulse point must be canonical zero.");for(double x:source->impulse)require(x==0&&!std::signbit(x),"Absent impulse vector must be canonical zero.");
            }
            self.validate_physical_commands({command});if(command.activate)require(std::none_of(self.game_physical_commands.begin(),self.game_physical_commands.end(),[&](const auto& c){return c.entity==id&&c.activate;}),"Duplicate gameplay physical activation in one Tick.");
            self.game_physical_commands.push_back(std::move(command));
        });
    }
