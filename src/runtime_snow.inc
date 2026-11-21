// SPDX-License-Identifier: Apache-2.0
// Shared frozen mesh construction preserves source triangle ordinals for rays.
    static JPH::RefConst<JPH::MeshShape> build_mesh_shape(const MeshAsset& mesh,
        const std::array<double,3>& scale,const std::string& id) {
        // Fill default settings directly. The list-taking constructors silently
        // sanitize geometry, while Create reports degenerate triangles.
        JPH::MeshShapeSettings settings;
        settings.mPerTriangleUserData=true;
        settings.mTriangleVertices.reserve(mesh.vertices.size());
        for(const auto& vertex:mesh.vertices) {
            std::array<float,3> point;
            for(std::size_t axis=0;axis<point.size();++axis) {
                const double value=vertex.position[axis]*scale[axis];
                require(std::isfinite(value) && std::abs(value)<=10000,"Scaled MeshCollider vertices must be within +/-10000 meters.");
                point[axis]=static_cast<float>(value);
            }
            settings.mTriangleVertices.emplace_back(point[0],point[1],point[2]);
        }
        settings.mIndexedTriangles.reserve(mesh.indices.size()/3);
        for(std::size_t i=0;i<mesh.indices.size();i+=3)
            settings.mIndexedTriangles.emplace_back(mesh.indices[i],mesh.indices[i+1],mesh.indices[i+2],0,static_cast<JPH::uint32>(i/3));
        const auto result=settings.Create();
        if(result.HasError())throw std::runtime_error("MeshCollider '"+id+"': "+result.GetError().c_str());
        return static_cast<const JPH::MeshShape*>(result.Get().GetPtr());
    }

    struct SnowSurface {
        snow::State state;
        std::array<std::shared_ptr<const MeshAsset>,2> meshes;
        std::array<JPH::RefConst<JPH::MeshShape>,2> shapes;
    };
    std::optional<snow::Region> snow_binding;
    std::shared_ptr<const SnowSurface> snow_live;
    std::optional<snow::FootReleaseState> snow_releases;
    std::array<entt::entity,2> snow_entities{};
    std::array<JPH::BodyID,2> snow_bodies;

    void initialize_snow() {
        auto base=std::make_shared<SnowSurface>();
        for(std::size_t tile=0;tile<snow_entities.size();++tile) {
            const auto entity=find(snow_binding->definition().tiles[tile]);
            snow_entities[tile]=entity;
            snow_bodies[tile]=registry.get<Body>(entity).id;
            base->meshes[tile]=registry.get<RuntimeMesh>(entity).mesh;
            base->shapes[tile]=topology->mesh_shapes.at(snow_bodies[tile].GetIndexAndSequenceNumber());
        }
        snow_live=std::move(base);
        if(snow_binding->definition().foot_release)snow_releases.emplace();
    }

    const JPH::MeshShape* ray_mesh(JPH::uint32 body) const {
        if(snow_live)for(std::size_t tile=0;tile<snow_bodies.size();++tile)
            if(snow_bodies[tile].GetIndexAndSequenceNumber()==body)return snow_live->shapes[tile].GetPtr();
        const auto found=topology->mesh_shapes.find(body);
        return found==topology->mesh_shapes.end() ? nullptr : found->second.GetPtr();
    }

    void build_snow_geometry(SnowSurface& surface,std::uint8_t dirty) const {
        profiling::Scope profile("runtime.snow.prepare",static_cast<std::int64_t>(tick));
        for(unsigned tile=0;tile<surface.meshes.size();++tile)if(dirty&(1u<<tile)) {
            auto mesh=snow_binding->mesh(tile,surface.state);
            auto shape=build_mesh_shape(*mesh,{1,1,1},snow_binding->definition().tiles[tile]);
            surface.meshes[tile]=std::move(mesh);
            surface.shapes[tile]=std::move(shape);
        }
        profiling::counter("runtime.snow.dirty_tiles",((dirty&1u)!=0)+((dirty&2u)!=0));
        profiling::counter("runtime.snow.compacted_samples",snow_binding->compacted_samples(surface.state));
    }

    void publish_snow(std::shared_ptr<const SnowSurface> next,bool wake_overlaps) {
        profiling::Scope profile("runtime.snow.publish",static_cast<std::int64_t>(tick));
        auto& bodies=physics.GetBodyInterface();
        const auto previous=snow_live;
        std::vector<JPH::BodyID> wake;
        if(wake_overlaps) {
            JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> nearby;
            for(std::size_t tile=0;tile<snow_bodies.size();++tile)if(previous->shapes[tile]!=next->shapes[tile]) {
                auto bounds=previous->shapes[tile]->GetLocalBounds();
                bounds.Encapsulate(next->shapes[tile]->GetLocalBounds());
                bounds.ExpandBy(JPH::Vec3::sReplicate(.02f));
                physics.GetBroadPhaseQuery().CollideAABox(bounds,nearby);
            }
            for(const auto id:nearby.mHits)
                if(bodies.GetMotionType(id)==JPH::EMotionType::Dynamic && !bodies.IsActive(id))wake.push_back(id);
            std::sort(wake.begin(),wake.end(),[](auto a,auto b) {
                return a.GetIndexAndSequenceNumber()<b.GetIndexAndSequenceNumber();
            });
            wake.erase(std::unique(wake.begin(),wake.end()),wake.end());
        }
        snow_live=std::move(next);
        for(std::size_t tile=0;tile<snow_bodies.size();++tile)if(previous->shapes[tile]!=snow_live->shapes[tile]) {
            bodies.SetShape(snow_bodies[tile],snow_live->shapes[tile].GetPtr(),false,JPH::EActivation::DontActivate);
            registry.get<RuntimeMesh>(snow_entities[tile]).mesh=snow_live->meshes[tile];
        }
        if(!wake.empty())bodies.ActivateBodies(wake.data(),static_cast<int>(wake.size()));
    }

    snow::SampleMask held_snow_samples() const {
        snow::SampleMask pinned;
        const auto& tiles=snow_binding->definition().tiles;
        (void)animations->has_held_foot([&](const RuntimeFootPlantAnchor& anchor) {
            const auto support=std::find(tiles.begin(),tiles.end(),anchor.support);
            if(support==tiles.end())return false;
            require(anchor.triangle && *anchor.triangle<2*snow::tile_cells*snow::tile_cells,
                "A planted snow anchor requires a valid source triangle.");
            const auto tile=static_cast<unsigned>(support-tiles.begin());
            const auto& mesh=*snow_live->meshes[tile];
            constexpr unsigned row=snow::tile_cells+1;
            // Adjacent faces include the fixed diagonal and can cross the tile seam.
            constexpr std::array<std::array<int,2>,7> ring{{{0,0},{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{1,1}}};
            for(unsigned corner=0;corner<3;++corner) {
                const auto vertex=mesh.indices[*anchor.triangle*3+corner];
                const auto x=static_cast<int>(vertex%row+tile*snow::tile_cells),z=static_cast<int>(vertex/row);
                for(const auto& offset:ring) {
                    const auto xx=x+offset[0],zz=z+offset[1];
                    if(xx>=0 && xx<static_cast<int>(snow::samples_x) && zz>=0 && zz<static_cast<int>(snow::samples_z))
                        pinned.set(static_cast<unsigned>(zz)*snow::samples_x+static_cast<unsigned>(xx));
                }
            }
            return false;
        });
        return pinned;
    }

    bool compact_snow(snow::StampCenter center,std::uint32_t radius_units,std::uint32_t depth_units) {
        auto compacted=snow_live->state;
        const auto dirty=snow_binding->apply(compacted,center,radius_units,depth_units);
        if(!dirty)return false;
        auto candidate=std::make_shared<SnowSurface>(*snow_live);
        candidate->state=compacted;
        build_snow_geometry(*candidate,dirty);
        publish_snow(std::move(candidate),true);
        return true;
    }

    void apply_snow_stamp(const RuntimeSnowStamp& stamp) {
        require(snow_live && snow_binding->contains(stamp.receiver),"Snow stamp requires a bound receiver.");
        require(stamp.expected_generation==snow_live->state.generation,"Stale snow generation.");
        require(stamp.ray.ignore.empty(),"Snow stamp rays cannot ignore colliders.");
        const auto hit=physics_raycast(stamp.ray);
        require(hit && hit->entity==stamp.receiver && hit->triangle==stamp.triangle && hit->normal &&
            (*hit->normal)[1]>0 && pose_frames::dot(*hit->normal,stamp.ray.direction)<0,
            "Snow stamp ray must first hit the expected upward-facing receiver triangle.");
        const auto center=snow_binding->quantize_center(hit->position[0],hit->position[2]);
        require(!snow_binding->changes_samples(snow_live->state,center,stamp.radius_units,stamp.depth_units,held_snow_samples()),
            "Snow compaction would alter a held foot anchor. Release it before stamping.");
        (void)compact_snow(center,stamp.radius_units,stamp.depth_units);
    }

    nlohmann::json archive_snow() const {
        auto archived=snow_binding->save(snow_live->state);
        if(snow_releases) {
            archived["version"]=2;
            archived["foot_release"]=snow::save_foot_releases(*snow_releases,snow_binding->definition());
        }
        return archived;
    }

    void restore_snow(nlohmann::json archived) {
        if(snow_releases) {
            require(archived.is_object() && archived.contains("version") && archived.at("version").is_number_integer() &&
                archived.at("version")==2 && archived.contains("foot_release"),
                "Snow foot-release snapshot needs version 2 and its intent ledger.");
            *snow_releases=snow::restore_foot_releases(archived.at("foot_release"),snow_binding->definition(),tick);
            archived.erase("foot_release");
            archived["version"]=1;
        }
        auto candidate=std::make_shared<SnowSurface>(*snow_live);
        candidate->state=snow_binding->restore(archived);
        require(candidate->state.generation<=tick,"Saved snow generation exceeds its committed tick.");
        require(!snow_releases || snow_releases->counts.applied<=candidate->state.generation,
            "Saved applied snow footprints exceed the committed compaction generation.");
        if(!candidate->state.generation)return;
        build_snow_geometry(*candidate,3);
        publish_snow(std::move(candidate),false);
    }

#include "runtime_snow_releases.inc"
