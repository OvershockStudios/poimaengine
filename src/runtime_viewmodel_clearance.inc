// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl. Only the owned snapshot's foreground is changed.
    void apply_viewmodel_clearance(SceneSnapshot& scene,const RuntimeCameraPose& foreground) const {
        const auto& camera=registry.get<RuntimeCamera>(find(scene.camera_id));if(!camera.viewmodel_clearance)return;
        require(scene.viewmodel.has_value(),"Clearance presentation requires the configured foreground lens.");
        require(!scene.viewmodel_clearance.has_value(),"Clearance policy must be evaluated exactly once.");
        const auto& profile=*camera.viewmodel_clearance;validate_viewmodel_clearance_profile(profile);
        ViewmodelClearanceResult result;result.profile=profile;
        auto& cast=result.cast;cast.radius=profile.radius;cast.half_height=profile.half_height;cast.distance=profile.max_retraction;
        const auto desired=multiply(multiply(scene.camera_world,local_matrix(foreground.position,foreground.rotation,{1,1,1})),local_matrix(profile.position,profile.rotation,{1,1,1}));
        const auto orientation=pose(desired).rotation;
        cast.rotation={orientation.GetX(),orientation.GetY(),orientation.GetZ(),orientation.GetW()};
        for(std::size_t k=0;k<3;++k){cast.position[k]=desired[12+k]+scene.camera_world[8+k]*profile.max_retraction;cast.direction[k]=-scene.camera_world[8+k];}
        std::size_t depth=0;
        for(auto id=scene.camera_id;!id.empty();id=registry.get<Node>(find(id)).parent) {
            require(++depth<=10000,"Clearance camera ancestry exceeds10000 nodes.");const auto e=find(id);
            if(registry.any_of<Body,Controller>(e))cast.ignore.push_back(id);
        }
        std::sort(cast.ignore.begin(),cast.ignore.end());result.ignored_entities=cast.ignore;
        const auto suppress=[&](ViewmodelClearanceStatus status) {
            result.status=status;result.suppressed=true;result.resolved=false;
            for(const auto& object:scene.viewmodel->objects)result.suppressed_entities.push_back(object.entity_id);
            std::sort(result.suppressed_entities.begin(),result.suppressed_entities.end());scene.viewmodel->objects.clear();
        };
        auto hit=capsule_cast(cast);
        if(hit && hit->initial_overlap) {
            result.anchor_hit=std::move(hit);result.retraction=profile.max_retraction;suppress(ViewmodelClearanceStatus::BlockedAnchor);
        } else {
            result.sweep_hit=std::move(hit);
            const double extension=result.sweep_hit ? std::max(0.,result.sweep_hit->distance-profile.skin) : profile.max_retraction;
            result.retraction=profile.max_retraction-extension;
            auto final=cast;final.distance=0;for(std::size_t k=0;k<3;++k)final.position[k]+=cast.direction[k]*extension;
            result.final_hit=capsule_cast(final);
            if(result.final_hit)suppress(ViewmodelClearanceStatus::BlockedFinal);
            else {
                result.resolved=true;result.suppressed=false;result.status=result.retraction>0 ? ViewmodelClearanceStatus::Retracted : ViewmodelClearanceStatus::Clear;
                if(result.retraction>0) {
                    const auto delta=multiply(multiply(scene.camera_world,local_matrix({0,0,result.retraction},{0,0,0,1},{1,1,1})),inverse_affine(scene.camera_world));
                    for(auto& object:scene.viewmodel->objects) {
                        object.world=multiply(delta,object.world);
                        for(double value:object.world)require(std::isfinite(value) && std::abs(value)<=1e12,"Clearance presentation matrix exceeds supported range.");
                    }
                }
            }
        }
        scene.camera_pose_receipt=true;scene.viewmodel_clearance=std::move(result);
    }
