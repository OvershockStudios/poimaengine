// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "runtime_body_ids.hpp"
#include <map>
#include <string>
#include <vector>
namespace poima {
struct RuntimePhysicalRigIdentity {
    std::vector<JPH::BodyID> parts;
    std::uint32_t collision_group=0;
};
// Trusted typed candidate-construction input. The enclosing save parser checks
// schema/frozen ownership first; constructor rejoins every ID with its actual
// complete definition before creating bodies. Never remap incoming contact IDs.
struct RuntimePhysicsBootstrap {
    RuntimeBodyIds ledger;
    std::map<std::string,JPH::BodyID> bodies;
    std::map<std::string,RuntimePhysicalRigIdentity> rigs;
    std::uint32_t next_collision_group=1;
};
}
