// SPDX-License-Identifier: Apache-2.0
// Included after the legacy save helpers. This separately versioned extension
// never changes a legacy payload field or installs untrusted recorder bytes.
namespace {
template<class State,class Checkpoint>void save_physical_final_history(const State& s,const Checkpoint& checkpoint) {
    require(checkpoint.rigs.size()==s.topology->physical_rigs.size(),"Physical save history owner membership differs.");
    for(const auto& [id,row]:checkpoint.rigs) {
        const auto& owner=*s.physical_owners.at(id);
        require(row.pose.current && row.pose.current->tick==s.tick,"Physical save requires the current committed final-pose history.");
        const bool passive=row.membership.mode==RuntimePhysicalRigMode::Passive;
        require(passive==row.pose.root_bone_to_wrapper.has_value(),"Physical mode and captured root-reference membership differ.");
        // A copy validates chronological finite-difference velocities and the
        // actual current wrapper/bone COM poses. Never rewrite saved history or
        // substitute instantaneous Jolt velocities for finite differences.
        auto verification=*owner.binding;verification.restore(row.pose);
        (void)verification.record_final(s.tick,s.physical_world());
        if(passive) {
            verification.capture_passive_reference(s.physical_world());
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
    SaveJson rigs=SaveJson::array();
    for(const auto& [id,row]:checkpoint.rigs)rigs.push_back({{"entity",id},
        {"membership",physical_state_json::membership_json(row.membership)},
        {"pose",physical_state_json::pose_state_json(row.pose)}});
    return {{"version",std::uint32_t{1}},
        {"bootstrap",physical_bootstrap_json::bootstrap_json(bootstrap,s.topology->live)},
        {"rigs",std::move(rigs)},{"constraint_order",physical_state_json::constraint_order_json(checkpoint.constraint_order)},
        {"owner",physical_bootstrap_json::owner_json(owner)},{"physics",std::move(physics)}};
} catch(const std::invalid_argument& error) {throw std::runtime_error(std::string("Physical snapshot: ")+error.what());}
void save_physical_extension_fields(const SaveJson& extension) {
    save_fields(extension,{"version","bootstrap","rigs","constraint_order","owner","physics"});
    require(extension.at("version").is_number_unsigned() && extension.at("version")==1u,"Unsupported physical snapshot extension version.");
    require(extension.at("rigs").is_array() && extension.at("rigs").size()<=RuntimeBodyIds::capacity,"Physical snapshot rig population exceeds its bound.");
}
RuntimePhysicsBootstrap save_physical_parse_bootstrap(const SaveJson& extension,const RuntimeDefinition& definition) try {
    return physical_bootstrap_json::parse_bootstrap(extension.at("bootstrap"),definition);
} catch(const std::invalid_argument& error) {throw std::runtime_error(std::string("Physical snapshot: ")+error.what());}
template<class State>auto save_physical_checkpoint(const State& s,const SaveJson& extension,
    const RuntimePhysicsBootstrap& bootstrap,std::uint64_t tick) try {
    typename State::PhysicalCheckpoint checkpoint;checkpoint.enabled=true;checkpoint.next_collision_group=bootstrap.next_collision_group;
    checkpoint.owner=physical_bootstrap_json::parse_owner(extension.at("owner"));
    physical_bootstrap_json::validate_owner_history(checkpoint.owner,bootstrap.ledger);
    const auto& rigs=extension.at("rigs");require(rigs.size()==s.topology->physical_rigs.size(),"Saved physical rig membership differs from the complete restored graph.");
    std::string previous;std::set<std::pair<std::string,std::string>> expected_constraints;
    for(const auto& value:rigs) {
        save_fields(value,{"entity","membership","pose"});const auto id=save_text(value.at("entity"));
        require(previous.empty() || previous<id,"Physical save owners must be sorted and unique.");previous=id;
        const auto found=s.physical_owners.find(id);require(found!=s.physical_owners.end(),"Physical save names an absent owner.");const auto& owner=*found->second;
        typename State::PhysicalSavedRig row;row.membership=physical_state_json::parse_membership(value.at("membership"));
        row.pose=physical_state_json::parse_pose_state(value.at("pose"));
        const auto& profile=owner.backend->definition();const auto& identity=bootstrap.rigs.at(id);
        row.ids=identity.parts;row.collision_group=identity.collision_group;
        require(row.membership.poses.size()==profile.parts.size() && row.membership.warm_starts.size()==profile.joints.size(),"Physical save part/joint membership differs from its frozen profile.");
        require((row.membership.mode==RuntimePhysicalRigMode::Passive)==row.membership.enrolled,"Physical save mode and enrollment differ.");
        const auto& configured=*s.topology->definitions.at(id).physical_rig;
        require(!configured.initially_passive || row.membership.mode==RuntimePhysicalRigMode::Passive,"An initially passive rig cannot return to Animated without a recovery policy.");
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
    save_physical_final_history(s,checkpoint);
} catch(const std::invalid_argument& error) {throw std::runtime_error(std::string("Physical snapshot: ")+error.what());}
} // namespace
