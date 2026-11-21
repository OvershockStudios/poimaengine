// SPDX-License-Identifier: Apache-2.0
    std::shared_ptr<const wetness::State> wetness_live;
    std::optional<wetness::Dose> gameplay_wetness_dose;

    static bool rain_water_content(const RuntimeDefinition& definition) {
        return definition.water && !definition.water->rain_cells.empty();
    }

    void initialize_wetness(const RuntimeDefinition& definition) {
        wetness::State state;
        for(const auto& entity:definition.entities)
            if(entity.surface_wetness)state.receivers.try_emplace(entity.id);
        if(!state.receivers.empty() || rain_water_content(definition) || definition_has_rain(definition))
            wetness_live=std::make_shared<wetness::State>(std::move(state));
    }

    const wetness::Profile* wetness_profile(const std::string& id) const noexcept {
        const auto found=topology->definitions.find(id);
        if(found==topology->definitions.end() || !found->second.surface_wetness)return nullptr;
        return &*found->second.surface_wetness;
    }

    void validate_wetness_dose(const wetness::Dose& dose) const {
        require(bool(wetness_live), "Wetness needs an authored SurfaceWetness receiver, rain emitter, or rain-enabled water region.");
        wetness::validate(dose);
        require(!wetness_live->next_quantum || dose.first_quantum==*wetness_live->next_quantum,
            "Wetness dose must begin at the next unapplied game second.");
    }

    static int32_t POIMA_CALL stage_gameplay_wetness(void* context,uint64_t first_quantum,
        const uint32_t* rain_nm,uint32_t count,PoimaGameError* error) {
        return callback(error,[&] {
            auto& self=*static_cast<Impl*>(context);
            require(self.game_phase==GamePhase::tick,"Wetness can advance only during Update.");
            require(!self.gameplay_wetness_dose,"Gameplay can queue one wetness dose batch per tick.");
            require(rain_nm && count>0 && count<=wetness::max_batch,"Wetness needs 1..64 one-second rain doses.");
            wetness::Dose dose{first_quantum,std::vector<std::uint32_t>(rain_nm,rain_nm+count)};
            self.validate_wetness_dose(dose);
            self.gameplay_wetness_dose=std::move(dose);
        });
    }

    std::optional<std::string> rain_shelter(const std::string& id,
        const std::array<double,3>& origin) const {
        RuntimeRay ray;
        ray.origin=origin;
        ray.direction={0,1,0};
        ray.distance=wetness::shelter_distance_m;
        ray.ignore.push_back(id);
        const auto hit=physics_raycast(ray,{},true);
        return hit ? std::optional(hit->entity) : std::nullopt;
    }

    const wetness::Dose* selected_wetness_dose(const wetness::Dose* caller) const {
        require(!caller || !gameplay_wetness_dose,"Wetness cannot receive both caller and gameplay doses in one tick.");
        return caller ? caller : gameplay_wetness_dose ? &*gameplay_wetness_dose : nullptr;
    }

    std::optional<std::uint32_t> advance_wetness(const wetness::Dose* dose) {
        if(!dose)return {};
        profiling::Scope scope("runtime.wetness.advance",static_cast<std::int64_t>(tick));
        validate_wetness_dose(*dose);
        auto next=std::make_shared<wetness::State>(*wetness_live);
        for(auto& [id, receiver]:next->receivers) {
            const auto& profile=*wetness_profile(id);
            const auto& world=registry.get<Node>(find(id)).world;
            std::array<double,3> origin;
            for(std::size_t axis=0;axis<3;++axis)
                origin[axis]=world[12+axis]+world[axis]*profile.probe_local_m[0]+
                    world[4+axis]*profile.probe_local_m[1]+world[8+axis]*profile.probe_local_m[2];
            receiver.blocker=rain_shelter(id,origin);
            for(const auto rain:dose->rain_nm)
                receiver.film_nm_q16=wetness::advance(profile,receiver.film_nm_q16,rain,!receiver.blocker);
        }
        next->next_quantum=dose->first_quantum+dose->rain_nm.size();
        wetness_live=std::move(next);
        const auto latest_rain=dose->rain_nm.back();
        gameplay_wetness_dose.reset();
        return latest_rain;
    }

    std::optional<SceneWetness> scene_wetness(const std::string& id) const {
        if(!wetness_live)return {};
        const auto found=wetness_live->receivers.find(id);
        if(found==wetness_live->receivers.end() || !found->second.film_nm_q16)return {};
        const auto& profile=*wetness_profile(id);
        const auto capacity=std::uint64_t(profile.capacity_nm)*wetness::units_per_nm;
        const auto film=static_cast<float>(static_cast<double>(found->second.film_nm_q16)/static_cast<double>(capacity));
        return SceneWetness{1-film*profile.darkening,std::lerp(1.f,profile.roughness_factor,film)};
    }

    nlohmann::json archive_wetness() const {
        using Json=nlohmann::json;
        auto receivers=Json::array();
        for(const auto& [id,row]:wetness_live->receivers)
            receivers.push_back({{"entity",id},{"film_nm_q16",row.film_nm_q16},
                {"blocker",row.blocker ? Json(*row.blocker) : Json(nullptr)}});
        return {{"version",1},{"next_quantum",wetness_live->next_quantum ? Json(*wetness_live->next_quantum) : Json(nullptr)},
            {"receivers",std::move(receivers)}};
    }

    void restore_wetness(const nlohmann::json& archive);
    std::optional<std::string> restore_rain_shelter(const nlohmann::json& value,const std::string& source) const;
