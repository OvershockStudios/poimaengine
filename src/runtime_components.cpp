// SPDX-License-Identifier: Apache-2.0
#include "runtime_components.hpp"
#include "poima/profiler.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace poima {
namespace {
using Json=nlohmann::json;
constexpr std::uint64_t revision_limit=9007199254740991ULL;
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
bool less_id(PoimaEntityId a,PoimaEntityId b) noexcept { return a.high<b.high || (a.high==b.high && a.low<b.low); }
bool equal_id(PoimaEntityId a,PoimaEntityId b) noexcept { return a.high==b.high && a.low==b.low; }
bool valid_id(const std::string& s) { return s.size()==32 && s.find_first_not_of("0123456789abcdef")==std::string::npos && s!=std::string(32,'0'); }
PoimaEntityId parse_id(const std::string& s,bool zero=false) {
    check(valid_id(s) || (zero && s==std::string(32,'0')),"Component identity must be 32 lowercase hexadecimal digits.");return gameplay_id(s);
}
void fields(const Json& j,std::initializer_list<const char*> keys) {
    check(j.is_object() && j.size()==keys.size(),"Invalid saved component object fields.");for(auto key:keys)check(j.contains(key),"Missing saved component field.");
}
PoimaGameComponentType descriptor(const components::Schema& s) {
    PoimaGameComponentType d{};d.type=gameplay_id(s.id);std::copy(s.fingerprint.begin(),s.fingerprint.end(),d.fingerprint);d.bytes=static_cast<std::uint32_t>(s.bytes());return d;
}
}
RuntimeComponents::RuntimeComponents(entt::registry& registry,const std::map<std::string,entt::entity>& identities,const RuntimeDefinition& definition) {
    // Normalize and fully validate even hand-built C++ definitions, before pools
    // become observable. JSON is used here only at world construction.
    check(definition.component_schemas.size()<=components::max_types,"Runtime component type budget exceeded.");
    for(const auto& schema:definition.component_schemas) {
        check(!schema.fields.empty() && schema.fields.size()<=components::max_fields,"Runtime component field budget exceeded.");
        for(std::size_t i=1;i<schema.fields.size();++i)check(schema.fields[i-1].id<schema.fields[i].id,"Native component field order must be canonical.");
        components::validate_payload(schema,components::defaults(schema));
    }
    schemas_=components::parse_manifest(components::manifest_json(definition.component_schemas));
    for(const auto& [id,e]:identities) {
        (void)e;
        // Legacy direct native tests historically use descriptive IDs. Such IDs
        // cannot participate in the fixed-128-bit gameplay ABI.
        if(valid_id(id))entities_.push_back(gameplay_id(id));
        else check(schemas_.empty(),"Custom component worlds require canonical entity identities.");
    }
    std::sort(entities_.begin(),entities_.end(),less_id);
    std::vector<decltype(&registry.storage<Cell>(0))> pools;
    pools.reserve(schemas_.size());
    entt::id_type pool_id=0;
    for(std::size_t i=0;i<schemas_.size();++i) {
        while(registry.storage(pool_id)) { check(pool_id!=std::numeric_limits<entt::id_type>::max(),"Component pool identity space exhausted.");++pool_id; }
        pools.push_back(&registry.storage<Cell>(pool_id));
        types_.push_back({gameplay_id(schemas_[i].id),i,{}});
        ++pool_id;
    }
    std::size_t count=0,bytes=0;
    for(const auto& entity:definition.entities)for(const auto& [id,payload]:entity.components) {
        const auto& t=type(parse_id(id));const auto index=t.schema;
        check(++count<=components::max_instances && payload.size()<=components::max_payload_bytes-bytes,"Runtime component storage budget exceeded.");bytes+=payload.size();
        components::validate_payload(schemas_[index],payload,&exists,this);
        auto& storage=*pools[index];const auto e=identities.at(entity.id);
        check(!storage.contains(e),"Duplicate native component membership.");
        storage.emplace(e,Cell{payload});
        types_[index].rows.push_back({parse_id(entity.id),entity.id,nullptr});
    }
    for(std::size_t i=0;i<types_.size();++i) {
        auto& rows=types_[i].rows;std::sort(rows.begin(),rows.end(),[](const Row& a,const Row& b){return less_id(a.id,b.id);});
        for(auto& row:rows)row.cell=&pools[i]->get(identities.at(row.text));
    }
}
const RuntimeComponents::Type& RuntimeComponents::type(PoimaEntityId id) const {
    const auto it=std::lower_bound(types_.begin(),types_.end(),id,[](const Type& t,PoimaEntityId value){return less_id(t.id,value);});
    check(it!=types_.end() && equal_id(it->id,id),"Component type is not registered in this runtime.");return *it;
}
const RuntimeComponents::Type& RuntimeComponents::type(const PoimaGameComponentType& binding) const {
    const auto& t=type(binding.type);const auto& s=schemas_[t.schema];
    check(binding.reserved==0 && binding.bytes==s.bytes() && std::equal(s.fingerprint.begin(),s.fingerprint.end(),binding.fingerprint),"Component descriptor differs from the frozen runtime schema.");return t;
}
bool RuntimeComponents::alive(PoimaEntityId id) const noexcept { return std::binary_search(entities_.begin(),entities_.end(),id,less_id); }
bool RuntimeComponents::exists(void* context,PoimaEntityId id) { return static_cast<RuntimeComponents*>(context)->alive(id); }
RuntimeComponents::Cell* RuntimeComponents::cell(const Type& t,PoimaEntityId entity) const {
    check(alive(entity),"Runtime entity does not exist.");
    const auto it=std::lower_bound(t.rows.begin(),t.rows.end(),entity,[](const Row& row,PoimaEntityId value){return less_id(row.id,value);});
    return it!=t.rows.end() && equal_id(it->id,entity) ? it->cell : nullptr;
}
std::optional<components::Payload> RuntimeComponents::read(const std::string& id,const std::string& entity) const {
    auto* c=cell(type(parse_id(id)),parse_id(entity));if(!c)return {};return c->bytes;
}
std::vector<std::string> RuntimeComponents::query(const std::string& id,const std::string& after,std::uint32_t limit) const {
    check(limit>=1 && limit<=257,"Component query limit must be between 1 and 257.");const auto& t=type(parse_id(id));const auto cursor=after.empty() ? PoimaEntityId{} : parse_id(after,true);
    const auto begin=std::upper_bound(t.rows.begin(),t.rows.end(),cursor,[](PoimaEntityId value,const Row& row){return less_id(value,row.id);});
    std::vector<std::string> out;out.reserve(std::min<std::size_t>(limit,static_cast<std::size_t>(t.rows.end()-begin)));
    for(auto it=begin;it!=t.rows.end() && out.size()<limit;++it)out.push_back(it->text);
    return out;
}
void RuntimeComponents::get(const PoimaGameComponentType& binding,PoimaEntityId entity,std::span<std::byte> output,std::uint32_t& present) const {
    const auto& t=type(binding);check(output.size()==binding.bytes,"Component read buffer has the wrong size.");present=0;
    if(const auto* c=cell(t,entity)) { std::copy(c->bytes.begin(),c->bytes.end(),output.begin());present=1; }
}
std::uint32_t RuntimeComponents::query(const PoimaGameComponentType& binding,PoimaEntityId after,std::span<PoimaEntityId> output) const {
    check(!output.empty() && output.size()<=256,"Gameplay component query capacity must be between 1 and 256.");const auto& t=type(binding);
    auto it=std::upper_bound(t.rows.begin(),t.rows.end(),after,[](PoimaEntityId value,const Row& row){return less_id(value,row.id);});
    std::uint32_t written=0;for(;it!=t.rows.end() && written<output.size();++it)output[written++]=it->id;return written;
}
void RuntimeComponents::stage(const PoimaGameComponentType& binding,PoimaEntityId entity,std::span<const std::byte> value) {
    check(batch_,"Component writes require an active runtime batch.");const auto& t=type(binding);auto* c=cell(t,entity);check(c,"Runtime entity has no instance of this component.");
    check(!c->staged,"Duplicate gameplay component target in one tick.");
    check(staged_.size()<components::max_commands && value.size()<=components::max_command_bytes-staged_bytes_,"Gameplay component command budget exceeded.");
    components::validate_payload(schemas_[t.schema],value,&exists,this);
    staged_.push_back({c,components::Payload(value.begin(),value.end())});c->staged=true;staged_bytes_+=value.size();
}
void RuntimeComponents::begin_batch() {
    check(!batch_ && staged_.empty() && journal_.empty(),"Component batch is already active.");original_revision_=revision_;batch_=true;
}
void RuntimeComponents::apply_tick() {
    check(batch_,"No active component batch.");if(staged_.empty())return;
    check(revision_<revision_limit,"Component revision limit reached.");
    profiling::Scope scope("runtime.components.publish");
    for(auto& change:staged_) {
        if(!change.cell->journaled) {
            journal_.push_back({change.cell,change.cell->bytes});
            change.cell->journaled=true;journal_bytes_+=change.cell->bytes.size();
        }
        change.cell->bytes.swap(change.bytes);change.cell->staged=false;
    }
    staged_.clear();staged_bytes_=0;++revision_;
    profiling::counter("runtime.components.journal_bytes",journal_bytes_);
    profiling::counter("runtime.components.journal_entries",journal_.size());
}
void RuntimeComponents::commit_batch() noexcept {
    for(auto& change:staged_)change.cell->staged=false;
    for(auto& change:journal_)change.cell->journaled=false;
    staged_.clear();journal_.clear();staged_bytes_=journal_bytes_=0;batch_=false;
}
void RuntimeComponents::rollback_batch() noexcept {
    for(auto& change:journal_)change.cell->bytes.swap(change.bytes);
    revision_=original_revision_;commit_batch();
}
void RuntimeComponents::edit(const std::string& id,const std::string& entity,const components::Payload& value) {
    const auto& t=type(parse_id(id));const auto binding=descriptor(schemas_[t.schema]);begin_batch();
    try { stage(binding,parse_id(entity),value);apply_tick();commit_batch(); }catch(...) { rollback_batch();throw; }
}
void RuntimeComponents::validate_module(const std::vector<components::Schema>& module) const {
    std::vector<bool> declared(types_.size());
    for(const auto& schema:module) {
        const auto& t=type(parse_id(schema.id));check(!declared[t.schema],"Duplicate gameplay component declaration.");declared[t.schema]=true;
        check(schema.fingerprint==schemas_[t.schema].fingerprint && schema.bytes()==schemas_[t.schema].bytes(),"Gameplay component schema is incompatible with the frozen runtime.");
    }
    for(std::size_t i=0;i<types_.size();++i)check(types_[i].rows.empty() || declared[i],"Gameplay module does not declare an instantiated runtime component type.");
}
std::string RuntimeComponents::save() const {
    check(!batch_,"Cannot save custom components during an active runtime batch.");Json types=Json::array();
    for(const auto& t:types_) {
        const auto& s=schemas_[t.schema];Json rows=Json::array();for(const auto& row:t.rows)rows.push_back({{"entity",row.text},{"values",Json::parse(components::values_json(s,row.cell->bytes,true))}});
        types.push_back({{"id",s.id},{"fingerprint",components::fingerprint_hex(s)},{"instances",std::move(rows)}});
    }
    return Json{{"revision",revision_},{"types",std::move(types)}}.dump();
}
void RuntimeComponents::load(const std::string& bytes) {
    check(!batch_,"Cannot restore custom components during an active runtime batch.");check(bytes.size()<=64*1024*1024,"Saved component state exceeds byte budget.");
    const auto state=Json::parse(bytes);fields(state,{"revision","types"});
    check(state.at("revision").is_number_integer() && state.at("revision")>=0 && state.at("revision")<=revision_limit,"Invalid saved component revision.");
    const auto& types=state.at("types");check(types.is_array() && types.size()==types_.size(),"Saved component type membership differs from frozen content.");std::vector<Change> changes;
    for(std::size_t i=0;i<types_.size();++i) {
        const auto& expected=types_[i];const auto& s=schemas_[i];const auto& saved=types[i];fields(saved,{"id","fingerprint","instances"});
        check(saved.at("id")==s.id && saved.at("fingerprint")==components::fingerprint_hex(s),"Saved component schema differs from frozen content.");
        const auto& rows=saved.at("instances");check(rows.is_array() && rows.size()==expected.rows.size(),"Saved component instance membership differs from frozen content.");
        for(std::size_t k=0;k<rows.size();++k) {
            fields(rows[k],{"entity","values"});check(rows[k].at("entity")==expected.rows[k].text,"Saved component entity membership differs from frozen content.");
            auto payload=components::parse_values(s,rows[k].at("values").dump(),true);components::validate_payload(s,payload,&exists,this);
            changes.push_back({expected.rows[k].cell,std::move(payload)});
        }
    }
    for(auto& change:changes)change.cell->bytes.swap(change.bytes);
    revision_=state.at("revision").get<std::uint64_t>();
}
}
