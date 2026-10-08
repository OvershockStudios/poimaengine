// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. Binding verification is construction-only;
// Tick queries use the immutable mesh and live native controller position.
    void initialize_navigation(const std::optional<RuntimeNavigationDefinition>& binding) {
        if(!binding)return;
        require(runtime_navigation_available(),"Runtime navigation backend is not built.");
        const auto hex=[](const std::string& value) {
            return value.size()==64 && std::all_of(value.begin(),value.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');});
        };
        require(binding->mesh && hex(binding->asset) && hex(binding->source_fingerprint),"Invalid frozen navigation binding.");
        require(binding->mesh->source_fingerprint()==binding->source_fingerprint,"Frozen navigation source fingerprint differs from its mesh.");
        const auto package=binding->mesh->encode();
        require(package.size()<=navigation::max_package_bytes && sha256(std::as_bytes(std::span(package.data(),package.size())))==binding->asset,
            "Frozen navigation asset differs from its mesh.");
        const auto metadata=nlohmann::json::parse(binding->mesh->metadata());
        require(metadata.at("profile")==nlohmann::json::parse(navigation::profile_json(binding->profile)),"Frozen navigation profile differs from its mesh.");
        const float radius=metadata.at("clearance").at("radius").get<float>();
        const float height=metadata.at("clearance").at("height").get<float>();
        require(std::isfinite(radius) && radius>0 && std::isfinite(height) && height>0,"Invalid frozen navigation clearance.");
        std::array<std::uint64_t,4> words{};
        for(std::size_t i=0;i<words.size();++i) {
            const auto* start=binding->asset.data()+16*i;
            const auto converted=std::from_chars(start,start+16,words[i],16);
            require(converted.ec==std::errc{} && converted.ptr==start+16,"Invalid navigation asset word.");
        }
        navigation_binding=binding;navigation_asset=words;navigation_radius=radius;navigation_height=height;
    }
    RuntimeNavigationPath navigation_query(const RuntimeNavigationRequest& request) const {
        require(runtime_navigation_available(),"Runtime navigation backend is not built.");
        require(bool(navigation_binding),"Runtime has no frozen navigation binding.");
        require(request.max_polygons>=1 && request.max_polygons<=256 && request.max_corners>=2 && request.max_corners<=256 &&
            request.max_nodes>=32 && request.max_nodes<=4096,"Invalid runtime navigation query limits.");
        for(float value:request.goal)require(std::isfinite(value) && std::abs(value)<=1e6f,"Invalid navigation goal.");
        for(float value:request.extents)require(std::isfinite(value) && value>=.01f && value<=100,"Invalid navigation projection extents.");
        const auto entity=find(request.entity);
        const auto* controller=registry.try_get<Controller>(entity);
        require(controller,"Navigation queries require a live CharacterController.");
        require(controller->settings.radius<=navigation_radius && controller->settings.height<=navigation_height,
            "Character capsule exceeds the baked navigation clearance.");
        const auto position=controller->character->GetPosition();
        navigation::PathRequest query;
        // Check the actual double position before narrowing, not just its cast.
        const std::array<double,3> start{position.GetX(),position.GetY(),position.GetZ()};
        for(std::size_t i=0;i<3;++i) {
            require(std::isfinite(start[i]) && std::abs(start[i])<=1e6,"Invalid navigation start.");
            query.start[i]=static_cast<float>(start[i]);
        }
        query.end=request.goal;query.extents=request.extents;query.max_polygons=request.max_polygons;
        query.max_corners=request.max_corners;query.max_nodes=request.max_nodes;
        return {navigation_binding->asset,navigation_binding->mesh->path(query)};
    }
    static int32_t POIMA_CALL query_navigation(void* context,const PoimaGameNavigationRequestV1* input,
        PoimaGameNavigationPointV1* corners,uint32_t capacity,PoimaGameNavigationResultV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Missing navigation service context.");auto& self=*static_cast<Impl*>(context);self.require_tick();
            // Failed attempts count too. Saturating rejects prevent uint wrap.
            require(self.game_navigation_calls<8,"Gameplay exceeded eight navigation query attempts in one tick.");++self.game_navigation_calls;
            require(input && corners && output,"Missing navigation query buffer.");
            struct Prefix {uint32_t version,bytes;};Prefix source{},destination{};
            std::memcpy(&source,input,sizeof(source));std::memcpy(&destination,output,sizeof(destination));
            require(source.version==1 && source.bytes==sizeof(PoimaGameNavigationRequestV1),"Invalid navigation request prefix.");
            require(destination.version==1 && destination.bytes>=sizeof(PoimaGameNavigationResultV1),"Invalid navigation result prefix.");
            // No tail reads until the readable extents above are established.
            PoimaGameNavigationRequestV1 command{};std::memcpy(&command,input,sizeof(command));
            require(command.reserved==0 && output->reserved[0]==0 && output->reserved[1]==0,"Invalid navigation reserved fields.");
            require(capacity>=2 && capacity<=256 && command.max_corners<=capacity,"Invalid navigation corner buffer capacity.");
            const auto address=reinterpret_cast<std::uintptr_t>(corners),result_address=reinterpret_cast<std::uintptr_t>(output);
            const auto corner_bytes=std::size_t(capacity)*sizeof(PoimaGameNavigationPointV1);
            require(address<=std::numeric_limits<std::uintptr_t>::max()-corner_bytes && result_address<=std::numeric_limits<std::uintptr_t>::max()-sizeof(*output),"Navigation output address range overflow.");
            require(address+corner_bytes<=result_address || result_address+sizeof(*output)<=address,"Navigation output buffers overlap.");
            RuntimeNavigationRequest request;request.entity=gameplay_id(command.agent);
            std::copy_n(command.goal,3,request.goal.begin());std::copy_n(command.extents,3,request.extents.begin());
            request.max_polygons=command.max_polygons;request.max_corners=command.max_corners;request.max_nodes=command.max_nodes;
            const auto answer=self.navigation_query(request);const auto& path=answer.path;
            PoimaGameNavigationResultV1 result{};result.version=1;result.bytes=sizeof(result);
            std::copy(self.navigation_asset.begin(),self.navigation_asset.end(),result.asset);
            if(path.status=="complete")result.status=0;
            else if(path.status=="partial")result.status=1;
            else if(path.status=="unreachable")result.status=2;
            else if(path.status=="buffer_limit")result.status=3;
            else if(path.status=="out_of_nodes")result.status=4;
            else throw std::runtime_error("Unknown native navigation result status.");
            require(path.corners.size()<=command.max_corners && path.polygons<=command.max_polygons,"Native navigation result exceeds query limits.");
            const auto point=[](const navigation::Point& value) {
                for(float coordinate:value)require(std::isfinite(coordinate) && std::abs(coordinate)<=1e6f,"Invalid native navigation result point.");
            };
            point(path.requested_start);std::copy(path.requested_start.begin(),path.requested_start.end(),result.requested_start);
            if(path.start_found) {
                point(path.projected_start);require(std::isfinite(path.start_distance) && path.start_distance>=0,"Invalid start projection distance.");
                result.flags|=1;std::copy(path.projected_start.begin(),path.projected_start.end(),result.projected_start);result.start_projection_distance=path.start_distance;
            }
            if(path.end_found) {
                point(path.projected_end);require(std::isfinite(path.end_distance) && path.end_distance>=0,"Invalid end projection distance.");
                result.flags|=2;std::copy(path.projected_end.begin(),path.projected_end.end(),result.projected_end);result.end_projection_distance=path.end_distance;
            }
            if(path.polygons) {
                require(path.start_found && path.end_found && !path.corners.empty() && result.status!=2,"Inconsistent native navigation corridor.");
                point(path.reachable_end);result.flags|=4;std::copy(path.reachable_end.begin(),path.reachable_end.end(),result.reachable_end);
            } else require(path.corners.empty() && result.status==2,"Inconsistent native navigation empty corridor.");
            std::array<PoimaGameNavigationPointV1,256> staged{};
            for(std::size_t i=0;i<path.corners.size();++i) {point(path.corners[i]);std::copy(path.corners[i].begin(),path.corners[i].end(),staged[i].position);}
            result.corner_count=static_cast<uint32_t>(path.corners.size());result.polygons=path.polygons;
            // Publish only after all validation/allocation. Unused entries and
            // caller's declared future result tail remain untouched.
            if(result.corner_count)std::memcpy(corners,staged.data(),result.corner_count*sizeof(staged[0]));
            std::memcpy(output,&result,sizeof(result));
        });
    }
