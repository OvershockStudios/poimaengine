// SPDX-License-Identifier: Apache-2.0
// Control receipts belong to the owner and survive runtime replacement.
    static Json control_json(const RuntimeControlOutcome& r,bool replayed) {
        Json result={{"session_id",epoch_text(r.source)},{"tick",r.tick},{"control_sequence",r.control_sequence},
            {"ui_revision",r.ui_revision},{"intent",static_cast<std::uint32_t>(r.intent)},{"replayed",replayed},
            {"current_session_id",epoch_text(r.current)},{"current_tick",r.current_tick},
            {"current_control_sequence",r.current_control_sequence},{"current_ui_revision",r.current_ui_revision},
            {"runtime_replaced",r.replaced},{"save_serviced",r.save_serviced}};
        if(r.operation)result["save_operation"]=gameplay_save_json(*r.operation);
        return result;
    }
    void resolve_control_save(RuntimeControlOutcome& r) {
        if(!runtime_ || !r.operation || (r.operation->state!=GameplaySaveState::queued && r.operation->state!=GameplaySaveState::resolving))return;
        const auto current=gameplay_id(runtime_id_);
        if(GameplaySaveEpoch{current.high,current.low}!=r.source)return;
        const auto ticket=r.operation->request.ticket;
        const auto* pending=runtime_->gameplay_saves().pending();
        if(!pending || pending->ticket!=ticket)return;
        // Resolve only the already accepted operation. No callback or fixed
        // simulation step runs, and no newer session can inherit this retry.
        service_gameplay_save();
        r.operation=runtime_->gameplay_saves().query(ticket,&gameplay_save_ledger_);
        const auto after=gameplay_id(runtime_id_);r.current={after.high,after.low};
        r.current_tick=runtime_->inspect().tick;r.current_control_sequence=runtime_->control_sequence();
        r.current_ui_revision=runtime_->ui_model().revision();r.replaced=r.current!=r.source;
    }
    Json control_dispatch(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","expected_ui_revision","expected_control_sequence",
            "expected_gameplay_revision","expected_structure_revision","id"},
            {"session_id","request_id","expected_tick","expected_ui_revision","expected_control_sequence","expected_gameplay_revision","id"});
        const auto session=identifier(params.at("session_id")),request=identifier(params.at("request_id")),element=identifier(params.at("id"));
        const auto tick=revision(params.at("expected_tick")),ui_revision=revision(params.at("expected_ui_revision"));
        const auto sequence=revision(params.at("expected_control_sequence")),gameplay_revision=revision(params.at("expected_gameplay_revision"));
        if(params.contains("expected_structure_revision"))revision(params.at("expected_structure_revision"));
        auto normalized=params;normalized["method"]="runtime.ui.activate";
        // Representation validation precedes equality: JSON integer/float
        // equality must not let malformed retries bypass the public schema.
        for(const auto& receipt:control_receipts_)if(receipt && receipt->params.at("session_id")==session && receipt->params.at("request_id")==request) {
            require(receipt->params==normalized,"Runtime request ID reused with different parameters.",-32010);
            resolve_control_save(receipt->outcome);
            return control_json(receipt->outcome,true);
        }
        for(const auto& receipt:structure_receipts_)if(receipt && receipt->params.at("session_id")==session && receipt->params.at("request_id")==request)
            throw Error(-32010,"Runtime request ID already belongs to a structural transaction.");
        for(const auto& receipt:advance_receipts_)if(receipt && receipt->params.at("session_id")==session && receipt->params.at("request_id")==request)
            throw Error(-32010,"Runtime request ID already belongs to a simulation advance.");
        runtime_guard(params);
        for(const auto& receipt:runtime_receipts_)require(receipt.at("params").at("request_id")!=request,"Runtime request ID already belongs to another operation.",-32010);
        structure_mutation_guard(params);
        require(tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(ui_revision==runtime_->ui_model().revision(),"UI revision conflict.",-32009);
        require(sequence==runtime_->control_sequence(),"Control sequence conflict.",-32009);
        require(gameplay_revision==runtime_->gameplay_revision(),"Gameplay revision conflict.",-32009);
        // Allocate the complete request identity before running user code. The
        // outcome below consists only of bounded value types, never JSON.
        auto prepared=std::make_unique<ControlReceipt>();prepared->params=std::move(normalized);
        auto& outcome=prepared->outcome;outcome.source=save_epoch(session);outcome.current=outcome.source;
        outcome.tick=tick;outcome.current_tick=tick;
        RuntimeControlResult accepted;
        prepare_gameplay_player_preferences_boundary();
        try { accepted=runtime_->control(ui_revision,sequence,element); }
        catch(const std::runtime_error& error) { throw Error(-32040,error.what()); }
        outcome.control_sequence=accepted.control_sequence;outcome.ui_revision=accepted.ui_revision;outcome.intent=accepted.intent;
        outcome.current_control_sequence=accepted.control_sequence;outcome.current_ui_revision=accepted.ui_revision;
        const auto index=control_receipt_next_;control_receipts_[index].swap(prepared);
        control_receipt_next_=(index+1)%control_receipts_.size();
        auto& retained=control_receipts_[index]->outcome;
        // The accepted callback and owner receipt precede preference publication.
        // Flush before queued storage can destroy its original Runtime.
        flush_gameplay_player_preferences();
        // Remember acceptance before irreversible storage and response building.
        // A Load may replace runtime_, but cannot replace this owner's receipt.
        if(const auto* queued=runtime_->gameplay_saves().pending()) {
            const auto ticket=queued->ticket;retained.save_serviced=true;
            service_gameplay_save();
            retained.operation=runtime_->gameplay_saves().query(ticket,&gameplay_save_ledger_);
        }
        const auto current=gameplay_id(runtime_id_);retained.current={current.high,current.low};
        retained.current_tick=runtime_->inspect().tick;retained.current_control_sequence=runtime_->control_sequence();
        retained.current_ui_revision=runtime_->ui_model().revision();retained.replaced=retained.current!=retained.source;
        return control_json(retained,false);
    }
