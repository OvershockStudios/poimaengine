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
RuntimeComponents::RuntimeComponents(entt::registry& registry,const std::map<std::string,entt::entity>& identities,const RuntimeDefinition& definition):registry_(registry) {
    static_assert(Pool::storage_policy==entt::deletion_policy::in_place);
    check(identities.size()<=max_live_entities,"Runtime entity storage budget exceeded.");
    check(definition.component_schemas.size()<=components::max_types,"Runtime component type budget exceeded.");
    for(const auto& schema:definition.component_schemas) {
        check(!schema.fields.empty() && schema.fields.size()<=components::max_fields,"Runtime component field budget exceeded.");
        for(std::size_t i=1;i<schema.fields.size();++i)check(schema.fields[i-1].id<schema.fields[i].id,"Native component field order must be canonical.");
        components::validate_payload(schema,components::defaults(schema));
    }
    schemas_=components::parse_manifest(components::manifest_json(definition.component_schemas));
    auto entities=std::make_shared<Entities>();entities->reserve(identities.size());owned_entities_.reserve(identities.size());
    for(const auto& [id,e]:identities) {
        check(registry_.valid(e),"Component owner is not a live registry entity.");
        if(valid_id(id)) { entities->push_back({gameplay_id(id),e});owned_entities_.push_back({gameplay_id(id),e}); }
        else { check(schemas_.empty(),"Custom component worlds require canonical entity identities.");owned_entities_.push_back({{},e});++legacy_entities_; }
    }
    std::sort(entities->begin(),entities->end(),[](const Entity& a,const Entity& b){return less_id(a.id,b.id);});
    entities_=std::move(entities);
    std::vector<std::shared_ptr<Rows>> rows;rows.reserve(schemas_.size());
    entt::id_type pool_id=0;
    for(std::size_t i=0;i<schemas_.size();++i) {
        while(registry.storage(pool_id)) { check(pool_id!=std::numeric_limits<entt::id_type>::max(),"Component pool identity space exhausted.");++pool_id; }
        rows.push_back(std::make_shared<Rows>());
        types_.push_back({gameplay_id(schemas_[i].id),i,&registry.storage<Cell>(pool_id),rows.back(),{}, {}});++pool_id;
        for(const auto& field:schemas_[i].fields) {
            if(field.kind==components::Kind::entity)types_.back().entity_offsets.push_back(field.offset);
            else if(field.kind==components::Kind::array && field.element_kind==components::Kind::entity)
                for(std::size_t element=0;element<field.capacity;++element)
                    types_.back().entity_offsets.push_back(field.offset+components::cell_bytes*(element+1));
        }
        // Full wire validation requires inactive collection cells to be zero,
        // so this fixed list safely validates all capacity slots after staging.
    }
    for(const auto& entity:definition.entities)for(const auto& [id,payload]:entity.components) {
        const auto& t=type(parse_id(id));const auto index=t.schema;
        check(cells_.size()<components::max_instances && payload.size()<=components::max_payload_bytes-retained_bytes_,"Runtime component storage budget exceeded.");
        components::validate_payload(schemas_[index],payload,&exists,this);
        const auto e=identities.at(entity.id);check(!t.pool->contains(e),"Duplicate native component membership.");
        auto& cell=t.pool->emplace(e,Cell{payload});const auto public_id=parse_id(entity.id);
        cells_.push_back({index,public_id,e,&cell});rows[index]->push_back({public_id,e,&cell});retained_bytes_+=payload.size();
    }
    for(auto& row:rows)std::sort(row->begin(),row->end(),[](const Row& a,const Row& b){return less_id(a.id,b.id);});
    live_instances_=cells_.size();live_bytes_=retained_bytes_;
}
RuntimeComponents::~RuntimeComponents() { if(batch_)rollback_batch(); }
const RuntimeComponents::Type& RuntimeComponents::type(PoimaEntityId id) const {
    const auto it=std::lower_bound(types_.begin(),types_.end(),id,[](const Type& t,PoimaEntityId value){return less_id(t.id,value);});
    check(it!=types_.end() && equal_id(it->id,id),"Component type is not registered in this runtime.");return *it;
}
const RuntimeComponents::Type& RuntimeComponents::type(const PoimaGameComponentType& binding) const {
    const auto& t=type(binding.type);const auto& s=schemas_[t.schema];
    check(binding.reserved==0 && binding.bytes==s.bytes() && std::equal(s.fingerprint.begin(),s.fingerprint.end(),binding.fingerprint),"Component descriptor differs from the frozen runtime schema.");return t;
}
bool RuntimeComponents::present(const Entities& rows,PoimaEntityId id) noexcept {
    const auto it=std::lower_bound(rows.begin(),rows.end(),id,[](const Entity& row,PoimaEntityId value){return less_id(row.id,value);});
    return it!=rows.end() && equal_id(it->id,id);
}
bool RuntimeComponents::alive(PoimaEntityId id) const noexcept { return present(*entities_,id); }
bool RuntimeComponents::exists(void* context,PoimaEntityId id) { return static_cast<RuntimeComponents*>(context)->alive(id); }
bool RuntimeComponents::candidate_alive(PoimaEntityId id) const noexcept { return prepared_ && present(*prepared_->entities,id); }
RuntimeComponents::Cell* RuntimeComponents::find_cell(const Rows& rows,PoimaEntityId entity) noexcept {
    const auto it=std::lower_bound(rows.begin(),rows.end(),entity,[](const Row& row,PoimaEntityId value){return less_id(row.id,value);});
    return it!=rows.end() && equal_id(it->id,entity) ? it->cell : nullptr;
}
RuntimeComponents::Cell* RuntimeComponents::cell(const Type& t,PoimaEntityId entity) const {
    check(alive(entity),"Runtime entity does not exist.");return find_cell(*t.rows,entity);
}
std::optional<components::Payload> RuntimeComponents::read(const std::string& id,const std::string& entity) const {
    auto* c=cell(type(parse_id(id)),parse_id(entity));if(!c)return {};return c->bytes;
}
std::vector<std::string> RuntimeComponents::query(const std::string& id,const std::string& after,std::uint32_t limit) const {
    check(limit>=1 && limit<=257,"Component query limit must be between 1 and 257.");const auto& t=type(parse_id(id));const auto cursor=after.empty() ? PoimaEntityId{} : parse_id(after,true);
    const auto& rows=*t.rows;const auto begin=std::upper_bound(rows.begin(),rows.end(),cursor,[](PoimaEntityId value,const Row& row){return less_id(value,row.id);});
    std::vector<std::string> out;out.reserve(std::min<std::size_t>(limit,static_cast<std::size_t>(rows.end()-begin)));
    for(auto it=begin;it!=rows.end() && out.size()<limit;++it)out.push_back(gameplay_id(it->id));
    return out;
}
void RuntimeComponents::get(const PoimaGameComponentType& binding,PoimaEntityId entity,std::span<std::byte> output,std::uint32_t& present) const {
    const auto& t=type(binding);check(output.size()==binding.bytes,"Component read buffer has the wrong size.");present=0;
    if(const auto* c=cell(t,entity)) { std::copy(c->bytes.begin(),c->bytes.end(),output.begin());present=1; }
}
std::uint32_t RuntimeComponents::query(const PoimaGameComponentType& binding,PoimaEntityId after,std::span<PoimaEntityId> output) const {
    check(!output.empty() && output.size()<=256,"Gameplay component query capacity must be between 1 and 256.");const auto& t=type(binding);
    const auto& rows=*t.rows;auto it=std::upper_bound(rows.begin(),rows.end(),after,[](PoimaEntityId value,const Row& row){return less_id(value,row.id);});
    std::uint32_t written=0;for(;it!=rows.end() && written<output.size();++it)output[written++]=it->id;return written;
}
void RuntimeComponents::reserve_birth(PoimaEntityId id,const std::map<std::string,components::Payload>& initial) {
    const ComponentSpawn birth{id,entt::null,&initial};reserve_births(std::span(&birth,1));
}
void RuntimeComponents::reserve_births(std::span<const ComponentSpawn> births) {
    check(batch_ && !prepared_,"Birth reservation requires an active unprepared runtime tick.");
    check(!births.empty() && births.size()<=components::max_commands-pending_births_.size(),"Pending birth command budget exceeded.");
    std::size_t bytes=0;
    for(std::size_t index=0;index<births.size();++index) {
        const auto& birth=births[index];const auto id=birth.id;
        check((id.high || id.low) && birth.initial,"Pending birth needs a nonzero identity and owned initial values.");
        check(std::none_of(owned_entities_.begin(),owned_entities_.end(),[&](const Entity& e){return equal_id(e.id,id);}),"Pending birth reuses an existing or retained entity ID.");
        check(std::none_of(pending_births_.begin(),pending_births_.end(),[&](const PendingBirth& e){return equal_id(e.id,id);}),"Duplicate pending birth identity.");
        for(std::size_t previous=0;previous<index;++previous)check(!equal_id(births[previous].id,id),"Duplicate instance birth identity.");
        for(const auto& [type_id,payload]:*birth.initial) {
            const auto& t=type(parse_id(type_id));components::validate_payload(schemas_[t.schema],payload);
            check(payload.size()<=components::max_command_bytes-staged_bytes_-pending_bytes_-bytes,"Pending birth payload budget exceeded.");bytes+=payload.size();
        }
    }
    // Validate every member and reserve storage before changing registration.
    pending_births_.reserve(pending_births_.size()+births.size());
    for(const auto& birth:births) {
        std::size_t member_bytes=0;for(const auto& [type_id,payload]:*birth.initial) {(void)type_id;member_bytes+=payload.size();}
        pending_births_.push_back({birth.id,birth.initial,member_bytes});
    }
    pending_bytes_+=bytes;
}
void RuntimeComponents::cancel_births(std::span<const PoimaEntityId> ids) {
    check(batch_ && !prepared_,"Birth cancellation requires an active unprepared runtime tick.");
    for(std::size_t index=0;index<ids.size();++index) {
        check(std::any_of(pending_births_.begin(),pending_births_.end(),[&](const PendingBirth& birth){return equal_id(birth.id,ids[index]);}),"Pending birth does not exist.");
        for(std::size_t previous=0;previous<index;++previous)check(!equal_id(ids[previous],ids[index]),"Duplicate canceled birth identity.");
    }
    // The individual removals perform only nonallocating bounded erases.
    for(const auto id:ids)cancel_birth(id);
}
void RuntimeComponents::cancel_birth(PoimaEntityId id) {
    check(batch_ && !prepared_,"Birth cancellation requires an active unprepared runtime tick.");
    const auto it=std::find_if(pending_births_.begin(),pending_births_.end(),[&](const PendingBirth& e){return equal_id(e.id,id);});
    check(it!=pending_births_.end(),"Pending birth does not exist.");
    pending_bytes_-=it->bytes;pending_births_.erase(it);
    std::erase_if(staged_,[&](const Staged& write) {
        if(write.key[1]!=id.high || write.key[2]!=id.low)return false;
        staged_bytes_-=write.bytes.size();return true;
    });
    // Stable erase moves vector indices. Rebuild the bounded table without allocation.
    staged_index_.fill(0);
    for(std::size_t i=0;i<staged_.size();++i)staged_index_[staged_slot(staged_[i].key)]=static_cast<std::uint16_t>(i+1);
}
void RuntimeComponents::stage_pending_checked(const PoimaGameComponentType& binding,PoimaEntityId entity,std::span<const std::byte> value) {
    check(batch_ && !prepared_,"Component writes require an active unprepared runtime tick.");const auto& t=type(binding);
    if(alive(entity))check(find_cell(*t.rows,entity),"Runtime entity has no instance of this component.");
    else {
        const auto it=std::find_if(pending_births_.begin(),pending_births_.end(),[&](const PendingBirth& e){return equal_id(e.id,entity);});
        check(it!=pending_births_.end(),"Component target is neither live nor a reserved birth.");
        check(it->initial->contains(schemas_[t.schema].id),"Pending birth has no instance of this component.");
    }
    stage(binding,entity,value);
}
void RuntimeComponents::stage(const PoimaGameComponentType& binding,PoimaEntityId entity,std::span<const std::byte> value) {
    check(batch_ && !prepared_,"Component writes require an active unprepared runtime tick.");const auto& t=type(binding);
    check(entity.high || entity.low,"Component target cannot be the zero entity ID.");
    const WriteKey key{t.schema,entity.high,entity.low};const auto slot=staged_slot(key);check(!staged_index_[slot],"Duplicate gameplay component target in one tick.");
    check(staged_.size()<components::max_commands && value.size()<=components::max_command_bytes-staged_bytes_-pending_bytes_,"Gameplay component command budget exceeded.");
    components::validate_payload(schemas_[t.schema],value);
    staged_.push_back({key,components::Payload(value.begin(),value.end())});
    staged_index_[slot]=static_cast<std::uint16_t>(staged_.size());staged_bytes_+=value.size();
}
std::size_t RuntimeComponents::staged_slot(const WriteKey& key) const noexcept {
    static_assert(components::max_commands<std::numeric_limits<std::uint16_t>::max());
    static_assert((components::max_commands&(components::max_commands-1))==0);
    // Mix all identity bits before bounded linear probing, including sequential IDs.
    std::uint64_t hash=0;
    for(auto part:key) {
        part+=0x9e3779b97f4a7c15ULL;part=(part^(part>>30))*0xbf58476d1ce4e5b9ULL;
        part=(part^(part>>27))*0x94d049bb133111ebULL;hash^=(part^(part>>31))+(hash<<6)+(hash>>2);
    }
    auto slot=static_cast<std::size_t>(hash)&(staged_index_.size()-1);
    while(staged_index_[slot] && staged_[staged_index_[slot]-1].key!=key)slot=(slot+1)&(staged_index_.size()-1);
    return slot;
}
void RuntimeComponents::clear_pending() noexcept { pending_births_.clear();pending_bytes_=0; }
void RuntimeComponents::clear_staged() noexcept {
    staged_.clear();staged_index_.fill(0);staged_bytes_=0;
}
void RuntimeComponents::validate_references(const Type& type,std::span<const std::byte> bytes,const Entities& entities) {
    // Payloads already passed full wire validation before becoming staged or stored.
    for(const auto offset:type.entity_offsets) {
        PoimaEntityId id{};
        for(unsigned i=0;i<8;++i) {
            id.high|=std::uint64_t(std::to_integer<unsigned char>(bytes[offset+i]))<<(8*i);
            id.low|=std::uint64_t(std::to_integer<unsigned char>(bytes[offset+8+i]))<<(8*i);
        }
        check((!id.high && !id.low) || present(entities,id),"Component entity reference does not resolve in this world.");
    }
}
void RuntimeComponents::begin_batch() {
    check(!batch_ && !prepared_ && staged_.empty() && pending_births_.empty() && journal_.empty(),"Component batch is already active.");
    original_revision_=revision_;original_entities_=entities_;
    for(auto& t:types_)t.original=t.rows;
    original_cells_=cells_.size();original_owners_=owned_entities_.size();
    original_live_instances_=live_instances_;original_live_bytes_=live_bytes_;original_retained_bytes_=retained_bytes_;structural_=false;batch_=true;
}
void RuntimeComponents::prepare_tick(std::span<const ComponentSpawn> spawns,std::span<const PoimaEntityId> despawns) {
    check(batch_ && !prepared_,"Component tick is absent or already prepared.");
    check(spawns.size()<=components::max_commands && despawns.size()<=components::max_commands-spawns.size(),"Component structural command budget exceeded.");
    check(spawns.size()<=max_retained_entities-owned_entities_.size(),"Runtime retained entity budget exceeded.");
    for(const auto& pending:pending_births_) {
        const auto found=std::find_if(spawns.begin(),spawns.end(),[&](const ComponentSpawn& spawn){return equal_id(spawn.id,pending.id);});
        check(found!=spawns.end() && found->initial && *found->initial==*pending.initial,"Pending birth is absent or its recipe differs from reserved content.");
    }
    Prepared candidate;candidate.entities=entities_;candidate.births.reserve(spawns.size());candidate.structural=!spawns.empty() || !despawns.empty();
    for(std::size_t i=0;i<types_.size();++i)candidate.rows[i]=types_[i].rows;
    // IDs and native owners remain reserved even after logical retirement until
    // this batch ends. The outer runtime owns permanent public-ID retirement.
    std::vector<PoimaEntityId> known;std::vector<entt::entity> owners;
    if(!spawns.empty()) {
        known.reserve(owned_entities_.size()+spawns.size());owners.reserve(owned_entities_.size()+spawns.size());
        for(const auto& e:owned_entities_) { if(e.id.high || e.id.low)known.push_back(e.id);owners.push_back(e.owner); }
    }
    std::array<std::vector<std::pair<const ComponentSpawn*,const components::Payload*>>,components::max_types> additions;
    std::size_t new_count=0,new_bytes=0;
    for(const auto& spawn:spawns) {
        check((spawn.id.high || spawn.id.low) && registry_.valid(spawn.owner) && spawn.initial,"Invalid component spawn identity, owner or payload map.");
        known.push_back(spawn.id);owners.push_back(spawn.owner);candidate.births.push_back({spawn.id,spawn.owner});
        for(const auto& [type_id,payload]:*spawn.initial) {
            const auto& t=type(parse_id(type_id));components::validate_payload(schemas_[t.schema],payload);
            check(new_count<max_retained_instances && payload.size()<=components::max_command_bytes-staged_bytes_-new_bytes,"Spawn and value payloads exceed the per-tick byte budget.");
            ++new_count;new_bytes+=payload.size();additions[t.schema].push_back({&spawn,&payload});
        }
    }
    std::sort(known.begin(),known.end(),less_id);
    check(std::adjacent_find(known.begin(),known.end(),equal_id)==known.end(),"Spawn reuses an existing or retained entity ID.");
    std::sort(owners.begin(),owners.end());check(std::adjacent_find(owners.begin(),owners.end())==owners.end(),"Spawn reuses an existing or retained native owner.");
    std::vector<PoimaEntityId> removed(despawns.begin(),despawns.end());std::sort(removed.begin(),removed.end(),less_id);
    check(std::adjacent_find(removed.begin(),removed.end(),equal_id)==removed.end(),"Duplicate component despawn target.");
    for(const auto id:removed)check(alive(id),"Component despawn target is not live.");
    const auto is_removed=[&](PoimaEntityId id){return std::binary_search(removed.begin(),removed.end(),id,less_id);};
    if(!spawns.empty() || !removed.empty()) {
        auto entities=std::make_shared<Entities>();entities->reserve(entities_->size()-removed.size()+spawns.size());
        for(const auto& e:*entities_)if(!is_removed(e.id))entities->push_back(e);
        entities->insert(entities->end(),candidate.births.begin(),candidate.births.end());
        std::sort(entities->begin(),entities->end(),[](const Entity& a,const Entity& b){return less_id(a.id,b.id);});
        check(entities->size()<=max_live_entities-legacy_entities_,"Runtime live entity budget exceeded.");candidate.entities=std::move(entities);
    }
    check(new_count<=max_retained_instances-cells_.size() && new_bytes<=max_retained_bytes-retained_bytes_,"Runtime retained component budget exceeded.");
    // No live state has changed. Reserve every append that publication needs
    // before creating registry cells; failed preparation erases those cells.
    cells_.reserve(cells_.size()+new_count);owned_entities_.reserve(owned_entities_.size()+spawns.size());
    candidate.created.reserve(new_count);candidate.writes.reserve(staged_.size());candidate.originals.reserve(staged_.size());
    try {
        for(std::size_t index=0;index<types_.size();++index) {
            const auto& t=types_[index];const auto& existing=*t.rows;
            const auto erased=removed.empty() ? 0u : static_cast<std::size_t>(std::count_if(existing.begin(),existing.end(),[&](const Row& row){return is_removed(row.id);}));
            const auto count=existing.size()-erased+additions[index].size();const auto bytes=count*schemas_[index].bytes();
            check(count<=components::max_instances-candidate.live_instances && bytes<=components::max_payload_bytes-candidate.live_bytes,"Runtime live component budget exceeded.");
            candidate.live_instances+=count;candidate.live_bytes+=bytes;
            if(erased || !additions[index].empty()) {
                candidate.component_changed=true;auto rows=std::make_shared<Rows>();rows->reserve(count);
                for(const auto& row:existing)if(!is_removed(row.id))rows->push_back(row);
                for(const auto& [spawn,payload]:additions[index]) {
                    check(!t.pool->contains(spawn->owner),"Spawn native owner already contains this component.");
                    auto& cell=t.pool->emplace(spawn->owner,Cell{*payload,false,true});
                    candidate.created.push_back({index,spawn->id,spawn->owner,&cell});candidate.created_bytes+=payload->size();
                    rows->push_back({spawn->id,spawn->owner,&cell});
                }
                std::sort(rows->begin(),rows->end(),[](const Row& a,const Row& b){return less_id(a.id,b.id);});candidate.rows[index]=std::move(rows);
            }
        }
        for(auto& [key,bytes]:staged_) {
            const auto index=static_cast<std::size_t>(key[0]);const PoimaEntityId entity{key[1],key[2]};
            // Candidate rows contain only live owners; a successful lookup proves liveness.
            auto* target=find_cell(*candidate.rows[index],entity);
            if(!target) {
                check(present(*candidate.entities,entity),"Component write target is not live in the final candidate.");
                check(false,"Runtime entity has no instance of this component.");
            }
            validate_references(types_[index],bytes,*candidate.entities);
            candidate.writes.push_back({target,&bytes});
            if(!target->born && !target->journaled) {
                check(journal_.size()+candidate.originals.size()<components::max_instances && target->bytes.size()<=components::max_payload_bytes-journal_bytes_-candidate.journal_bytes,"Runtime component journal budget exceeded.");
                candidate.originals.push_back({target,target->bytes});candidate.journal_bytes+=target->bytes.size();
            }
        }
        // Validate surviving untouched references when removing an entity. New
        // rows always validate; a queued full override supplies the final value.
        for(std::size_t index=0;index<types_.size();++index) {
            if(removed.empty() && additions[index].empty())continue;
            for(const auto& row:*candidate.rows[index]) {
                if(removed.empty() && !row.cell->born)continue;
                const auto written=staged_index_[staged_slot({index,row.id.high,row.id.low})];
                if(written)continue; // Queued overrides were checked above.
                validate_references(types_[index],row.cell->bytes,*candidate.entities);
            }
        }
        candidate.component_changed|=!staged_.empty();
        if(candidate.component_changed)check(revision_<revision_limit,"Component revision limit reached.");
        journal_.reserve(journal_.size()+candidate.originals.size());
        prepared_.emplace(std::move(candidate));
    } catch(...) {
        for(const auto& cell:candidate.created)types_[cell.type].pool->remove(cell.owner);
        throw;
    }
}
void RuntimeComponents::publish_tick() {
    check(batch_ && prepared_,"No prepared component tick to publish.");profiling::Scope scope("runtime.components.publish");
    auto& candidate=*prepared_;
    for(auto& change:candidate.originals) { change.cell->journaled=true;journal_.push_back(std::move(change)); }
    journal_bytes_+=candidate.journal_bytes;
    for(const auto& write:candidate.writes)write.cell->bytes.swap(*write.bytes);
    for(std::size_t i=0;i<types_.size();++i)types_[i].rows.swap(candidate.rows[i]);
    entities_.swap(candidate.entities);
    cells_.insert(cells_.end(),candidate.created.begin(),candidate.created.end());
    owned_entities_.insert(owned_entities_.end(),candidate.births.begin(),candidate.births.end());
    retained_bytes_+=candidate.created_bytes;live_instances_=candidate.live_instances;live_bytes_=candidate.live_bytes;
    if(candidate.component_changed)++revision_;
    structural_|=candidate.structural;
    // Ownership has moved to the batch journal; reset without erasing cells.
    prepared_.reset();clear_staged();clear_pending();
    profiling::counter("runtime.components.journal_bytes",journal_bytes_);profiling::counter("runtime.components.journal_entries",journal_.size());
}
void RuntimeComponents::apply_tick() { check(batch_ && !prepared_,"No active unprepared component batch.");if(staged_.empty() && pending_births_.empty())return;prepare_tick();publish_tick(); }
void RuntimeComponents::discard_prepared() noexcept {
    if(prepared_) {
        for(const auto& cell:prepared_->created)types_[cell.type].pool->remove(cell.owner);
        prepared_.reset();
    }
}
void RuntimeComponents::commit_batch() noexcept {
    if(!batch_)return;
    discard_prepared();
    for(auto& change:journal_)change.cell->journaled=false;
    journal_.clear();clear_staged();clear_pending();original_entities_.reset();for(auto& t:types_)t.original.reset();
    if(structural_) {
    std::erase_if(cells_,[&](const OwnedCell& cell) {
        if(alive(cell.id)) { cell.cell->born=false;return false; }
        retained_bytes_-=cell.cell->bytes.size();types_[cell.type].pool->remove(cell.owner);return true;
    });
    std::erase_if(owned_entities_,[&](const Entity& e){return (e.id.high || e.id.low) && !alive(e.id);});
    }
    staged_bytes_=journal_bytes_=0;batch_=false;
}
void RuntimeComponents::rollback_batch() noexcept {
    if(!batch_)return;
    discard_prepared();
    for(auto& change:journal_) { change.cell->bytes.swap(change.bytes);change.cell->journaled=false; }
    journal_.clear();clear_staged();clear_pending();
    for(auto& t:types_) { t.rows.swap(t.original);t.original.reset(); }
    entities_.swap(original_entities_);original_entities_.reset();
    for(std::size_t i=original_cells_;i<cells_.size();++i)types_[cells_[i].type].pool->remove(cells_[i].owner);
    cells_.resize(original_cells_);owned_entities_.resize(original_owners_);
    revision_=original_revision_;live_instances_=original_live_instances_;live_bytes_=original_live_bytes_;retained_bytes_=original_retained_bytes_;
    staged_bytes_=journal_bytes_=0;batch_=false;
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
    for(std::size_t i=0;i<types_.size();++i)check(types_[i].rows->empty() || declared[i],"Gameplay module does not declare an instantiated runtime component type.");
}
std::string RuntimeComponents::save() const {
    check(!batch_,"Cannot save custom components during an active runtime batch.");Json types=Json::array();
    for(const auto& t:types_) {
        const auto& s=schemas_[t.schema];Json rows=Json::array();for(const auto& row:*t.rows)rows.push_back({{"entity",gameplay_id(row.id)},{"values",Json::parse(components::values_json(s,row.cell->bytes,true))}});
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
        const auto& rows=saved.at("instances");check(rows.is_array() && rows.size()==expected.rows->size(),"Saved component instance membership differs from frozen content.");
        for(std::size_t k=0;k<rows.size();++k) {
            fields(rows[k],{"entity","values"});check(rows[k].at("entity")==gameplay_id((*expected.rows)[k].id),"Saved component entity membership differs from frozen content.");
            auto payload=components::parse_values(s,rows[k].at("values").dump(),true);components::validate_payload(s,payload,&exists,this);
            changes.push_back({(*expected.rows)[k].cell,std::move(payload)});
        }
    }
    for(auto& change:changes)change.cell->bytes.swap(change.bytes);
    revision_=state.at("revision").get<std::uint64_t>();
}
}
