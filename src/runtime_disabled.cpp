// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include <stdexcept>
namespace poima {
struct Runtime::Impl {};
bool Runtime::available() { return false; }
Runtime::Runtime(const RuntimeDefinition&) { throw std::runtime_error("Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON."); }
Runtime::~Runtime()=default;
const std::string& Runtime::presentation_source_id() const { throw std::runtime_error("Simulation is not built."); }
const std::vector<RuntimeSpawnTemplate>& Runtime::spawn_templates() const { throw std::runtime_error("Simulation is not built."); }
RuntimeStructureResult Runtime::change_structure(std::uint64_t,const std::vector<RuntimeSpawnRequest>&,const std::vector<std::string>&) { throw std::runtime_error("Simulation is not built."); }
std::uint64_t Runtime::structure_revision() const { throw std::runtime_error("Simulation is not built."); }
RuntimeSummary Runtime::inspect() const { throw std::runtime_error("Simulation is not built."); }
RuntimeEntityState Runtime::entity(const std::string&) const { throw std::runtime_error("Simulation is not built."); }
std::string Runtime::save_snapshot(const std::string&) const { throw std::runtime_error("Simulation is not built."); }
void Runtime::validate_snapshot(const RuntimeDefinition&,const std::string&,const std::string&) { throw std::runtime_error("Simulation is not built."); }
std::unique_ptr<Runtime> Runtime::from_snapshot(const RuntimeDefinition&,const std::string&,const std::string&,const std::optional<GameplayConfig>&) { throw std::runtime_error("Simulation is not built."); }
std::unique_ptr<Runtime> Runtime::from_snapshot_with_gameplay(const RuntimeDefinition&,const std::string&,const std::string&,std::unique_ptr<Gameplay>) { throw std::runtime_error("Simulation is not built."); }
std::vector<RuntimeStructureResult> Runtime::step(std::uint32_t, const std::vector<RuntimeInput>&, const std::vector<KinematicTarget>&,const std::vector<SoundCommand>&,const std::vector<AnimationCommand>&,const std::vector<RuntimeStructureTick>&) { throw std::runtime_error("Simulation is not built."); }
std::optional<RuntimeAnimationState> Runtime::animation(const std::string&) const { throw std::runtime_error("Simulation is not built."); }
std::optional<RuntimeAnimationLayerState> Runtime::animation_layer(const std::string&,std::uint32_t) const { throw std::runtime_error("Simulation is not built."); }
std::optional<RuntimeRayHit> Runtime::raycast(const RuntimeRay&) const { throw std::runtime_error("Simulation is not built."); }
void Runtime::gameplay_save_host(GameplaySaveEpoch,const GameplaySaveLedger*) { throw std::runtime_error("Simulation is not built."); }
GameplaySaveQueue& Runtime::gameplay_saves() { throw std::runtime_error("Simulation is not built."); }
const GameplaySaveQueue& Runtime::gameplay_saves() const { throw std::runtime_error("Simulation is not built."); }
std::uint64_t Runtime::gameplay_revision() const { throw std::runtime_error("Simulation is not built."); }
std::string Runtime::gameplay_inspect() const { throw std::runtime_error("Simulation is not built."); }
void Runtime::gameplay_load(const GameplayConfig&,const std::string&) { throw std::runtime_error("Simulation is not built."); }
void Runtime::gameplay_edit(const std::string&) { throw std::runtime_error("Simulation is not built."); }
const std::vector<components::Schema>& Runtime::component_schemas() const { throw std::runtime_error("Simulation is not built."); }
std::uint64_t Runtime::component_revision() const { throw std::runtime_error("Simulation is not built."); }
const ui::Model& Runtime::ui_model() const { throw std::runtime_error("Simulation is not built."); }
void Runtime::ui_edit(std::uint64_t,const std::vector<ui::Edit>&,std::optional<std::string>) { throw std::runtime_error("Simulation is not built."); }
std::uint64_t Runtime::control_sequence() const { throw std::runtime_error("Simulation is not built."); }
RuntimeControlResult Runtime::control(std::uint64_t,std::uint64_t,const std::string&) { throw std::runtime_error("Simulation is not built."); }
std::optional<components::Payload> Runtime::component_read(const std::string&,const std::string&) const { throw std::runtime_error("Simulation is not built."); }
std::vector<std::string> Runtime::component_query(const std::string&,const std::string&,std::uint32_t) const { throw std::runtime_error("Simulation is not built."); }
void Runtime::component_edit(const std::string&,const std::string&,const components::Payload&) { throw std::runtime_error("Simulation is not built."); }
const SoundState& Runtime::sound_state() const { throw std::runtime_error("Simulation is not built."); }
AudioSnapshot Runtime::audio_snapshot(const std::string&) const { throw std::runtime_error("Simulation is not built."); }
SceneLighting Runtime::lighting() const { throw std::runtime_error("Simulation is not built."); }
SceneSnapshot Runtime::snapshot() const { throw std::runtime_error("Simulation is not built."); }
SceneSnapshot Runtime::snapshot(const std::string&) const { throw std::runtime_error("Simulation is not built."); }
}
