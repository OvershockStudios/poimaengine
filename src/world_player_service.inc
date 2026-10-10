// SPDX-License-Identifier: Apache-2.0
// Owner-only frame-driven player state. The stable adapter owns no Runtime pointer.
    struct PreparedPlayer { PlayerOptions options;Json input_info,settings_application;
        std::shared_ptr<GameplayPlayerPreferences> gameplay_preferences; };
    static PlayerPreferenceGraphicsConstraints preference_graphics_constraints(const PlayerOptions& options) {
        return {options.render.deferred,options.render.ambient_occlusion.mode!=AmbientOcclusionMode::none,
            options.render.scene_debug_view==SceneDebugView::color,options.render.reconstruction!=ReconstructionMode::none};
    }
    static void validate_player_parameters(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","controller","camera","mode","sequence","max_frames","path","width","height","gpu","samples","culling","clustered_lighting","frames_in_flight","scene_debug_view","scene_product_probes","lighting_path","ambient_occlusion","reconstruction","profile","audio","input_profile","input_revision","gamepad","expected_structure_revision","settings_profile","settings_revision","settings_overrides"},
            {"session_id","request_id","expected_tick","camera","mode"});
    }
    PreparedPlayer prepare_player(const Json& params) {
        runtime_guard(params);
        const auto expected=revision(params.at("expected_tick"));
        structure_mutation_guard(params);require(expected==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(!params.contains("audio") || params.at("audio").is_boolean(),"audio must be Boolean.");
        require(!params.value("audio",false) || audio_available(),"Player audio is not built.",-32003);
        PlayerOptions options;options.audio=params.value("audio",false);
        if(params.contains("controller"))options.controller=identifier(params.at("controller"));
        options.camera=identifier(params.at("camera"));
        require(params.at("mode")=="interactive" || params.at("mode")=="replay","Player mode must be interactive or replay.");
        options.replay=params.at("mode")=="replay";
        require(!options.replay || !options.controller.empty(),"Replay requires a CharacterController; interactive menus may omit controller.");
        Json input_info={{"source","defaults"},{"revision",0},{"content_hash",nullptr},{"applied",!options.replay}};
        require(!params.contains("input_revision") || params.contains("input_profile"),"input_revision requires input_profile.");
        if(params.contains("input_profile")) {
            try {
                const auto loaded=(read_only_ ? input_profiles::load_read_only(input_profile_path(params.at("input_profile"))) : input_profiles::load(input_profile_path(params.at("input_profile"))));
                if(params.contains("input_revision"))require(revision(params.at("input_revision"))==loaded.revision,"Input profile revision conflict.",-32009);
                options.input_profile=std::make_shared<const InputProfile>(loaded.profile);
                input_info={{"source","profile"},{"revision",loaded.revision},{"content_hash",loaded.content_hash},{"applied",!options.replay}};
            }catch(const input_profiles::ProfileError& e) { throw Error(e.code,e.what()); }
        }
        if(!options.input_profile)options.input_profile=std::make_shared<const InputProfile>(default_gamepad_input_profile());
        options.gamepad_selection.mode=options.input_profile->gamepad ? "only_connected" : "disabled";
        if(params.contains("gamepad")) {
            const auto& choice=params.at("gamepad");fields(choice,{"mode","id"},{"mode"});
            require(choice.at("mode")=="disabled" || choice.at("mode")=="only_connected" || choice.at("mode")=="explicit","Invalid gamepad selection mode.");
            options.gamepad_selection.mode=choice.at("mode").get<std::string>();
            require(choice.contains("id")== (options.gamepad_selection.mode=="explicit"),"Only explicit gamepad selection requires an id.");
            if(choice.contains("id")) { const auto id=revision(choice.at("id"));require(id>=1 && id<=4294967295ULL,"Gamepad id must be a nonzero uint32.");options.gamepad_selection.id=static_cast<std::uint32_t>(id); }
        }
        require(options.gamepad_selection.mode=="disabled" || options.input_profile->gamepad.has_value(),"Gamepad selection requires a v2 profile; copy a v1 profile to a new v2 destination first.");
        input_info["format"]=options.input_profile->gamepad ? "poima.input.v2" : "poima.input.v1";
        require(options.controller.empty() || runtime_->is_player_controller(options.controller),"Player requires a camera-bound CharacterController when controller is supplied.",-32004);
        try { runtime_->snapshot(options.camera); }
        catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        if(options.replay) {
            require(!params.contains("max_frames"),"Replay ends with its sequence; max_frames is interactive only.");
            require(params.contains("sequence") && params.at("sequence").is_array() && !params.at("sequence").empty() && params.at("sequence").size()<=256,
                "Replay requires 1..256 input segments.");
            std::uint64_t total=0;
            for(auto segment:params.at("sequence")) {
                fields(segment,{"ticks","move","look","jump","use","motions","sounds"},{"ticks"});
                const auto ticks=revision(segment.at("ticks")); require(ticks>=1 && ticks<=600,"Replay segment must be 1..600 ticks.");
                total+=ticks; require(total<=36000 && expected+total<=max_revision,"Replay exceeds the tick limit.");
                auto motions=parse_motions(segment.value("motions",Json::array()));segment.erase("motions");
                auto sounds=parse_sounds(segment.value("sounds",Json::array()));segment.erase("sounds");
                segment.erase("ticks"); segment["entity"]=options.controller;
                options.sequence.push_back({static_cast<std::uint32_t>(ticks),parse_input(segment),std::move(motions),std::move(sounds)});
            }
        } else {
            require(!params.contains("sequence"),"Interactive play takes input from the window, not a replay sequence.");
            if(params.contains("max_frames")) { const auto n=revision(params.at("max_frames")); require(n<=36000,"max_frames must be 0..36000."); options.max_frames=static_cast<std::uint32_t>(n); }
        }
        auto settings_application=apply_player_settings(params,options);
        options.render.capture_exclusive=read_only_;
        if(!options.replay && options.gamepad_selection.mode!="disabled") {
            require(GamepadHost::available(),"Gamepad device host is not built.",-32003);
            try { if(!gamepad_host_)gamepad_host_=std::make_shared<GamepadHost>();options.gamepad_host=gamepad_host_; }
            catch(const std::exception& e) { throw Error(-32071,e.what()); }
        }

        auto binding=std::make_shared<GameplayPlayerPreferences>(options.preferences,gameplay_id(new_id()),
            options.replay,preference_graphics_constraints(options));
        GameplayPlayerPreferences::Observation initial;
        initial.requested_gain=options.preferences->snapshot()->values.master_gain;
        initial.audio_outcome=options.audio ? PoimaPreferenceAudioNotInitialized : PoimaPreferenceAudioDisabled;
        binding->observe(initial);
        return {std::move(options),std::move(input_info),std::move(settings_application),std::move(binding)};
    }
    struct PlayerSessionAdapter final : PlayerSession {
            World& owner;explicit PlayerSessionAdapter(World& value):owner(value) {}
            frame_performance::Recorder* frame_recorder() noexcept override { return &owner.performance_.recorder(); }
            void preference_observation(const PlayerReport& report) noexcept override { owner.observe_gameplay_player_preferences(report); }
            std::string identity() const override { return owner.runtime_id_; }
            std::uint64_t tick() const override { return owner.runtime_->inspect().tick; }
            bool controller_valid(const std::string& id) const override {
                return owner.runtime_->is_player_controller(id);
            }
            SceneSnapshot snapshot(const std::string& camera) const override { return owner.runtime_->snapshot(camera); }
            PlayerAudioState audio_state(const std::string& listener) const override {
                return {tick(),owner.runtime_->audio_snapshot(listener),owner.runtime_->sound_state().voices()};
            }
            std::shared_ptr<const ui::Presentation> ui_presentation() const override { return owner.runtime_->ui_model().presentation(); }
            PlayerControlResult control(const std::string& session_id,std::uint64_t ui_revision,const std::string& target) override {
                const auto result=owner.control_dispatch({{"session_id",session_id},{"request_id",new_id()},
                    {"expected_tick",tick()},{"expected_ui_revision",ui_revision},
                    {"expected_control_sequence",owner.runtime_->control_sequence()},
                    {"expected_gameplay_revision",owner.runtime_->gameplay_revision()},
                    {"expected_structure_revision",owner.runtime_->structure_revision()},{"id",target}});
                return {static_cast<RuntimeControlIntent>(result.at("intent").get<std::uint32_t>()),result.at("save_serviced").get<bool>()};
            }
            bool advance(const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>& motions,const std::vector<SoundCommand>& sounds) override {
                return owner.advance_runtime(1,inputs,motions,sounds).save_serviced;
            }
    };

    Json player_report_json(const PlayerOptions& options,Json input_info,Json settings_application,
        const PlayerReport& report,const std::string& mode) const {
        std::optional<SceneSnapshot> camera;try { camera=runtime_->snapshot(options.camera); }catch(const std::exception&) {}
        Json result={{"session_id",runtime_id_},{"world_id",runtime_definition_.world_id},{"revision",runtime_definition_.authored_revision},
            {"previous_tick",report.initial_tick},{"tick",report.final_tick},{"mode",mode},{"replayed",false},
            {"success",report.render.success},{"stop_reason",report.stop_reason},{"detail",report.render.detail},
            {"frames_presented",report.render.frames_presented},{"swapchain_rebuilds",report.swapchain_rebuilds},
            {"dropped_wall_seconds",report.dropped_seconds},{"gpu",report.render.gpu_name},{"hardware",report.render.hardware},
            {"nvrhi_errors",report.render.validation_errors},{"width",report.render.width},{"height",report.render.height},{"samples",report.render.samples},
            {"capture_written",report.render.capture_written},{"path",options.render.capture.empty() ? Json(nullptr) : Json(options.render.capture)},
            {"camera",options.camera},{"camera_world",camera ? Json(camera->camera_world) : Json(nullptr)},{"lighting",camera ? lighting_json(camera->lighting) : Json(nullptr)},{"render_diagnostics",render_diagnostics(report.render.diagnostics)},{"build_version",build_metadata().version}};
        result["initial_session_id"]=report.initial_session;result["current_session_id"]=report.final_session;result["runtime_replacements"]=report.runtime_replacements;
        result["input_profile"]=input_info;result["gamepad"]=Json::parse(report.gamepad_json);
        if(!settings_application.is_null()) {
            finish_player_settings(settings_application,options,report,camera);
            result["settings"]=std::move(settings_application);
        }
        if(options.preferences || !result.value("settings",Json(nullptr)).is_null()) {
            auto effective_input=*options.input_profile;
            if(options.preferences) {
                const auto& values=options.preferences->snapshot()->values;
                effective_input.sensitivity_x=values.sensitivity_x;effective_input.sensitivity_y=values.sensitivity_y;
                effective_input.invert_x=values.invert_x;effective_input.invert_y=values.invert_y;
            }
            result["input_profile"]["effective_content_hash"]=content_hash(input_profiles::profile_json(effective_input).dump());
        }
        result["live_settings"]=player_live_settings(options,report,camera);
        const auto& audio=report.audio;result["audio"]={{"enabled",audio.enabled},{"driver",audio.driver},{"submitted_frames",audio.submitted_frames},{"max_queued_frames",audio.max_queued_frames},{"empty_queue_observations",audio.empty_queue_observations},{"backpressure_ms",audio.backpressure_ms},{"stream_drained",audio.stream_drained},{"timeline_resets",audio.timeline_resets},{"voices_started",audio.stream.voices_started},{"peak",audio.stream.peak},{"over_range_samples",audio.stream.over_range_samples},{"dsp_ms",audio.stream.dsp_ms}};
        return result;
    }
    struct PlayerOwner {
        std::string id,mode,observed_session;
        bool observed_paused=false;
        std::uint64_t tick=0,structure_revision=0,ui_revision=0,control_sequence=0;
        PreparedPlayer prepared;
        std::unique_ptr<PlayerSessionAdapter> session;
        std::unique_ptr<PlayerWindow> window;
        Json cached_report;
        std::uint64_t applied_ui_control_sequence=0;
    };
    static constexpr bool player_available() noexcept { return POIMA_SIMULATION!=0 && POIMA_RENDER_SMOKE!=0; }
    bool player_active() const noexcept { return player_owner_ && bool(player_owner_->window); }
    void invalidate_player_control() noexcept {
        if(player_control_revision_<max_revision)++player_control_revision_;
    }
    void attach_gameplay_player_preferences(const std::shared_ptr<GameplayPlayerPreferences>& binding) noexcept {
        gameplay_player_preferences_=binding;
        if(runtime_)runtime_->gameplay_player_preferences_host(binding);
    }
    void detach_gameplay_player_preferences(const std::shared_ptr<GameplayPlayerPreferences>& binding) noexcept {
        if(binding)binding->deactivate();
        if(gameplay_player_preferences_==binding) {
            gameplay_player_preferences_.reset();
            if(runtime_)runtime_->gameplay_player_preferences_host({});
        }
    }
    void prepare_gameplay_player_preferences_boundary() noexcept {
        if(gameplay_player_preferences_)
            gameplay_player_preferences_->mutation_capacity(!player_active() || player_control_revision_<max_revision);
    }
    void flush_gameplay_player_preferences() noexcept {
        if(runtime_ && runtime_->commit_gameplay_player_preferences() && player_active())invalidate_player_control();
    }
    void observe_gameplay_player_preferences(const PlayerReport& report) noexcept {
        if(!gameplay_player_preferences_)return;
        GameplayPlayerPreferences::Observation observed;
        const auto& preferences=report.preferences;
        if(preferences.observed_revision) {observed.mask|=1;observed.observed_revision=*preferences.observed_revision;}
        if(preferences.applied_revision) {observed.mask|=2;observed.applied_revision=*preferences.applied_revision;}
        if(preferences.presented_revision) {observed.mask|=4;observed.presented_revision=*preferences.presented_revision;}
        if(preferences.effective_vertical_fov) {observed.mask|=8;observed.effective_fov=*preferences.effective_vertical_fov;}
        if(preferences.applied_revision) {observed.mask|=16;observed.effective_ui_scale=report.effective_ui_scale;}
        if(preferences.sink_gain) {observed.mask|=32;observed.sink_gain=*preferences.sink_gain;}
        observed.requested_gain=preferences.requested_master_gain;
        observed.audio_outcome=preferences.audio_outcome=="sink_gain_verified" ? PoimaPreferenceAudioSinkGainVerified :
            preferences.audio_outcome=="apply_failed" ? PoimaPreferenceAudioApplyFailed :
            preferences.audio_outcome=="initialization_failed" ? PoimaPreferenceAudioInitializationFailed :
            preferences.audio_outcome=="not_initialized" ? PoimaPreferenceAudioNotInitialized : PoimaPreferenceAudioDisabled;
        gameplay_player_preferences_->observe(observed);
    }
    struct BlockingPreferenceScope {
        World& owner;std::shared_ptr<GameplayPlayerPreferences> binding;
        BlockingPreferenceScope(World& value,std::shared_ptr<GameplayPlayerPreferences> attachment):owner(value),binding(std::move(attachment)) {
            owner.attach_gameplay_player_preferences(binding);
        }
        ~BlockingPreferenceScope() {owner.detach_gameplay_player_preferences(binding);}
    };
    void player_boundary(PlayerOwner& player) {
        if(!runtime_)return;
        player.tick=runtime_->inspect().tick;
        player.structure_revision=runtime_->structure_revision();
        player.ui_revision=runtime_->ui_model().revision();
        player.control_sequence=runtime_->control_sequence();
    }
    void reconcile_player() {
        if(!player_active())return;
        auto& player=*player_owner_;
        const auto current=runtime_id_;
        const auto paused=player.window->paused();
        if(current!=player.observed_session)player.applied_ui_control_sequence=0;
        if(current!=player.observed_session || paused!=player.observed_paused)invalidate_player_control();
        player.observed_session=current;player.observed_paused=paused;player_boundary(player);
        // Native window callbacks have already applied their returned intent.
        // External callbacks reach their dedicated intent hook before this boundary.
        player.applied_ui_control_sequence=player.control_sequence;
        // Cache projection at owner boundaries; inspection never extracts geometry.
        player.cached_report=player_report_json(player.prepared.options,player.prepared.input_info,
            player.prepared.settings_application,player.window->report(),player.mode);
        if(player.window->finished()) {
            // Normal finish retires actual GPU work. A fault must retain loss,
            // never report the unfinished ticket as a completed duration.
            auto& capture=performance_.recorder();capture.stop_admission();
            if(capture.status().gpu_pending)capture.drop_remaining(frame_performance::GpuDropReason::owner_shutdown);
            // Explicit performance.stop freezes after it records drain timing.
            detach_gameplay_player_preferences(player.prepared.gameplay_preferences);
            player.window.reset();player.session.reset();invalidate_player_control();
        }
    }
    bool poll_player() {
        if(!player_active())return false;
        player_owner_->window->poll();reconcile_player();return player_active();
    }
    void stop_player() {
        if(!player_active())return;
        player_owner_->window->request_stop();player_owner_->window->poll();reconcile_player();
        require(!player_active(),"Native player did not complete owner stop.",-32080);
    }
    Json player_info(bool include_report=true) const {
        Json result={{"available",player_available()},{"generation",player_generation_},
            {"player_id",nullptr},{"control_revision",player_control_revision_},{"state","absent"},
            {"active",false},{"ready",false},{"paused",false},{"mode",nullptr},
            {"session_id",nullptr},{"current_session_id",nullptr},{"tick",nullptr},
            {"structure_revision",nullptr},{"ui_revision",nullptr},{"control_sequence",nullptr},{"report",nullptr}};
        if(!player_owner_)return result;
        const auto& player=*player_owner_;const bool active=bool(player.window);
        const bool paused=active ? player.window->paused() : player.observed_paused;
        const bool ready=active && player.window->ready();
        result["player_id"]=player.id;result["mode"]=player.mode;result["active"]=active;
        result["ready"]=ready;result["paused"]=paused;
        result["state"]=!active ? "finished" : !ready ? "initializing" : paused ? "paused" : "running";
        result["session_id"]=active ? runtime_id_ : player.observed_session;
        result["current_session_id"]=result.at("session_id");
        result["tick"]=active ? runtime_->inspect().tick : player.tick;
        result["structure_revision"]=active ? runtime_->structure_revision() : player.structure_revision;
        result["ui_revision"]=active ? runtime_->ui_model().revision() : player.ui_revision;
        result["control_sequence"]=active ? runtime_->control_sequence() : player.control_sequence;
        if(include_report)result["report"]=player.cached_report;
        return result;
    }
    struct PlayerReceipt {
        Json params,result;
        std::optional<RenderReport> capture;
        std::exception_ptr failure;
        bool committed=false,control=false;
        std::uint64_t control_revision=0;
        bool active=false,ready=false,paused=false;
        enum class State { initializing,running,paused,finished } state=State::initializing;
    };
    Json player_receipt_json(const PlayerReceipt& receipt,bool replayed) const {
        require(receipt.committed,"Player command outcome is not terminal.",-32080);
        if(receipt.failure) {
            try {std::rethrow_exception(receipt.failure);}
            catch(const Error&) {throw;}
            catch(const std::exception& error) {throw Error(-32020,error.what());}
        }
        auto result=receipt.result;result["replayed"]=replayed;
        if(receipt.control) {
            result["control_revision"]=receipt.control_revision;result["active"]=receipt.active;
            result["ready"]=receipt.ready;result["paused"]=receipt.paused;
            result["state"]=receipt.state==PlayerReceipt::State::finished ? "finished" :
                receipt.state==PlayerReceipt::State::initializing ? "initializing" : receipt.state==PlayerReceipt::State::paused ? "paused" : "running";
        }
        if(receipt.capture) {
            const auto& capture=*receipt.capture;auto& metadata=result["capture"];
            metadata["width"]=capture.width;metadata["height"]=capture.height;metadata["samples"]=capture.samples;
            metadata["gpu"]=capture.gpu_name;metadata["hardware"]=capture.hardware;
            metadata["frames_presented"]=capture.frames_presented;metadata["capture_written"]=capture.capture_written;
            metadata["nvrhi_errors"]=capture.validation_errors;metadata["render_diagnostics"]=render_diagnostics(capture.diagnostics);
        }
        return result;
    }
    std::optional<Json> player_retry(const Json& normalized) const {
        for(const auto& receipt:player_receipts_)if(receipt && receipt->params.at("request_id")==normalized.at("request_id")) {
            require(receipt->params==normalized,"Player request ID reused with different parameters.",-32010);
            return player_receipt_json(*receipt,true);
        }
        return {};
    }
    std::unique_ptr<PlayerReceipt> reserve_player_receipt(const Json& normalized) const {
        auto receipt=std::make_unique<PlayerReceipt>();receipt->params=normalized;return receipt;
    }
    PlayerReceipt& publish_player_receipt(std::unique_ptr<PlayerReceipt> receipt) noexcept {
        const auto index=player_receipt_next_;player_receipts_[index].swap(receipt);
        player_receipt_next_=(index+1)%player_receipts_.size();return *player_receipts_[index];
    }
    void freeze_player_control(PlayerReceipt& receipt) noexcept {
        receipt.control_revision=player_control_revision_;receipt.active=player_active();
        if(receipt.active) {
            receipt.ready=player_owner_->window->ready();receipt.paused=player_owner_->window->paused();
            receipt.state=!receipt.ready ? PlayerReceipt::State::initializing : receipt.paused ? PlayerReceipt::State::paused : PlayerReceipt::State::running;
        }else {
            receipt.ready=false;receipt.paused=player_owner_->observed_paused;receipt.state=PlayerReceipt::State::finished;
        }
        receipt.committed=true;
    }
    void player_control_guard(const Json& params) const {
        require(player_owner_ && identifier(params.at("player_id"))==player_owner_->id,"Player identity is not the retained owner.",-32004);
        require(revision(params.at("expected_control_revision"))==player_control_revision_,"Player control revision conflict.",-32009);
        require(player_active(),"The retained player is finished.",-32004);
        require(player_control_revision_<max_revision,"Player control revision exhausted.",-32009);
    }
    Json player_settings_info() const {
        require(player_owner_ && player_owner_->prepared.options.preferences,"No retained native player settings owner.",-32004);
        const auto& player=*player_owner_;
        auto settings=Json::parse(player.prepared.options.preferences->inspect());
        if(player.cached_report.contains("live_settings") && !player.cached_report.at("live_settings").is_null() &&
            player.cached_report.at("live_settings").at("revision")==settings.at("revision"))
            settings=player.cached_report.at("live_settings");
        else settings=player_live_settings(player.prepared.options,
            player.window ? player.window->report() : PlayerReport{},std::nullopt);
        return {{"player_id",player.id},{"session_id",player_active() ? runtime_id_ : player.observed_session},
            {"control_revision",player_control_revision_},{"settings",std::move(settings)}};
    }
    static void validate_next_player_preferences(const PlayerOptions& options,const PlayerPreferences::State& candidate) {
        try {validate_player_preference_graphics(candidate,preference_graphics_constraints(options));}
        catch(const PlayerPreferences::Error& error) {throw Error(error.code,error.what());}
    }
    Json player_dispatch(const std::string& method,const Json& params) {
        if(method=="player.settings.inspect") {
            fields(params,{"player_id"});
            if(params.contains("player_id"))require(player_owner_ && identifier(params.at("player_id"))==player_owner_->id,
                "Player identity is not retained.",-32004);
            return player_settings_info();
        }
        if(method=="player.settings.transact") {
            fields(params,{"player_id","request_id","expected_control_revision","expected_settings_revision","set","reset","preview"},
                {"player_id","request_id","expected_control_revision","expected_settings_revision"});
            identifier(params.at("player_id"));identifier(params.at("request_id"));revision(params.at("expected_control_revision"));
            const auto expected=revision(params.at("expected_settings_revision"));
            require(!params.contains("preview") || params.at("preview").is_boolean(),"Settings preview must be Boolean.");
            const bool preview=params.value("preview",false);
            Json values;
            try { values=player_settings::validate_values(params.value("set",Json::object())); }
            catch(const player_settings::SettingsError& error) { throw Error(error.code,error.what()); }
            const auto resets=params.value("reset",Json::array());
            const auto known=player_settings::values_schema().at("properties");
            require(resets.is_array() && resets.size()<=known.size(),"Player settings reset exceeds its ID budget.");
            std::set<std::string> reset;
            for(const auto& entry:resets) {
                require(entry.is_string() && known.contains(entry.get<std::string>()),"Unknown player settings reset ID.");
                const auto key=entry.get<std::string>();
                require(reset.insert(key).second && !values.contains(key),"Player settings resets must be unique and disjoint from set.");
            }
            auto normalized=params;normalized["method"]=method;normalized["set"]=values;normalized["reset"]=reset;normalized["preview"]=preview;
            if(auto retry=player_retry(normalized))return *retry;
            player_control_guard(params);
            require(!player_owner_->prepared.options.replay,"Recorded replay rejects intervening preference changes.",-32080);
            auto preferences=player_owner_->prepared.options.preferences;
            require(bool(preferences),"Native player preferences are unavailable.",-32003);
            std::shared_ptr<const PlayerPreferences::State> candidate;
            try { candidate=preferences->prepare(Json{{"expected_revision",expected},{"set",values},{"reset",reset}}.dump()); }
            catch(const PlayerPreferences::Error& error) { throw Error(error.code,error.what()); }
            validate_next_player_preferences(player_owner_->prepared.options,*candidate);
            const auto before=preferences->snapshot();
            auto projected=player_settings_info();
            projected["settings"]["revision"]=candidate->revision;
            projected["settings"]["values"]=Json::parse(candidate->requested_json);
            projected["settings"]["sources"]=Json::parse(candidate->sources_json);
            projected["settings"]["fields"]=Json::parse(candidate->fields_json);
            projected["previous_settings_revision"]=before->revision;
            projected["preview"]=preview;
            projected["changed"]=before->requested_json!=candidate->requested_json;
            projected["replayed"]=false;
            // Preview projects the candidate while retaining the actual current
            // application report. It publishes no state and retains no receipt.
            if(preview)return projected;
            projected["control_revision"]=player_control_revision_+1;
            projected["accepted"]=true;
            auto prepared=reserve_player_receipt(normalized);prepared->result=std::move(projected);prepared->committed=true;
            auto& receipt=publish_player_receipt(std::move(prepared));
            // The complete configuration and receipt exist before device work.
            // GPU attachments are not recreated. Native application can fail;
            // the accepted intent remains inspectable and the retry is bounded.
            preferences->publish(std::move(candidate));invalidate_player_control();
            try {
                player_owner_->window->synchronize();
                const auto native=player_owner_->window->report();
                const bool failed=player_owner_->window->finished() && !native.render.success;
                reconcile_player();
                if(failed)throw Error(-32020,native.render.detail);
                auto observed=player_settings_info();
                observed["previous_settings_revision"]=before->revision;observed["preview"]=false;
                observed["changed"]=receipt.result.at("changed");observed["replayed"]=false;observed["accepted"]=true;
                receipt.result=std::move(observed);
            }catch(const std::exception& error) {
                const auto detail=std::string(error.what()).substr(0,1024);
                receipt.failure=std::make_exception_ptr(Error(-32020,"Player preferences accepted; native application failed: "+detail,
                    {{"accepted",true},{"settings_revision",preferences->snapshot()->revision},{"control_revision",player_control_revision_}}));
            }
            return player_receipt_json(receipt,false);
        }
        if(method=="player.inspect") {
            fields(params,{"player_id"});
            if(params.contains("player_id")) {
                const auto id=identifier(params.at("player_id"));
                require(player_owner_ && id==player_owner_->id,"Player identity is not retained.",-32004);
            }
            return player_info();
        }
        if(method=="player.start") {
            require(params.is_object() && params.contains("expected_generation"),"Player start requires expected_generation.");
            const auto generation=revision(params.at("expected_generation"));
            require(!params.contains("initially_paused") || params.at("initially_paused").is_boolean(),"initially_paused must be Boolean.");
            auto launch=params;launch.erase("expected_generation");launch.erase("initially_paused");
            validate_player_parameters(launch);
            identifier(launch.at("session_id"));identifier(launch.at("request_id"));revision(launch.at("expected_tick"));
            identifier(launch.at("camera"));
            if(launch.contains("controller"))identifier(launch.at("controller"));
            // JSON numeric equality must not admit a malformed floating retry
            // where the wire requires an integer. This validation performs no I/O.
            for(const auto* key:{"expected_structure_revision","input_revision","settings_revision","max_frames","width","height","gpu","samples","frames_in_flight"})
                if(launch.contains(key))revision(launch.at(key));
            if(launch.contains("gamepad") && launch.at("gamepad").is_object() && launch.at("gamepad").contains("id"))revision(launch.at("gamepad").at("id"));
            if(launch.contains("sequence") && launch.at("sequence").is_array())for(const auto& segment:launch.at("sequence")) {
                require(segment.is_object() && segment.contains("ticks"),"Replay segment requires ticks.");
                revision(segment.at("ticks"));
                (void)parse_motions(segment.value("motions",Json::array()));
                (void)parse_sounds(segment.value("sounds",Json::array()));
            }
            auto normalized=params;normalized["method"]=method;
            if(auto replay=player_retry(normalized))return *replay;
            require(player_available(),"Native player service requires simulation and Vulkan player support.",-32003);
            require(generation==player_generation_,"Player generation conflict.",-32009);
            require(!player_active(),"Stop the active player before starting another window.",-32080);
            require(!performance_.recorder().status().admitting,"Stop performance capture before changing player owner.",-32080);
            require(player_generation_<max_revision,"Player generation exhausted.",-32009);
            auto prepared=prepare_player(launch);prepared.options.render.capture_exclusive=true;
            auto receipts=reserve_player_receipt(normalized);
            auto candidate=std::make_unique<PlayerOwner>();candidate->id=new_id();candidate->mode=launch.at("mode");
            candidate->prepared=std::move(prepared);candidate->observed_session=runtime_id_;
            candidate->session=std::make_unique<PlayerSessionAdapter>(*this);
            candidate->window=std::make_unique<PlayerWindow>(candidate->prepared.options,*candidate->session,params.value("initially_paused",false));
            candidate->observed_paused=candidate->window->paused();player_boundary(*candidate);
            // All admission/ack allocation precedes publication. GPU creation is deferred.
            Json ack={{"available",true},{"generation",player_generation_+1},{"player_id",candidate->id},
                {"control_revision",0},{"state","initializing"},{"active",true},{"ready",false},
                {"paused",candidate->observed_paused},{"mode",candidate->mode},{"session_id",runtime_id_},
                {"current_session_id",runtime_id_},{"tick",candidate->tick},{"structure_revision",candidate->structure_revision},
                {"ui_revision",candidate->ui_revision},{"control_sequence",candidate->control_sequence},{"report",nullptr},{"replayed",false}};
            receipts->result=ack;receipts->committed=true;
            player_owner_=std::move(candidate);++player_generation_;player_control_revision_=0;
            attach_gameplay_player_preferences(player_owner_->prepared.gameplay_preferences);
            publish_player_receipt(std::move(receipts));return ack;
        }
        if(method=="player.control") {
            fields(params,{"player_id","request_id","expected_control_revision","action"},
                {"player_id","request_id","expected_control_revision","action"});
            identifier(params.at("player_id"));identifier(params.at("request_id"));revision(params.at("expected_control_revision"));
            require(params.at("action")=="pause" || params.at("action")=="resume" || params.at("action")=="stop","Unknown player action.");
            auto normalized=params;normalized["method"]=method;
            if(auto replay=player_retry(normalized))return *replay;
            player_control_guard(params);auto prepared=reserve_player_receipt(normalized);
            prepared->result=player_info(false);prepared->control=true;
            // Retain a native command outcome before SDL/state changes or JSON formatting.
            auto& receipt=publish_player_receipt(std::move(prepared));
            try {
                if(params.at("action")=="stop")stop_player();
                else {
                    player_owner_->window->pause(params.at("action")=="pause");
                    player_owner_->observed_paused=player_owner_->window->paused();
                    invalidate_player_control();reconcile_player();
                }
            }catch(...) {receipt.failure=std::current_exception();}
            freeze_player_control(receipt);return player_receipt_json(receipt,false);
        }
        require(method=="player.capture","Unknown player service method.",-32601);
        fields(params,{"player_id","request_id","expected_control_revision","session_id","tick","path","expected_structure_revision","expected_ui_revision"},
            {"player_id","request_id","expected_control_revision","session_id","tick","path"});
        identifier(params.at("player_id"));identifier(params.at("request_id"));revision(params.at("expected_control_revision"));
        identifier(params.at("session_id"));revision(params.at("tick"));
        if(params.contains("expected_structure_revision"))revision(params.at("expected_structure_revision"));
        if(params.contains("expected_ui_revision"))revision(params.at("expected_ui_revision"));
        auto normalized=params;normalized["method"]=method;
        if(auto replay=player_retry(normalized))return *replay;
        player_control_guard(params);runtime_guard(params);
        require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        structure_mutation_guard(params);
        if(params.contains("expected_ui_revision"))require(revision(params.at("expected_ui_revision"))==runtime_->ui_model().revision(),"Runtime UI revision conflict.",-32009);
        require(player_owner_->window->ready(),"Player has no successful presentation yet.",-32003);
        const auto output=render_options(Json{{"path",params.at("path")}}).capture;
        const auto snapshot=player_snapshot(player_owner_->prepared.options,*player_owner_->session);
        auto prepared=reserve_player_receipt(normalized);prepared->result=player_info(false);
        prepared->result["capture"]={{"path",output},{"format","BMP"},{"camera",player_owner_->prepared.options.camera},
            {"camera_world",snapshot.camera_world},
            {"lens",{{"vertical_fov",snapshot.vertical_fov},{"near",snapshot.near_plane},{"far",snapshot.far_plane}}},
            {"object_count",snapshot.objects.size()},{"ui_revision",runtime_->ui_model().revision()},
            {"lighting",lighting_json(snapshot.lighting)},{"build_version",build_metadata().version}};
        auto& receipt=publish_player_receipt(std::move(prepared));
        try {
            auto capture=player_owner_->window->capture(output);
            require(capture.available,capture.detail,-32003);
            require(capture.success && capture.capture_written,capture.detail,-32020);
            static_assert(std::is_nothrow_move_constructible_v<RenderReport>);
            receipt.capture.emplace(std::move(capture));
        }catch(...) {receipt.failure=std::current_exception();}
        receipt.committed=true;
        // A formatter allocation failure still leaves the capture outcome retryable.
        reconcile_player();return player_receipt_json(receipt,false);
    }
    static bool player_runtime_mutation(const std::string& method) {
        if(method.starts_with("save."))return method!="save.inspect" && method!="save.status";
        if(!method.starts_with("runtime."))return false;
        // Runtime collector performs managed GC/unloading; it is not a pure read.
        constexpr std::array reads{"runtime.status","runtime.inspect","runtime.observe","runtime.entity","runtime.lighting",
            "runtime.raycast","runtime.components","runtime.component.get","runtime.component.query","runtime.gameplay.inspect",
            "runtime.template.get","runtime.template.query","runtime.instance","runtime.ui.inspect","runtime.audio.inspect","runtime.audio.voices",
            "runtime.save.status","runtime.save.result"};
        return std::find(reads.begin(),reads.end(),method)==reads.end();
    }
    void guard_player_request(const std::string& method) const {
        if(!player_active() || method.starts_with("player."))return;
        require(method!="runtime.start" && method!="runtime.stop" && method!="session.close" && method!="host.shutdown",
            "Stop the native player before changing its owner lifetime.",-32080);
        require(method!="runtime.play" && method!="runtime.audio.replay" && !method.ends_with(".capture"),
            "An attached player owns graphics/audio presentation; capture through player.capture.",-32080);
        if(player_runtime_mutation(method))require(!player_owner_->prepared.options.replay && player_owner_->window->paused(),
            "Pause an interactive player before runtime mutation; recorded replay forbids intervening mutations.",-32080);
    }
    void synchronize_player_request(const std::string& method,const Json& result) noexcept {
        if(!player_active() || !player_runtime_mutation(method))return;
        try {
            if(result.value("save_serviced",false))player_owner_->window->note_performance_intervention(frame_performance::Flag::storage);
            player_owner_->window->synchronize();
            if(method=="runtime.ui.activate" && player_active() && !player_owner_->window->finished() && result.contains("intent")) {
                const bool same_epoch=!result.at("runtime_replaced").get<bool>() && result.at("session_id")==runtime_id_ && result.at("current_session_id")==runtime_id_;
                const auto sequence=result.at("control_sequence").get<std::uint64_t>();
                if(same_epoch && sequence==runtime_->control_sequence() && result.at("current_control_sequence")==sequence && sequence>player_owner_->applied_ui_control_sequence) {
                    const auto intent=static_cast<RuntimeControlIntent>(result.at("intent").get<std::uint32_t>());
                    if(intent!=RuntimeControlIntent::none)player_owner_->window->pause(intent==RuntimeControlIntent::pause);
                    player_owner_->applied_ui_control_sequence=sequence;
                }
            }
            reconcile_player();
        }catch(...) {
            // An already accepted edit/save must retain its successful receipt.
            // Retry reconciliation on the next owner pump; never rerun the edit.
        }
    }
