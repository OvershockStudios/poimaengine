// SPDX-License-Identifier: Apache-2.0
// Owner-only post-batch coordination. Gameplay callbacks never enter this code.
    static GameplaySaveEpoch save_epoch(const std::string& id) {
        const auto value=gameplay_id(id);return {value.high,value.low};
    }
    static std::string epoch_text(GameplaySaveEpoch epoch) { return gameplay_id(PoimaEntityId{epoch.high,epoch.low}); }
    void configure_runtime_saves(Runtime& runtime) {
        runtime.gameplay_save_host(save_epoch(new_id()),&gameplay_save_ledger_);
        runtime.gameplay_player_preferences_host(gameplay_player_preferences_);
        require(runtime.gameplay_saves().configure(!save_root_.empty(),save_configuration_generation_),"Cannot configure gameplay save host.");
    }
    static Json gameplay_save_json(const GameplaySaveResult& result) {
        const auto& r=result.request;const auto& c=result.completion;
        return {{"epoch",epoch_text(r.ticket.epoch)},{"sequence",r.ticket.sequence},{"kind",static_cast<unsigned>(r.kind)},
            {"state",static_cast<unsigned>(result.state)},{"requested_tick",r.requested_tick},{"committed_tick",r.committed_tick},
            {"generation",c.generation},{"recovered",c.recovered},{"error_code",c.error_code},{"diagnostic",c.diagnostic.data()},
            {"restored_epoch",c.restored_epoch.valid() ? Json(epoch_text(c.restored_epoch)) : Json(nullptr)},{"restored_tick",c.restored_tick}};
    }
    void finish_gameplay_save(const GameplaySaveRequest& request,const GameplaySaveCompletion& completion) noexcept {
        const auto published=gameplay_save_ledger_.complete(request,completion);
        // All fields were validated before irreversible effects. A violation is
        // an engine bug; never discard an operation whose outcome is unknown.
        if(published==GameplaySavePublication::recorded || published==GameplaySavePublication::replayed) {
            if(runtime_->gameplay_saves().epoch()==request.ticket.epoch)runtime_->gameplay_saves().clear(request.ticket);
            pending_gameplay_write_.reset();
        } else runtime_->gameplay_saves().mark_resolving(-32070,"Internal save result validation failed; operation retained.");
    }
    void resolve_gameplay_write(const GameplaySaveRequest& request) noexcept {
        if(!pending_gameplay_write_)return;
        GameplaySaveCompletion completion;
        try {
            const auto& pending=*pending_gameplay_write_;
            const auto observed=saves::Store(pending.path).observe_receipt(pending.operation,pending.hash);
            if(observed.receipt) { completion.succeeded=true;completion.generation=observed.receipt->committed.generation; }
            else if(observed.current_generation<=pending.expected_generation ||
                (observed.oldest_retained_generation>0 && pending.expected_generation+1>=observed.oldest_retained_generation)) {
                completion.error_code=pending.error_code;completion.diagnostic=pending.diagnostic;
            } else {
                runtime_->gameplay_saves().mark_resolving(-32070,"Save receipt is outside retained history; publication cannot be determined.");return;
            }
            finish_gameplay_save(request,completion);
        }catch(const std::exception& error) { runtime_->gameplay_saves().mark_resolving(-32070,error.what()); }
        catch(...) { runtime_->gameplay_saves().mark_resolving(-32070,"Save receipt verification failed; operation retained."); }
    }
    void service_gameplay_save() noexcept {
        profiling::Scope trace("save.service");
        const auto* pending=runtime_->gameplay_saves().pending();if(!pending || !pending->committed)return;
        const auto request=*pending;
        if(pending_gameplay_write_) { resolve_gameplay_write(request);return; }
        GameplaySaveCompletion completion;
        try {
            require(request.configuration_generation==save_configuration_generation_,"Gameplay save storage configuration changed.",-32009);
            const std::string slot(request.slot.data());const auto location=save_store_path(slot);saves::Store store(location);
            const auto observed=store.inspect();
            const auto generation=request.has_expected_generation ? request.expected_generation : observed.generation;
            if(request.kind==GameplaySaveKind::save) {
                const auto bytes=runtime_save_bytes();
                const auto identity=epoch_text(request.ticket.epoch)+":"+std::to_string(request.ticket.sequence);
                const auto operation=content_hash(identity).substr(0,32);
                const auto hash=content_hash(Json{{"ticket",identity},{"slot",slot},{"generation",generation},{"tick",request.committed_tick},
                    {"content",runtime_content_hash_}}.dump());
                // Prepare retained identity before entering a possibly published
                // write. Uncertain outcomes are resolved without a new snapshot.
                PendingGameplayWrite retained;retained.path=location;retained.operation=operation;retained.hash=hash;retained.expected_generation=generation;
                retained.error_code=-32070;gameplay_save_diagnostic(retained.diagnostic,"Save write failed before publication.");
                pending_gameplay_write_=std::move(retained);
                try {
                    const auto written=store.write(generation,operation,bytes,false,hash);
                    completion.succeeded=true;completion.generation=written.committed.generation;
                }catch(const std::exception& error) {
                    const auto* storage=dynamic_cast<const saves::Error*>(&error);
                    pending_gameplay_write_->error_code=storage && storage->kind==saves::ErrorKind::conflict ? -32009 : -32070;
                    gameplay_save_diagnostic(pending_gameplay_write_->diagnostic,error.what());
                    resolve_gameplay_write(request);return;
                }
            } else {
                auto load=prepare_runtime_load(slot,generation,request.allow_recovery,new_id(),runtime_gameplay_config_);
                completion.succeeded=true;completion.generation=load.storage.selected->generation;completion.recovered=load.storage.recovered;
                completion.restored_epoch=load.runtime->gameplay_saves().epoch();completion.restored_tick=load.runtime->inspect().tick;
                // Completion is fixed-size and cannot allocate. Publish metadata
                // immediately before the no-throw owner swap, on this one thread.
                const auto published=gameplay_save_ledger_.complete(request,completion);
                if(published!=GameplaySavePublication::recorded)throw Error(-32070,"Cannot publish gameplay load completion.");
                activate_runtime_load(load);return;
            }
        }catch(const std::exception& error) {
            completion={};const auto* typed=dynamic_cast<const Error*>(&error);const auto* storage=dynamic_cast<const saves::Error*>(&error);
            completion.error_code=typed ? typed->code : storage && storage->kind==saves::ErrorKind::conflict ? -32009 : -32070;
            gameplay_save_diagnostic(completion.diagnostic,error.what());
        }catch(...) { completion={};completion.error_code=-32070;gameplay_save_diagnostic(completion.diagnostic,"Gameplay save operation failed."); }
        // If an unexpected exception escaped after write entry, its publication
        // status still must be resolved before any terminal result is recorded.
        if(pending_gameplay_write_ && !completion.succeeded) { resolve_gameplay_write(request);return; }
        finish_gameplay_save(request,completion);
    }
    RuntimeAdvance advance_runtime(std::uint32_t ticks,const std::vector<RuntimeInput>& inputs,
        const std::vector<KinematicTarget>& motions={},const std::vector<SoundCommand>& sounds={},const std::vector<AnimationCommand>& animations={}) {
        require(bool(runtime_),"No runtime is active.",-32030);
        profiling::SessionScope trace_session(runtime_id_);
        profiling::Scope trace_advance("owner.advance",static_cast<std::int64_t>(runtime_->inspect().tick));
        const bool previously_pending=runtime_->gameplay_saves().pending()!=nullptr;
        std::optional<GameplaySaveResult> previous_operation;
        if(previously_pending) {
            const auto previous_ticket=runtime_->gameplay_saves().pending()->ticket;
            service_gameplay_save();
            previous_operation=runtime_->gameplay_saves().query(previous_ticket,&gameplay_save_ledger_);require(!runtime_->gameplay_saves().pending(),"Save publication is unresolved; no simulation tick advanced.",-32070);
        }
        RuntimeAdvance result;result.save_serviced=previously_pending;result.operation=previous_operation;result.source=save_epoch(runtime_id_);result.current=result.source;
        result.previous_tick=runtime_->inspect().tick;result.committed_tick=result.previous_tick+ticks;result.current_tick=result.committed_tick;result.stepped=ticks;
        result.first_voice=runtime_->sound_state().next_id();
        prepare_gameplay_player_preferences_boundary();
        try { runtime_->step(ticks,inputs,motions,sounds,animations); }
        catch(const std::runtime_error& error) { throw Error(-32040,error.what()); }
        // Publish only after the complete native batch succeeds, before a
        // queued Load can replace the Runtime that staged the preference.
        flush_gameplay_player_preferences();
        result.next_voice=runtime_->sound_state().next_id();
        if(const auto* queued=runtime_->gameplay_saves().pending()) {
            const auto ticket=queued->ticket;result.save_serviced=true;service_gameplay_save();
            result.operation=runtime_->gameplay_saves().query(ticket,&gameplay_save_ledger_);
        }
        // Existing session strings are validated UUIDs, so this conversion does
        // not allocate or fail. RuntimeAdvance itself is a fixed-size value.
        const auto current=gameplay_id(runtime_id_);result.current={current.high,current.low};
        result.current_tick=runtime_->inspect().tick;result.replaced=result.current!=result.source;return result;
    }
    static Json advance_json(const RuntimeAdvance& r,bool replayed=false) {
        Json result={{"session_id",epoch_text(r.source)},{"previous_tick",r.previous_tick},{"tick",r.committed_tick},{"stepped",r.stepped},{"replayed",replayed},
            {"sound_events",{{"first_voice",r.first_voice},{"next_voice",r.next_voice}}},{"current_session_id",epoch_text(r.current)},
            {"current_tick",r.current_tick},{"runtime_replaced",r.replaced},{"save_serviced",r.save_serviced}};
        if(r.operation)result["save_operation"]=gameplay_save_json(*r.operation);
        return result;
    }
