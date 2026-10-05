// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/gameplay_abi.h"
#include "poima/components.hpp"
#include <memory>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace poima {
struct GameplayConfig {
    std::string hostfxr,bridge,assembly,type;
    bool native_aot=false;
    std::string native_library,native_sha256,native_schema;
};
void validate_gameplay_schema(const std::string& schema);
std::string validate_gameplay_values(const std::string& schema,const std::string& values);
PoimaEntityId gameplay_id(const std::string& value);
std::string gameplay_id(PoimaEntityId value);
// restore registers the trusted module but skips Initialize. Its caller must
// validate/install complete typed state before exposing the instance to Tick.
enum class GameplayInitialization { defaults, restore };
// Control-plane metadata/edits use JSON. Tick calls use only the POD ABI above.
class Gameplay {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    static bool available();
    static bool native_available();
    static std::string collect();
    Gameplay(const GameplayConfig&,const Gameplay* previous=nullptr,GameplayInitialization initialization=GameplayInitialization::defaults);
    ~Gameplay();
    Gameplay(const Gameplay&)=delete;
    Gameplay& operator=(const Gameplay&)=delete;
    std::string inspect() const;
    const std::vector<components::Schema>& component_schemas() const;
    void edit(const std::string& values);
    std::vector<std::uint64_t>& state();
    void tick(const PoimaGameServices&,std::span<const PoimaGameInput>,std::uint64_t tick);
};
}
