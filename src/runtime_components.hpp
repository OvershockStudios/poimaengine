// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include <entt/entity/registry.hpp>
#include <map>

namespace poima {
// Private runtime storage. Every payload lives in a named pool of the same
// native registry; indices contain only stable lookup/presentation metadata.
class RuntimeComponents {
    struct Cell { components::Payload bytes;bool journaled=false,staged=false; };
    struct Row { PoimaEntityId id;std::string text;Cell* cell=nullptr; };
    struct Type { PoimaEntityId id;std::size_t schema=0;std::vector<Row> rows; };
    struct Change { Cell* cell;components::Payload bytes; };
    std::vector<components::Schema> schemas_;
    std::vector<Type> types_;
    std::vector<PoimaEntityId> entities_;
    std::vector<Change> staged_,journal_;
    std::uint64_t revision_=0,original_revision_=0;
    std::size_t staged_bytes_=0,journal_bytes_=0;
    bool batch_=false;
    const Type& type(PoimaEntityId) const;
    const Type& type(const PoimaGameComponentType&) const;
    Cell* cell(const Type&,PoimaEntityId) const;
    static bool exists(void*,PoimaEntityId);
public:
    RuntimeComponents(entt::registry&,const std::map<std::string,entt::entity>&,const RuntimeDefinition&);
    const std::vector<components::Schema>& schemas() const noexcept { return schemas_; }
    std::uint64_t revision() const noexcept { return revision_; }
    bool alive(PoimaEntityId) const noexcept;
    std::optional<components::Payload> read(const std::string& type,const std::string& entity) const;
    std::vector<std::string> query(const std::string& type,const std::string& after,std::uint32_t limit) const;
    void get(const PoimaGameComponentType&,PoimaEntityId,std::span<std::byte>,std::uint32_t& present) const;
    std::uint32_t query(const PoimaGameComponentType&,PoimaEntityId,std::span<PoimaEntityId>) const;
    void stage(const PoimaGameComponentType&,PoimaEntityId,std::span<const std::byte>);
    void edit(const std::string&,const std::string&,const components::Payload&);
    void validate_module(const std::vector<components::Schema>&) const;
    void begin_batch();
    void apply_tick();
    void commit_batch() noexcept;
    void rollback_batch() noexcept;
    std::size_t journal_bytes() const noexcept { return journal_bytes_; }
    std::size_t journal_entries() const noexcept { return journal_.size(); }
    std::string save() const;
    void load(const std::string&);
};
}
