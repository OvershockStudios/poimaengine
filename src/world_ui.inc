// SPDX-License-Identifier: Apache-2.0
// Logical UI state is authoritative even without a window or RmlUi.
#include "world_control.inc"
    Json ui_runtime_info() const {
        return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"ui_revision",runtime_->ui_model().revision()},{"control_sequence",runtime_->control_sequence()}};
    }
    Json ui_dispatch(const std::string& method,const Json& params) {
        require(params.is_object() && params.contains("session_id"),"Runtime UI requests require session_id.");
        runtime_guard(params);
        const auto& model=runtime_->ui_model();
        if(method=="runtime.ui.inspect") {
            fields(params,{"session_id","tick","ui_revision","after","limit"},{"session_id","tick"});
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            if(params.contains("ui_revision"))require(revision(params.at("ui_revision"))==model.revision(),"UI revision conflict.",-32009);
            require(!params.contains("after") || params.contains("ui_revision"),"UI pagination requires ui_revision.");
            const auto after=params.contains("after") ? identifier(params.at("after")) : std::string{};
            const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;
            require(limit>=1 && limit<=256,"UI page limit must be 1..256.");
            auto result=ui_runtime_info();result["modal"]=model.modal().empty() ? Json(nullptr) : Json(model.modal());
            result["elements"]=Json::array();result["next_after"]=nullptr;
            const auto state=model.inspect();const auto& definitions=model.definition();
            const auto authored=Json::parse(ui::definition_json(definitions));
            for(std::size_t i=0;i<state.size();++i) {
                const auto& value=state[i];if(value.id<=after)continue;
                if(result["elements"].size()==limit) {result["next_after"]=result["elements"].back().at("id");break;}
                const auto& definition=definitions[i];
                result["elements"].push_back({{"id",value.id},{"parent",definition.parent.empty() ? Json(nullptr) : Json(definition.parent)},
                    {"name",definition.name},{"kind",definition.kind==ui::Kind::panel ? "panel" : definition.kind==ui::Kind::label ? "label" : "button"},
                    {"action",definition.action.empty() ? Json(nullptr) : Json(definition.action)},{"text",value.text},
                    {"visible",value.visible},{"enabled",value.enabled},{"effective_visible",value.effective_visible},
                    {"effective_enabled",value.effective_enabled},{"eligible",value.eligible}});
                for(const auto* key:{"layout","style"})if(authored.at(value.id).contains(key))
                    result["elements"].back()[key]=authored.at(value.id).at(key);
            }
            return result;
        }
        require(method=="runtime.ui.edit","Unknown runtime UI operation.",-32601);
        fields(params,{"session_id","request_id","expected_tick","expected_ui_revision","edits","modal"},
            {"session_id","request_id","expected_tick","expected_ui_revision","edits"});
        const auto request=identifier(params.at("request_id"));
        const auto expected_tick=revision(params.at("expected_tick")),expected_ui=revision(params.at("expected_ui_revision"));
        const auto& changes=params.at("edits");require(changes.is_array() && changes.size()<=256,"UI edit accepts at most 256 patches.");
        std::vector<ui::Edit> edits;edits.reserve(changes.size());
        for(const auto& change:changes) {
            fields(change,{"id","text","visible","enabled"},{"id"});ui::Edit edit;edit.id=identifier(change.at("id"));
            if(change.contains("text")) {require(change.at("text").is_string(),"UI text must be a string.");edit.text=change.at("text").get<std::string>();}
            if(change.contains("visible")) {require(change.at("visible").is_boolean(),"UI visibility must be Boolean.");edit.visible=change.at("visible").get<bool>();}
            if(change.contains("enabled")) {require(change.at("enabled").is_boolean(),"UI enabled state must be Boolean.");edit.enabled=change.at("enabled").get<bool>();}
            edits.push_back(std::move(edit));
        }
        std::optional<std::string> modal;
        if(params.contains("modal"))modal=params.at("modal").is_null() ? std::string{} : identifier(params.at("modal"));
        auto normalized=params;normalized["method"]=method;
        for(const auto& receipt:runtime_receipts_)if(receipt.at("params").at("request_id")==request) {
            require(receipt.at("params")==normalized,"Runtime request ID reused with different parameters.",-32010);
            auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        for(const auto& receipt:advance_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_ && receipt->params.at("request_id")==request)
            throw Error(-32010,"Runtime request ID already belongs to a simulation advance.");
        require(expected_tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(expected_ui==model.revision(),"UI revision conflict.",-32009);
        require(expected_ui<max_revision,"UI revision limit reached.");
        // Prepare the complete retry receipt before publication. No response
        // allocation after the native commit is needed to remember the result.
        auto result=ui_runtime_info();result["ui_revision"]=expected_ui+1;result["replayed"]=false;
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());
        receipts.push_back({{"params",normalized},{"result",result}});
        component_checked([&]{runtime_->ui_edit(expected_ui,edits,std::move(modal));});
        runtime_receipts_.swap(receipts);return result;
    }
