// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. Assessment is read-only native geometry;
// state/history advancement occurs only at a committed fixed-tick boundary.
    static bool recovery_transform_equal(const RuntimeTransform& a,const RuntimeTransform& b) {
        return detail::handoff_array_same_bits(a.position,b.position) && detail::handoff_array_same_bits(a.rotation,b.rotation) &&
            detail::handoff_array_same_bits(a.scale,b.scale);
    }
    static bool recovery_frame_equal(const RuntimePhysicalRecoveryFrame& a,const RuntimePhysicalRecoveryFrame& b) {
        if(a.tick!=b.tick || !recovery_transform_equal(a.actor,b.actor) || !recovery_transform_equal(a.wrapper,b.wrapper) || a.nodes.size()!=b.nodes.size())return false;
        for(std::size_t i=0;i<a.nodes.size();++i)if(a.nodes[i].entity!=b.nodes[i].entity || !recovery_transform_equal(a.nodes[i].local,b.nodes[i].local))return false;
        return true;
    }
    RuntimePhysicalRecoveryFrame capture_recovery_frame(const std::string& id,const PhysicalOwner& owner) const {
        RuntimePhysicalRecoveryFrame frame;frame.tick=tick;
        frame.actor=registry.get<Node>(find(owner.handoff->actor)).local;frame.wrapper=registry.get<Node>(find(id)).local;
        for(const auto& node:owner.binding->node_entities())frame.nodes.push_back({node,registry.get<Node>(find(node)).local});return frame;
    }
    void validate_recovery_history(const std::string& id,const RuntimePhysicalRecoveryState& state) const {
        const auto& owner=*physical_owners.at(id);const auto& config=*topology->definitions.at(id).physical_rig->recovery;
        require(state.current && state.current->tick==tick && recovery_frame_equal(*state.current,capture_recovery_frame(id,owner)),
            "Recovery current complete pose differs from committed native publication.");
        require(state.stable_ticks<=config.stability_ticks && state.generation==animations->physical_return_generation(id),
            "Recovery count or animation generation differs from its native owner.");
        const auto check_frame=[&](const RuntimePhysicalRecoveryFrame& frame) {
            const auto& nodes=owner.binding->node_entities();require(frame.nodes.size()==nodes.size(),"Recovery must retain every original model node.");
            for(std::size_t i=0;i<nodes.size();++i)require(frame.nodes[i].entity==nodes[i],"Recovery original model node order differs.");
        };
        check_frame(*state.current);if(state.previous)check_frame(*state.previous);
        require(state.assessment && (state.assessment->status==RuntimePhysicalRecoveryStatus::Locomotion)==
            (owner.backend->mode()==RuntimePhysicalRigMode::Animated),"Recovery assessment and actual collision ownership differ.");
        if(state.assessment->status==RuntimePhysicalRecoveryStatus::Ready)require(state.stable_ticks==config.stability_ticks,
            "Recovery ready status lacks its full stability window.");
        if(state.assessment->status==RuntimePhysicalRecoveryStatus::Stabilizing)require(state.stable_ticks<config.stability_ticks,
            "Recovery stabilizing status has already completed its window.");
        const auto ledger=body_ids.snapshot();
        const auto lineage=[&](JPH::BodyID id,bool live) {
            const auto found=std::find_if(ledger.rows.begin(),ledger.rows.end(),[&](const auto& row){return row.index==id.GetIndex();});
            require(found!=ledger.rows.end() && found->generation>=id.GetSequenceNumber() && (!live || body_ids.owns(id)),
                "Recovery support/blocker is outside its actual body-ID lineage.");
        };
        const auto query_lineage=[&](const RuntimePhysicalRecoveryAssessment& a,bool live) {
            if(!a.clearance)return;
            for(const auto& support:a.clearance->supports)if(support)lineage(support->body,live);
            if(a.clearance->blocker)lineage(a.clearance->blocker->body,live);
        };
        require(state.assessment->structure_revision<=structure_revision,"Recovery assessment has a future structure revision.");
        query_lineage(*state.assessment,state.assessment->structure_revision==structure_revision);
        const auto eligible_bounds=[&](const RuntimePhysicalRecoveryAssessment& a) {
            if(a.status!=RuntimePhysicalRecoveryStatus::Ready && a.status!=RuntimePhysicalRecoveryStatus::Stabilizing)return;
            require(a.maximum_linear_speed<=config.max_linear_speed && a.maximum_angular_speed<=config.max_angular_speed &&
                a.minimum_upright_dot>=std::cos(config.max_tilt_degrees*radians) && a.wrapper_distance<=.1 && a.wrapper_angle<=30*radians,
                "Recovery admission exceeds its authored posture/velocity or fixed wrapper envelope.");
            require(a.clearance && a.clearance->clear(),"Recovery admission has no complete geometry.");
            for(const auto& support:a.clearance->supports)require(support && support->static_body && support->normal[1]>=.95 &&
                support->sole_gap>=-.030001 && support->sole_gap<=.050001,"Recovery admission violates its static sole-support envelope.");
        };
        eligible_bounds(*state.assessment);
        if(state.transfer) {
            require(state.transfer->outgoing.actor==owner.handoff->actor && state.transfer->outgoing.passive &&
                state.transfer->outgoing.passive->wrapper_local,"Recovery historical transfer lacks its actual wrapper/actor witness.");
            query_lineage(state.transfer->admission,false);
            eligible_bounds(state.transfer->admission);
            require(state.transfer->admission.structure_revision<=structure_revision,"Recovery transfer has a future structural witness.");
            const auto old_support=state.transfer->outgoing.passive->support.ground_body_id;
            if(old_support!=JPH::BodyID::cInvalidBodyID)lineage(JPH::BodyID(old_support),false);
        }
        const auto sidecar=animations->save_physical_return_state(id,tick);
        require(sidecar.has_value()==state.transfer.has_value(),"Recovery transfer and complete animation sidecar membership differ.");
        if(sidecar) {
            const auto returned=nlohmann::json::parse(*sidecar);const auto& transfer=*state.transfer;
            require(returned.at("generation")==transfer.generation && returned.at("start_tick")==transfer.tick &&
                returned.at("source_tick").get<std::uint64_t>()+1==transfer.tick && returned.at("duration_ticks")==config.blend_ticks,
                "Recovery animation phase/duration differs from the exact physical transfer and authored profile.");
            const auto& canceled=returned.at("canceled_tick");
            if(owner.backend->mode()==RuntimePhysicalRigMode::Animated)require(canceled.is_null(),"Animated recovery retains a canceled physical phase.");
            else require(owner.handoff->passive && canceled==owner.handoff->passive->takeover_tick,
                "Physical recovery cancellation differs from its actual new takeover.");
            // At the first returned boundary the original current source is
            // still retained independently in the physical history. Later the
            // sidecar remains bounded historical input, not authenticated history.
            if(tick==transfer.tick) {
                require(state.previous && state.previous->tick+1==tick,"First recovery boundary lacks its actual prior source frame.");
                const auto pose_json=[](const RuntimeTransform& t){return nlohmann::json{{"position",t.position},{"rotation",t.rotation},{"scale",t.scale}};};
                require(returned.at("current_wrapper").dump()==pose_json(state.previous->wrapper).dump(),"Returned wrapper differs from the actual original source.");
                const auto& poses=returned.at("current");require(poses.size()==state.previous->nodes.size(),"Returned source omits original nodes.");
                for(std::size_t i=0;i<state.previous->nodes.size();++i)
                    require(poses[i].dump()==pose_json(state.previous->nodes[i].local).dump(),"Returned node differs from the actual original source.");
            }
        }
        if(state.assessment->structure_revision==structure_revision) {
            const auto actual=assess_recovery(id,owner);const auto& saved=*state.assessment;
            const auto expected_status=saved.status==RuntimePhysicalRecoveryStatus::Stabilizing?RuntimePhysicalRecoveryStatus::Ready:saved.status;
            require(actual.status==expected_status,"Recovery saved status differs from current native admission.");
            const auto near=[](double a,double b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=1e-10*std::max({1.0,std::abs(a),std::abs(b)});};
            require(near(actual.maximum_linear_speed,saved.maximum_linear_speed)&&near(actual.maximum_angular_speed,saved.maximum_angular_speed)&&
                near(actual.minimum_upright_dot,saved.minimum_upright_dot)&&near(actual.wrapper_distance,saved.wrapper_distance)&&near(actual.wrapper_angle,saved.wrapper_angle),
                "Recovery saved measurements differ from current complete native parts/wrapper.");
            require(actual.clearance.has_value()==saved.clearance.has_value(),"Recovery saved/current geometry membership differs.");
            if(actual.clearance) {
                require(actual.clearance->refusal==saved.clearance->refusal && actual.clearance->support_index==saved.clearance->support_index,
                    "Recovery saved/current geometry refusal differs.");
                for(std::size_t i=0;i<2;++i) {
                    const auto& a=actual.clearance->supports[i];const auto& b=saved.clearance->supports[i];
                    require(a.has_value()==b.has_value(),"Recovery current support membership differs.");if(!a)continue;
                    require(a->body==b->body&&a->subshape==b->subshape&&a->user_data==b->user_data&&a->static_body==b->static_body&&near(a->sole_gap,b->sole_gap),
                        "Recovery current support identity/type/gap differs.");
                    for(std::size_t k=0;k<3;++k)require(near(a->position[k],b->position[k])&&near(a->normal[k],b->normal[k]),"Recovery current support geometry differs.");
                }
                const auto& a=actual.clearance->blocker;const auto& b=saved.clearance->blocker;
                require(a.has_value()==b.has_value(),"Recovery current blocker membership differs.");
                if(a) {
                    require(a->body==b->body&&a->subshape==b->subshape&&a->normal_valid==b->normal_valid&&near(a->penetration,b->penetration),"Recovery current blocker identity/depth differs.");
                    for(std::size_t k=0;k<3;++k)require(near(a->point_on_capsule[k],b->point_on_capsule[k])&&near(a->point_on_obstacle[k],b->point_on_obstacle[k])&&near(a->normal[k],b->normal[k]),
                        "Recovery current blocker geometry differs.");
                }
            }
        }
    }
    static bool recovery_same_support(const RuntimePhysicalRecoveryAssessment& a,const RuntimePhysicalRecoveryAssessment& b) {
        if(!a.clearance || !b.clearance || !a.clearance->clear() || !b.clearance->clear())return false;
        for(std::size_t i=0;i<2;++i)if(a.clearance->supports[i]->body!=b.clearance->supports[i]->body)return false;
        return true;
    }
    RuntimePhysicalRecoveryAssessment assess_recovery(const std::string& id,const PhysicalOwner& owner) const {
        using Status=RuntimePhysicalRecoveryStatus;
        RuntimePhysicalRecoveryAssessment result;result.tick=tick;result.structure_revision=structure_revision;result.actor=registry.get<Node>(find(owner.handoff->actor)).local;
        if(owner.backend->mode()==RuntimePhysicalRigMode::Animated){result.status=Status::Locomotion;return result;}
        const auto& config=*topology->definitions.at(id).physical_rig->recovery;
        const auto& state=*owner.recovery;
        if(!state.current || !state.previous || state.current->tick!=tick || state.previous->tick+1!=tick)return result;
        const auto& controller=registry.get<Controller>(find(owner.handoff->actor));
        if(controller.crouched || controller.requested_crouch || controller.stand_blocked){result.status=Status::NotStanding;return result;}
        const auto native=physical_native_poses(owner);
        for(const auto& body:native) {
            result.maximum_linear_speed=std::max(result.maximum_linear_speed,std::hypot(body.linear_velocity[0],body.linear_velocity[1],body.linear_velocity[2]));
            result.maximum_angular_speed=std::max(result.maximum_angular_speed,std::hypot(body.angular_velocity[0],body.angular_velocity[1],body.angular_velocity[2]));
        }
        const auto part=[&](const std::string& name)->const RuntimePhysicalRigPose& {
            const auto found=std::find_if(native.begin(),native.end(),[&](const auto& p){return p.part==name;});
            require(found!=native.end(),"Recovery references an absent physical part.");return *found;
        };
        for(const auto& upright:config.uprights) {
            const auto up=physical_rig::rotate(part(upright.part).rotation,upright.up_axis);
            result.minimum_upright_dot=std::min(result.minimum_upright_dot,up[1]);
        }
        const auto& wrapper=registry.get<Node>(find(id));
        const auto& p=wrapper.local.position;const auto& initial=wrapper.initial.position;
        result.wrapper_distance=std::hypot(p[0]-initial[0],p[1]-initial[1],p[2]-initial[2]);
        const auto relative=physical_rig::multiply(physical_rig::conjugate(wrapper.initial.rotation),wrapper.local.rotation);
        result.wrapper_angle=2*std::atan2(std::hypot(relative[0],relative[1],relative[2]),std::abs(relative[3]));
        if(result.minimum_upright_dot<std::cos(config.max_tilt_degrees*radians)){result.status=Status::NotUpright;return result;}
        if(result.maximum_linear_speed>config.max_linear_speed || result.maximum_angular_speed>config.max_angular_speed){result.status=Status::Moving;return result;}
        // A narrow near-standing return, including heading. This does not turn
        // a backwards/translated full body into a stand-up or root-correction policy.
        if(result.wrapper_distance>.1 || result.wrapper_angle>30*radians){result.status=Status::WrapperDisplaced;return result;}
        std::vector<JPH::BodyID> outgoing;for(const auto& p:owner.backend->definition().parts)outgoing.push_back(owner.backend->body_id(p.name));
        RuntimePhysicalRecoveryQuery query;query.standing_shape=controller.standing_shape.GetPtr();
        query.position=JPH::RVec3(result.actor.position[0],result.actor.position[1],result.actor.position[2]);
        query.rotation=controller.character->GetRotation();query.outgoing_parts=outgoing;query.suppressed_controller=controller.character->GetBodyID();
        query.probe_up=.03;query.probe_down=.05;query.minimum_up_dot=.95;
        for(std::size_t i=0;i<2;++i) {
            const auto& sole=config.supports[i];const auto& b=part(sole.part);
            const auto point=physical_rig::compose({b.position,b.rotation},{sole.point,{0,0,0,1}}).position;
            query.soles[i]=JPH::RVec3(point[0],point[1],point[2]);
            if((query.soles[i]-query.position).LengthSq()>16){result.status=Status::SoleOutOfRange;return result;}
        }
        result.clearance=runtime_physical_recovery_query(physics,query);
        using Refusal=RuntimePhysicalRecoveryRefusal;
        switch(result.clearance->refusal) {
        case Refusal::None:result.status=Status::Ready;break;
        case Refusal::MissingSupport:result.status=Status::MissingSupport;break;
        case Refusal::MovingSupport:result.status=Status::MovingSupport;break;
        case Refusal::InvalidSupport:result.status=Status::InvalidSupport;break;
        case Refusal::ProxyBlocked:result.status=Status::ProxyBlocked;break;
        case Refusal::MotorBlocked:result.status=Status::MotorBlocked;break;
        case Refusal::BudgetExceeded:result.status=Status::QueryBudget;break;
        case Refusal::InvalidNativeResult:result.status=Status::InvalidNativeResult;break;
        }
        return result;
    }
    void record_recovery_final(const std::string& id,PhysicalOwner& owner) {
        if(!owner.recovery)return;
        auto& state=*owner.recovery;auto frame=capture_recovery_frame(id,owner);
        if(state.current && state.current->tick==tick) {
            require(recovery_frame_equal(*state.current,frame),"Repeated recovery frame changed its complete committed locals.");return;
        }
        require(!state.current || state.current->tick+1==tick,"Recovery full-pose history has a tick gap.");
        state.previous=std::move(state.current);state.current=std::move(frame);
        auto current=assess_recovery(id,owner);
        if(current.status==RuntimePhysicalRecoveryStatus::Ready) {
            const auto required=topology->definitions.at(id).physical_rig->recovery->stability_ticks;
            state.stable_ticks=state.assessment && state.assessment->tick+1==tick && recovery_same_support(*state.assessment,current)
                ? std::min(required,state.stable_ticks+1) : 1;
            if(state.stable_ticks<required)current.status=RuntimePhysicalRecoveryStatus::Stabilizing;
        }else state.stable_ticks=0;
        state.assessment=std::move(current);
    }
    void request_physical_recovery(const std::string& id,PhysicalOwner& owner) {
        using Status=RuntimePhysicalRecoveryStatus;
        require(owner.recovery && owner.handoff,"Physical return requires an authored recovery profile and controller ownership.");
        auto& state=*owner.recovery;const auto& config=*topology->definitions.at(id).physical_rig->recovery;
        auto assessment=assess_recovery(id,owner);
        // Re-query AFTER same-tick structural changes. A previous ready receipt
        // never grants permission to overwrite a replaced support or obstacle.
        if(assessment.status==Status::Ready && (state.stable_ticks<config.stability_ticks || !state.assessment ||
            !recovery_same_support(*state.assessment,assessment)))assessment.status=Status::Stabilizing;
        state.last_attempt_tick=tick+1;state.last_attempt_result=assessment.status;
        if(assessment.status!=Status::Ready)return;
        require(state.generation<physical_state_json::max_tick && state.current && state.previous && owner.handoff->passive,
            "Physical return requires complete owned history and an available generation.");
        const auto& current=*state.current;const auto& previous=*state.previous;
        physical_rig::Vec3 incoming;
        for(std::size_t k=0;k<3;++k)incoming[k]=(current.actor.position[k]-previous.actor.position[k])*60;
        if(std::hypot(incoming[0],incoming[1],incoming[2])>2) {state.last_attempt_result=Status::Moving;return;}
        const JPH::Vec3 velocity(static_cast<float>(incoming[0]),static_cast<float>(incoming[1]),static_cast<float>(incoming[2]));
        RuntimePhysicalRecoveryTransfer transfer;transfer.tick=tick+1;transfer.generation=state.generation+1;
        transfer.motor_velocity={velocity.GetX(),velocity.GetY(),velocity.GetZ()};transfer.outgoing=*owner.handoff;transfer.admission=assessment;
        animations->resume_from_physics(id,tick,config.blend_ticks,current.nodes,previous.nodes,current.wrapper,previous.wrapper);
        // Clear both target attribution and applied velocity controls before
        // removing articulation. The independent source clock may keep running,
        // but it no longer writes a physics-owned pose in Animated mode.
        for(const auto& joint:owner.backend->definition().joints)if(joint.drive) {
            const auto drive=owner.backend->drive(joint.name);require(drive.has_value(),"Recovery drive is absent.");
            owner.backend->set_drive({joint.name,drive->target_rotation,0});
            if(owner.animation)owner.animation->select(joint.name,RuntimePhysicalAnimation::Source::Manual);
        }
        owner.backend->enter_animated();
        auto& motor=*registry.get<Controller>(find(owner.handoff->actor)).character;
        const auto& p=current.actor.position;
        motor.ResumeAfterPhysical(JPH::RVec3(p[0],p[1],p[2]),velocity,
            {assessment.clearance->supports[0]->body,assessment.clearance->supports[1]->body});
        owner.handoff->passive.reset();
        auto binding=owner.binding->snapshot();binding.root_bone_to_wrapper.reset();owner.binding->restore(std::move(binding));
        state.generation=transfer.generation;state.transfer=std::move(transfer);state.stable_ticks=0;state.last_attempt_result=Status::Returned;
        require(animations->physical_return_generation(id)==state.generation,"Physical and animation recovery generations differ.");
    }
