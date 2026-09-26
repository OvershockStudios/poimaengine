// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include <stdexcept>
namespace poima {
struct Runtime::Impl {};
bool Runtime::available() { return false; }
Runtime::Runtime(const RuntimeDefinition&) { throw std::runtime_error("Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON."); }
Runtime::~Runtime()=default;
RuntimeSummary Runtime::inspect() const { throw std::runtime_error("Simulation is not built."); }
RuntimeEntityState Runtime::entity(const std::string&) const { throw std::runtime_error("Simulation is not built."); }
void Runtime::step(std::uint32_t, const std::vector<RuntimeInput>&, const std::vector<KinematicTarget>&,const std::vector<SoundCommand>&) { throw std::runtime_error("Simulation is not built."); }
std::optional<RuntimeRayHit> Runtime::raycast(const RuntimeRay&) const { throw std::runtime_error("Simulation is not built."); }
std::uint64_t Runtime::gameplay_revision() const { throw std::runtime_error("Simulation is not built."); }
std::string Runtime::gameplay_inspect() const { throw std::runtime_error("Simulation is not built."); }
void Runtime::gameplay_load(const GameplayConfig&,const std::string&) { throw std::runtime_error("Simulation is not built."); }
void Runtime::gameplay_edit(const std::string&) { throw std::runtime_error("Simulation is not built."); }
const SoundState& Runtime::sound_state() const { throw std::runtime_error("Simulation is not built."); }
AudioSnapshot Runtime::audio_snapshot(const std::string&) const { throw std::runtime_error("Simulation is not built."); }
SceneLighting Runtime::lighting() const { throw std::runtime_error("Simulation is not built."); }
SceneSnapshot Runtime::snapshot(const std::string&) const { throw std::runtime_error("Simulation is not built."); }
}
