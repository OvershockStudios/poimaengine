// SPDX-License-Identifier: Apache-2.0
// Included inside the serialized native World owner; no world threads/jobs.
    std::string navigation_cache_id_;
    std::shared_ptr<const navigation::Mesh> navigation_cache_;
    std::optional<std::uint64_t> navigation_source_revision_;
    navigation::Source navigation_source_cache_;
    std::string navigation_source_hash_;
    std::shared_ptr<const navigation::Mesh> navigation_load(const std::string& id,bool verify_bytes=false) {
        try {
            const auto path=asset_package_path(asset_directory(),id,".pnav");
            if(!verify_bytes && navigation_cache_ && navigation_cache_id_==id)return navigation_cache_;
            const auto length=fs::file_size(path);
            require(length<=navigation::max_package_bytes,"Navigation package exceeds 16 MiB.",-32050);
            std::string bytes(static_cast<std::size_t>(length),'\0');std::ifstream file(path,std::ios::binary);
            require(bool(file.read(bytes.data(),static_cast<std::streamsize>(length))) && content_hash(bytes)==id,"Navigation package hash mismatch/read failure.",-32050);
            if(navigation_cache_id_!=id || !navigation_cache_) {
                auto candidate=navigation::Mesh::decode(bytes);navigation_cache_=std::move(candidate);navigation_cache_id_=id;
            }
            return navigation_cache_;
        } catch(const Error& error) {throw Error(error.code,asset_diagnostic(error.what()));}catch(const std::exception& error){throw Error(-32050,asset_diagnostic(error.what()));}
    }
    Json navigation_dispatch(const std::string& method,const Json& params) {
        if(method=="world.navigation.bake")fields(params,{"revision","profile"},{"revision"});
        else if(method=="world.navigation.inspect")fields(params,{"revision","asset"},{"revision","asset"});
        else if(method=="world.navigation.path")fields(params,{"revision","asset","start","end","extents","max_polygons","max_corners","max_nodes"},{"revision","asset","start","end"});
        else throw Error(-32601,"Unknown navigation method.");
        current_revision(params);
        require(navigation::available(),"Static navigation is not built; configure POIMA_ENABLE_NAVIGATION=ON.",-32003);
        const auto authored_revision=revision(doc_.at("revision"));
        if(!navigation_source_revision_ || *navigation_source_revision_!=authored_revision) {
            try {
                auto candidate=navigation::collision_source(doc_.at("entities"),world_matrices(doc_.at("entities")));
                auto digest=navigation::source_fingerprint(candidate);
                navigation_source_cache_=std::move(candidate);navigation_source_hash_=std::move(digest);navigation_source_revision_=authored_revision;
            }catch(const std::exception& error){throw Error(-32602,asset_diagnostic(error.what()));}
        }
        const auto& source=navigation_source_cache_;
        const auto& fingerprint=navigation_source_hash_;
        std::shared_ptr<const navigation::Mesh> mesh;std::string id;bool reused=false;
        if(method=="world.navigation.bake") {
            try {
                const auto profile=navigation::parse_profile(params.value("profile",Json::object()).dump());
                const auto wanted=Json::parse(navigation::profile_json(profile));
                if(navigation_cache_ && navigation_cache_->source_fingerprint()==fingerprint && Json::parse(navigation_cache_->metadata()).at("profile")==wanted) {
                    id=navigation_cache_id_;mesh=navigation_load(id,true);reused=true;
                } else {
                    ModelCache collision_models;
                    auto geometry=navigation::collision_geometry(source,[&](const std::string& asset,std::uint32_t primitive) {
                        try {const auto model=collision_models.get(asset_directory(),asset);require(primitive<model->primitives.size(),"Navigation MeshCollider primitive does not exist.",-32050);return model->primitives[primitive];}
                        catch(const Error& error){throw Error(error.code,asset_diagnostic(error.what()));}catch(const std::exception& error){throw Error(-32050,asset_diagnostic(error.what()));}
                    });
                    mesh=navigation::Mesh::bake(geometry,profile,fingerprint);const auto bytes=mesh->encode();id=content_hash(bytes);
                    fs::create_directories(asset_directory());const auto package=asset_directory()/(id+".pnav");
                    if(fs::exists(package)) {auto existing=navigation_load(id,true);require(existing->encode()==bytes,"Navigation artifact identity collision.",-32050);mesh=std::move(existing);reused=true;}
                    else {
                        // Validate destination store before writing; existing
                        // stores may not be symlinks/junctions/reparse points.
                        const auto status=fs::symlink_status(asset_directory());require(fs::is_directory(status) && !fs::is_symlink(status),"Navigation asset store must be a regular directory.",-32050);
#ifdef _WIN32
                        const auto attributes=GetFileAttributesW(asset_directory().c_str());require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Navigation asset store cannot be a reparse point.",-32050);
#endif
                        const auto pending=asset_directory()/(id+"-"+new_presentation_source_id()+".pnav.pending");
                        try {write_flushed(pending,bytes);replace_file(pending,package);}catch(...){std::error_code ignored;fs::remove(pending,ignored);throw;}
                    }
                    navigation_cache_=mesh;navigation_cache_id_=id;
                }
            }catch(const Error& error){throw Error(error.code,asset_diagnostic(error.what()));}
            catch(const fs::filesystem_error& error){throw Error(-32050,asset_diagnostic(error.what()));}
            catch(const std::exception& error){throw Error(-32602,asset_diagnostic(error.what()));}
        } else {
            require(params.at("asset").is_string() && valid_asset_id(params.at("asset")),"Navigation asset must be a 64-hex identity.");id=params.at("asset");mesh=navigation_load(id);
            require(mesh->source_fingerprint()==fingerprint,"Navigation source fingerprint is stale; bake the current collision topology.",-32009);
        }
        Json result={{"revision",doc_.at("revision")},{"scope","authored"},{"asset",id},{"source_fingerprint",fingerprint},{"metadata",Json::parse(mesh->metadata())}};
        if(method=="world.navigation.bake") {
            result["reused"]=reused;
            result["ignored_moving_colliders"]=source.ignored_moving_colliders;
            if(source.ignored_runtime_obstacles)result["ignored_runtime_obstacles"]=source.ignored_runtime_obstacles;
            return result;
        }
        if(method=="world.navigation.inspect")return result;
        navigation::PathRequest request;
        request.start=navigation_vector(params.at("start"),-1000000,1000000);
        request.end=navigation_vector(params.at("end"),-1000000,1000000);
        navigation_limits(params,request,4096);
        try { result["path"]=navigation_path_json(mesh->path(request),request); }
        catch(const std::exception& error){throw Error(-32602,asset_diagnostic(error.what()));}
        return result;
    }
    static navigation::Point navigation_vector(const Json& value,double lo,double hi) {
        require(value.is_array() && value.size()==3,"Navigation vector requires three numbers.");
        navigation::Point point;
        for(std::size_t k=0;k<3;++k) {
            require(value[k].is_number(),"Navigation vector needs numeric coordinates.");
            const auto number=value[k].get<double>();
            require(std::isfinite(number) && number>=lo && number<=hi,"Navigation vector coordinate is out of bounds.");
            point[k]=static_cast<float>(number);
        }
        return point;
    }
    template<class Request>
    static void navigation_limits(const Json& params,Request& request,std::uint32_t output_limit) {
        if(params.contains("extents"))request.extents=navigation_vector(params.at("extents"),.01,100);
        const auto budget=[&](const char* key,std::uint32_t lo,std::uint32_t hi,std::uint32_t& target) {
            if(!params.contains(key))return;
            const auto value=revision(params.at(key));
            require(value>=lo && value<=hi,"Navigation query budget is out of bounds.");
            target=static_cast<std::uint32_t>(value);
        };
        budget("max_polygons",1,output_limit,request.max_polygons);
        budget("max_corners",2,output_limit,request.max_corners);
        budget("max_nodes",32,4096,request.max_nodes);
    }
    template<class Request>
    static Json navigation_path_json(const navigation::Path& path,const Request& request) {
        return {{"status",path.status},{"complete",path.status=="complete"},{"requested_start",path.requested_start},{"requested_end",path.requested_end},
            {"projected_start",path.start_found ? Json(path.projected_start) : Json(nullptr)},{"projected_end",path.end_found ? Json(path.projected_end) : Json(nullptr)},
            {"start_projection_distance",path.start_found ? Json(path.start_distance) : Json(nullptr)},{"end_projection_distance",path.end_found ? Json(path.end_distance) : Json(nullptr)},
            {"reachable_end",path.polygons ? Json(path.reachable_end) : Json(nullptr)},{"polygons",path.polygons},{"corners",path.corners},
            {"limits",{{"max_polygons",request.max_polygons},{"max_corners",request.max_corners},{"max_nodes",request.max_nodes}}}};
    }
    Json runtime_navigation_dispatch(const Json& params) const {
        fields(params,{"session_id","tick","structure_revision","entity","goal","extents","max_polygons","max_corners","max_nodes"},
            {"session_id","tick","structure_revision","entity","goal"});
        require(runtime_navigation_available(),"Runtime navigation backend is not built.",-32003);
        runtime_guard(params);
        const auto tick=revision(params.at("tick"));
        require(tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        structure_read_guard(params);
        RuntimeNavigationRequest request;
        request.entity=identifier(params.at("entity"));
        request.goal=navigation_vector(params.at("goal"),-1000000,1000000);
        navigation_limits(params,request,256);
        try {
            const auto route=runtime_->navigation_path(request);
            Json blockers=Json::array();
            for(const auto& obstacle:route.obstacles)blockers.push_back({{"entity",obstacle.entity},
                {"minimum",obstacle.bounds.minimum},{"maximum",obstacle.bounds.maximum}});
            return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
                {"scope","runtime"},{"entity",request.entity},{"asset",route.asset},{"path",navigation_path_json(route.path,request)},
                {"obstacles",std::move(blockers)},{"obstacle_filtered",route.path.obstacle_filtered},{"blocked",route.blocked}};
        }catch(const std::exception& error){throw Error(-32602,asset_diagnostic(error.what()));}
    }
