// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. No candidate is published by a callback or by
// an individual Tick in a multi-tick batch. Only the serialized host flushes a
// successfully validated native boundary before queued storage/replacement.
    void clear_player_preferences_commands() noexcept { player_preferences_prepared.reset(); }
    void validate_player_preferences_boundary() const {
        if(!player_preferences_prepared)return;
        const auto host=player_preferences_host.lock();
        require(host && host->can_commit(*player_preferences_prepared),
            "Player preference owner changed before callback boundary commit.");
    }
    static int32_t POIMA_CALL get_player_preferences(void* context,
        PoimaGamePreferenceSnapshotV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            auto& self=*static_cast<Impl*>(context);self.require_callback();
            require(output && output->version==1 && output->bytes==sizeof(*output) && output->reserved==0,
                "Invalid preference snapshot output header.");
            const auto host=self.player_preferences_host.lock();
            const auto value=host ? host->snapshot() : GameplayPlayerPreferences::unavailable_snapshot();
            *output=value;
        });
    }
    static int32_t POIMA_CALL patch_player_preferences(void* context,
        const PoimaGamePreferencePatchV1* input,PoimaGamePreferenceEnqueueV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            auto& self=*static_cast<Impl*>(context);self.require_callback();
            require(input && output && output->version==1 && output->bytes==sizeof(*output) && output->reserved==0,
                "Invalid preference patch input/output header.");
            // Copy before writing output; no caller pointer survives this call.
            const auto patch=*input;
            GameplayPlayerPreferences::validate_patch_encoding(patch);
            const auto host=self.player_preferences_host.lock();
            if(!host) { *output=GameplayPlayerPreferences::unavailable_enqueue();return; }
            auto result=host->prepare(patch,bool(self.player_preferences_prepared));
            // Expected typed rejections do not discard an earlier valid stage.
            if(result.prepared)self.player_preferences_prepared=std::move(result.prepared);
            *output=result.enqueue;
        });
    }
    static int32_t POIMA_CALL get_player_preference_result(void* context,
        const PoimaGamePreferenceTicket* input,PoimaGamePreferenceResultV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            auto& self=*static_cast<Impl*>(context);self.require_callback();
            require(input && output && output->version==1 && output->bytes==sizeof(*output) && output->reserved==0,
                "Invalid preference result input/output header.");
            const auto ticket=*input;
            require((ticket.high || ticket.low) && ticket.sequence!=0,"Invalid preference ticket encoding.");
            const auto host=self.player_preferences_host.lock();
            *output=host ? host->query(ticket,self.player_preferences_prepared.get()) :
                GameplayPlayerPreferences::unavailable_result(ticket);
        });
    }
