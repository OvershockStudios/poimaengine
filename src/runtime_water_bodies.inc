// SPDX-License-Identifier: Apache-2.0
    struct WaterBodyForces {
        double volume_m3;
        water::Displacement submerged{};
        double density_ratio=0;
    };

    static constexpr double water_density_kg_m3=1000;
    WaterBodyForces water_body_forces(const JPH::Body& body,double mass,
                                      std::span<double> occupied_cell_volumes={}) const {
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
        result.submerged=water_region->displaced_by_box(center,axes,occupied_cell_volumes);
        result.submerged.volume_m3=std::min(result.submerged.volume_m3,result.volume_m3);
        if (result.submerged.volume_m3<=result.volume_m3*1e-10) {
            result.submerged={};
            return result;
        }
        const auto fraction=result.submerged.volume_m3/result.volume_m3;
        constexpr double max_lift_to_weight=32;
        result.density_ratio=std::min(water_density_kg_m3*result.volume_m3/mass,max_lift_to_weight/fraction);
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
        if (water_region && water_region->currents()) result.water_velocity_m_s=wet.water_velocity_m_s;
        if (wet.volume_m3>0) {
            const auto center=body.GetCenterOfMassPosition();
            result.center_of_buoyancy=std::array{
                center.GetX()+wet.center_offset_m[0],
                center.GetY()+wet.center_offset_m[1],
                center.GetZ()+wet.center_offset_m[2]};
        }
        return result;
    }

    static void apply_relative_water_drag(JPH::Body& body,const water::Displacement& wet,JPH::Vec3Arg arm,
                                         float submerged_fraction,float drag,float mass) {
        if (drag==0) return;
        const auto& velocity=wet.water_velocity_m_s;
        const auto fluid=JPH::Vec3(float(velocity[0]),float(velocity[1]),float(velocity[2]));
        const auto relative=fluid-(body.GetLinearVelocity()+body.GetAngularVelocity().Cross(arm));
        const auto speed=relative.Length();
        if (speed<=1e-6f) return;
        const auto direction=relative/speed;
        const auto normal=body.GetRotation().InverseRotate(direction).Abs();
        const auto extent=static_cast<const JPH::BoxShape&>(*body.GetShape()).GetHalfExtent();
        const auto area=4.*(normal.GetX()*extent.GetY()*extent.GetZ()+
            normal.GetY()*extent.GetX()*extent.GetZ()+normal.GetZ()*extent.GetX()*extent.GetY());
        // Limit the impulse using effective mass along the point's relative
        // velocity. The drag cannot overshoot the surrounding water velocity.
        const auto lever=arm.Cross(direction);
        const auto inverse_mass=1./mass+std::max(0.f,lever.Dot(body.GetInverseInertia()*lever));
        const auto impulse=.5*water_density_kg_m3*drag*area*submerged_fraction*speed*speed*water::step_seconds;
        body.AddImpulse(direction*static_cast<float>(std::min(impulse,speed/inverse_mass)),
            body.GetCenterOfMassPosition()+arm);
    }

    std::size_t apply_water_buoyancy(std::span<water::WakeDriver> wake_drivers,std::span<double> occupied_cell_volumes) {
        if (!water_region || topology->buoyant.empty()) return 0;
        profiling::Scope scope("runtime.water.buoyancy",static_cast<std::int64_t>(tick));
        const auto gravity=physics.GetGravity();
        std::size_t wake_count=0;
        for (const auto entity : topology->buoyant) {
            const auto id=registry.get<Body>(entity).id;
            const auto& collider=*topology->definitions.at(registry.get<Node>(entity).id).collider;
            bool wake=false;
            {
                JPH::BodyLockWrite lock(physics.GetBodyLockInterface(),id);
                require(lock.Succeeded(),"Buoyant box is absent from physics.");
                auto& body=lock.GetBody();
                const auto forces=water_body_forces(body,collider.mass,occupied_cell_volumes);
                const auto& wet=forces.submerged;
                if (wet.volume_m3<=0) continue;
                const auto& drag=*collider.buoyancy;
                const auto& offset=wet.center_offset_m;
                const auto wet_fraction=static_cast<float>(wet.volume_m3/forces.volume_m3);
                const auto arm=JPH::Vec3(float(offset[0]),float(offset[1]),float(offset[2]));
                if (water_region->wakes() && wet_fraction>.01f && wet_fraction<.99f) {
                    const auto relative=body.GetLinearVelocity()+body.GetAngularVelocity().Cross(arm);
                    const auto x=relative.GetX()-wet.water_velocity_m_s[0];
                    const auto z=relative.GetZ()-wet.water_velocity_m_s[2];
                    if (std::hypot(x,z)>.05) {
                        require(wake_count<wake_drivers.size(),"Buoyant wake driver capacity exceeded.");
                        const auto position=body.GetCenterOfMassPosition()+arm;
                        const auto factor=4.*wet_fraction*(1-wet_fraction)*
                            std::min(1.,std::cbrt(forces.volume_m3)/water_region->grid().config().cell_size_m);
                        wake_drivers[wake_count++]={position.GetX(),position.GetZ(),x*factor,z*factor};
                    }
                }
                const auto flowing=water_region->currents().has_value();
                if (flowing) apply_relative_water_drag(body,wet,arm,wet_fraction,drag.linear_drag,collider.mass);
                const bool applied=body.ApplyBuoyancyImpulse(static_cast<float>(forces.volume_m3),
                    static_cast<float>(wet.volume_m3),arm,
                    static_cast<float>(forces.density_ratio),flowing ? 0.f : drag.linear_drag*wet_fraction,drag.angular_drag,
                    JPH::Vec3::sZero(),gravity,1.0f/60.0f);
                wake=applied && !body.IsActive();
            }
            if (wake) physics.GetBodyInterface().ActivateBody(id);
        }
        return wake_count;
    }
