// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl. Queries never change bodies, clocks or game state.
    std::optional<CapsuleCastHit> capsule_cast(const CapsuleCastRequest& query) const {
        validate_capsule_cast_request(query);
        std::set<JPH::uint32> ignored;
        for(const auto& id:query.ignore) {
            const auto e=find(id);
            if(const auto* body=registry.try_get<Body>(e))ignored.insert(body->id.GetIndexAndSequenceNumber());
            if(const auto* c=registry.try_get<Controller>(e))ignored.insert(c->character->GetBodyID().GetIndexAndSequenceNumber());
        }
        struct Budget {std::uint32_t bodies=0,contacts=0;} budget;
        struct Filter final : JPH::BodyFilter {
            const std::set<JPH::uint32>& ignored;Budget& budget;
            Filter(const std::set<JPH::uint32>& i,Budget& b):ignored(i),budget(b){}
            bool ShouldCollide(const JPH::BodyID& body) const override {
                if(ignored.contains(body.GetIndexAndSequenceNumber()))return false;
                require(++budget.bodies<=capsule_query_body_budget,"Capsule candidate-body budget exceeded; clearance is unknown.");return true;
            }
        } filter(ignored,budget);
        const JPH::RVec3 base(query.position[0],query.position[1],query.position[2]);
        const auto q=JPH::Quat(static_cast<float>(query.rotation[0]),static_cast<float>(query.rotation[1]),static_cast<float>(query.rotation[2]),static_cast<float>(query.rotation[3])).Normalized();
        const auto transform=JPH::RMat44::sRotationTranslation(q,base);
        auto shape=JPH::CapsuleShapeSettings(static_cast<float>(query.half_height),static_cast<float>(query.radius)).Create();
        require(!shape.HasError(),"Cannot create the validated capsule query shape.");
        const auto project=[&](const JPH::CollideShapeResult& hit,double fraction,bool initial)->CapsuleCastHit {
            require(std::isfinite(fraction) && fraction>=0 && fraction<=1,"Invalid native capsule fraction.");
            CapsuleCastHit result;result.entity=topology->body_names.at(hit.mBodyID2.GetIndexAndSequenceNumber());
            result.fraction=initial ? 0 : fraction;result.distance=result.fraction*query.distance;result.initial_overlap=initial;
            result.penetration_depth=initial ? std::max(0.,double(hit.mPenetrationDepth)) : 0;
            require(std::isfinite(result.penetration_depth) && result.penetration_depth<=1e6,"Native capsule depth is out of bounds.");
            for(std::size_t k=0;k<3;++k) {
                result.point_on_query[k]=query.position[k]+hit.mContactPointOn1[static_cast<JPH::uint>(k)];
                result.point_on_obstacle[k]=query.position[k]+hit.mContactPointOn2[static_cast<JPH::uint>(k)];
                require(std::isfinite(result.point_on_query[k]) && std::abs(result.point_on_query[k])<=1e12 &&
                    std::isfinite(result.point_on_obstacle[k]) && std::abs(result.point_on_obstacle[k])<=1e12,"Native capsule contact is out of bounds.");
            }
            const auto& axis=hit.mPenetrationAxis;const double length=std::hypot(double(axis.GetX()),double(axis.GetY()),double(axis.GetZ()));
            require(std::isfinite(length),"Native capsule normal is nonfinite.");
            if(length>1e-20)result.normal=std::array<double,3>{-axis.GetX()/length,-axis.GetY()/length,-axis.GetZ()/length};
            if(const auto mesh=topology->mesh_shapes.find(hit.mBodyID2.GetIndexAndSequenceNumber());mesh!=topology->mesh_shapes.end())
                result.triangle=mesh->second->GetTriangleUserData(hit.mSubShapeID2);
            return result;
        };
        const auto key=[](const CapsuleCastHit& hit) {
            return std::tuple(hit.entity,hit.triangle.value_or(std::numeric_limits<std::uint32_t>::max()),hit.point_on_obstacle,hit.point_on_query);
        };
        struct Overlap final : JPH::CollideShapeCollector {
            const decltype(project)& project_hit;const decltype(key)& tie_key;Budget& budget;std::optional<CapsuleCastHit> hit;
            Overlap(const decltype(project)& p,const decltype(key)& k,Budget& b):project_hit(p),tie_key(k),budget(b){}
            void AddHit(const JPH::CollideShapeResult& value) override {
                require(++budget.contacts<=capsule_query_contact_budget,"Capsule contact budget exceeded; clearance is unknown.");
                require(std::isfinite(value.mPenetrationDepth),"Native capsule overlap depth is nonfinite.");
                if(value.mPenetrationDepth<0)return;
                auto candidate=project_hit(value,0,true);
                if(!hit || candidate.penetration_depth>hit->penetration_depth ||
                    (candidate.penetration_depth==hit->penetration_depth && tie_key(candidate)<tie_key(*hit)))hit=std::move(candidate);
            }
        } overlap(project,key,budget);
        JPH::CollideShapeSettings overlap_settings;overlap_settings.mBackFaceMode=JPH::EBackFaceMode::CollideWithBackFaces;
        overlap_settings.mCollisionTolerance=static_cast<float>(capsule_query_collision_tolerance);
        physics.GetNarrowPhaseQuery().CollideShape(shape.Get(),JPH::Vec3::sReplicate(1),transform,overlap_settings,base,overlap,{}, {},filter);
        if(overlap.hit || query.distance==0)return overlap.hit;
        const double length=std::hypot(query.direction[0],query.direction[1],query.direction[2]);
        const JPH::Vec3 delta(static_cast<float>(query.direction[0]/length*query.distance),static_cast<float>(query.direction[1]/length*query.distance),static_cast<float>(query.direction[2]/length*query.distance));
        struct Sweep final : JPH::CastShapeCollector {
            const decltype(project)& project_hit;const decltype(key)& tie_key;Budget& budget;std::optional<CapsuleCastHit> hit;
            Sweep(const decltype(project)& p,const decltype(key)& k,Budget& b):project_hit(p),tie_key(k),budget(b){}
            void AddHit(const JPH::ShapeCastResult& value) override {
                require(++budget.contacts<=capsule_query_contact_budget,"Capsule contact budget exceeded; clearance is unknown.");
                if(value.mFraction<0 || value.mFraction>1)return;
                auto candidate=project_hit(value,value.mFraction,value.mFraction==0);
                if(!hit || candidate.fraction<hit->fraction || (candidate.fraction==hit->fraction &&
                    (candidate.penetration_depth>hit->penetration_depth || (candidate.penetration_depth==hit->penetration_depth && tie_key(candidate)<tie_key(*hit)))))hit=std::move(candidate);
            }
        } sweep(project,key,budget);
        JPH::ShapeCastSettings cast_settings;cast_settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
        cast_settings.mReturnDeepestPoint=true;cast_settings.mCollisionTolerance=static_cast<float>(capsule_query_collision_tolerance);
        const JPH::RShapeCast cast(shape.Get(),JPH::Vec3::sReplicate(1),transform,delta);
        physics.GetNarrowPhaseQuery().CastShape(cast,cast_settings,base,sweep,{}, {},filter);
        return sweep.hit;
    }
    static int32_t POIMA_CALL cast_capsule_gameplay(void* context,const PoimaGameCapsuleCastRequestV1* source,
        PoimaGameCapsuleCastResultV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Capsule query context is absent.");auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_capsule_query_attempts<capsule_query_tick_budget,"Gameplay exceeded32 capsule query attempts in one Tick.");++self.game_capsule_query_attempts;
            require(source && output,"Capsule query request/output is absent.");
            require(source->version==1 && source->bytes>=sizeof(PoimaGameCapsuleCastRequestV1) && source->reserved==0 &&
                source->ignore_count<=128 && (!source->ignore_count || source->ignore),"Invalid capsule query request header/ignore array.");
            require(output->version==1 && output->bytes>=sizeof(PoimaGameCapsuleCastResultV1) && output->flags==0 && output->reserved==0 && output->reserved_tail==0,"Invalid capsule query result header/reserved fields.");
            CapsuleCastRequest request;std::copy_n(source->position,3,request.position.begin());std::copy_n(source->rotation,4,request.rotation.begin());
            request.radius=source->radius;request.half_height=source->half_height;std::copy_n(source->direction,3,request.direction.begin());request.distance=source->distance;
            for(std::uint32_t i=0;i<source->ignore_count;++i)request.ignore.push_back(gameplay_id(source->ignore[i]));
            const auto hit=self.capsule_cast(request);PoimaGameCapsuleCastResultV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);
            if(hit) {
                candidate.present=1;candidate.entity=gameplay_id(hit->entity);candidate.fraction=hit->fraction;candidate.distance=hit->distance;candidate.penetration_depth=hit->penetration_depth;
                std::copy(hit->point_on_query.begin(),hit->point_on_query.end(),candidate.point_on_query);std::copy(hit->point_on_obstacle.begin(),hit->point_on_obstacle.end(),candidate.point_on_obstacle);
                if(hit->initial_overlap)candidate.flags|=1;
                if(hit->normal){candidate.flags|=2;std::copy(hit->normal->begin(),hit->normal->end(),candidate.normal);}
                if(hit->triangle){candidate.flags|=4;candidate.triangle=*hit->triangle;}
            }
            *output=candidate;
        });
    }
