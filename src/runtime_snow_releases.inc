// SPDX-License-Identifier: Apache-2.0
    using SnowFootObservations=std::array<RuntimeFootPlantObservation,snow::max_held_releases>;

    static std::string_view snow_rig_view(const snow::FootReleaseIntent& intent) {
        return {intent.rig.data(),intent.rig.size()};
    }

    static const RuntimeGroundingState* snow_foot_state(std::span<const RuntimeFootPlantObservation> feet,
        const snow::FootReleaseIntent& intent) {
        const auto key=std::pair(snow_rig_view(intent),intent.slot);
        const auto at=std::ranges::lower_bound(feet,key,{},[](const RuntimeFootPlantObservation& foot) {
            return std::pair(foot.rig,foot.slot);
        });
        return at!=feet.end() && at->rig==key.first && at->slot==key.second ? at->state : nullptr;
    }

    bool matches_snow_hold(const snow::FootReleaseIntent& intent,const RuntimeGroundingState* state) const {
        if(!state || state->decision_tick!=tick || !state->planting)return false;
        const auto& plant=*state->planting;
        if(plant.status!="locked" || !plant.anchor || plant.anchor->acquired_tick!=intent.acquired_tick ||
            plant.anchor->support!=snow_binding->definition().tiles[intent.tile] || plant.anchor->triangle!=intent.triangle)
            return false;
        const auto& point=plant.anchor->local_point;
        return snow_binding->quantize_center(point[0],point[2])==intent.center &&
            (!intent.heading_bin || snow::quantize_sole_heading(plant.anchor->local_forward)==intent.heading_bin);
    }

    static void add_snow_release_count(std::uint64_t& counter,std::size_t count=1) noexcept {
        constexpr std::uint64_t wire_max=9007199254740991ull;
        counter+=std::min<std::uint64_t>(count,wire_max-counter);
    }

    void prune_snow_holds() {
        if(!snow_releases || !snow_releases->held_count)return;
        auto& ledger=*snow_releases;
        SnowFootObservations feet;
        const auto observations=std::span(feet).first(animations->foot_plant_observations(feet));
        const auto end=ledger.holding.begin()+ledger.held_count;
        const auto kept=std::remove_if(ledger.holding.begin(),end,[&](const auto& intent) {
            return !matches_snow_hold(intent,snow_foot_state(observations,intent));
        });
        add_snow_release_count(ledger.counts.cancelled,static_cast<std::size_t>(end-kept));
        ledger.held_count=static_cast<std::size_t>(kept-ledger.holding.begin());
    }

    std::optional<snow::FootReleaseIntent> acquired_snow_foot(const RuntimeFootPlantObservation& foot) const {
        const auto& state=*foot.state;
        if(state.decision_tick!=tick || state.status!="contact" || !state.cast || !state.hit || !state.planting)
            return {};
        const auto& plant=*state.planting;
        if(plant.status!="locked" || !plant.anchor || plant.anchor->acquired_tick!=tick ||
            !plant.position_error || *plant.position_error<0 ||
            *plant.position_error>snow_binding->definition().foot_release->sole_tolerance_m || !plant.target_forward)
            return {};
        const auto& anchor=*plant.anchor;
        const auto& tiles=snow_binding->definition().tiles;
        const auto support=std::ranges::find(tiles,anchor.support);
        if(support==tiles.end() || !anchor.triangle || anchor.support!=state.hit->entity ||
            anchor.triangle!=state.hit->triangle || foot.rig.size()!=snow::RigKey{}.size() ||
            foot.rig.find_first_not_of("0123456789abcdef")!=std::string_view::npos ||
            foot.rig.find_first_not_of('0')==std::string_view::npos)
            return {};
        const auto tile=static_cast<unsigned>(support-tiles.begin());
        const auto center=snow_binding->quantize_center(anchor.local_point[0],anchor.local_point[2]);
        if(!snow::release_center_in_triangle(tile,*anchor.triangle,center))return {};
        std::optional<std::uint8_t> heading;
        if(snow_binding->definition().foot_release->sole) {
            heading=snow::quantize_sole_heading(anchor.local_forward);
            if(!heading)return {};
        }
        return snow::FootReleaseIntent{snow::release_rig_key(foot.rig),foot.slot,tick,*anchor.triangle,tile,center,0,heading};
    }

    void record_snow_foot_decisions() {
        if(!snow_releases)return;
        auto& ledger=*snow_releases;
        SnowFootObservations feet;
        const auto observations=std::span(feet).first(animations->foot_plant_observations(feet));
        std::array<snow::FootReleaseIntent,snow::max_held_releases> released;
        std::size_t released_count=0;
        std::size_t kept=0;
        // Before the final solve, prune_snow_holds matched these rows to the
        // immediately preceding lock. Only this tick's explicit lift releases it.
        for(std::size_t i=0;i<ledger.held_count;++i) {
            auto intent=ledger.holding[i];
            const auto* state=snow_foot_state(observations,intent);
            if(matches_snow_hold(intent,state))ledger.holding[kept++]=intent;
            else if(state && state->decision_tick==tick && state->status=="lifted" && state->planting &&
                state->planting->status=="lifted" && !state->planting->anchor) {
                intent.released_tick=tick;
                released[released_count++]=intent;
            } else add_snow_release_count(ledger.counts.cancelled);
        }
        ledger.held_count=kept;
        // All newly released rows share the same release tick and follow existing ones.
        std::ranges::sort(std::span(released).first(released_count),{},[](const auto& intent) {
            return std::tie(intent.acquired_tick,intent.rig,intent.slot);
        });
        const auto admitted=std::min(released_count,ledger.ready.size()-ledger.ready_count);
        std::copy_n(released.begin(),admitted,ledger.ready.begin()+ledger.ready_count);
        ledger.ready_count+=admitted;
        add_snow_release_count(ledger.counts.overflow,released_count-admitted);
        for(const auto& foot:observations)if(const auto intent=acquired_snow_foot(foot)) {
            require(ledger.held_count<ledger.holding.size(),"Snow foot-release holding budget exceeded.");
            ledger.holding[ledger.held_count++]=*intent;
        }
        if(ledger.held_count!=kept)
            std::ranges::sort(std::span(ledger.holding).first(ledger.held_count),{},[](const auto& intent) {
                return std::tie(intent.rig,intent.slot);
            });
    }

    void apply_ready_snow_foot() {
        if(!snow_releases || !snow_releases->ready_count)return;
        auto& ledger=*snow_releases;
        const auto pinned=held_snow_samples();
        for(std::size_t i=0;i<ledger.ready_count;++i) {
            const auto& intent=ledger.ready[i];
            if(snow_binding->footprint_changes_samples(snow_live->state,intent.center,intent.heading_bin,pinned))
                continue;
            auto compacted=snow_live->state;
            const auto dirty=snow_binding->apply_footprint(compacted,intent.center,intent.heading_bin);
            const bool changed=publish_snow_compaction(std::move(compacted),dirty);
            add_snow_release_count(changed ? ledger.counts.applied : ledger.counts.saturated);
            std::move(ledger.ready.begin()+i+1,ledger.ready.begin()+ledger.ready_count,ledger.ready.begin()+i);
            --ledger.ready_count;
            return;
        }
    }

    void validate_restored_snow_foot_releases() const {
        if(!snow_releases)return;
        const auto& ledger=*snow_releases;
        SnowFootObservations feet;
        const auto observations=std::span(feet).first(animations->foot_plant_observations(feet));
        for(const auto& intent:std::span(ledger.holding).first(ledger.held_count))
            require(matches_snow_hold(intent,snow_foot_state(observations,intent)),
                "Saved snow holding intent differs from its live planted foot.");
        for(const auto& intent:std::span(ledger.ready).first(ledger.ready_count)) {
            const auto rig=snow_rig_view(intent);
            const auto live=topology->definitions.find(std::string(rig));
            if(live==topology->definitions.end()) {
                require(entity_ids->allocated(gameplay_id(std::string(rig))),
                    "Saved released snow foot has no allocated rig identity.");
            } else {
                const auto& rig_definition=live->second.animation_rig;
                require(rig_definition && std::ranges::any_of(rig_definition->ik,[&](const auto& chain) {
                    return chain.slot==intent.slot && chain.grounding && chain.grounding->plant_lock;
                }),"Saved released snow foot differs from its live rig slot.");
                if(const auto* state=snow_foot_state(observations,intent)) {
                    const auto& plant=state->planting;
                    if(plant && plant->status=="locked" && plant->anchor)
                        require(intent.released_tick<plant->anchor->acquired_tick,
                            "Saved released snow foot overlaps a live planted foot.");
                    if(state->decision_tick==intent.released_tick)
                        require(state->status=="lifted" && plant && plant->status=="lifted" && !plant->anchor,
                            "Saved released snow foot differs from its committed lift decision.");
                }
            }
        }
    }
