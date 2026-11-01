// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <nlohmann/json.hpp>

namespace JPH { class PhysicsSystem; }
namespace poima {
inline constexpr std::size_t runtime_physics_state_max_bodies=4096;
inline constexpr std::size_t runtime_physics_state_max_pairs=8192;
inline constexpr std::size_t runtime_physics_state_max_manifolds=8192;
inline constexpr std::size_t runtime_physics_state_max_native_bytes=8*1024*1024;
inline constexpr std::size_t runtime_physics_state_max_json_bytes=64*1024*1024;

// Owner-thread/quiescent only, pinned Jolt double-position rigid-body profile.
// Global previous dt/gravity, complete broadphase body motion/sleep state,
// regular contact cache and CCD keys. Constraints are deliberately separate:
// caller restores named joint warm starts/global stable constraint order.
// No soft-body stream, arbitrary recorder bytes, base64 or padding payloads.
// Capture and restore target both require every allocated live body's collision
// cache invalid flag to be clear. Pending shape/explicit invalidation is rejected;
// it is neither silently omitted nor cleared by this codec.
nlohmann::json capture_runtime_physics_state(const JPH::PhysicsSystem&);

// Entire schema/counts/native-representable numbers/references are checked
// before any mutation. Target must already own the exact broadphase body IDs,
// rigid types and motion-property presence. Cached retired IDs remain hash keys;
// this module never dereferences a subshape ID or queries its material.
// Caller separately verifies frozen shape/mass/material/solver/configuration
// identity; these are not part of Jolt's mutable SaveState stream. Byte coverage
// alone is not proof of full future continuation or collision-cache validity.
void validate_runtime_physics_state(const JPH::PhysicsSystem&,const nlohmann::json&);

// Builds a fresh trusted recorder from validated named fields. Failed native
// restore attempts to roll back using an internally captured trusted snapshot;
// root candidate-world restoration provides the enclosing atomic publication.
// PhysicsSystem must be initialized with >=4096 bodies/8192 pairs/manifolds,
// or the caller must independently prove the snapshot fits its configuration.
void restore_runtime_physics_state(JPH::PhysicsSystem&,const nlohmann::json&);
}
