// SPDX-License-Identifier: Apache-2.0
// Included in World. Reads use the current committed native particle state.
    Json inspect_particles(const Json& params) const {
        fields(params, {"session_id", "tick", "after", "limit"}, {"session_id", "tick"});
        runtime_guard(params);
        const auto tick = runtime_->inspect().tick;
        require(revision(params.at("tick")) == tick, "Runtime tick conflict.", -32009);
        const auto limit = params.contains("limit") ? revision(params.at("limit")) : 32;
        require(limit >= 1 && limit <= 64, "Particle observation page limit must be 1..64.");
        std::uint64_t after = 0;
        if (params.contains("after")) {
            const auto& cursor = params.at("after");
            require(cursor.is_string(), "Particle cursor must be a decimal ID string.");
            const auto& text = cursor.get_ref<const std::string&>();
            require(!text.empty() && text.size() <= 20 && text[0] >= '1' && text[0] <= '9',
                "Particle cursor must be a nonzero decimal ID without leading zeroes.");
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), after);
            require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "Particle cursor is outside the ID range.");
        }
        const auto observed = runtime_->particle_observation();
        const auto counts = [](const particles::Counts& c) {
            return Json{{"live", c.live}, {"spawned", c.spawned}, {"dropped", c.dropped}, {"expired", c.expired}, {"removed", c.removed}};
        };
        auto sources = Json::array();
        for (const auto& source : observed.sources) {
            sources.push_back({{"id", std::to_string(source.id)}, {"entity", source.entity}, {"draining", source.draining},
                {"position_m", source.status.pose.position_m}, {"rotation", source.status.pose.rotation},
                {"emitting", source.status.emitting}, {"counts", counts(source.status.particles)}});
        }
        auto instances = Json::array();
        Json next = nullptr;
        std::uint64_t last = 0;
        for (const auto& instance : observed.instances) {
            if (instance.id <= after) continue;
            if (instances.size() == limit) { next = std::to_string(last);break; }
            instances.push_back({{"id", std::to_string(instance.id)}, {"emitter", std::to_string(instance.emitter)},
                {"position_m", instance.position_m}, {"velocity_m_s", instance.velocity_m_s}, {"age_ticks", instance.age_ticks},
                {"lifetime_ticks", instance.lifetime_ticks}, {"size_m", instance.size_m}, {"linear_rgba", instance.linear_rgba}});
            last = instance.id;
        }
        return {{"session_id", runtime_id_}, {"tick", tick}, {"active", observed.active},
            {"counts", counts(observed.status.particles)}, {"sources", std::move(sources)},
            {"particles", std::move(instances)}, {"next_after", std::move(next)}};
    }
