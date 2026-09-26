// SPDX-License-Identifier: Apache-2.0
#include "input_profile_store.hpp"
#include "world_storage.hpp"
#include "asset_store.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>

namespace poima::input_profiles {
namespace {
constexpr std::uint64_t max_revision=9007199254740991ULL;
constexpr std::size_t max_bytes=64*1024,max_receipts=32;
constexpr std::array<const char*,6> action_names{"forward","backward","left","right","jump","use"};
constexpr std::array<const char*,5> suffixes{"",".lock",".pending",".previous",".previous.pending"};
void require(bool value,const std::string& message,int code=-32602) { if(!value)throw ProfileError(code,message); }
void fields(const Json& value,std::initializer_list<const char*> names) {
    require(value.is_object(),"Expected an input profile object.");
    require(value.size()==names.size(),"Input profile object has missing or unknown fields.");
    for(const auto* name:names)require(value.contains(name),std::string("Missing input profile field: ")+name);
}
std::uint64_t revision(const Json& value) {
    require(value.is_number_integer(),"Input revision must be a safe nonnegative JSON integer.");
    if(value.is_number_unsigned()) { const auto result=value.get<std::uint64_t>();require(result<=max_revision,"Input revision exceeds the safe JSON integer range.");return result; }
    const auto result=value.get<std::int64_t>();require(result>=0 && std::uint64_t(result)<=max_revision,"Input revision exceeds the safe JSON integer range.");return std::uint64_t(result);
}
std::string request_id(const Json& value) {
    require(value.is_string(),"Input request ID must be a string.");const auto result=value.get<std::string>();
    require(result.size()==32 && std::all_of(result.begin(),result.end(),[](char c) { return (c>='0' && c<='9') || (c>='a' && c<='f'); }),"Input request ID must contain 32 lowercase hexadecimal characters.");return result;
}
InputProfile parse_profile(const Json& value) {
    fields(value,{"bindings","sensitivity_x","sensitivity_y","invert_x","invert_y"});
    const auto& bindings=value.at("bindings");fields(bindings,{"forward","backward","left","right","jump","use"});
    InputProfile result;
    for(std::size_t i=0;i<action_names.size();++i) {
        const auto& ids=bindings.at(action_names[i]);require(ids.is_array() && ids.size()<=4,"Each action permits zero to four input controls.");
        for(const auto& id:ids) { require(id.is_string(),"Input control ID must be a string.");result.bindings[i].push_back(id.get<std::string>()); }
    }
    for(const auto* name:{"sensitivity_x","sensitivity_y"})require(value.at(name).is_number(),"Mouse sensitivity must be numeric.");
    for(const auto* name:{"invert_x","invert_y"})require(value.at(name).is_boolean(),"Mouse axis inversion must be boolean.");
    result.sensitivity_x=value.at("sensitivity_x").get<double>();result.sensitivity_y=value.at("sensitivity_y").get<double>();
    result.invert_x=value.at("invert_x").get<bool>();result.invert_y=value.at("invert_y").get<bool>();
    try { validate_input_profile(result); }catch(const std::exception& e) { throw ProfileError(-32602,e.what()); }
    return result;
}
Json normalized_params(const Json& source) {
    require(source.is_object(),"Input transaction must be an object.");
    auto result=source;if(!result.contains("preview"))result["preview"]=false;
    fields(result,{"request_id","expected_revision","profile","preview"});
    (void)request_id(result.at("request_id"));(void)revision(result.at("expected_revision"));
    require(result.at("preview").is_boolean(),"Input preview must be boolean.");
    result["profile"]=profile_json(parse_profile(result.at("profile")));return result;
}
fs::path sidecar(const fs::path& path,const char* suffix) { return fs::path(path).concat(suffix); }
bool regular_or_missing(const fs::path& path) {
    std::error_code error;const auto status=fs::symlink_status(path,error);
    require(!error || error==std::errc::no_such_file_or_directory,"Cannot inspect input profile path.",-32070);
    require(!fs::is_symlink(status),"Input profile and sidecars cannot be symbolic links.",-32070);
    if(!fs::exists(status))return false;
    require(fs::is_regular_file(status),"Input profile and sidecars must be regular files.",-32070);
    require(fs::hard_link_count(path)==1,"Input profile and sidecars cannot have hard-link aliases.",-32070);
    return true;
}
void guard_paths(const fs::path& path,bool owns_lock=false) {
    const auto name=path.filename().u8string();
    require(name.ends_with(u8".poima-input.json"),"Input profile filename must end with .poima-input.json.");
    require(fs::is_directory(path.parent_path()),"Input profile parent directory must exist.",-32070);
    for(const auto* suffix:suffixes) {
#ifdef _WIN32
        // WriterLock owns an exclusive Windows handle. Opening another handle
        // to inspect link counts can fail sharing checks; share=0 also prevents
        // replacing this already-validated lock pathname until its release.
        if(owns_lock && std::string_view(suffix)==".lock")continue;
#else
        (void)owns_lock;
#endif
        (void)regular_or_missing(sidecar(path,suffix));
    }
}
std::string read_bytes(const fs::path& path) {
    require(regular_or_missing(path),"Input profile does not exist.",-32070);
    const auto size=fs::file_size(path);require(size<=max_bytes,"Input profile exceeds the 64 KiB limit.",-32070);
    std::ifstream file(path,std::ios::binary);require(bool(file),"Cannot open input profile.",-32070);
    std::string result(static_cast<std::size_t>(size),'\0');file.read(result.data(),static_cast<std::streamsize>(result.size()));
    require(file.gcount()==static_cast<std::streamsize>(result.size()) && file.peek()==std::char_traits<char>::eof() && !file.bad(),"Input profile changed size or failed while reading.",-32070);
    return result;
}
std::string profile_hash(const Json& profile) { return content_hash(profile.dump()); }
Json parse_document(const std::string& bytes) {
    try {
        std::vector<std::set<std::string>> keys;
        auto callback=[&](int depth,Json::parse_event_t event,Json& parsed) {
            require(depth<=16,"Input profile JSON exceeds the nesting limit.");
            if(event==Json::parse_event_t::object_start)keys.emplace_back();
            else if(event==Json::parse_event_t::key)require(!keys.empty() && keys.back().insert(parsed.get<std::string>()).second,"Duplicate input profile JSON field.");
            else if(event==Json::parse_event_t::object_end)keys.pop_back();
            return true;
        };
        auto doc=Json::parse(bytes,callback);fields(doc,{"format","revision","profile","receipts"});
        require(doc.at("format")=="poima.input.v1","Unsupported input profile format; no automatic migration was performed.");
        const auto current=revision(doc.at("revision"));doc["profile"]=profile_json(parse_profile(doc.at("profile")));
        const auto& receipts=doc.at("receipts");require(receipts.is_array() && receipts.size()==std::min<std::uint64_t>(current,max_receipts),"Invalid input profile receipt history.");
        std::set<std::string> requests;auto expected=current-receipts.size();
        for(const auto& receipt:receipts) {
            fields(receipt,{"params","result"});const auto params=normalized_params(receipt.at("params"));
            require(params.at("preview")==false,"Preview requests cannot be persisted as receipts.");
            require(requests.insert(request_id(params.at("request_id"))).second,"Duplicate input profile receipt ID.");
            require(revision(params.at("expected_revision"))==expected,"Input profile receipt sequence is invalid.");
            const auto& result=receipt.at("result");fields(result,{"revision","previous_revision","changed","preview","persisted","replayed","content_hash","application"});
            require(revision(result.at("previous_revision"))==expected && revision(result.at("revision"))==++expected,"Input profile receipt revision is inconsistent.");
            require(result.at("changed").is_boolean() && result.at("preview")==false && result.at("persisted")==true && result.at("replayed")==false && result.at("application")=="next_play","Invalid input profile receipt metadata.");
            require(result.at("content_hash")==profile_hash(params.at("profile")),"Input profile receipt hash does not match its profile.");
        }
        if(!receipts.empty())require(doc.at("profile")==receipts.back().at("params").at("profile"),"Input profile differs from the latest committed receipt.");
        return doc;
    }catch(const std::exception& e) { throw ProfileError(-32070,std::string("Invalid input profile; existing bytes preserved: ")+e.what()); }
}
struct Document { bool persisted=false;std::string bytes;Json value; };
Document read_document(const fs::path& path) {
    Document doc;doc.persisted=regular_or_missing(path);
    if(doc.persisted) { doc.bytes=read_bytes(path);doc.value=parse_document(doc.bytes); }
    else doc.value={{"format","poima.input.v1"},{"revision",0},{"profile",profile_json(default_input_profile())},{"receipts",Json::array()}};
    return doc;
}
Json summary(const Document& doc) {
    return {{"profile",doc.value.at("profile")},{"revision",doc.value.at("revision")},{"persisted",doc.persisted},{"content_hash",profile_hash(doc.value.at("profile"))},{"application","next_play"}};
}
void unchanged(const fs::path& path,const Document& doc,bool owns_lock=false) {
    guard_paths(path,owns_lock);require(regular_or_missing(path)==doc.persisted && (!doc.persisted || read_bytes(path)==doc.bytes),"Input profile changed outside this transaction; inspect and retry.",-32009);
}
Json prepare_transaction(const Document& doc,const Json& params,bool& replayed) {
    for(const auto& receipt:doc.value.at("receipts"))if(receipt.at("params").at("request_id")==params.at("request_id")) {
        require(receipt.at("params")==params,"Input request ID reused with different parameters.",-32010);
        auto result=receipt.at("result");result["profile"]=receipt.at("params").at("profile");result["replayed"]=true;replayed=true;return result;
    }
    const auto current=revision(doc.value.at("revision"));require(revision(params.at("expected_revision"))==current,"Input profile revision conflict; inspect and retry.",-32009);
    require(current<max_revision,"Input profile revision limit reached.");const bool preview=params.at("preview").get<bool>();
    Json result={{"profile",params.at("profile")},{"revision",preview ? current : current+1},{"previous_revision",current},
        {"changed",params.at("profile")!=doc.value.at("profile")},{"preview",preview},{"persisted",!preview},{"replayed",false},
        {"content_hash",profile_hash(params.at("profile"))},{"application","next_play"}};
    if(preview)result["proposed_revision"]=current+1;
    return result;
}
template<class F> auto storage_errors(F&& work) -> decltype(work()) {
    try { return work(); }catch(const ProfileError&) { throw; }catch(const std::exception& e) { throw ProfileError(-32070,e.what()); }
}
}

Json profile_json(const InputProfile& profile) {
    Json bindings=Json::object();for(std::size_t i=0;i<action_names.size();++i)bindings[action_names[i]]=profile.bindings[i];
    return {{"bindings",bindings},{"sensitivity_x",profile.sensitivity_x},{"sensitivity_y",profile.sensitivity_y},{"invert_x",profile.invert_x},{"invert_y",profile.invert_y}};
}
Json profile_schema() {
    Json bindings=Json::object();for(const auto* name:action_names)bindings[name]={{"type","array"},{"maxItems",4},{"uniqueItems",true},{"items",{{"type","string"},{"maxLength",32},{"description","Physical ID from input.describe controls with reserved:false."}}}};
    return {{"type","object"},{"additionalProperties",false},{"required",{"bindings","sensitivity_x","sensitivity_y","invert_x","invert_y"}},
        {"properties",{{"bindings",{{"type","object"},{"additionalProperties",false},{"required",action_names},{"properties",bindings}}},
            {"sensitivity_x",{{"type","number"},{"minimum",0},{"maximum",10}}},{"sensitivity_y",{{"type","number"},{"minimum",0},{"maximum",10}}},
            {"invert_x",{{"type","boolean"}}},{"invert_y",{{"type","boolean"}}}}}};
}
Json describe() {
    Json controls=Json::array(),reserved=Json::array();for(const auto& control:input_controls()) {
        controls.push_back({{"id",control.id},{"label",control.label},{"device",control.kind==InputControlKind::keyboard ? "keyboard" : "mouse"},{"reserved",control.reserved}});
        if(control.reserved)reserved.push_back(control.id);
    }
    return {{"format","poima.input.v1"},{"profile_schema",profile_schema()},{"actions",action_names},{"controls",controls},{"reserved_controls",reserved},
        {"defaults",profile_json(default_input_profile())},{"application","next_play"},{"sensitivity_units","degrees per relative mouse count"},
        {"binding_policy","Zero to four alternatives per action; a control can belong to only one action. Keyboard IDs identify physical keys. Mouse buttons use SDL numbering (middle=2, right=3)."},
        {"revision_policy","Every accepted non-preview set increments the revision, including unchanged values. Preview does not write. The latest 32 successful request receipts survive restart."},
        {"content_hash_scope","SHA256 of canonical compact profile JSON; excludes revision and receipts."},
        {"persistence",{{"filename_suffix",".poima-input.json"},{"max_bytes",max_bytes},{"max_receipts",max_receipts},{"backup_suffix",".previous"},{"automatic_recovery",false}}}};
}
Json inspect(const fs::path& path) {
    return storage_errors([&]() -> Json {
        guard_paths(path);if(!regular_or_missing(path))return summary(read_document(path));
        world_detail::WriterLock lock(sidecar(path,".lock"));guard_paths(path,true);return summary(read_document(path));
    });
}
Loaded load(const fs::path& path) {
    return storage_errors([&]() -> Loaded {
        guard_paths(path);require(regular_or_missing(path),"Requested input profile does not exist.",-32070);
        world_detail::WriterLock lock(sidecar(path,".lock"));guard_paths(path,true);const auto doc=read_document(path);
        require(doc.persisted,"Requested input profile disappeared.",-32070);
        return {parse_profile(doc.value.at("profile")),revision(doc.value.at("revision")),profile_hash(doc.value.at("profile"))};
    });
}
Json transact(const fs::path& path,const Json& source) {
    return storage_errors([&]() -> Json {
        const auto params=normalized_params(source);guard_paths(path);
        if(params.at("preview").get<bool>()) {
            const auto doc=read_document(path);bool replayed=false;auto result=prepare_transaction(doc,params,replayed);unchanged(path,doc);return result;
        }
        world_detail::WriterLock lock(sidecar(path,".lock"));guard_paths(path,true);const auto doc=read_document(path);
        bool replayed=false;auto result=prepare_transaction(doc,params,replayed);if(replayed)return result;
        auto candidate=doc.value;candidate["profile"]=params.at("profile");candidate["revision"]=result.at("revision");
        auto saved_result=result;saved_result.erase("profile");auto& receipts=candidate["receipts"];
        if(receipts.size()==max_receipts)receipts.erase(receipts.begin());
        receipts.push_back({{"params",params},{"result",saved_result}});
        // Compact JSON leaves room for 32 receipts even with four long control IDs per action.
        const auto bytes=candidate.dump()+'\n';require(bytes.size()<=max_bytes,"Input profile and receipts would exceed 64 KiB.");
        unchanged(path,doc,true);world_detail::write_flushed(sidecar(path,".pending"),bytes);unchanged(path,doc,true);
        if(doc.persisted) {
            world_detail::write_flushed(sidecar(path,".previous.pending"),doc.bytes);unchanged(path,doc,true);
            world_detail::replace_file(sidecar(path,".previous.pending"),sidecar(path,".previous"));
        }
        unchanged(path,doc,true);world_detail::replace_file(sidecar(path,".pending"),path);return result;
    });
}
}
