// SPDX-License-Identifier: Apache-2.0
    struct WaterBodyForces {
        double volume_m3;
        water::Displacement submerged{};
        double density_ratio=0;
    };

    WaterBodyForces water_body_forces(const JPH::Body& body,double mass) const {
        const auto half=static_cast<const JPH::BoxShape&>(*body.GetShape()).GetHalfExtent();
        WaterBodyForces result{8.*half.GetX()*half.GetY()*half.GetZ()};
        if (!water_region) return result;
        const auto world=body.GetCenterOfMassPosition();
        const std::array<double,3> center{world.GetX(),world.GetY(),world.GetZ()};
        std::array<std::array<double,3>,3> axes;
        const auto rotation=body.GetRotation();
        const std::array local{JPH::Vec3(half.GetX(),0,0),JPH::Vec3(0,half.GetY(),0),JPH::Vec3(0,0,half.GetZ())};
        for (std::size_t i=0;i<local.size();++i) {
            const auto axis=rotation*local[i];
            axes[i]={axis.GetX(),axis.GetY(),axis.GetZ()};
        }
        result.submerged=water_region->displaced_by_box(center,axes);
        result.submerged.volume_m3=std::min(result.submerged.volume_m3,result.volume_m3);
        if (result.submerged.volume_m3<=result.volume_m3*1e-10) {
            result.submerged={};
            return result;
        }
        const auto fraction=result.submerged.volume_m3/result.volume_m3;
        constexpr double water_density=1000;
        constexpr double max_lift_to_weight=32;
        result.density_ratio=std::min(water_density*result.volume_m3/mass,max_lift_to_weight/fraction);
        return result;
    }

    std::optional<RuntimeWaterBody> water_body_sample(const std::string& id) const {
        const auto entity=topology->definitions.find(id);
        if (entity==topology->definitions.end() || !entity->second.collider || !entity->second.collider->buoyancy)
            return std::nullopt;
        const auto mass=entity->second.collider->mass;
        JPH::BodyLockRead lock(physics.GetBodyLockInterface(),registry.get<Body>(find(id)).id);
        require(lock.Succeeded(),"Buoyant box is absent from physics.");
        const auto& body=lock.GetBody();
        const auto forces=water_body_forces(body,mass);
        const auto& wet=forces.submerged;
        RuntimeWaterBody result{forces.volume_m3,wet.volume_m3,
            mass*forces.density_ratio*wet.volume_m3/forces.volume_m3*physics.GetGravity().Length(),std::nullopt};
        if (wet.volume_m3>0) {
            const auto center=body.GetCenterOfMassPosition();
            result.center_of_buoyancy=std::array{
                center.GetX()+wet.center_offset_m[0],
                center.GetY()+wet.center_offset_m[1],
                center.GetZ()+wet.center_offset_m[2]};
        }
        return result;
    }

    void apply_water_buoyancy() {
        if (!water_region || topology->buoyant.empty()) return;
        profiling::Scope scope("runtime.water.buoyancy",static_cast<std::int64_t>(tick));
        const auto gravity=physics.GetGravity();
        for (const auto entity : topology->buoyant) {
            const auto id=registry.get<Body>(entity).id;
            const auto& collider=*topology->definitions.at(registry.get<Node>(entity).id).collider;
            bool wake=false;
            {
                JPH::BodyLockWrite lock(physics.GetBodyLockInterface(),id);
                require(lock.Succeeded(),"Buoyant box is absent from physics.");
                auto& body=lock.GetBody();
                const auto forces=water_body_forces(body,collider.mass);
                const auto& wet=forces.submerged;
                if (wet.volume_m3<=0) continue;
                const auto& drag=*collider.buoyancy;
                const auto& offset=wet.center_offset_m;
                const auto wet_fraction=static_cast<float>(wet.volume_m3/forces.volume_m3);
                const bool applied=body.ApplyBuoyancyImpulse(static_cast<float>(forces.volume_m3),
                    static_cast<float>(wet.volume_m3),JPH::Vec3(float(offset[0]),float(offset[1]),float(offset[2])),
                    static_cast<float>(forces.density_ratio),drag.linear_drag*wet_fraction,drag.angular_drag,
                    JPH::Vec3::sZero(),gravity,1.0f/60.0f);
                wake=applied && !body.IsActive();
            }
            if (wake) physics.GetBodyInterface().ActivateBody(id);
        }
    }
