// SPDX-License-Identifier: Apache-2.0
    Json settings_dispatch(const std::string& method,const Json& params) {
        if(method=="settings.describe") { fields(params,{});return player_settings::describe(); }
        require(method=="settings.inspect" || method=="settings.transact","Unknown settings method.",-32601);
        if(method=="settings.inspect")fields(params,{"path"},{"path"});
        else fields(params,{"path","request_id","expected_revision","set","reset","preview"},{"path","request_id","expected_revision"});
        const auto file=preference_profile_path(params.at("path"),"Settings");
        try {
            if(method=="settings.inspect")return read_only_ ? player_settings::inspect_read_only(file) : player_settings::inspect(file);
            auto operation=params;operation.erase("path");return player_settings::transact(file,operation);
        }catch(const player_settings::SettingsError& error) { throw Error(error.code,error.what()); }
    }
    Json apply_player_settings(const Json& params,PlayerOptions& options) const {
        const bool requested=params.contains("settings_profile") || params.contains("settings_overrides") || params.contains("settings_revision");
        require(!params.contains("settings_revision") || params.contains("settings_profile"),"settings_revision requires settings_profile.");
        if(!requested) { options.render=render_options(params);return nullptr; }
        Json values=Json::object(),sources=Json::object();
        Json profile={{"source","none"},{"revision",0},{"content_hash",nullptr}};
        try {
            if(params.contains("settings_profile")) {
                const auto path=preference_profile_path(params.at("settings_profile"),"Settings");
                auto loaded=read_only_ ? player_settings::load_read_only(path) : player_settings::load(path);
                if(params.contains("settings_revision"))require(revision(params.at("settings_revision"))==loaded.revision,"Settings profile revision conflict.",-32009);
                values=std::move(loaded.values);
                profile={{"source","profile"},{"revision",loaded.revision},{"content_hash",loaded.content_hash}};
                for(auto it=values.begin();it!=values.end();++it)sources[it.key()]="profile";
            }
            if(params.contains("settings_overrides")) {
                const auto overrides=player_settings::validate_values(params.at("settings_overrides"));
                for(auto it=overrides.begin();it!=overrides.end();++it) { values[it.key()]=it.value();sources[it.key()]="session_override"; }
            }
        }catch(const player_settings::SettingsError& error) { throw Error(error.code,error.what()); }
        // Explicit existing launch options remain last in the precedence chain.
        for(const auto* key:{"samples","frames_in_flight"})if(params.contains(key)) {
            const std::string setting=std::string("graphics.")+key;
            auto single=Json::object();single[setting]=params.at(key);
            try { values[setting]=player_settings::validate_values(single).at(setting); }
            catch(const player_settings::SettingsError& error) { throw Error(error.code,error.what()); }
            sources[setting]="explicit_option";
        }
        auto rendering=params;
        for(const auto* key:{"samples","frames_in_flight"}) {
            const std::string setting=std::string("graphics.")+key;
            if(values.contains(setting))rendering[key]=values.at(setting);
        }
        options.render=render_options(rendering); // Validate combined renderer dependencies before opening a player.
        InputProfile input=*options.input_profile;
        if(values.contains("input.sensitivity_x"))input.sensitivity_x=values.at("input.sensitivity_x").get<double>();
        if(values.contains("input.sensitivity_y"))input.sensitivity_y=values.at("input.sensitivity_y").get<double>();
        if(values.contains("input.invert_x"))input.invert_x=values.at("input.invert_x").get<bool>();
        if(values.contains("input.invert_y"))input.invert_y=values.at("input.invert_y").get<bool>();
        validate_input_profile(input);
        options.input_profile=std::make_shared<const InputProfile>(std::move(input));
        const auto camera=runtime_->snapshot(options.camera);
        if(values.contains("camera.vertical_fov"))options.vertical_fov=values.at("camera.vertical_fov").get<double>();
        if(values.contains("ui.scale"))options.ui_scale=values.at("ui.scale").get<float>();
        Json effective={{"camera.vertical_fov",options.vertical_fov.value_or(camera.vertical_fov)},
            {"input.sensitivity_x",options.input_profile->sensitivity_x},{"input.sensitivity_y",options.input_profile->sensitivity_y},
            {"input.invert_x",options.input_profile->invert_x},{"input.invert_y",options.input_profile->invert_y},
            {"ui.scale",options.ui_scale ? Json(*options.ui_scale) : Json(nullptr)},
            {"graphics.samples",options.render.samples},{"graphics.frames_in_flight",options.render.frames_in_flight}};
        Json rows=Json::object();
        for(auto it=effective.begin();it!=effective.end();++it) {
            const auto& key=it.key();
            const std::string inherited=key.starts_with("input.") ? "input_profile" : key=="camera.vertical_fov" ? "authored_camera" : key=="ui.scale" ? "window_density" : "engine_default";
            rows[key]={{"requested",values.contains(key) ? values.at(key) : Json(nullptr)},
                {"effective",it.value()},{"source",sources.contains(key) ? sources.at(key) : Json(inherited)},{"outcome","not_presented"}};
        }
        return {{"format","poima.settings.application.v1"},{"application","next_player"},{"profile",profile},{"fields",rows}};
    }
    static void finish_player_settings(Json& application,const PlayerOptions& options,const PlayerReport& report,const std::optional<SceneSnapshot>& final_camera) {
        // Report final configured owner state; an error may have prevented its
        // last presentation. Never retain a replaced camera's initial lens.
        application["fields"]["camera.vertical_fov"]["effective"]=options.vertical_fov ? Json(*options.vertical_fov) : final_camera ? Json(final_camera->vertical_fov) : Json(nullptr);
        application["fields"]["ui.scale"]["effective"]=report.effective_ui_scale;
        for(auto it=application["fields"].begin();it!=application["fields"].end();++it)
            it.value()["outcome"]=(options.replay && it.key().starts_with("input.")) ? "validated_only" : report.render.frames_presented>0 ? "applied" : "not_presented";
    }
