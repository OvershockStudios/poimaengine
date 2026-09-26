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
void Runtime::step(std::uint32_t, const std::vector<RuntimeInput>&) { throw std::runtime_error("Simulation is not built."); }
SceneLighting Runtime::lighting() const { throw std::runtime_error("Simulation is not built."); }
SceneSnapshot Runtime::snapshot(const std::string&) const { throw std::runtime_error("Simulation is not built."); }
}
