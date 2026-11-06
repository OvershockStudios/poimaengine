// SPDX-License-Identifier: Apache-2.0
// Included after the legacy save helpers. This separately versioned extension
// never changes a legacy payload field or installs untrusted recorder bytes.
namespace {
bool save_controller_handoff_definition(const RuntimeDefinition& definition) {
    const auto linked=[](const auto& entity){return entity.physical_rig && entity.physical_rig->controller_handoff.has_value();};
    if(std::any_of(definition.entities.begin(),definition.entities.end(),linked))return true;
    for(const auto& recipe:definition.templates)if(std::any_of(recipe.entities.begin(),recipe.entities.end(),linked))return true;
    return false;
}
bool save_procedural_pose_definition(const RuntimeDefinition& definition) {
    const auto enabled=[](const auto& entity){return entity.physical_rig && entity.physical_rig->procedural_pose.has_value();};
    if(std::any_of(definition.entities.begin(),definition.entities.end(),enabled))return true;
    for(const auto& recipe:definition.templates)if(std::any_of(recipe.entities.begin(),recipe.entities.end(),enabled))return true;
    return false;
}
bool save_grounded_procedural_definition(const RuntimeDefinition& definition) {
    const auto enabled=[](const auto& entity){return entity.physical_rig &&
        entity.physical_rig->procedural_pose==poima::physical_rig::ProceduralPosePolicy::GroundedAnimatedOnly;};
    if(std::any_of(definition.entities.begin(),definition.entities.end(),enabled))return true;
    for(const auto& recipe:definition.templates)if(std::any_of(recipe.entities.begin(),recipe.entities.end(),enabled))return true;
    return false;
}
bool save_physical_drive_definition(const RuntimeDefinition& definition) {
    const auto enabled=[](const auto& entity){return entity.physical_rig &&
        std::any_of(entity.physical_rig->profile.joints.begin(),entity.physical_rig->profile.joints.end(),
            [](const auto& joint){return joint.drive.has_value();});};
    if(std::any_of(definition.entities.begin(),definition.entities.end(),enabled))return true;
    for(const auto& recipe:definition.templates)if(std::any_of(recipe.entities.begin(),recipe.entities.end(),enabled))return true;
    return false;
}
unsigned save_physical_ownership_version(const RuntimeDefinition& definition) {
    return save_grounded_procedural_definition(definition)?4u:
        (save_procedural_pose_definition(definition)?3u:(save_controller_handoff_definition(definition)?2u:1u));
}
void save_handoff_support_lineage(const RuntimeControllerSupportState& support,const RuntimeBodyIds& ledger) {
    if(support.ground_body_id==JPH::BodyID::cInvalidBodyID)return;
    const JPH::BodyID id(support.ground_body_id);const auto snapshot=ledger.snapshot();
    const auto found=std::find_if(snapshot.rows.begin(),snapshot.rows.end(),[&](const auto& row){return row.index==id.GetIndex();});
    require(found!=snapshot.rows.end() && found->generation>=id.GetSequenceNumber(),
        "Frozen controller support is outside the complete used body-ID lineage.");
}
template<class State>void save_handoff_witnesses(const State& s,const std::string& rig,const RuntimeControllerHandoffState& handoff) {
    const auto& c=s.registry.template get<Controller>(s.find(handoff.actor));
    require(c.character->IsVirtual() && c.character->IsSuppressed()==handoff.passive.has_value(),
        "Physical handoff and controller ownership phase differ.");
    require(s.physics.GetBodyInterface().IsAdded(c.character->GetBodyID())==!handoff.passive.has_value(),
        "Physical handoff and actual capsule broadphase membership differ.");
    s.validate_procedural_supports(rig);
    if(!handoff.passive)return;
    require(c.character->CaptureProxy()==handoff.passive->proxy && c.character->CaptureSupport()==handoff.passive->support,
        "Suppressed proxy/support differs from its complete named witness.");
    save_handoff_support_lineage(handoff.passive->support,s.body_ids);
    if(s.animations->procedural_enabled(rig)) {
        // The logical motor remains at its last animated pose while its
        // presentation actor follows physics. The opt-in graph excludes camera
        // sidecar writers on the actor/wrapper/model nodes; separate camera
        // children are unaffected. Reconstruct the capture frame independently
        // of the historical animation document and the current corpse pose.
        const auto p=c.character->GetPosition();const auto q=c.character->GetRotation();
        const auto& actor=s.registry.template get<Node>(s.find(handoff.actor)).initial;
        const auto& wrapper=s.registry.template get<Node>(s.find(rig)).initial;
        const auto expected=multiply(local_matrix({p.GetX(),p.GetY(),p.GetZ()},
            {q.GetX(),q.GetY(),q.GetZ(),q.GetW()},actor.scale),
            local_matrix(wrapper.position,wrapper.rotation,wrapper.scale));
        const auto captured=s.animations->procedural_wrapper(rig);
        require(captured && detail::handoff_array_same_bits(*captured,expected),
            "Historical procedural wrapper differs from the frozen controller and authored local frame.");
    }
}
template<class State,class Checkpoint>void save_physical_final_history(const State& s,const Checkpoint& checkpoint) {
    require(checkpoint.rigs.size()==s.topology->physical_rigs.size(),"Physical save history owner membership differs.");
    for(const auto& [id,row]:checkpoint.rigs) {
        const auto& owner=*s.physical_owners.at(id);
        require(row.pose.current && row.pose.current->tick==s.tick,"Physical save requires the current committed final-pose history.");
        const bool passive=row.membership.mode!=RuntimePhysicalRigMode::Animated;
        require(passive==row.pose.root_bone_to_wrapper.has_value(),"Physical mode and captured root-reference membership differ.");
        const auto& configured=s.topology->definitions.at(id).physical_rig->controller_handoff;
        require(row.handoff.has_value()==configured.has_value() &&
            (!row.handoff || row.handoff->actor==configured->actor),"Physical handoff witness differs from frozen authored ownership.");
        if(row.handoff) {
            require(row.handoff->passive.has_value()==passive,"Linked controller phase differs from physical mode.");
            require(!row.handoff->passive || row.handoff->passive->takeover_tick<=s.tick,"Controller takeover tick is in the future.");
            save_handoff_witnesses(s,id,*row.handoff);
        }
        if(s.animations->procedural_enabled(id)) {
            require(s.animations->procedural_suspended(id)==passive,"Procedural pose ownership differs from physical mode.");
            require(s.animations->procedural_tick(id)==std::optional{passive ? row.handoff->passive->takeover_tick : s.tick},
                "Procedural pose witness differs from the last committed animated boundary.");
        }
        // A copy validates chronological finite-difference velocities and the
        // actual current wrapper/bone COM poses. Never rewrite saved history or
        // substitute instantaneous Jolt velocities for finite differences.
        auto verification=*owner.binding;verification.restore(row.pose);
        (void)verification.record_final(s.tick,s.physical_world(),s.physical_local());
        if(passive) {
            verification.capture_passive_reference(s.physical_world(),s.physical_local());
            const auto& actual=*verification.state().root_bone_to_wrapper;const auto& expected=*row.pose.root_bone_to_wrapper;
            require(save_near(actual.position,expected.position) && save_near(actual.rotation,expected.rotation),
                "Physical saved root reference differs from the actual final-pose calibration.");
        }
    }
}
void save_physical_lineage(const SaveJson& physics,const RuntimeBodyIds& ledger) {
    const auto snapshot=ledger.snapshot();std::array<const RuntimeBodyIds::SnapshotRow*,RuntimeBodyIds::capacity> used{};
    for(const auto& row:snapshot.rows)used[row.index]=&row;
    const auto historical=[&](const SaveJson& value,bool live=false) {
        const auto raw=static_cast<std::uint32_t>(save_integer(value,std::numeric_limits<std::uint32_t>::max()));const JPH::BodyID id(raw);
        require(!id.IsInvalid() && !(raw&JPH::BodyID::cBroadPhaseBit) && id.GetIndex()<RuntimeBodyIds::capacity,"Physical cache identity has invalid body bits/slot.");
        const auto* row=used[id.GetIndex()];require(row && row->generation>=id.GetSequenceNumber() && (!live || ledger.owns(id)),
            "Physical cache/body identity is outside the complete used ledger history.");
    };
    for(const auto& body:physics.at("bodies"))historical(body.at("id"),true);
    const auto key=[&](const SaveJson& value){historical(value.at("body1"));historical(value.at("body2"));};
    for(const auto& pair:physics.at("pairs")) {
        historical(pair.at("body_a"));historical(pair.at("body_b"));
        for(const auto& manifold:pair.at("manifolds"))key(manifold.at("key"));
    }
    for(const auto& value:physics.at("ccd"))key(value);
}
template<class State>SaveJson save_physical_extension(const State& s) try {
    auto checkpoint=s.checkpoint_physical();require(checkpoint.enabled,"Physical save extension requires frozen physical content.");
    save_physical_final_history(s,checkpoint);
    const auto bootstrap=s.physical_bootstrap();
    JPH::PoimaBodyOwnerState owner;auto physics=capture_runtime_physics_state_joined(s.physics,owner);
    require(physical_bootstrap_json::owner_json(owner)==physical_bootstrap_json::owner_json(checkpoint.owner),
        "Physical save capture changed its quiescent owner state.");
    physical_bootstrap_json::validate_owner_history(owner,bootstrap.ledger);save_physical_lineage(physics,bootstrap.ledger);
    const bool handoffs=save_controller_handoff_definition(s.topology->live);
    const bool procedural=save_procedural_pose_definition(s.topology->live);
    const bool grounded=save_grounded_procedural_definition(s.topology->live);SaveJson rigs=SaveJson::array();
    for(const auto& [id,row]:checkpoint.rigs) {
        SaveJson rig={{"entity",id},{"membership",physical_state_json::membership_json(row.membership)},
            {"pose",physical_state_json::pose_state_json(row.pose)}};
        if(handoffs)rig["handoff"]=row.handoff?physical_state_json::handoff_state_json(*row.handoff):SaveJson(nullptr);
        if(procedural) {
            const auto terminal=s.animations->procedural_terminal(id);
            rig["procedural_terminal"]=terminal ? SaveJson::parse(*terminal) : SaveJson(nullptr);
        }
        if(grounded) {
            const auto& configured=*s.topology->definitions.at(id).physical_rig;
            const bool captures=configured.procedural_pose==poima::physical_rig::ProceduralPosePolicy::GroundedAnimatedOnly;
            const auto witness=captures?s.animations->procedural_witness(id):std::optional<std::string>{};
            require(witness.has_value()==captures,"Grounded physical save requires its complete committed capture.");
            rig["procedural_capture"]=witness?SaveJson::parse(*witness):SaveJson(nullptr);
            if(captures && row.membership.enrolled)
                require(rig.at("procedural_capture").dump()==rig.at("procedural_terminal").dump(),"Passive grounded capture differs from its terminal.");
        }
        rigs.push_back(std::move(rig));
    }
    return {{"version",std::uint32_t(save_physical_drive_definition(s.topology->live)?5:(grounded?4:(procedural?3:(handoffs?2:1))))},
        {"bootstrap",physical_bootstrap_json::bootstrap_json(bootstrap,s.topology->live)},
        {"rigs",std::move(rigs)},{"constraint_order",physical_state_json::constraint_order_json(checkpoint.constraint_order)},
        {"owner",physical_bootstrap_json::owner_json(owner)},{"physics",std::move(physics)}};
} catch(const std::invalid_argument& error) {throw std::runtime_error(std::string("Physical snapshot: ")+error.what());}
void save_physical_extension_fields(const SaveJson& extension,const RuntimeDefinition& definition) {
    save_fields(extension,{"version","bootstrap","rigs","constraint_order","owner","physics"});
    const auto expected=save_physical_drive_definition(definition)?5u:save_physical_ownership_version(definition);
    require(extension.at("version").is_number_unsigned() && extension.at("version")==expected,"Physical snapshot extension version differs from frozen ownership policy.");
    require(extension.at("rigs").is_array() && extension.at("rigs").size()<=RuntimeBodyIds::capacity,"Physical snapshot rig population exceeds its bound.");
}
RuntimePhysicsBootstrap save_physical_parse_bootstrap(const SaveJson& extension,const RuntimeDefinition& definition,std::uint64_t tick) try {
    auto result=physical_bootstrap_json::parse_bootstrap(extension.at("bootstrap"),definition);
    result.snapshot_tick=tick;const auto ownership=save_physical_ownership_version(definition);
    if(ownership>=2u) {
        std::map<std::string,const RuntimeEntityDefinition*> expected;std::set<std::string> actors;
        for(const auto& entity:definition.entities)if(entity.physical_rig)expected.emplace(entity.id,&entity);
        std::string previous;
        for(const auto& row:extension.at("rigs")) {
            if(ownership==4u)save_fields(row,{"entity","membership","pose","handoff","procedural_terminal","procedural_capture"});
            else if(ownership==3u)save_fields(row,{"entity","membership","pose","handoff","procedural_terminal"});
            else save_fields(row,{"entity","membership","pose","handoff"});
            const auto rig=save_text(row.at("entity"));
            require(previous.empty() || previous<rig,"Physical handoff bootstrap rows must be sorted and unique.");previous=rig;
            const auto found=expected.find(rig);require(found!=expected.end(),"Physical handoff bootstrap names an unknown rig.");
            const auto& configured=found->second->physical_rig->controller_handoff;
            require(row.at("handoff").is_null()==!configured,"Physical handoff bootstrap differs from frozen linkage.");
            if(ownership>=3u) {
                const auto& terminal=row.at("procedural_terminal");
                const bool suspended=found->second->physical_rig->procedural_pose && configured && !row.at("handoff").at("passive").is_null();
                require(terminal.is_null()==!suspended,"Procedural terminal membership differs from frozen phase/policy.");
                if(suspended) {
                    require(terminal.is_object(),"Procedural terminal must be a typed versioned object.");
                    require(result.procedural_terminals.emplace(rig,terminal.dump()).second,"Duplicate procedural terminal owner.");
                }
            }
            if(ownership==4u) {
                const auto& capture=row.at("procedural_capture");
                const bool grounded=found->second->physical_rig->procedural_pose==poima::physical_rig::ProceduralPosePolicy::GroundedAnimatedOnly;
                require(capture.is_null()==!grounded,"Grounded capture membership differs from frozen policy.");
                if(grounded) {
                    require(capture.is_object(),"Grounded capture must be a complete typed witness.");
                    if(!row.at("procedural_terminal").is_null())
                        require(capture.dump()==row.at("procedural_terminal").dump(),"Passive grounded capture and terminal differ.");
                    else require(result.procedural_captures.emplace(rig,capture.dump()).second,"Duplicate live grounded capture.");
                }
            }
            if(!configured)continue;
            auto handoff=physical_state_json::parse_handoff_state(row.at("handoff"));
            require(handoff.actor==configured->actor && actors.insert(handoff.actor).second,"Physical handoff bootstrap actor differs or is multiply owned.");
            const auto actor=result.bodies.find(handoff.actor);require(actor!=result.bodies.end(),"Physical handoff actor has no exact ordinary body identity.");
            if(handoff.passive) {
                require(handoff.passive->proxy.body_id==actor->second.GetIndexAndSequenceNumber(),"Suppressed proxy ID differs from the exact cold bootstrap.");
                save_handoff_support_lineage(handoff.passive->support,result.ledger);
            }
            require(result.handoffs.emplace(rig,std::move(handoff)).second,"Duplicate physical handoff bootstrap owner.");
        }
        require(extension.at("rigs").size()==expected.size(),"Physical handoff bootstrap omits a live rig.");
    }
    return result;
} catch(const std::invalid_argument& error) {throw std::runtime_error(std::string("Physical snapshot: ")+error.what());}
template<class State>auto save_physical_checkpoint(const State& s,const SaveJson& extension,
    const RuntimePhysicsBootstrap& bootstrap,std::uint64_t tick) try {
    const auto ownership=save_physical_ownership_version(s.topology->live);
    typename State::PhysicalCheckpoint checkpoint;checkpoint.enabled=true;checkpoint.next_collision_group=bootstrap.next_collision_group;
    checkpoint.owner=physical_bootstrap_json::parse_owner(extension.at("owner"));
    physical_bootstrap_json::validate_owner_history(checkpoint.owner,bootstrap.ledger);
    const auto& rigs=extension.at("rigs");require(rigs.size()==s.topology->physical_rigs.size(),"Saved physical rig membership differs from the complete restored graph.");
    std::string previous;std::set<std::pair<std::string,std::string>> expected_constraints;
    for(const auto& value:rigs) {
        if(ownership==4u)save_fields(value,{"entity","membership","pose","handoff","procedural_terminal","procedural_capture"});
        else if(ownership==3u)save_fields(value,{"entity","membership","pose","handoff","procedural_terminal"});
        else if(ownership==2u)save_fields(value,{"entity","membership","pose","handoff"});
        else save_fields(value,{"entity","membership","pose"});
        const auto id=save_text(value.at("entity"));
        require(previous.empty() || previous<id,"Physical save owners must be sorted and unique.");previous=id;
        const auto found=s.physical_owners.find(id);require(found!=s.physical_owners.end(),"Physical save names an absent owner.");const auto& owner=*found->second;
        typename State::PhysicalSavedRig row;row.membership=physical_state_json::parse_membership(value.at("membership"));
        row.pose=physical_state_json::parse_pose_state(value.at("pose"));
        const auto& profile=owner.backend->definition();const auto& identity=bootstrap.rigs.at(id);
        row.ids=identity.parts;row.collision_group=identity.collision_group;
        require(row.membership.poses.size()==profile.parts.size() && row.membership.warm_starts.size()==profile.joints.size(),"Physical save part/joint membership differs from its frozen profile.");
        require((row.membership.mode!=RuntimePhysicalRigMode::Animated)==row.membership.enrolled,"Physical save mode and enrollment differ.");
        const auto& configured=*s.topology->definitions.at(id).physical_rig;
        if(configured.controller_handoff) {
            const auto found=bootstrap.handoffs.find(id);require(found!=bootstrap.handoffs.end(),"Physical handoff saved owner is missing.");
            row.handoff=found->second;
            require(row.handoff->actor==configured.controller_handoff->actor &&
                row.handoff->passive.has_value()==row.membership.enrolled,"Physical saved controller phase/owner differs from rig mode.");
            require(!row.handoff->passive || row.handoff->passive->takeover_tick<=tick,"Controller saved takeover tick is in the future.");
        }
        if(configured.procedural_pose && row.membership.enrolled)
            require(s.animations->procedural_suspended(id) && s.animations->procedural_tick(id)==std::optional{row.handoff->passive->takeover_tick},
                "Historical procedural tick differs from controller takeover.");
        require(!configured.initially_passive || row.membership.mode!=RuntimePhysicalRigMode::Animated,"An initially passive rig cannot return to Animated without a recovery policy.");
        for(std::size_t n=0;n<profile.parts.size();++n)require(row.membership.poses[n].part==profile.parts[n].name,"Physical saved part order differs from its profile.");
        for(std::size_t n=0;n<profile.joints.size();++n) {
            const auto& joint=profile.joints[n];const auto& warm=row.membership.warm_starts[n];
            require(warm.name==joint.name && warm.hinge==std::holds_alternative<poima::physical_rig::Hinge>(joint.limits),"Physical saved warm-start joint/type differs from its frozen profile.");
            if(row.membership.enrolled)expected_constraints.emplace(id,joint.name);
        }
        require(row.pose.current && row.pose.current->tick==tick &&
            row.pose.root_bone_to_wrapper.has_value()==row.membership.enrolled,"Physical save current history/root-reference membership differs.");
        auto verification=*owner.binding;verification.restore(row.pose); // no registry/history mutation
        require(checkpoint.rigs.emplace(id,std::move(row)).second,"Duplicate physical save owner.");
    }
    checkpoint.constraint_order=physical_state_json::parse_constraint_order(extension.at("constraint_order"));
    std::set<std::pair<std::string,std::string>> actual_constraints;
    for(const auto& key:checkpoint.constraint_order)require(expected_constraints.contains(key) && actual_constraints.insert(key).second,"Physical saved constraint order has an unknown/duplicate member.");
    require(actual_constraints==expected_constraints,"Physical saved global constraint order is incomplete.");
    return checkpoint;
} catch(const std::invalid_argument& error) {throw std::runtime_error(std::string("Physical snapshot: ")+error.what());}
template<class State,class Checkpoint>void save_physical_verify_restored(const State& s,const Checkpoint& checkpoint,const SaveJson& rows) try {
    // Legacy rows remain authoritative duplicate witnesses. GetPosition uses
    // shape origin (not GBC COM), notably for translated character proxies.
    const auto& bodies=s.physics.GetBodyInterface();
    for(const auto& row:rows)if(!row.at("body").is_null()) {
        const auto id=save_text(row.at("id"));const auto entity=s.find(id);const auto* body=s.registry.template try_get<Body>(entity);const auto* character=s.registry.template try_get<Controller>(entity);
        require(body || character,"Saved ordinary-body witness has no native owner.");const auto native=body?body->id:character->character->GetBodyID();const auto& saved=row.at("body");
        JPH::RVec3 p;JPH::Quat q;bodies.GetPositionAndRotation(native,p,q);
        require(save_vector<3>(saved.at("position"),1e6)==save_xyz(p).template get<std::array<double,3>>() &&
            save_rotation(saved.at("rotation"))==save_xyzw(q).template get<std::array<double,4>>() &&
            save_vector<3>(saved.at("linear_velocity"),1e6)==save_xyz(bodies.GetLinearVelocity(native)).template get<std::array<double,3>>() &&
            save_vector<3>(saved.at("angular_velocity"),1e6)==save_xyz(bodies.GetAngularVelocity(native)).template get<std::array<double,3>>() &&
            saved.at("active").template get<bool>()==bodies.IsActive(native),"Legacy body witness differs from the joined native physics state.");
    }
    // Includes off-broadphase Animated poses omitted from GBC and exact named
    // warm starts. Do not normalize values, mask fields, or select one witness.
    for(const auto& [id,row]:checkpoint.rigs)require(
        physical_state_json::membership_json(s.physical_owners.at(id)->backend->snapshot_membership())==physical_state_json::membership_json(row.membership),
        "Physical membership/instantaneous native poses differ after joined restoration.");
    for(const auto& [id,row]:checkpoint.rigs)if(row.handoff) {
        const auto& actual=s.physical_owners.at(id)->handoff;
        require(actual && physical_state_json::handoff_state_json(*actual)==physical_state_json::handoff_state_json(*row.handoff),
            "Physical handoff state differs after joined restoration.");
        save_handoff_witnesses(s,id,*row.handoff);
    }
    save_physical_final_history(s,checkpoint);
} catch(const std::invalid_argument& error) {throw std::runtime_error(std::string("Physical snapshot: ")+error.what());}
} // namespace
