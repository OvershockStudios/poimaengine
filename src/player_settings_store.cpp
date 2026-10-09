// SPDX-License-Identifier: Apache-2.0
#include "player_settings_store.hpp"
#include "world_storage.hpp"
#include "asset_store.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <set>
#include <string_view>
#include <vector>

namespace poima::player_settings {
namespace {
constexpr std::uint64_t max_revision=9007199254740991ULL;
constexpr std::size_t max_bytes=64*1024,max_receipts=32;
constexpr std::array<const char*,9> ids{
    "camera.vertical_fov","input.sensitivity_x","input.sensitivity_y",
    "input.invert_x","input.invert_y","ui.scale","graphics.samples","graphics.frames_in_flight","audio.master_gain"};
constexpr std::array<const char*,5> suffixes{"",".lock",".pending",".previous",".previous.pending"};
void require(bool condition,const char* message,int code=-32602) {
    if(!condition)throw SettingsError(code,message);
}
void fields(const Json& value,std::initializer_list<const char*> names) {
    require(value.is_object() && value.size()==names.size(),"Settings object has missing or unknown fields.");
    for(const auto* name:names)require(value.contains(name),"Settings object has missing or unknown fields.");
}
std::uint64_t revision(const Json& value) {
    require(value.is_number_integer(),"Settings revision must be a safe nonnegative JSON integer.");
    if(value.is_number_unsigned()) {
        const auto result=value.get<std::uint64_t>();
        require(result<=max_revision,"Settings revision exceeds the safe JSON integer range.");
        return result;
    }
    const auto result=value.get<std::int64_t>();
    require(result>=0 && static_cast<std::uint64_t>(result)<=max_revision,"Settings revision exceeds the safe JSON integer range.");
    return static_cast<std::uint64_t>(result);
}
std::string request_id(const Json& value) {
    require(value.is_string(),"Settings request ID must be a string.");
    const auto& result=value.get_ref<const std::string&>();
    require(result.size()==32 && std::all_of(result.begin(),result.end(),[](char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    }),"Settings request ID must contain 32 lowercase hexadecimal characters.");
    return result;
}
bool known_id(const std::string& id) {
    return std::find(ids.begin(),ids.end(),id)!=ids.end();
}
Json normalized_params(const Json& source) {
    require(source.is_object() && source.size()<=5,"Settings transaction must be a bounded object.");
    for(const auto& item:source.items())require(item.key()=="request_id" || item.key()=="expected_revision" ||
        item.key()=="set" || item.key()=="reset" || item.key()=="preview","Unknown settings transaction field.");
    require(source.contains("request_id") && source.contains("expected_revision"),"Settings transaction needs request_id and expected_revision.");
    const auto request=request_id(source.at("request_id"));
    const auto expected=revision(source.at("expected_revision"));
    const auto values=source.contains("set") ? validate_values(source.at("set")) : Json::object();
    std::set<std::string> reset;
    if(source.contains("reset")) {
        const auto& input=source.at("reset");
        require(input.is_array() && input.size()<=ids.size(),"Settings reset exceeds the registry ID count.");
        for(const auto& item:input) {
            require(item.is_string(),"Settings reset IDs must be strings.");
            const auto& id=item.get_ref<const std::string&>();
            require(known_id(id),"Unknown settings reset ID.");
            require(reset.insert(id).second,"Settings reset IDs must be unique.");
            require(!values.contains(id),"A settings ID cannot be set and reset in one transaction.");
        }
    }
    bool preview=false;
    if(source.contains("preview")) {
        require(source.at("preview").is_boolean(),"Settings preview must be boolean.");
        preview=source.at("preview").get<bool>();
    }
    return {{"request_id",request},{"expected_revision",expected},{"set",values},{"reset",reset},{"preview",preview}};
}
Json merged_values(Json values,const Json& params) {
    for(const auto& id:params.at("reset"))values.erase(id.get<std::string>());
    for(const auto& item:params.at("set").items())values[item.key()]=item.value();
    return values;
}
std::string values_hash(const Json& values) { return content_hash(values.dump()); }
fs::path sidecar(const fs::path& path,const char* suffix) { return fs::path(path).concat(suffix); }
fs::path absolute_path(const fs::path& path) {
    require(!path.empty(),"Settings path must not be empty.");
    const auto& native=path.native();
    require(native.find(fs::path::value_type{})==fs::path::string_type::npos,"Settings path cannot contain NUL.");
    for(const auto& part:path)require(part!=fs::path(".."),"Settings paths cannot contain parent traversal.");
    return fs::absolute(path);
}
bool regular_or_missing(const fs::path& path) {
    std::error_code error;const auto status=fs::symlink_status(path,error);
    require(!error || error==std::errc::no_such_file_or_directory,"Cannot inspect settings path.",-32070);
    require(!fs::is_symlink(status),"Settings files and sidecars cannot be symbolic links.",-32070);
    if(!fs::exists(status))return false;
    require(fs::is_regular_file(status),"Settings files and sidecars must be regular files.",-32070);
    require(fs::hard_link_count(path)==1,"Settings files and sidecars cannot have hard-link aliases.",-32070);
    return true;
}
void guard_paths(const fs::path& path,bool owns_lock=false) {
    require(path.filename().u8string().ends_with(u8".poima-settings.json"),"Settings filename must end with .poima-settings.json.");
    for(auto parent=path.parent_path();!parent.empty();) {
        std::error_code error;const auto status=fs::symlink_status(parent,error);
        require(!error && fs::is_directory(status) && !fs::is_symlink(status),"Settings parent directories must exist and cannot be symbolic links.",-32070);
        const auto next=parent.parent_path();if(next==parent)break;parent=next;
    }
    for(const auto* suffix:suffixes) {
#ifdef _WIN32
        // The validated Windows lock is held with share=0. Inspecting its link
        // count through another handle would fail; replacement is also denied.
        if(owns_lock && std::string_view(suffix)==".lock")continue;
#else
        (void)owns_lock;
#endif
        (void)regular_or_missing(sidecar(path,suffix));
    }
}
std::string read_bytes(const fs::path& path) {
    require(regular_or_missing(path),"Settings file does not exist.",-32070);
    const auto size=fs::file_size(path);
    require(size<=max_bytes,"Settings file exceeds the 64 KiB limit.",-32070);
    std::ifstream file(path,std::ios::binary);
    require(bool(file),"Cannot open settings file.",-32070);
    std::string result(static_cast<std::size_t>(size),'\0');
    file.read(result.data(),static_cast<std::streamsize>(result.size()));
    require(file.gcount()==static_cast<std::streamsize>(result.size()) && file.peek()==std::char_traits<char>::eof() && !file.bad(),
        "Settings file changed size or failed while reading.",-32070);
    return result;
}
std::string bounded_message(const char* prefix,const std::exception& error) {
    std::string result=prefix;
    const std::string_view message(error.what());
    // Storage diagnostics are bounded and ASCII-safe even when OS paths or
    // parser excerpts contain invalid UTF-8. No caller value is echoed whole.
    for(const auto c:message.substr(0,384))result.push_back(c>=32 && c<=126 ? c : '?');
    return result;
}
Json parse_document(const std::string& bytes) {
    try {
        std::vector<std::set<std::string>> keys;
        const auto callback=[&](int depth,Json::parse_event_t event,Json& parsed) {
            require(depth<=16,"Settings JSON exceeds the nesting limit.");
            if(event==Json::parse_event_t::object_start)keys.emplace_back();
            else if(event==Json::parse_event_t::key)require(!keys.empty() && keys.back().insert(parsed.get<std::string>()).second,"Duplicate settings JSON field.");
            else if(event==Json::parse_event_t::object_end)keys.pop_back();
            return true;
        };
        auto doc=Json::parse(bytes,callback);
        fields(doc,{"format","revision","values","receipts"});
        require(doc.at("format")=="poima.settings.v1","Unsupported settings format; no automatic migration was performed.");
        const auto current=revision(doc.at("revision"));
        doc["values"]=validate_values(doc.at("values"));
        auto& receipts=doc.at("receipts");
        require(receipts.is_array() && receipts.size()==std::min<std::uint64_t>(current,max_receipts),"Invalid settings receipt history.");
        std::set<std::string> requests;
        auto expected=current-static_cast<std::uint64_t>(receipts.size());
        Json previous=Json::object();bool have_previous=expected==0;
        for(auto& receipt:receipts) {
            fields(receipt,{"params","result"});
            const auto params=normalized_params(receipt.at("params"));
            require(params.at("preview")==false,"Preview requests cannot be persisted as settings receipts.");
            require(requests.insert(request_id(params.at("request_id"))).second,"Duplicate settings receipt ID.");
            require(revision(params.at("expected_revision"))==expected,"Invalid settings receipt sequence.");
            auto& result=receipt.at("result");
            fields(result,{"values","revision","previous_revision","changed","preview","persisted","replayed","content_hash","application","state"});
            require(revision(result.at("previous_revision"))==expected && revision(result.at("revision"))==++expected,"Settings receipt revisions are inconsistent.");
            require(result.at("changed").is_boolean() && result.at("preview")==false && result.at("persisted")==true &&
                result.at("replayed")==false && result.at("application")=="next_player" && result.at("state")=="stored_intent","Invalid settings receipt metadata.");
            result["values"]=validate_values(result.at("values"));
            require(result.at("content_hash")==values_hash(result.at("values")),"Settings receipt hash differs from its values.");
            if(have_previous) {
                require(merged_values(previous,params)==result.at("values"),"Settings receipt values do not match its patch.");
                require(result.at("changed").get<bool>()==(previous!=result.at("values")),"Settings receipt change flag is inconsistent.");
            } else {
                for(const auto& item:params.at("set").items())require(result.at("values").contains(item.key()) && result.at("values").at(item.key())==item.value(),"Settings receipt set value is inconsistent.");
                for(const auto& id:params.at("reset"))require(!result.at("values").contains(id.get<std::string>()),"Settings receipt reset value remains present.");
            }
            previous=result.at("values");have_previous=true;receipt["params"]=params;
        }
        require(!receipts.empty() ? doc.at("values")==previous : doc.at("values").empty(),"Settings values differ from the committed receipt history.");
        return doc;
    }catch(const std::exception& error) {
        throw SettingsError(-32070,bounded_message("Invalid settings; existing bytes preserved: ",error));
    }
}
struct Document { bool persisted=false;std::string bytes;Json value; };
Document read_document(const fs::path& path) {
    Document result;result.persisted=regular_or_missing(path);
    if(result.persisted) {result.bytes=read_bytes(path);result.value=parse_document(result.bytes);}
    else result.value={{"format","poima.settings.v1"},{"revision",0},{"values",Json::object()},{"receipts",Json::array()}};
    return result;
}
Json summary(const Document& doc) {
    return {{"format","poima.settings.v1"},{"values",doc.value.at("values")},{"revision",doc.value.at("revision")},
        {"persisted",doc.persisted},{"content_hash",values_hash(doc.value.at("values"))},{"application","next_player"},{"state","stored_intent"}};
}
void unchanged(const fs::path& path,const Document& doc,bool owns_lock=false) {
    guard_paths(path,owns_lock);
    require(regular_or_missing(path)==doc.persisted && (!doc.persisted || read_bytes(path)==doc.bytes),"Settings changed outside this transaction; inspect and retry.",-32009);
}
Json prepare_transaction(const Document& doc,const Json& params,bool& replayed) {
    for(const auto& receipt:doc.value.at("receipts"))if(receipt.at("params").at("request_id")==params.at("request_id")) {
        require(receipt.at("params")==params,"Settings request ID reused with different parameters.",-32010);
        auto result=receipt.at("result");result["replayed"]=true;replayed=true;return result;
    }
    const auto current=revision(doc.value.at("revision"));
    require(revision(params.at("expected_revision"))==current,"Settings revision conflict; inspect and retry.",-32009);
    require(current<max_revision,"Settings revision limit reached.");
    const bool preview=params.at("preview").get<bool>();
    const auto values=merged_values(doc.value.at("values"),params);
    Json result={{"values",values},{"revision",preview ? current : current+1},{"previous_revision",current},
        {"changed",values!=doc.value.at("values")},{"preview",preview},{"persisted",!preview},{"replayed",false},
        {"content_hash",values_hash(values)},{"application","next_player"},{"state",preview ? "preview_intent" : "stored_intent"}};
    if(preview)result["proposed_revision"]=current+1;
    return result;
}
template<class F> auto storage_errors(F&& work) -> decltype(work()) {
    try {return work();}
    catch(const SettingsError&) {throw;}
    catch(const std::exception& error) {throw SettingsError(-32070,bounded_message("Settings storage operation failed: ",error));}
}
Loaded loaded(const Document& doc) {
    require(doc.persisted,"Requested settings file does not exist.",-32070);
    return {doc.value.at("values"),revision(doc.value.at("revision")),values_hash(doc.value.at("values"))};
}
}

Json validate_values(const Json& values) {
    require(values.is_object() && values.size()<=ids.size(),"Settings values must be a sparse object bounded by the registry ID count.");
    Json result=Json::object();
    for(const auto& item:values.items()) {
        const auto& id=item.key();const auto& value=item.value();
        require(known_id(id),"Unknown player setting ID.");
        if(id=="input.invert_x" || id=="input.invert_y") {
            require(value.is_boolean(),"Input inversion settings must be boolean.");result[id]=value;
        } else if(id=="graphics.samples" || id=="graphics.frames_in_flight") {
            require(value.is_number_integer(),"Graphics settings require integer enumerants.");
            const int alternate=id=="graphics.samples" ? 4 : 2;
            require(value==1 || value==alternate,"Unsupported graphics setting enumerant.");
            result[id]=value==1 ? 1 : alternate;
        } else {
            require(value.is_number(),"FOV, sensitivity, UI scale and master gain settings must be numeric.");
            const auto number=value.get<double>();
            const double minimum=id=="camera.vertical_fov" ? 5 : id=="ui.scale" ? .25 : 0;
            const double maximum=id=="camera.vertical_fov" ? 150 : id=="ui.scale" ? 8 : id=="audio.master_gain" ? 1 : 10;
            require(std::isfinite(number) && number>=minimum && number<=maximum,"Numeric player setting is nonfinite or outside its bounds.");
            result[id]=number==0 ? 0.0 : number;
        }
    }
    return result;
}
Json values_schema() {
    Json properties=Json::object();
    properties["camera.vertical_fov"]={{"type","number"},{"minimum",5},{"maximum",150}};
    for(const auto* id:{"input.sensitivity_x","input.sensitivity_y"})properties[id]={{"type","number"},{"minimum",0},{"maximum",10}};
    for(const auto* id:{"input.invert_x","input.invert_y"})properties[id]={{"type","boolean"}};
    properties["ui.scale"]={{"type","number"},{"minimum",.25},{"maximum",8}};
    properties["graphics.samples"]={{"type","integer"},{"enum",{1,4}}};
    properties["graphics.frames_in_flight"]={{"type","integer"},{"enum",{1,2}}};
    properties["audio.master_gain"]={{"type","number"},{"minimum",0},{"maximum",1}};
    return {{"type","object"},{"additionalProperties",false},{"maxProperties",ids.size()},{"properties",properties}};
}
Json describe() {
    const auto schema=values_schema();
    const Json defaults={{"camera.vertical_fov",60.0},{"input.sensitivity_x",.1},{"input.sensitivity_y",.1},
        {"input.invert_x",false},{"input.invert_y",false},{"ui.scale",1.0},{"graphics.samples",4},{"graphics.frames_in_flight",2},{"audio.master_gain",1.0}};
    Json settings=Json::array();
    for(const auto* id:ids) {
        const std::string_view name(id);
        const auto source=name.starts_with("camera.") ? "authored_camera" : name.starts_with("input.") ? "input_profile" : name.starts_with("ui.") ? "window_density" : "engine_default";
        const auto unit=name=="camera.vertical_fov" ? "degrees" : name.starts_with("input.sensitivity_") ? "degrees per relative mouse unit" : name=="ui.scale" ? "physical pixels per logical UI pixel" : "dimensionless";
        settings.push_back({{"id",id},{"version",1},{"schema",schema.at("properties").at(id)},
            {"default",defaults.at(id)},{"default_source",source},{"units",unit},{"application","next_player"}});
        if(name=="audio.master_gain") {
            settings.back()["live_application"]="output_sink_only";
            settings.back()["scope"]="Player output master gain; does not change emitters, propagation, mixer state or saves.";
        }
    }
    return {{"format","poima.settings.v1"},{"values_schema",schema},{"settings",settings},{"defaults",defaults},
        {"application","next_player"},{"state","stored_intent"},
        {"override_policy","Sparse IDs override inherited player inputs; reset removes an override. Registry defaults are fallback metadata, not effective values."},
        {"revision_policy","Every accepted non-preview transaction increments revision, including unchanged values. Preview writes nothing. The latest 32 successful receipts survive restart; matching receipts replay before the stale-revision check."},
        {"canonical_params","Omitted set/reset/preview normalize to empty/empty/false. Numeric values normalize by type; reset IDs sort. Same request ID with different normalized parameters rejects."},
        {"content_hash_scope","SHA256 of canonical compact sparse values JSON; excludes revision and receipts."},
        {"persistence",{{"filename_suffix",".poima-settings.json"},{"max_bytes",max_bytes},{"max_receipts",max_receipts},
            {"backup_suffix",".previous"},{"automatic_recovery",false},{"read_only_sidecars",false},
            {"locking","Cooperative OS-held document writer lock; not an adversarial filesystem-race boundary."},
            {"durability","Flushed staging and previous copy, then atomic name replacement; power-loss directory durability is not promised."}}}};
}
Json inspect(const fs::path& input) {
    return storage_errors([&]() -> Json {
        const auto path=absolute_path(input);guard_paths(path);
        if(!regular_or_missing(path))return summary(read_document(path));
        world_detail::WriterLock lock(sidecar(path,".lock"));guard_paths(path,true);
        return summary(read_document(path));
    });
}
Json inspect_read_only(const fs::path& input) {
    return storage_errors([&]() -> Json {const auto path=absolute_path(input);guard_paths(path);return summary(read_document(path));});
}
Loaded load(const fs::path& input) {
    return storage_errors([&]() -> Loaded {
        const auto path=absolute_path(input);guard_paths(path);
        require(regular_or_missing(path),"Requested settings file does not exist.",-32070);
        world_detail::WriterLock lock(sidecar(path,".lock"));guard_paths(path,true);
        return loaded(read_document(path));
    });
}
Loaded load_read_only(const fs::path& input) {
    return storage_errors([&]() -> Loaded {const auto path=absolute_path(input);guard_paths(path);return loaded(read_document(path));});
}
Json transact(const fs::path& input,const Json& source) {
    return storage_errors([&]() -> Json {
        const auto params=normalized_params(source);const auto path=absolute_path(input);guard_paths(path);
        if(params.at("preview").get<bool>()) {
            const auto doc=read_document(path);bool replayed=false;
            auto result=prepare_transaction(doc,params,replayed);unchanged(path,doc);return result;
        }
        world_detail::WriterLock lock(sidecar(path,".lock"));guard_paths(path,true);
        const auto doc=read_document(path);bool replayed=false;
        auto result=prepare_transaction(doc,params,replayed);if(replayed)return result;
        auto candidate=doc.value;candidate["values"]=result.at("values");candidate["revision"]=result.at("revision");
        auto& receipts=candidate["receipts"];
        if(receipts.size()==max_receipts)receipts.erase(receipts.begin());
        receipts.push_back({{"params",params},{"result",result}});
        const auto bytes=candidate.dump()+'\n';require(bytes.size()<=max_bytes,"Settings and receipts would exceed 64 KiB.");
        unchanged(path,doc,true);world_detail::write_flushed(sidecar(path,".pending"),bytes);unchanged(path,doc,true);
        if(doc.persisted) {
            world_detail::write_flushed(sidecar(path,".previous.pending"),doc.bytes);unchanged(path,doc,true);
            world_detail::replace_file(sidecar(path,".previous.pending"),sidecar(path,".previous"));
        }
        unchanged(path,doc,true);world_detail::replace_file(sidecar(path,".pending"),path);
        return result;
    });
}
}
