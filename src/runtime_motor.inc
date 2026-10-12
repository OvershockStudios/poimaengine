// SPDX-License-Identifier: Apache-2.0
// Included in the private runtime namespace. Query proxies are owned separately
// from CharacterVirtual, so creating a candidate never publishes a physics body.
class CharacterMotor {
    JPH::PhysicsSystem& physics_;
    JPH::TempAllocator& allocator_;
    JPH::Ref<JPH::Character> physical_;
    JPH::Ref<JPH::CharacterVirtual> virtual_;
    JPH::BodyID proxy_;
    RuntimeCharacterLocomotionProfile profile_;
    bool jump_pending_=false;
    float jump_speed_=0;
    JPH::Vec3 reported_velocity_=JPH::Vec3::sZero();
    JPH::RVec3 tick_origin_=JPH::RVec3::sZero();
    float pending_seconds_=0;
    JPH::CharacterBase& base() const {return virtual_ ? static_cast<JPH::CharacterBase&>(*virtual_) : static_cast<JPH::CharacterBase&>(*physical_);}
    JPH::Vec3 bounded_velocity(JPH::Vec3Arg velocity) const {
        const float maximum=physics_.GetBodyInterface().GetMaxLinearVelocity(proxy_);
        require(!velocity.IsNaN() && std::isfinite(velocity.LengthSq()),"Invalid virtual character velocity.");
        return velocity.LengthSq()>maximum*maximum ? velocity.Normalized()*maximum : velocity;
    }
    void publish_proxy_pose() {
        auto& bodies=physics_.GetBodyInterface();
        bodies.SetPositionAndRotation(proxy_,virtual_->GetPosition()+JPH::Vec3(0,query_padding,0),virtual_->GetRotation(),JPH::EActivation::DontActivate);
        bodies.SetLinearAndAngularVelocity(proxy_,JPH::Vec3::sZero(),JPH::Vec3::sZero());
    }
public:
    static constexpr float query_padding=.02f;
    CharacterMotor(const JPH::CharacterSettings& settings,const std::optional<RuntimeCharacterLocomotionProfile>& profile,
        JPH::RVec3Arg position,JPH::QuatArg rotation,JPH::PhysicsSystem& physics,JPH::TempAllocator& allocator,
        const std::optional<JPH::BodyID>& id):physics_(physics),allocator_(allocator) {
        if(!profile) {
            physical_=id ? new JPH::Character(&settings,position,rotation,0,&physics,*id)
                         : new JPH::Character(&settings,position,rotation,0,&physics);
            return;
        }
        profile_=*profile;
        JPH::BodyCreationSettings body(settings.mShape,position+JPH::Vec3(0,query_padding,0),rotation,JPH::EMotionType::Kinematic,settings.mLayer);
        body.mAllowSleeping=false;body.mFriction=0;
        auto& bodies=physics.GetBodyInterface();
        auto* native=id ? bodies.CreateBodyWithID(*id,body) : bodies.CreateBody(body);
        require(native!=nullptr,"Cannot allocate the virtual character query body.");proxy_=native->GetID();
        try {
            JPH::CharacterVirtualSettings config;
            config.mID=JPH::CharacterID(proxy_.GetIndexAndSequenceNumber());
            config.mShape=settings.mShape;config.mSupportingVolume=settings.mSupportingVolume;
            config.mMaxSlopeAngle=profile_.max_slope_degrees*(std::numbers::pi_v<float>/180);
            config.mCharacterPadding=query_padding;
            // No mInnerBodyShape: its upstream constructor auto-adds a body.
            virtual_=new JPH::CharacterVirtual(&config,position,rotation,0,&physics);
        } catch(...) {bodies.DestroyBody(proxy_);throw;}
    }
    ~CharacterMotor() {
        if(virtual_) {
            auto& bodies=physics_.GetBodyInterface();
            if(bodies.IsAdded(proxy_))bodies.RemoveBody(proxy_);
            virtual_=nullptr;bodies.DestroyBody(proxy_);
        }
    }
    bool IsVirtual() const {return virtual_!=nullptr;}
    JPH::BodyID GetBodyID() const {return virtual_ ? proxy_ : physical_->GetBodyID();}
    void AddToPhysicsSystem() {
        if(virtual_)physics_.GetBodyInterface().AddBody(proxy_,JPH::EActivation::Activate);
        else physical_->AddToPhysicsSystem();
    }
    void RemoveFromPhysicsSystem() {
        if(virtual_)physics_.GetBodyInterface().RemoveBody(proxy_);
        else physical_->RemoveFromPhysicsSystem();
    }
    JPH::RVec3 GetPosition() const {return virtual_ ? virtual_->GetPosition() : physical_->GetPosition();}
    JPH::Quat GetRotation() const {return virtual_ ? virtual_->GetRotation() : physical_->GetRotation();}
    // Jolt's virtual velocity is an integration intent, not the displacement
    // actually achieved against walls or by stair/floor corrections.
    JPH::Vec3 GetLinearVelocity() const {return virtual_ ? reported_velocity_ : physical_->GetLinearVelocity();}
    JPH::Vec3 GetIntegrationVelocity() const {return virtual_ ? virtual_->GetLinearVelocity() : physical_->GetLinearVelocity();}
    void SetRotation(JPH::QuatArg rotation) {
        if(virtual_) {virtual_->SetRotation(rotation);publish_proxy_pose();}
        else physical_->SetRotation(rotation);
    }
    void SetLinearVelocity(JPH::Vec3Arg velocity) {
        if(virtual_) {reported_velocity_=bounded_velocity(velocity);virtual_->SetLinearVelocity(reported_velocity_);}
        else physical_->SetLinearVelocity(velocity);
    }
    JPH::CharacterBase::EGroundState GetGroundState() const {return base().GetGroundState();}
    JPH::BodyID GetGroundBodyID() const {return base().GetGroundBodyID();}
    JPH::Vec3 GetGroundNormal() const {return base().GetGroundNormal();}
    JPH::Vec3 GetGroundVelocity() const {return base().GetGroundVelocity();}
    void BeginTick() {jump_pending_=false;jump_speed_=0;}
    void RequestJump(float speed) {jump_pending_=true;jump_speed_=speed;}
    void CheckCollision(JPH::RVec3Arg position,JPH::QuatArg rotation,JPH::Vec3Arg movement,float separation,
        const JPH::Shape* shape,JPH::RVec3Arg offset,JPH::CollideShapeCollector& collector) const {
        if(!virtual_) {physical_->CheckCollision(position,rotation,movement,separation,shape,offset,collector);return;}
        JPH::IgnoreSingleBodyFilter ignore(proxy_);
        virtual_->CheckCollision(position,rotation,movement,separation,shape,offset,collector,
            physics_.GetDefaultBroadPhaseLayerFilter(1),physics_.GetDefaultLayerFilter(1),ignore,{});
    }
    bool SetShape(const JPH::Shape* shape,float penetration) {
        if(!virtual_)return physical_->SetShape(shape,penetration);
        JPH::IgnoreSingleBodyFilter ignore(proxy_);
        if(!virtual_->SetShape(shape,penetration,physics_.GetDefaultBroadPhaseLayerFilter(1),
            physics_.GetDefaultLayerFilter(1),ignore,{},allocator_))return false;
        physics_.GetBodyInterface().SetShape(proxy_,shape,false,JPH::EActivation::DontActivate);
        publish_proxy_pose();return true;
    }
    void UpdateVirtual(float seconds) {
        if(!virtual_)return;
        require(seconds>0 && std::isfinite(seconds),"Invalid virtual character integration interval.");
        const auto previous=virtual_->GetPosition();
        tick_origin_=previous;pending_seconds_=seconds;
        virtual_->UpdateGroundVelocity();
        const auto requested=virtual_->GetLinearVelocity();const auto ground=virtual_->GetGroundVelocity();
        auto velocity=JPH::Vec3(requested.GetX(),requested.GetY(),requested.GetZ());
        if(virtual_->GetGroundState()==JPH::CharacterBase::EGroundState::OnGround &&
            (jump_pending_ || requested.GetY()-ground.GetY()<.1f))
            velocity=JPH::Vec3(requested.GetX()+ground.GetX(),ground.GetY()+(jump_pending_ ? jump_speed_ : 0),requested.GetZ()+ground.GetZ());
        velocity=bounded_velocity(velocity+physics_.GetGravity()*seconds);
        // ExtendedUpdate cancels sliding components itself while retaining the
        // original requested horizontal displacement for its stair solver.
        virtual_->SetLinearVelocity(velocity);
        JPH::CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mWalkStairsStepUp=JPH::Vec3(0,profile_.step_height,0);
        settings.mStickToFloorStepDown=JPH::Vec3(0,-profile_.ground_snap,0);
        JPH::IgnoreSingleBodyFilter ignore(proxy_);
        virtual_->ExtendedUpdate(seconds,physics_.GetGravity(),settings,physics_.GetDefaultBroadPhaseLayerFilter(1),
            physics_.GetDefaultLayerFilter(1),ignore,{},allocator_);
        require(!virtual_->GetMaxHitsExceeded(),"Virtual character contact collection budget exceeded.");
        reported_velocity_=JPH::Vec3((virtual_->GetPosition()-previous)/seconds);
        const float maximum=physics_.GetBodyInterface().GetMaxLinearVelocity(proxy_)+
            (profile_.step_height+profile_.ground_snap+2*query_padding)/seconds+1;
        require(!reported_velocity_.IsNaN() && std::isfinite(reported_velocity_.LengthSq()) &&
            reported_velocity_.LengthSq()<=maximum*maximum,"Virtual character resolved velocity exceeds the integration/correction budget.");
        // A blocked upward jump must stop pressing into a ceiling. Stair and
        // floor corrections never become next tick's ballistic velocity.
        auto integrated=virtual_->GetLinearVelocity();
        if(virtual_->GetGroundState()!=JPH::CharacterBase::EGroundState::OnGround &&
            integrated.GetY()>0 && reported_velocity_.GetY()<integrated.GetY()-.001f) {
            integrated.SetY(std::max(0.0f,reported_velocity_.GetY()));virtual_->SetLinearVelocity(integrated);
        }
        publish_proxy_pose();
    }
    void PostSimulation(float separation) {
        if(!virtual_) {physical_->PostSimulation(separation);return;}
        const bool supported=virtual_->IsSupported();
        const bool descending=reported_velocity_.GetY()<=1.0e-6f &&
            virtual_->GetLinearVelocity().GetY()-virtual_->GetGroundVelocity().GetY()<=1.0e-6f;
        JPH::IgnoreSingleBodyFilter ignore(proxy_);
        virtual_->RefreshContacts(physics_.GetDefaultBroadPhaseLayerFilter(1),physics_.GetDefaultLayerFilter(1),ignore,{},allocator_);
        require(!virtual_->GetMaxHitsExceeded(),"Virtual character contact collection budget exceeded.");
        // Refresh drops solve-preserved edge contacts outside collision slop.
        // Retain the supported-to-air transition for a real downward sweep;
        // otherwise the next ExtendedUpdate starts airborne and cannot snap.
        if(pending_seconds_>0 && supported && !virtual_->IsSupported() && descending &&
            !jump_pending_ && profile_.ground_snap>0) {
            virtual_->StickToFloor(JPH::Vec3(0,-profile_.ground_snap,0),physics_.GetDefaultBroadPhaseLayerFilter(1),
                physics_.GetDefaultLayerFilter(1),ignore,{},allocator_);
            require(!virtual_->GetMaxHitsExceeded(),"Virtual character contact collection budget exceeded.");
        }
        if(pending_seconds_>0) {
            reported_velocity_=JPH::Vec3((virtual_->GetPosition()-tick_origin_)/pending_seconds_);
            const float maximum=physics_.GetBodyInterface().GetMaxLinearVelocity(proxy_)+
                (profile_.step_height+profile_.ground_snap+2*query_padding)/pending_seconds_+1;
            require(!reported_velocity_.IsNaN() && std::isfinite(reported_velocity_.LengthSq()) &&
                reported_velocity_.LengthSq()<=maximum*maximum,"Virtual character resolved velocity exceeds the integration/correction budget.");
            pending_seconds_=0;publish_proxy_pose();
        }
    }
    void SaveState(JPH::StateRecorder& recorder) const {
        if(!virtual_) {physical_->SaveState(recorder);return;}
        virtual_->SaveState(recorder);recorder.Write(jump_pending_);recorder.Write(jump_speed_);recorder.Write(reported_velocity_);
    }
    void RestoreState(JPH::StateRecorder& recorder) {
        if(!virtual_) {physical_->RestoreState(recorder);return;}
        virtual_->RestoreState(recorder);recorder.Read(jump_pending_);recorder.Read(jump_speed_);recorder.Read(reported_velocity_);
        pending_seconds_=0;
    }
    void RestoreMotorPose(JPH::RVec3Arg position,JPH::QuatArg rotation,JPH::Vec3Arg velocity,JPH::Vec3Arg reported) {
        require(IsVirtual(),"Saved motor state requires a virtual character.");
        virtual_->SetPosition(position);virtual_->SetRotation(rotation);virtual_->SetLinearVelocity(bounded_velocity(velocity));
        reported_velocity_=reported;
        jump_pending_=false;jump_speed_=0;pending_seconds_=0;publish_proxy_pose();
    }
};
