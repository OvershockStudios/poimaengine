// SPDX-License-Identifier: Apache-2.0
// Included inside the serialized native World owner; no world threads/jobs.
    static std::string navigation_diagnostic(const char* message) {
        // POSIX filesystem paths may contain non-UTF-8 bytes. Bound before
        // replacement, then truncate only at a valid output codepoint boundary.
        auto text=Json::parse(Json(std::string(message).substr(0,1024)).dump(-1,' ',false,Json::error_handler_t::replace)).get<std::string>();
        auto length=std::min<std::size_t>(text.size(),1024);
        while(length<text.size() && length>0 && (static_cast<unsigned char>(text[length])&0xc0u)==0x80u)--length;
        text.resize(length);return text;
    }
    struct CheckedNavigation {RuntimeNavigationDefinition definition;std::uint64_t bytes=0;};
    CheckedNavigation navigation_read(const std::string& id) const {
        require(navigation::available(),"Navigation binding requires POIMA_ENABLE_NAVIGATION=ON.",-32003);
        try {
            const auto path=asset_package_path(asset_directory(),id,".pnav");const auto length=fs::file_size(path);
            require(length>0 && length<=navigation::max_package_bytes,"Navigation package must contain1..16MiB.",-32050);
            std::string bytes(static_cast<std::size_t>(length),'\0');std::ifstream file(path,std::ios::binary);
            require(bool(file.read(bytes.data(),static_cast<std::streamsize>(length))) && content_hash(bytes)==id,"Navigation package hash mismatch/read failure.",-32050);
            auto mesh=navigation::Mesh::decode(bytes);require(mesh->encode()==bytes,"Navigation package must use canonical checked encoding.",-32050);
            const auto metadata=Json::parse(mesh->metadata());
            RuntimeNavigationDefinition definition{id,mesh->source_fingerprint(),navigation::parse_profile(metadata.at("profile").dump()),std::move(mesh)};
            return {std::move(definition),static_cast<std::uint64_t>(length)};
        }catch(const Error& error){throw Error(error.code,navigation_diagnostic(error.what()));}
        catch(const std::exception& error){throw Error(-32050,navigation_diagnostic(error.what()));}
    }
    CheckedNavigation navigation_binding(const Json& source) const {
        const auto& binding=source.at("navigation");fields(binding,{"asset"},{"asset"});
        require(binding.at("asset").is_string() && valid_asset_id(binding.at("asset")),"Navigation binding asset must be64 lowercase hexadecimal digits.");
        auto checked=navigation_read(binding.at("asset").get<std::string>());
        std::string fingerprint;
        try {fingerprint=navigation::source_fingerprint(navigation::collision_source(source.at("entities"),world_matrices(source.at("entities"))));}
        catch(const std::exception& error){throw Error(-32602,navigation_diagnostic(error.what()));}
        require(checked.definition.source_fingerprint==fingerprint,"Navigation binding topology is stale; bake and explicitly bind the current static collision source.",-32009);
        return checked;
    }
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
        } catch(const Error& error) {throw Error(error.code,navigation_diagnostic(error.what()));}catch(const std::exception& error){throw Error(-32050,navigation_diagnostic(error.what()));}
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
            }catch(const std::exception& error){throw Error(-32602,navigation_diagnostic(error.what()));}
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
                        catch(const Error& error){throw Error(error.code,navigation_diagnostic(error.what()));}catch(const std::exception& error){throw Error(-32050,navigation_diagnostic(error.what()));}
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
            }catch(const Error& error){throw Error(error.code,navigation_diagnostic(error.what()));}
            catch(const fs::filesystem_error& error){throw Error(-32050,navigation_diagnostic(error.what()));}
            catch(const std::exception& error){throw Error(-32602,navigation_diagnostic(error.what()));}
        } else {
            require(params.at("asset").is_string() && valid_asset_id(params.at("asset")),"Navigation asset must be a 64-hex identity.");id=params.at("asset");mesh=navigation_load(id);
            require(mesh->source_fingerprint()==fingerprint,"Navigation source fingerprint is stale; bake the current collision topology.",-32009);
        }
        Json result={{"revision",doc_.at("revision")},{"scope","authored"},{"asset",id},{"source_fingerprint",fingerprint},{"metadata",Json::parse(mesh->metadata())}};
        if(method=="world.navigation.bake") {result["reused"]=reused;result["ignored_moving_colliders"]=source.ignored_moving_colliders;return result;}
        if(method=="world.navigation.inspect")return result;
        navigation::PathRequest request;
        auto vector=[](const Json& value,double lo,double hi) {
            require(value.is_array() && value.size()==3,"Navigation vector requires three numbers.");navigation::Point point;
            for(std::size_t k=0;k<3;++k){require(value[k].is_number(),"Navigation vector needs numeric coordinates.");const auto number=value[k].get<double>();require(std::isfinite(number) && number>=lo && number<=hi,"Navigation vector coordinate is out of bounds.");point[k]=static_cast<float>(number);}return point;
        };
        request.start=vector(params.at("start"),-1000000,1000000);request.end=vector(params.at("end"),-1000000,1000000);
        if(params.contains("extents"))request.extents=vector(params.at("extents"),.01,100);
        for(auto [key,destination]:{std::pair{"max_polygons",&request.max_polygons},{"max_corners",&request.max_corners},{"max_nodes",&request.max_nodes}})if(params.contains(key)) {
            const auto value=revision(params.at(key));require(value<=4096 && value>=(std::string_view(key)=="max_corners" ? 2u : std::string_view(key)=="max_nodes" ? 32u : 1u),"Navigation query budget is out of bounds.");*destination=static_cast<std::uint32_t>(value);
        }
        try {
            const auto path=mesh->path(request);
            result["path"]={{"status",path.status},{"complete",path.status=="complete"},{"requested_start",path.requested_start},{"requested_end",path.requested_end},
                {"projected_start",path.start_found ? Json(path.projected_start) : Json(nullptr)},{"projected_end",path.end_found ? Json(path.projected_end) : Json(nullptr)},
                {"start_projection_distance",path.start_found ? Json(path.start_distance) : Json(nullptr)},{"end_projection_distance",path.end_found ? Json(path.end_distance) : Json(nullptr)},
                {"reachable_end",path.polygons ? Json(path.reachable_end) : Json(nullptr)},{"polygons",path.polygons},{"corners",path.corners},
                {"limits",{{"max_polygons",request.max_polygons},{"max_corners",request.max_corners},{"max_nodes",request.max_nodes}}}};
        }catch(const std::exception& error){throw Error(-32602,navigation_diagnostic(error.what()));}
        return result;
    }
