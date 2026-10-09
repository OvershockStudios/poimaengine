// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. Gameplay reads committed UI; commands publish
// once after a successful callback, never through the presentation document.
    void clear_ui_commands() noexcept {
        ui_commands.clear();ui_modal.reset();ui_calls=ui_text_bytes=0;
    }
    void require_tick() const { require(game_phase==GamePhase::tick,"This service is available only during Tick."); }
    void require_callback() const { require(game_phase!=GamePhase::idle,"This service requires a gameplay callback."); }
    const ui::Element& ui_definition(const std::string& id) const {
        const auto& values=ui_model->definition();
        const auto found=std::lower_bound(values.begin(),values.end(),id,[](const auto& e,const auto& key){return e.id<key;});
        require(found!=values.end() && found->id==id,"Unknown UI element.");return *found;
    }
    static std::string ui_id(PoimaUiId id) { return gameplay_id(PoimaEntityId{id.high,id.low}); }
    static int32_t POIMA_CALL get_ui(void* context,const PoimaUiId* id,PoimaGameUiState* output,char* text,uint32_t capacity,uint32_t* required,PoimaGameError* error) {
        return callback(error,[&] {
            auto& self=*static_cast<Impl*>(context);self.require_callback();
            require(id && output && required && capacity<=16384 && (capacity==0 || text),"Invalid UI read pointers/capacity.");
            *output={};*required=0;const auto key=ui_id(*id);self.ui_definition(key);
            const auto state=self.ui_model->presentation();
            const auto row=std::lower_bound(state->elements.begin(),state->elements.end(),key,[](const auto& e,const auto& value){return e.element.id<value;});
            require(row!=state->elements.end() && row->element.id==key,"UI presentation membership differs from authority.");
            const auto& value=row->element;*required=static_cast<uint32_t>(value.text.size());
            *output={self.ui_model->revision(),static_cast<uint32_t>(value.kind),value.visible,value.enabled,
                row->effective_visible,row->effective_enabled,row->eligible,*required,0};
            if(capacity) {require(capacity>=*required,"UI text buffer is too small.");std::memcpy(text,value.text.data(),value.text.size());}
        });
    }
    static int32_t POIMA_CALL edit_ui(void* context,const PoimaGameUiPatch* patches,uint32_t count,const PoimaGameUiModalEdit* modal,PoimaGameError* error) {
        return callback(error,[&] {
            auto& self=*static_cast<Impl*>(context);self.require_callback();
            require(self.ui_calls++<4096,"Gameplay UI call budget exceeded.");
            require(count<=256 && (count==0 || patches) && (count!=0 || (modal && modal->change==1)),"Invalid or empty UI edit.");
            auto next=self.ui_commands;auto next_modal=self.ui_modal;std::set<std::string> seen;std::size_t bytes=0;
            for(uint32_t i=0;i<count;++i) {
                const auto& p=patches[i];const auto id=ui_id(p.id);self.ui_definition(id);
                require(seen.insert(id).second && p.mask>=1 && p.mask<=7 && p.visible<=1 && p.enabled<=1,"Invalid UI patch identity/mask/flags.");
                require((p.mask&1) ? (p.text_bytes<=16384 && (p.text_bytes==0 || p.text)) : (!p.text && p.text_bytes==0),"Invalid UI text encoding.");
                require((p.mask&2) || p.visible==0,"Unused UI visibility must be zero.");
                require((p.mask&4) || p.enabled==0,"Unused UI enabled flag must be zero.");
                auto& edit=next[id];edit.id=id;
                if(p.mask&1) {bytes+=p.text_bytes;edit.text=std::string(p.text ? p.text : "",p.text_bytes);}
                if(p.mask&2)edit.visible=p.visible!=0;
                if(p.mask&4)edit.enabled=p.enabled!=0;
            }
            require(bytes<=1024*1024-self.ui_text_bytes,"Gameplay UI text command budget exceeded.");
            if(modal) {
                require(modal->reserved==0 && modal->change<=1,"Invalid UI modal encoding.");
                if(modal->change) {
                    next_modal=(modal->id.high || modal->id.low) ? ui_id(modal->id) : std::string{};
                    if(!next_modal->empty())require(self.ui_definition(*next_modal).kind==ui::Kind::panel,"UI modal must name a panel.");
                }else require(modal->id.high==0 && modal->id.low==0,"Unchanged modal must have a zero ID.");
            }
            // Validate values and budgets immediately, but defer modal visibility
            // until the callback ends (hide + clear can be separate calls).
            ui::Definition validation;const auto current=self.ui_model->presentation();validation.reserve(current->elements.size());
            for(const auto& row:current->elements) {
                auto value=row.element;
                if(const auto found=next.find(value.id);found!=next.end()) {
                    const auto& edit=found->second;if(edit.text)value.text=*edit.text;if(edit.visible)value.visible=*edit.visible;if(edit.enabled)value.enabled=*edit.enabled;
                }
                validation.push_back(std::move(value));
            }
            ui::validate_definition(validation);
            self.ui_commands.swap(next);self.ui_modal.swap(next_modal);self.ui_text_bytes+=bytes;
        });
    }
    static int32_t POIMA_CALL get_control(void* context,PoimaGameUiControlEvent* output,PoimaGameError* error) {
        return callback(error,[&] {auto& self=*static_cast<Impl*>(context);require(output && self.game_phase==GamePhase::control,"No active Control event.");*output=self.control_event;});
    }
    static int32_t POIMA_CALL request_control(void* context,uint32_t intent,PoimaGameError* error) {
        return callback(error,[&] {
            auto& self=*static_cast<Impl*>(context);require(self.game_phase==GamePhase::control && (intent==1 || intent==2),"Invalid control phase/intent.");
            const auto next=static_cast<RuntimeControlIntent>(intent);
            require(self.control_intent==RuntimeControlIntent::none || self.control_intent==next,"Conflicting playback intents in one Control callback.");self.control_intent=next;
        });
    }
    PoimaGameInstanceServicesV1 services() {
        PoimaGameInstanceServicesV1 result{};
        result.navigation={{{{{7,sizeof(PoimaGameInstanceServicesV1),this,&get_entity,&cast_ray,&move_body,&sound_event,&get_animation,&set_animation,
            &save_info,&save_request,&save_result,&component_query,&component_get,&component_set,&entity_alive,
            &spawn_entity,&despawn_entity,&template_component_get,&get_ui,&edit_ui,&get_control,&request_control},
            &get_animation_extended,&set_animation_extended},&get_animation_layer,&set_animation_layer},&set_character_input},&query_navigation};
        result.instance_node=&resolve_instance_node;return result;
    }
    std::shared_ptr<ui::Model> prepare_ui_commands() const {
        if(ui_commands.empty() && !ui_modal)return {};
        std::vector<ui::Edit> edits;edits.reserve(ui_commands.size());for(const auto& [id,value]:ui_commands) {(void)id;edits.push_back(value);}
        auto candidate=std::make_shared<ui::Model>(*ui_model);candidate->edit(ui_model->revision(),edits,ui_modal);return candidate;
    }
    RuntimeControlResult control(std::uint64_t expected_ui,std::uint64_t expected_sequence,const std::string& id) {
        require(game_phase==GamePhase::idle && !save_queue.pending(),"Resolve pending work before Control.");
        require(game && expected_ui==ui_model->revision() && expected_sequence==control_sequence,"Control requires a loaded game and current UI/control revisions.");
        require(control_sequence<9007199254740991ULL,"Control sequence exhausted.");
        require(game_revision<9007199254740991ULL,"Gameplay revision limit reached.");
        const auto& definition=ui_definition(id);const auto state=ui_model->inspect();
        const auto found=std::find_if(state.begin(),state.end(),[&](const auto& row){return row.id==id;});
        require(found!=state.end() && found->eligible,"UI action is not logically eligible.");
        const auto save_checkpoint=save_queue;auto game_checkpoint=game->state();
        clear_ui_commands();game_character_inputs.clear();control_intent=RuntimeControlIntent::none;control_event={};
        const auto numeric=gameplay_id(id);control_event.element={numeric.high,numeric.low};control_event.sequence=control_sequence+1;
        control_event.action_bytes=static_cast<uint32_t>(definition.action.size());std::copy(definition.action.begin(),definition.action.end(),control_event.action);
        try {
            game_phase=GamePhase::control;const auto api=services();game->control(api.navigation.character.animation.animation.baseline,tick);game_phase=GamePhase::idle;
            game->validate_entity_references([](void* context,PoimaEntityId entity){return static_cast<RuntimeComponents*>(context)->alive(entity);},components.get());
            auto candidate=prepare_ui_commands();
            require(!save_queue.pending() || save_queue.commit(tick),"Cannot commit Control save intent.");
            if(candidate)ui_model.swap(candidate);
            // Control can change TState at an unchanged tick. Invalidate old
            // gameplay drafts as well as ordered control/save observations.
            ++game_revision;++control_sequence;
            const RuntimeControlResult result{control_sequence,ui_model->revision(),control_intent};
            clear_ui_commands();game_character_inputs.clear();control_event={};control_intent=RuntimeControlIntent::none;return result;
        }catch(...) {
            game_phase=GamePhase::idle;save_queue=save_checkpoint;game->state().swap(game_checkpoint);
            clear_ui_commands();game_character_inputs.clear();control_event={};control_intent=RuntimeControlIntent::none;throw;
        }
    }
