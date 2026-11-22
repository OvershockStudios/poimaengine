// SPDX-License-Identifier: Apache-2.0
    bool has_solar_target=false;
    std::optional<SolarLightingSample> solar_lighting_live;
    std::optional<SolarLightingSample> staged_solar_lighting;

    static bool authored_solar_target(const RuntimeDefinition& definition) {
        const auto environment=std::ranges::find_if(definition.entities,[](const auto& entity) {
            return entity.environment.has_value();
        });
        if(environment==definition.entities.end() || !environment->environment->sky.enabled ||
            environment->environment->sky.sun.empty())return false;
        const auto sun=std::ranges::find(definition.entities,environment->environment->sky.sun,&RuntimeEntityDefinition::id);
        return sun!=definition.entities.end() && sun->light && sun->light->enabled && sun->light->kind==LightKind::directional;
    }

    void validate_solar_sample(const SolarLightingSample& sample) const {
        require(has_solar_target,"Solar lighting requires an enabled procedural sky linked to an enabled directional light.");
        validate_solar_lighting(sample);
    }

    static int32_t POIMA_CALL stage_solar_lighting(void* context,const PoimaGameSolarLightingV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            auto& self=*static_cast<Impl*>(context);
            require(self.game_phase==GamePhase::tick,"Solar lighting can change only during Update.");
            require(!self.staged_solar_lighting,"Gameplay can stage one solar lighting sample per tick.");
            require(source && source->version==1 && source->bytes==sizeof(PoimaGameSolarLightingV1),
                "Invalid solar lighting command layout.");
            SolarLightingSample sample;
            sample.world_milliseconds=source->world_milliseconds;
            std::copy_n(source->toward_sun,3,sample.toward_sun.begin());
            sample.sun_gain=source->sun_gain;
            std::copy_n(source->sun_tint,3,sample.sun_tint.begin());
            sample.sky_gain=source->sky_gain;
            std::copy_n(source->ambient,3,sample.ambient.begin());
            self.validate_solar_sample(sample);
            self.staged_solar_lighting=sample;
        });
    }
