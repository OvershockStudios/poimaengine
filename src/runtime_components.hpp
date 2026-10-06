// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include <entt/entity/registry.hpp>
#include <array>
#include <map>

namespace poima {
// The runtime owns the entity handle and initial map. The map is borrowed only
// during prepare_tick; prepared cells/index data are owned by the native store.
struct ComponentSpawn {
    PoimaEntityId id{};
    entt::entity owner=entt::null;
    const std::map<std::string,components::Payload>* initial=nullptr;
};
// Payloads remain in named pools of the same registry. Live sorted indices do
// not expose retained rollback cells. Do not compact/sort the underlying pools.
class RuntimeComponents {
    struct Cell {
        static constexpr bool in_place_delete=true;
        components::Payload bytes;
        bool journaled=false,born=false;
    };
    using Pool=entt::registry::storage_for_type<Cell>;
    struct Row { PoimaEntityId id;entt::entity owner;Cell* cell=nullptr; };
    using Rows=std::vector<Row>;
    struct Entity { PoimaEntityId id;entt::entity owner; };
    using Entities=std::vector<Entity>;
    struct Type {
        PoimaEntityId id;std::size_t schema;Pool* pool;
        std::shared_ptr<const Rows> rows,original;
        std::vector<std::size_t> entity_offsets;
    };
    struct Change { Cell* cell;components::Payload bytes; };
    struct OwnedCell { std::size_t type;PoimaEntityId id;entt::entity owner;Cell* cell; };
    using WriteKey=std::array<std::uint64_t,3>; // schema index, entity high, low
    struct Staged { WriteKey key;components::Payload bytes; };
    struct PendingBirth { PoimaEntityId id;const std::map<std::string,components::Payload>* initial;std::size_t bytes; };
    struct Write { Cell* cell;components::Payload* bytes; };
    struct Prepared {
        std::array<std::shared_ptr<const Rows>,components::max_types> rows;
        std::shared_ptr<const Entities> entities;
        Entities births;
        std::vector<OwnedCell> created;
        std::vector<Write> writes;
        std::vector<Change> originals;
        std::size_t live_instances=0,live_bytes=0,created_bytes=0,journal_bytes=0;
        bool component_changed=false,structural=false;
    };
    entt::registry& registry_;
    std::vector<components::Schema> schemas_;
    std::vector<Type> types_;
    std::shared_ptr<const Entities> entities_,original_entities_;
    Entities owned_entities_; // includes retired entities until batch commit
    std::vector<OwnedCell> cells_; // append-only while a batch is active
    std::vector<Staged> staged_;
    std::vector<PendingBirth> pending_births_;
    std::size_t pending_bytes_=0;
    // At most half full. Entries are staging-vector indices plus one; zero is empty.
    std::array<std::uint16_t,components::max_commands*2> staged_index_{};
    std::vector<Change> journal_;
    std::optional<Prepared> prepared_;
    std::uint64_t revision_=0,original_revision_=0;
    std::size_t staged_bytes_=0,journal_bytes_=0,live_instances_=0,live_bytes_=0,retained_bytes_=0,legacy_entities_=0;
    std::size_t original_cells_=0,original_owners_=0,original_live_instances_=0,original_live_bytes_=0,original_retained_bytes_=0;
    bool batch_=false,structural_=false;
    const Type& type(PoimaEntityId) const;
    const Type& type(const PoimaGameComponentType&) const;
    static Cell* find_cell(const Rows&,PoimaEntityId) noexcept;
    Cell* cell(const Type&,PoimaEntityId) const;
    static bool present(const Entities&,PoimaEntityId) noexcept;
    static bool exists(void*,PoimaEntityId);
    std::size_t staged_slot(const WriteKey&) const noexcept;
    void clear_staged() noexcept;
    void clear_pending() noexcept;
    static void validate_references(const Type&,std::span<const std::byte>,const Entities&);
    void discard_prepared() noexcept;
public:
    static constexpr std::size_t max_live_entities=10000,max_retained_entities=20000;
    static constexpr std::size_t max_retained_instances=components::max_instances*2;
    static constexpr std::size_t max_retained_bytes=components::max_payload_bytes*2;
    RuntimeComponents(entt::registry&,const std::map<std::string,entt::entity>&,const RuntimeDefinition&);
    ~RuntimeComponents();
    RuntimeComponents(const RuntimeComponents&)=delete;
    RuntimeComponents& operator=(const RuntimeComponents&)=delete;
    const std::vector<components::Schema>& schemas() const noexcept { return schemas_; }
    std::uint64_t revision() const noexcept { return revision_; }
    bool alive(PoimaEntityId) const noexcept;
    std::optional<components::Payload> read(const std::string& type,const std::string& entity) const;
    std::vector<std::string> query(const std::string& type,const std::string& after,std::uint32_t limit) const;
    void get(const PoimaGameComponentType&,PoimaEntityId,std::span<std::byte>,std::uint32_t& present) const;
    std::uint32_t query(const PoimaGameComponentType&,PoimaEntityId,std::span<PoimaEntityId>) const;
    // Schema/wire/duplicate guards are immediate; entity and reference guards
    // resolve against the complete candidate in prepare_tick.
    void stage(const PoimaGameComponentType&,PoimaEntityId,std::span<const std::byte>);
    // Managed writes may initialize only registered births or existing cells.
    // Reads remain committed-only. Native stage retains deferred target checks.
    void stage_pending_checked(const PoimaGameComponentType&,PoimaEntityId,std::span<const std::byte>);
    // Borrowed immutable recipe must outlive this tick's publication/cancellation.
    void reserve_birth(PoimaEntityId,const std::map<std::string,components::Payload>& initial);
    void cancel_birth(PoimaEntityId);
    void edit(const std::string&,const std::string&,const components::Payload&);
    void validate_module(const std::vector<components::Schema>&) const;
    void begin_batch();
    void prepare_tick(std::span<const ComponentSpawn> spawns={},std::span<const PoimaEntityId> despawns={});
    bool candidate_alive(PoimaEntityId) const noexcept;
    void publish_tick();
    void apply_tick();
    // The runtime must call these before destroying owned native entities.
    // Preparation/rollback never changes registry entity-handle ownership.
    void commit_batch() noexcept;
    void rollback_batch() noexcept;
    std::size_t journal_bytes() const noexcept { return journal_bytes_; }
    std::size_t journal_entries() const noexcept { return journal_.size(); }
    std::size_t live_instances() const noexcept { return live_instances_; }
    std::size_t live_bytes() const noexcept { return live_bytes_; }
    std::size_t retained_instances() const noexcept { return cells_.size()+(prepared_ ? prepared_->created.size() : 0); }
    std::size_t retained_bytes() const noexcept { return retained_bytes_+(prepared_ ? prepared_->created_bytes : 0); }
    std::string save() const;
    void load(const std::string&);
};
}
