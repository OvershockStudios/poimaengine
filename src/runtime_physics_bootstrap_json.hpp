// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "runtime_physical_restore.hpp"
#include "poima/runtime.hpp"
#include <nlohmann/json.hpp>
namespace JPH { struct PoimaBodyOwnerState; }
namespace poima::physical_bootstrap_json {
// Typed cold-construction metadata. Every USED ledger slot is retained, including
// retired generation0/255 rows. Binding IDs are never renumbered or remapped.
// Format/version, exact typed fields, sorted rows, frozen definition membership,
// named physical-part order, groups and occupied-ledger bijection are checked.
nlohmann::json bootstrap_json(const RuntimePhysicsBootstrap&,const RuntimeDefinition&);
RuntimePhysicsBootstrap parse_bootstrap(const nlohmann::json&,const RuntimeDefinition&);
// Jolt allocator must already be initialized before owner-vector allocation.
// Separate quiescent Jolt owner metadata. Parse validates syntax/counts/order/IDs
// only; actual active/body/flag membership still requires Jolt's owner adapter.
// Retired duplicate queue tokens retain their original exact order/full IDs.
nlohmann::json owner_json(const JPH::PoimaBodyOwnerState&);
JPH::PoimaBodyOwnerState parse_owner(const nlohmann::json&);
// Against the complete SEALED ledger: every queued ID must have a USED slot with
// generation >= its sequence; owned queued IDs and owned flagged IDs are a
// one-to-one pair. Active IDs must also be owned. No native state is mutated.
void validate_owner_history(const JPH::PoimaBodyOwnerState&,const RuntimeBodyIds&);
// These joins do not validate joint/profile calibration, contacts, poses, global
// physics bytes, or atomic whole-world restoration/cold future continuation.
} // namespace poima::physical_bootstrap_json
