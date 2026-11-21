// SPDX-License-Identifier: Apache-2.0
    static wetness::Dose parse_wetness_dose(const Json& raw) {
        fields(raw,{"first_quantum","rain_nm"},{"first_quantum","rain_nm"});
        wetness::Dose dose;
        dose.first_quantum=revision(raw.at("first_quantum"));
        require(dose.first_quantum<=wetness::max_quantum,"Wetness first_quantum exceeds the supported clock.");
        const auto& rain=raw.at("rain_nm");
        require(rain.is_array() && !rain.empty() && rain.size()<=wetness::max_batch,
            "Wetness rain_nm needs 1..64 one-second doses.");
        require(rain.size()-1<=wetness::max_quantum-dose.first_quantum,
            "Wetness dose extends beyond the last complete game second.");
        dose.rain_nm.reserve(rain.size());
        for(const auto& sample:rain) {
            const auto amount=revision(sample);
            require(amount<=wetness::max_rain_nm,"Each wetness dose must be within 0..1000000 nanometres.");
            dose.rain_nm.push_back(static_cast<std::uint32_t>(amount));
        }
        return dose;
    }

    Json inspect_wetness(const Json& params) const {
        fields(params,{"session_id","tick","structure_revision","entity"},{"session_id","tick","structure_revision"});
        runtime_guard(params);
        structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        const auto entity=params.contains("entity") ? identifier(params.at("entity")) : std::string{};
        const auto* state=runtime_->wetness_state();
        auto receivers=Json::array();
        if(state)for(const auto& [id,row]:state->receivers) {
            if(!entity.empty() && entity!=id)continue;
            const auto* profile=runtime_->wetness_profile(id);
            require(profile,"Wetness receiver has no frozen profile.",-32040);
            receivers.push_back({{"entity",id},{"film_nm_q16",row.film_nm_q16},{"capacity_nm",profile->capacity_nm},
                {"blocker",row.blocker ? Json(*row.blocker) : Json(nullptr)},
                {"exposed",state->next_quantum ? Json(!row.blocker) : Json(nullptr)}});
        }
        return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"next_quantum",state && state->next_quantum ? Json(*state->next_quantum) : Json(nullptr)},
            {"receivers",std::move(receivers)}};
    }
