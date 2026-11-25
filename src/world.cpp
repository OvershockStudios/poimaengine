// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include <limits>
#include "poima/world.hpp"
#include "world_stdio.hpp"
#include "poima/animation.hpp"
#include "poima/scene.hpp"
#include "render_capture_session.hpp"
#include "poima/runtime.hpp"
#include "poima/snow.hpp"
#include "poima/snow_releases.hpp"
#include "poima/surface_wetness.hpp"
#include "poima/terrain_asset.hpp"
#include "foliage_authoring.hpp"
#include "water_asset.hpp"
#include "particle_scene.hpp"
#include <charconv>
#include "physical_rig_json.hpp"
#include "poima/physical_target.hpp"
#include "pose_frames.hpp"
#include "surface_measure.hpp"
#include "audio_buses.hpp"
#include "audio_water.hpp"
#include "surface_marks_json.hpp"
#include "attachment_outcome_json.hpp"
#include "poima/runtime_ik.hpp"
#include "poima/runtime_grounding.hpp"
#include "poima/ui_model.hpp"
#if POIMA_GAME_UI
#include "poima/ui_presenter.hpp"
#include "poima/ui_document.hpp"
#endif
#include "world_asset_commands.hpp"
#include "world_authored_scene.hpp"
#include "world_capture.hpp"
#include "world_content.hpp"
#include "world_discovery.hpp"
#include "world_document.hpp"
#include "world_model_assets.hpp"
#include "world_presentation_json.hpp"
#include "world_post_effect_assets.hpp"
#include "world_preferences_commands.hpp"
#include "world_region_assets.hpp"
#include "world_runtime_definition.hpp"
#include "world_runtime_json.hpp"
#include "world_schema.hpp"
#include "world_spatial_queries.hpp"
#include "world_transaction_operations.hpp"
#include "poima/player.hpp"
#include "poima/player_diagnostics.hpp"
#include "poima/player_preferences.hpp"
#include "poima/gameplay_player_preferences.hpp"
#include "poima/build_metadata.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include "poima/save_store.hpp"
#include "poima/gameplay_save_writer.hpp"
#include <chrono>
#include "poima/save_upgrade_document.hpp"
#include "poima/save_upgrade_snapshot.hpp"
#include "world_storage.hpp"
#include "project_io_path.hpp"
#include "profiler_service.hpp"
#include "frame_performance_service.hpp"
#include "development_service.hpp"
#include "poima/development_profiles.hpp"
#include "asset_store.hpp"
#include "asset_catalog.hpp"
#include "asset_provenance.hpp"
#include "input_profile_store.hpp"
#include "player_settings_store.hpp"
#include "authoring_reference.hpp"
#include "material_service.hpp"
#include "poima/jobs.hpp"
#include "navigation_geometry.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <functional>
#include <exception>
#include <type_traits>

namespace poima {
namespace {
using namespace world_detail;

void describe_audio_routing(Json& observation, const AudioEmitter& emitter, float event_gain = 1) {
    if (!emitter.bus) return;
    observation["bus"] = emitter.bus->name;
    observation["bus_gain"] = emitter.bus->gain;
    observation["effective_gain"] = emitter.gain * event_gain * emitter.bus->gain;
}

std::string new_id() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string result(32, '0');
    for (auto& c : result) c = hex[random() & 15];
    return result;
}
Json object_schema(Json properties, Json required = Json::array()) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
Json parse(const std::string& text) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&keys](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 64, "JSON nesting exceeds 64 levels.", -32700);
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key)
            require(keys.back().insert(value.get<std::string>()).second, "Duplicate JSON object key.", -32700);
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
std::string read(const fs::path& path) {
    require(fs::file_size(path) <= max_document_bytes, "World document exceeds 16 MiB.");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read world document.");
    std::string bytes;
    std::array<char, 65536> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        require(bytes.size() + count <= max_document_bytes, "World document exceeds 16 MiB.");
        bytes.append(buffer.data(), count);
    }
    if (stream.bad()) throw std::runtime_error("Cannot finish reading world document.");
    require(bytes.size() <= max_document_bytes, "World document exceeds 16 MiB.");
    return bytes;
}
constexpr std::array authoring_methods{"component.schema.import","world.reference","world.transact","world.undo","world.redo","asset.import","asset.image.import","asset.audio.import","asset.post-effect.import","asset.label","input.transact","settings.transact","development.compile","development.profiles","development.publish","development.export","development.verify","development.candidate","project.candidate.retain","development.jobs","development.inspect","development.diagnostics","development.cancel","development.forget","project.inspect","project.gameplay.adopt","project.source.read","project.source.write","project.verification.read","project.verification.write","asset.material.generate","asset.material.job","asset.material.jobs","asset.material.cancel","asset.material.forget","world.navigation.bake","asset.provenance.create","asset.animation.author","asset.mesh.extract_rigid","world.terrain.inspect","world.terrain.cook","world.foliage.plan","world.water.plan","world.water.cook"};
class World {
    profiling::Service profiler_;
    frame_performance::Service performance_;
    PlayerProgressObserver player_progress_;
    bool startup_splash_=false;
    fs::path path_;
    bool read_only_=false;
    std::unique_ptr<WriterLock> lock_;
    Json doc_;
    std::string disk_;
    bool exists_ = false;
    GameplaySaveLedger gameplay_save_ledger_;
    std::unique_ptr<Runtime> runtime_;
    mutable std::unique_ptr<SceneCaptureSession> capture_renderer_;
    mutable Json capture_renderer_key_;
    std::shared_ptr<GamepadHost> gamepad_host_;
    RuntimeDefinition runtime_definition_;
    Json runtime_document_;
    // Both documents publish on the owner thread. Runtime ticks do not change
    // either authored baseline, so cache only this predicate, not the summary.
    mutable std::optional<bool> runtime_source_stale_cache_;
    void invalidate_runtime_source_stale() noexcept { runtime_source_stale_cache_.reset(); }
    bool runtime_source_stale() const {
        if(!runtime_source_stale_cache_.has_value())
            runtime_source_stale_cache_=runtime_document_.at("revision")!=doc_.at("revision") || !same_authored_state(runtime_document_,doc_);
        return *runtime_source_stale_cache_;
    }
    std::string runtime_content_hash_;
    std::optional<GameplayConfig> runtime_gameplay_config_;
    fs::path protected_root_,save_root_;
    std::uint64_t save_configuration_generation_=0;
    Json save_receipts_=Json::array();
    std::string runtime_id_, stopped_runtime_id_;
    std::set<std::string> used_runtime_ids_;
    Json runtime_start_params_, runtime_start_result_;
    Json runtime_receipts_=Json::array();
    Json playback_receipts_=Json::array();
    development::Service development_;
    materials::Service materials_;
    // Owner requests survive replacement; callbacks mutate only runtime queues.
    struct PendingGameplayWrite {
        fs::path path;std::string operation,hash;std::uint64_t expected_generation=0;std::int32_t error_code=-32070;std::array<char,256> diagnostic{};
    };
    std::optional<PendingGameplayWrite> pending_gameplay_write_;
    bool background_gameplay_saves_=false;
    std::unique_ptr<GameplaySaveWriter> gameplay_save_writer_;
    std::chrono::steady_clock::time_point save_resolution_after_{};
    struct RuntimeAdvance {
        GameplaySaveEpoch source,current;
        std::uint64_t previous_tick=0,committed_tick=0,current_tick=0,first_voice=0,next_voice=0;
        std::uint32_t stepped=0;bool replaced=false,save_serviced=false;
        std::optional<GameplaySaveResult> operation;
    };
    struct AdvanceReceipt { Json params;RuntimeAdvance outcome; };
    std::array<std::unique_ptr<AdvanceReceipt>,32> advance_receipts_;
    std::size_t advance_receipt_next_=0;
    // Store the native outcome before formatting JSON, so an allocation failure
    // while serializing a response can be recovered by an identical retry.
    struct StructureReceipt {
        Json params;
        RuntimeStructureResult outcome;
        std::uint64_t component_revision=0;
    };
    std::array<std::unique_ptr<StructureReceipt>,32> structure_receipts_;
    std::size_t structure_receipt_next_=0;
    struct RuntimeControlOutcome {
        GameplaySaveEpoch source,current;
        std::uint64_t tick=0,current_tick=0,control_sequence=0,ui_revision=0;
        std::uint64_t current_control_sequence=0,current_ui_revision=0;
        RuntimeControlIntent intent=RuntimeControlIntent::none;
        bool replaced=false,save_serviced=false;
        std::optional<GameplaySaveResult> operation;
    };
    struct ControlReceipt { Json params;RuntimeControlOutcome outcome; };
    std::array<std::unique_ptr<ControlReceipt>,32> control_receipts_;
    std::size_t control_receipt_next_=0;

    mutable ModelCache model_cache_;
    mutable std::optional<std::set<std::string>> authored_model_cache_references_;
    mutable std::optional<SceneSnapshot> authored_cache_;
    AuthoredPresentation authored_presentation_;

    struct Edit { Json before,after;std::size_t bytes;std::string request_id; };
    using History=std::vector<std::shared_ptr<const Edit>>;
    History undo_,redo_;
    std::uint64_t skipped_large_edits_=0;
    static std::size_t history_bytes(const History& first,const History& second) {
        std::size_t bytes=0;for(const auto& e:first)bytes+=e->bytes;for(const auto& e:second)bytes+=e->bytes;return bytes;
    }
    void prune_model_cache() const {
        if(model_cache_.models.empty() && model_cache_.images.empty())return;
        if(!authored_model_cache_references_) {
            std::set<std::string> referenced;
            std::function<void(const Json&)> visit=[&](const Json& value) {
                if(value.is_string()) { const auto& text=value.get_ref<const std::string&>();if(valid_asset_id(text))referenced.insert(text); }
                else if(value.is_structured())for(const auto& item:value)visit(item);
            };
            visit(doc_.at("entities"));if(doc_.contains("templates"))visit(doc_.at("templates"));
            authored_model_cache_references_.emplace(std::move(referenced));
        }
        // Candidate validation can insert assets without publishing authoring.
        // Reuse only the authored references; prune both live maps every time.
        const auto& referenced=*authored_model_cache_references_;
        for(auto it=model_cache_.models.begin();it!=model_cache_.models.end();) {
            if(referenced.contains(it->first))++it;else { model_cache_.bytes-=it->second.bytes;it=model_cache_.models.erase(it); }
        }
        for(auto it=model_cache_.images.begin();it!=model_cache_.images.end();) {
            if(referenced.contains(it->first))++it;else { model_cache_.bytes-=it->second.bytes;it=model_cache_.images.erase(it); }
        }
    }
    void current_revision(const Json& params) const {
        if (params.contains("revision")) require(revision(params.at("revision")) == revision(doc_.at("revision")),
            "Revision conflict; inspect the current world and retry.", -32009);
    }
    static Json& entity(Json& doc, const Json& id) {
        const auto text = identifier(id);
        require(doc.at("entities").contains(text), "Entity does not exist.", -32004);
        return doc["entities"][text];
    }
    void persist(Json&& candidate) {
        require(!read_only_,"This packaged world is read-only.",-32081);
        auto presentation=authored_presentation_.stage(candidate);
        auto bytes = candidate.dump(2) + '\n';
        require(bytes.size() <= max_document_bytes, "World document would exceed 16 MiB.");
        require(fs::exists(path_) == exists_ && (!exists_ || read(path_) == disk_),
            "World file changed outside this session; reopen before editing.", -32009);
        const auto publish = [&](const fs::path& source, const fs::path& destination,
                                 StorageStage stage, std::string_view prepared) {
            const StorageRetryObserver observer{nullptr, [](void*, StorageStage failed_stage, const StorageRetryFacts& facts) noexcept {
                report_world_publication_retry_pending(std::cerr, failed_stage, facts);
            }};
            const auto retried = replace_world_publication(source, destination, stage, [&] {
                require(world_publication_unchanged(path_, exists_, disk_, source, prepared),
                    "World file changed outside this session; reopen before editing.", -32009);
            }, &observer);
            report_world_publication_recovery(std::cerr, stage, retried);
        };
        auto pending = path_; pending += ".pending";
        write_flushed(pending, bytes, StorageStage::primary_staging);
        if (exists_) {
            auto previous = path_; previous += ".previous";
            auto previous_pending = path_; previous_pending += ".previous.pending";
            write_flushed(previous_pending, disk_, StorageStage::previous_staging);
            publish(previous_pending, previous, StorageStage::previous_publish, disk_);
        }
        publish(pending, path_, StorageStage::primary_publish, bytes);
        // All potentially failing preparation precedes publication. Swapping
        // state after rename cannot accidentally roll back a committed request.
        doc_.swap(candidate);
        authored_model_cache_references_.reset();
        invalidate_runtime_source_stale();
        authored_presentation_.publish(std::move(presentation));
        disk_.swap(bytes);
        exists_ = true;
    }
public:
    struct PlayerOwner;
    // Latest owner retained for terminal metadata only; its GPU lifetime retires promptly.
    std::unique_ptr<PlayerOwner> player_owner_;
    std::shared_ptr<GameplayPlayerPreferences> gameplay_player_preferences_;
    std::uint64_t player_generation_=0,player_control_revision_=0;
    struct PlayerReceipt;
    std::array<std::unique_ptr<PlayerReceipt>,32> player_receipts_;
    std::size_t player_receipt_next_=0;
    void require_registered_custom_type(const std::string& type) const {
        const auto schemas=authored_component_schemas(doc_);(void)authored_schema(schemas,custom_type(type));
    }
    profiling::Recorder& profiler() noexcept { return profiler_.recorder(); }
    void configure_development_profiles(const std::string& filename) {
        require(!read_only_,"Read-only runtime hosts cannot configure development tools.",-32081);
        development_.configure(development::load_profiles(filename));
    }
    void begin_development_shutdown() { development_.begin_shutdown(); }
    bool development_shutdown_complete() const noexcept { return development_.shutdown_complete(); }
    WorldProfilerContext profiler_context() const {
        WorldProfilerContext result;
        if(runtime_) { std::copy(runtime_id_.begin(),runtime_id_.end(),result.session.begin());result.tick=static_cast<std::int64_t>(runtime_->inspect().tick); }
        return result;
    }
    void observe_blocking_player(PlayerProgressObserver observer) { player_progress_=std::move(observer); }
    void enable_startup_splash() { startup_splash_=true; }
    explicit World(const std::string& utf8_path,WorldOpenMode mode,const std::string& protected_root) :
        path_(std::u8string(utf8_path.begin(), utf8_path.end())),
        read_only_(mode==WorldOpenMode::read_only_runtime) {
        require(mode==WorldOpenMode::authoring || mode==WorldOpenMode::read_only_runtime,"Unknown world open mode.");
        if(read_only_) {
            using project_detail::canonical_project_path;
            using project_detail::project_io_path;
            require(fs::is_regular_file(project_io_path(path_)),"A read-only runtime world must be an existing regular file.");
            path_=canonical_project_path(path_);
            protected_root_=protected_root.empty() ? path_.parent_path() :
                canonical_project_path(fs::path(std::u8string(protected_root.begin(),protected_root.end())));
            require(fs::is_directory(project_io_path(protected_root_)) && path_components_contain(path_,protected_root_),
                "Protected bundle root must contain this world.");
            exists_=true;
            disk_=read(project_io_path(path_));
        } else {
            path_=fs::weakly_canonical(fs::absolute(path_));
            lock_=std::make_unique<WriterLock>(fs::path(path_).concat(".lock"));
            exists_=fs::exists(path_);
            if(exists_)disk_=read(path_);
        }
        if (exists_) { doc_ = parse(disk_); validate(doc_);validate_animation_document(doc_,asset_directory(),model_cache_); }
        else doc_ = {{"format", "poima.authored-world"}, {"version", 1}, {"world_id", new_id()},
                     {"revision", 0}, {"entities", Json::object()}, {"retired_ids", Json::array()}, {"receipts", Json::array()}};
        authored_presentation_.publish(authored_presentation_.stage(doc_));
    }
    FrozenContent freeze_content(const Json& source) const {
        return freeze_world_content(source, asset_directory());
    }
    WorldPackageContent package_content() const { return freeze_content(doc_).package; }
    std::shared_ptr<const ui::Presentation> runtime_ui_presentation() const {
        require(bool(runtime_),"No runtime is active for UI observation.",-32030);
        return runtime_->ui_model().presentation();
    }
    WorldRuntimeStatus runtime_status() const {
        WorldRuntimeStatus status;status.available=Runtime::available();status.active=bool(runtime_);
        if(runtime_) { status.presentation_source_id=runtime_->presentation_source_id();status.session_id=runtime_id_;status.tick=runtime_->inspect().tick;status.authored_revision=runtime_definition_.authored_revision;status.structure_revision=runtime_->structure_revision();status.ui_revision=runtime_->ui_model().revision();status.control_sequence=runtime_->control_sequence();status.material_revision=runtime_->material_revision(); }
        return status;
    }
    WorldTickAdvance advance_tick(const std::string& expected_session,std::uint64_t expected_tick,
        const std::vector<RuntimeInput>& inputs) {
        require(expected_session.size()==32 && std::all_of(expected_session.begin(),expected_session.end(),[](char c) {
            return (c>='0' && c<='9') || (c>='a' && c<='f');
        }),"ID must be 32 lowercase hexadecimal characters.");
        require(expected_tick<=max_revision,"Runtime tick exceeds supported range.");
        require(bool(runtime_),"No runtime is active.",-32030);
        require(expected_session==runtime_id_,"Runtime session conflict.",-32031);
        require(expected_tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        // The native gameplay ABI has a fixed 32-controller input array. The
        // JSON route already enforces this; the typed route must do so too.
        require(inputs.size()<=32,"Runtime inputs must contain at most 32 characters.");
        for(const auto& input:inputs)require(input.entity.size()==32 && std::all_of(input.entity.begin(),input.entity.end(),[](char c) {
            return (c>='0' && c<='9') || (c>='a' && c<='f');
        }),"Input entity ID must be 32 lowercase hexadecimal characters.");
        const auto result=advance_runtime(1,inputs);
        return {result.committed_tick,result.current_tick,result.replaced,result.save_serviced};
    }
    WorldAudioState audio_state(const std::string& expected_session,std::uint64_t expected_tick,const std::string& listener) const {
        identifier(expected_session);identifier(listener);
        require(expected_tick<=max_revision,"Runtime tick exceeds supported range.");
        require(bool(runtime_),"No runtime is active.",-32030);
        require(expected_session==runtime_id_,"Runtime session conflict.",-32031);
        require(expected_tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        const auto camera=std::find_if(runtime_->live_definition().entities.begin(),runtime_->live_definition().entities.end(),
            [&](const auto& entity){return entity.id==listener && entity.camera.has_value();});
        require(camera!=runtime_->live_definition().entities.end(),"Runtime camera entity/component does not exist.",-32004);
        require(rigid_transform(runtime_->entity(listener).world),"Audio listener camera hierarchy must not scale or shear.");
        return {runtime_id_,listener,expected_tick,runtime_->audio_snapshot(listener),runtime_->sound_state().voices()};
    }
    WorldGameplayStatus gameplay_status() const {
        WorldGameplayStatus status;status.active=bool(runtime_);
        if(runtime_) { status.session_id=runtime_id_;status.tick=runtime_->inspect().tick;status.revision=runtime_->gameplay_revision(); }
        return status;
    }
    WorldComponentStatus component_status() const {
        WorldComponentStatus result;result.active=bool(runtime_);if(runtime_) {result.session_id=runtime_id_;result.tick=runtime_->inspect().tick;result.revision=runtime_->component_revision();}return result;
    }
    WorldSaveStatus save_configuration_status() const {
        return {save_configuration_generation_,save_root_.empty() ? std::string{} : path_text(save_root_)};
    }
    SceneSnapshot editor_snapshot(const EditorCamera& camera,bool live,const Json* preview=nullptr) const {
        validate_editor_camera(camera);
        if(live)require(bool(runtime_),"No runtime is active for the editor snapshot.",-32030);
        SceneSnapshot result{};
        if(live)result=runtime_->snapshot();
        else if(!preview && authored_cache_ && authored_cache_->revision==revision(doc_.at("revision")))result=*authored_cache_;
        else {
            prune_model_cache();
            result=build_editor_authored_scene(doc_,preview,asset_directory(),model_cache_,authored_presentation_);
            if(!preview)authored_cache_=result;
        }
        result.camera_id="editor";result.camera_world=camera.world;result.vertical_fov=camera.vertical_fov;result.near_plane=camera.near_plane;result.far_plane=camera.far_plane;
        if(live)runtime_->append_particles(result);
        return result;
    }
    SceneSnapshot editor_preview(const EditorCamera& camera,const std::string& id,const Json& transform) const {
        require(!runtime_,"Stop runtime before editing transforms.",-32009);
        require(doc_.at("entities").contains(id),"Preview entity does not exist.",-32004);
        validate_transform(transform);
        auto candidate=doc_;candidate["entities"][id]["components"]["Transform"]=transform;
        return editor_snapshot(camera,false,&candidate);
    }
    std::vector<WorldControllerInfo> controllers(bool live) const {
        std::vector<WorldControllerInfo> result;
        if(live) {
            require(bool(runtime_),"No runtime is active for controller enumeration.",-32030);
            for(const auto& entity:runtime_->live_definition().entities)if(entity.character && !entity.character->camera.empty())result.push_back({entity.id,entity.character->camera});
        } else {
            for(const auto& [id,entity]:doc_.at("entities").items())if(entity.at("components").contains("CharacterController")) {
                const auto& camera=entity.at("components").at("CharacterController").at("camera");
                if(!camera.is_null())result.push_back({id,camera.get<std::string>()});
            }
        }
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id<b.id;});return result;
    }
    std::vector<WorldCameraInfo> cameras(bool live) const {
        std::vector<WorldCameraInfo> result;
        if(live) {
            require(bool(runtime_),"No runtime is active for camera enumeration.",-32030);
            for(const auto& entity:runtime_->live_definition().entities)if(entity.camera) {
                const auto lens=runtime_->camera_lens(entity.id)->world;
                result.push_back({entity.id,lens.vertical_fov,lens.near_plane,lens.far_plane});
            }
        } else {
            for(const auto& [id,entity]:doc_.at("entities").items())if(entity.at("components").contains("Camera")) {
                const auto& lens=entity.at("components").at("Camera");
                result.push_back({id,lens.at("vertical_fov"),lens.at("near"),lens.at("far")});
            }
        }
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        return result;
    }
    SceneSnapshot camera_snapshot(const std::string& id,bool live) const {
        identifier(id);
        if(live) {
            require(bool(runtime_),"No runtime is active for the camera snapshot.",-32030);
            const auto found=std::find_if(runtime_->live_definition().entities.begin(),runtime_->live_definition().entities.end(),
                [&](const auto& entity){return entity.id==id&&entity.camera.has_value();});
            require(found!=runtime_->live_definition().entities.end(),"Runtime camera entity/component does not exist.",-32004);
            try { return runtime_->snapshot(id); }
            catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        }
        const auto& entities=doc_.at("entities");
        require(entities.contains(id)&&entities.at(id).at("components").contains("Camera"),"Camera entity/component does not exist.",-32004);
        const auto& lens=entities.at(id).at("components").at("Camera");
        EditorCamera camera;
        camera.world=world_matrices(entities).at(id);camera.vertical_fov=lens.at("vertical_fov");
        camera.near_plane=lens.at("near");camera.far_plane=lens.at("far");
        auto result=editor_snapshot(camera,false);result.camera_id=id;
        select_scene_camera_draw(result,lens.contains("viewmodel") ? std::optional<SceneLens>(scene_lens_value(lens.at("viewmodel"))) : std::nullopt,
            viewmodel_projection_value(lens));
        return result;
    }
    Json asset_reference_dispatch(const Json& params) const {
        fields(params,{"revision","asset","owner","after","limit"});current_revision(params);
        return inspect_asset_references(params,doc_);
    }
    Json dispatch(const std::string& method, const Json& params) {
        pump_gameplay_save_writer();
        if(auto executor=jobs::peek_owner_executor())profiling::collect_jobs(*executor);
        require(!read_only_ || std::find(authoring_methods.begin(),authoring_methods.end(),method)==authoring_methods.end(),"This packaged world is read-only; authoring and input mutations are unavailable.",-32081);
        if(method=="jobs.status") {
            fields(params,{},{});
            Json result={{"scope","owner_thread"},{"initialized",false},{"policy",{{"default_workers",2},
                {"environment","POIMA_JOB_WORKERS"},{"worker_range",{0,8}},{"configuration","Set before the first runtime or material job on this owner; zero selects serial reference execution."}}}};
            if(auto executor=jobs::peek_owner_executor()) {
                const auto state=executor->status();const auto config=executor->config();
                auto lane=[](const jobs::LaneStatus& s,const jobs::Limits& limits) {
                    return Json{{"groups",s.groups},{"tasks",s.tasks},{"dependency_links",s.dependency_links},
                        {"high_water_groups",s.high_water_groups},{"high_water_tasks",s.high_water_tasks},
                        {"limits",{{"groups",limits.groups},{"tasks",limits.tasks},{"dependency_links",limits.dependency_links}}}};
                };
                result["initialized"]=true;result["workers"]=state.workers;
                result["frame_reserved_workers"]=state.frame_reserved_workers;result["serial"]=state.serial;
                result["stopping"]=state.stopping;result["executing"]=state.executing;
                result["frame"]=lane(state.frame,config.frame);result["background"]=lane(state.background,config.background);
                result["trace_capacity_per_thread"]=config.trace_capacity_per_thread;
                result["trace_dropped"]=state.trace_dropped;
            }
            return result;
        }
        // This authored metadata read deliberately bypasses cache pruning and
        // every package/snapshot resolver. Other dispatch behavior is unchanged.
        if(method=="world.asset.references")return asset_reference_dispatch(params);
        if(method=="asset.catalog")return asset_dispatch(method,params);
        if(method=="world.reference") {
            try { return authoring_reference::read(params); }
            catch(const std::invalid_argument& error) { throw Error(-32602,error.what()); }
        }
        if(method=="world.asset.provenance" || method.starts_with("asset.provenance."))return provenance_dispatch(method,params);
        if(method.starts_with("world.navigation."))return navigation_dispatch(method,params);
        if(method.starts_with("world.terrain."))return terrain_dispatch(method,params);
        if(method=="world.foliage.plan")return foliage_plan(params);
        if(method.starts_with("world.water."))return water_plan(method,params);
        prune_model_cache();
        if(method.starts_with("profiler.")) {
            try { return profiler_.dispatch(method,params); }
            catch(const profiling::ServiceError& error) { throw Error(error.code,error.what()); }
        }
        if(method.starts_with("performance.")) {
            try {
                const auto active=player_active();
                return performance_.dispatch(method,params,active ? player_owner_->id : std::string{},
                    active && player_owner_->window->ready(),active ? std::function<void()>([this]{player_owner_->window->drain_performance();}) : std::function<void()>{});
            } catch(const frame_performance::ServiceError& e) {throw Error(e.code,e.what());}
        }
        if(method.starts_with("development.") || method=="project.inspect" || method=="project.gameplay.adopt" || method=="project.source.read" || method=="project.source.write" || method=="project.verification.read" || method=="project.verification.write" || method=="project.candidate.retain") {
            try { return development_.dispatch(method,params,path_); }
            catch(const development::ServiceError& error) { throw Error(error.code,error.what()); }
        }
        if(method.starts_with("asset.material.")) {
            try { return materials_.dispatch(method,params,asset_directory()); }
            catch(const materials::Error& error) { throw Error(error.code,error.what()); }
        }
        if(method.starts_with("player."))return player_dispatch(method,params);
        if(method.starts_with("save.")) {
            if(player_active() && (method=="save.write" || method=="save.load"))player_owner_->window->note_performance_intervention(frame_performance::Flag::storage);
            return save_dispatch(method,params);
        }
        if(method.starts_with("input."))return dispatch_input_profile(method,params,preference_paths(),gamepad_host_);
        if(method.starts_with("settings."))return dispatch_saved_player_settings(method,params,preference_paths());
        if (method == "world.describe" || method == "world.describe.select") {
            if(method=="world.describe.select")validate_discovery_selection(params);
            else (void)discovery_view(params);
            auto result=world_schema::describe(sky_json(SkySettings{}),max_revision,max_document_bytes);result["methods"].update(profiling::Service::schemas());result["methods"].update(frame_performance::Service::schemas());result["methods"].update(development::Service::schemas());result["development"]={{"execution","Trusted authoring-only compilation; native publish/export require immutable host-selected profiles; no shell or automatic runtime reload"},{"jobs","Single background worker, 32 retained jobs, bounded diagnostic tails; inspect/cancel/forget"},{"diagnostics","Bounded recognized MSBuild/C# records without raw logs; state/exit code remain authoritative, no errors inferred from an empty list"},{"receipts","128 session-local development receipts; method and profile identity guarded; expired IDs rejected within 4096-request lifetime budget"},{"paths","Absolute executable, project and output; cwd is project parent; generated Debug/Release dotnet build arguments"},{"qualification","See DEVELOPMENT_JOBS.md; source availability is separate from shipped-package qualification"}};result["profiler"]={{"capacity","64..65536 fixed events; allocation occurs at capture start"},{"lifetime","Session-owned and diagnostic only; runtime replacement/rollback does not discard observations"},{"reading","Stop before immutable paged reading; full capture stops accepting events and reports loss"},{"scope","CPU owner and native job worker threads, separate GPU duration samples; no calibrated GPU/CPU timeline, managed stacks or allocation/VRAM profiler"}};result["performance"]={{"scope","Ready native player polls; bounded independent frame capture with completed GPU retirement"},{"capacity","1..262144 rows; start preallocates storage, record paths do not allocate"},{"stop","Close admission, drain actual submissions, report loss explicitly, freeze before paged reading"},{"timing","Present-return cadence includes host gaps; CPU wall stages overlap; GPU durations have an independent clock; no scanout/input latency or VRAM claim"}};result["read_only"]=read_only_;result["mode"]=read_only_ ? "read_only_runtime" : "authoring";
            result["authoring_contract"]=authoring_contract_identity(read_only_);
            if(read_only_) { result["unavailable_mutations"]=authoring_methods;for(const auto* name:authoring_methods)result["methods"].erase(name); }
            return result;
        }
        if(method=="world.ui.layout") {
            fields(params,{"revision","width","height","scale"},{"revision","width","height"});
            current_revision(params);
            const auto width=revision(params.at("width")),height=revision(params.at("height"));
            require(width>=1 && width<=8192 && height>=1 && height<=8192,"UI layout extent must be 1..8192.");
            require(!params.contains("scale") || params.at("scale").is_number(),"UI scale must be numeric.");
            const auto scale=params.value("scale",1.0);
            require(std::isfinite(scale) && scale>=.25 && scale<=8,"UI scale must be .25..8.");
#if POIMA_GAME_UI
            auto definition=ui::parse_definition(doc_.value("ui",Json::object()).dump());
            ModelCache assets;
            resolve_ui_images(definition,asset_directory(),assets);
            const ui::Model model(definition);
            UiPresenter presenter;
            (void)presenter.frame(model.presentation(),static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),static_cast<float>(scale));
            auto controls=Json::array();
            for(const auto& row:presenter.inspect())controls.push_back({{"id",row.id},{"kind",row.kind==UiElementKind::button ? "button" : "label"},
                {"bounds",row.bounds},{"clip",row.clip},{"hittable",row.hittable},{"visible",row.visible},{"enabled",row.enabled},{"focused",row.focused}});
            return {{"revision",doc_.at("revision")},{"width",width},{"height",height},{"scale",scale},{"controls",std::move(controls)}};
#else
            throw Error(-32003,"Native UI presentation is not available in this build.");
#endif
        }
        if(method=="world.ui.get" || method=="world.ui.list") {
            const bool single=method=="world.ui.get";
            if(single)fields(params,{"id","revision"},{"id"});else fields(params,{"revision","after","limit"});
            current_revision(params);if(params.contains("after"))require(params.contains("revision"),"Pagination requires a revision.");
            const auto catalog=doc_.value("ui",Json::object());Json result={{"revision",doc_.at("revision")}};
            if(single) {
                const auto id=identifier(params.at("id"));require(catalog.contains(id),"UI element does not exist.",-32004);
                result["id"]=id;result["element"]=catalog.at(id);return result;
            }
            const auto after=params.contains("after") ? identifier(params.at("after")) : std::string{};
            const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;require(limit>=1 && limit<=256,"UI page limit must be 1..256.");
            Json rows=Json::array(),next=nullptr;
            for(const auto& [id,element]:catalog.items())if(id>after) {
                if(rows.size()==limit) {next=rows.back().at("id");break;}
                rows.push_back({{"id",id},{"element",element}});
            }
            result["elements"]=std::move(rows);result["next_after"]=next;return result;
        }
        if(method=="template.get" || method=="template.query") {
            if(method=="template.get")fields(params,{"id","revision"},{"id"});else fields(params,{"revision","after","limit"});
            current_revision(params);if(params.contains("after"))require(params.contains("revision"),"Pagination requires a revision.");
            return inspect_templates(doc_,params,method=="template.get");
        }
        if(method=="component.schemas") {
            fields(params,{"id"});const auto schemas=authored_component_schemas(doc_);Json values=Json::array();
            if(params.contains("id"))values.push_back(Json::parse(components::schema_json(authored_schema(schemas,identifier(params.at("id"))))));
            else for(const auto& schema:schemas)values.push_back(Json::parse(components::schema_json(schema)));
            return {{"revision",doc_.at("revision")},{"schemas",std::move(values)}};
        }
        if(method=="component.schema.import") {
            fields(params,{"request_id","base_revision","manifest"},{"request_id","base_revision","manifest"});
            identifier(params.at("request_id"));revision(params.at("base_revision"));
            const auto schemas=component_checked([&]{return components::parse_manifest(params.at("manifest").dump());});require(!schemas.empty(),"Schema import requires at least one schema.");
            Json ops=Json::array();for(const auto& schema:schemas)ops.push_back({{"op","component.schema.set"},{"schema",Json::parse(components::schema_json(schema))}});
            return transact({{"request_id",params.at("request_id")},{"base_revision",params.at("base_revision")},{"ops",ops}},"component.schema.import");
        }
        if(method=="world.dependencies") {
            fields(params,{});const auto content=package_content();Json assets=Json::array();
            for(const auto& asset:content.assets)assets.push_back({{"filename",asset.filename},{"sha256",asset.sha256},{"bytes",asset.bytes}});
            Json result={{"revision",content.revision},{"needs_audio",content.needs_audio},{"assets",assets}};
            if(content.needs_navigation)result["needs_navigation"]=true;
            if(content.needs_particles)result["needs_particles"]=true;
            if(content.needs_sprite_particles)result["needs_sprite_particles"]=true;
            if(content.needs_particle_collisions)result["needs_particle_collisions"]=true;
            if(content.needs_particle_impact_bursts)result["needs_particle_impact_bursts"]=true;
            if(content.needs_surface_mark_masks)result["needs_surface_mark_masks"]=true;
            if(content.needs_mesh_lod)result["needs_mesh_lod"]=true;
            if(content.needs_water_rain)result["needs_water_rain"]=true;
            if(content.needs_water_ripples)result["needs_water_ripples"]=true;
            if(content.needs_water_wave_layers)result["needs_water_wave_layers"]=true;
            if(content.needs_water_listener_audio)result["needs_water_listener_audio"]=true;
            if(content.needs_water_optics)result["needs_water_optics"]=true;
            if(content.needs_water_refraction)result["needs_water_refraction"]=true;
            if(content.needs_water_caustics)result["needs_water_caustics"]=true;
            if(content.needs_water_shore_foam)result["needs_water_shore_foam"]=true;
            if(content.needs_water_terrain_fit)result["needs_water_terrain_fit"]=true;
            if(content.needs_water_surface_swell)result["needs_water_surface_swell"]=true;
            if(content.needs_water_swell_variation)result["needs_water_swell_variation"]=true;
            if(content.needs_water_rough_reflection)result["needs_water_rough_reflection"]=true;
            if(content.needs_water_solar_scattering)result["needs_water_solar_scattering"]=true;
            if(content.needs_water_offshore)result["needs_water_offshore"]=true;
            if(content.needs_snow_water)result["needs_snow_water"]=true;
            if(content.needs_physics_checkpoint)result["needs_physics_checkpoint"]=true;
            if(content.needs_output_grade)result["needs_output_grade"]=true;
            if(content.needs_auto_exposure)result["needs_auto_exposure"]=true;
            if(content.needs_atmosphere_sky)result["needs_atmosphere_sky"]=true;
            if(content.needs_orbital_atmosphere)result["needs_orbital_atmosphere"]=true;
            if(content.needs_aerial_perspective)result["needs_aerial_perspective"]=true;
            if(content.needs_volumetric_clouds)result["needs_volumetric_clouds"]=true;
            if(content.needs_cloud_shadows)result["needs_cloud_shadows"]=true;
            if(content.needs_screen_reflections)result["needs_screen_reflections"]=true;
            if(content.needs_depth_of_field)result["needs_depth_of_field"]=true;
            if(content.needs_foliage_motion)result["needs_foliage_motion"]=true;
            if(content.needs_height_fog)result["needs_height_fog"]=true;
            if(!content.provenance_records.empty()) {
                Json records=Json::array();
                for(const auto& f:content.provenance_records)records.push_back({{"filename",f.filename},{"sha256",f.sha256},{"bytes",f.bytes}});
                result["provenance_records"]=std::move(records);result["asset_credits_sha256"]=content_hash(content.asset_credits);
            }
            return result;
        }
        if (method == "world.inspect") {
            fields(params, {});
            Json result={{"world_id", doc_.at("world_id")}, {"revision", doc_.at("revision")},
                    {"entity_count", doc_.at("entities").size()}, {"persisted", exists_},{"read_only",read_only_},{"mode",read_only_ ? "read_only_runtime" : "authoring"},
                    {"coordinate_system", "right-handed Y-up; meters; local XYZW quaternion transforms"}};
            if(doc_.contains("navigation"))result["navigation"]=doc_.at("navigation");
            if(doc_.contains("water"))result["water"]=doc_.at("water");
            if(doc_.contains("snow"))result["snow"]=doc_.at("snow");
            if(doc_.contains("physics_checkpoint"))result["physics_checkpoint"]=doc_.at("physics_checkpoint");
            if(doc_.contains("surface_mark_profiles"))result["surface_mark_profiles"]=doc_.at("surface_mark_profiles");
            if(doc_.contains("audio_buses"))result["audio_buses"]=doc_.at("audio_buses");
            return result;
        }
        if(method=="world.history") {
            fields(params,{});return {{"revision",doc_.at("revision")},{"undo_count",undo_.size()},{"redo_count",redo_.size()},
                {"bytes",history_bytes(undo_,redo_)},{"max_entries",32},{"max_bytes",max_document_bytes},{"session_local",true},{"skipped_large_edits",skipped_large_edits_}};
        }
        if(method=="world.undo" || method=="world.redo")return restore_history(method,params);
        if(method=="world.lighting") { fields(params,{"revision"});current_revision(params);auto result=lighting_json(authored_lighting(doc_,world_matrices(doc_.at("entities"))));result["revision"]=doc_.at("revision");return result; }
        if (method == "entity.material")return inspect_material(params);
        if (method == "entity.get") {
            fields(params, {"id", "revision", "component"}, {"id"}); current_revision(params);
            auto value = entity(doc_, params.at("id"));
            if (params.contains("component")) {
                require(params.at("component").is_string() && value.at("components").contains(params.at("component").get<std::string>()), "Entity does not have the requested component.", -32004);
                value = value.at("components").at(params.at("component").get<std::string>());
            }
            return {{"revision", doc_.at("revision")}, {"id", params.at("id")}, {"value", value}};
        }
        if (method == "entity.query") {
            fields(params, {"revision", "parent", "after", "limit", "component"}); current_revision(params);
            if(params.contains("component") && params.at("component").is_string() && custom_component(params.at("component").get<std::string>())) {
                const auto schemas=authored_component_schemas(doc_);(void)authored_schema(schemas,custom_type(params.at("component").get<std::string>()));
            } else if (params.contains("component")) require(params["component"] == "Transform" || params["component"] == "Camera" || params["component"] == "CameraDraw" || params["component"] == "MeshRenderer" || params["component"] == "BoxCollider" || params["component"] == "MeshCollider" || params["component"] == "CharacterController" || params["component"] == "StaticMesh" || params["component"] == "PbrMaterial" || params["component"] == "PbrTextures" || params["component"] == "Light" || params["component"] == "LightingEnvironment" || params["component"] == "AcousticMaterial" || params["component"] == "AudioEmitter" || params["component"] == "AnimationRig" || params["component"] == "PhysicalRig" || params["component"] == "RigNode" || params["component"] == "SkinnedMesh" || params["component"] == "HitRegion" || params["component"] == "ParticleEmitter" || params["component"] == "FootprintSurface" ||
                params["component"] == "SurfaceWetness", "Unknown component type.");
            const auto after = params.contains("after") ? identifier(params.at("after")) : std::string{};
            if (params.contains("after")) require(params.contains("revision"), "Pagination requires a revision.");
            if (params.contains("parent") && !params.at("parent").is_null()) identifier(params.at("parent"));
            const auto limit = params.contains("limit") ? revision(params.at("limit")) : 64;
            require(limit >= 1 && limit <= 256, "Query limit must be 1..256.");
            Json result = Json::array(); Json next = nullptr;
            for (const auto& [id, e] : doc_.at("entities").items()) {
                if (id <= after || (params.contains("parent") && params.at("parent") != e.at("parent")) ||
                    (params.contains("component") && !e.at("components").contains(params["component"].get<std::string>()))) continue;
                if (result.size() == limit) { next = result.back().at("id"); break; }
                Json types = Json::array();
                for (const auto& [type, unused] : e.at("components").items()) { (void)unused; types.push_back(type); }
                result.push_back({{"id", id}, {"name", e.at("name")}, {"parent", e.at("parent")}, {"components", types}});
            }
            return {{"revision", doc_.at("revision")}, {"entities", result}, {"next_after", next}};
        }
        if (method == "entity.world_transform") {
            fields(params, {"id", "revision"}, {"id"}); current_revision(params); entity(doc_, params.at("id"));
            try {
                return {{"id", params.at("id")}, {"revision", doc_.at("revision")},
                    {"matrix", world_matrices(doc_.at("entities")).at(identifier(params.at("id")))}, {"layout", "column_major"}};
            } catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
        }
        if (method.starts_with("asset.")) return asset_dispatch(method,params);
        if (method=="world.audio.inspect" || method=="world.audio.capture") return audio_dispatch(method,params,false);
        if (method == "world.capture") return capture(params);
        if (method.starts_with("runtime.")) return runtime_dispatch(method,params);
        if (method == "world.transact") return transact(params);
        if (method == "session.close") { fields(params, {}); return {{"closed", true}}; }
        throw Error(-32601, "Unknown world method.");
    }
    fs::path asset_directory() const { return fs::path(path_).concat(".assets"); }
#include "world_navigation.inc"
#include "world_provenance.inc"
    void validate_mesh_lod_edit(const Json& document,const std::set<std::string>& changed) const {
        const auto& entities=document.at("entities");
        // Only introduced/changed LOD or texture bindings need cooked validation.
        // General open/inspection and unrelated edits keep working with missing assets.
        for(const auto& id:changed)if(entities.contains(id)) {
            const auto& components=entities.at(id).at("components");
            if(!components.contains("StaticMesh") || !components.at("StaticMesh").contains("lods"))continue;
            if(doc_.at("entities").contains(id)) {
                const auto& prior=doc_.at("entities").at(id).at("components");
                if(!optional_authored_value_differs(prior,components,"StaticMesh") &&
                    !optional_authored_value_differs(prior,components,"PbrTextures"))continue;
            }
            (void)resolve_mesh_component(components,asset_directory(),model_cache_);
        }
    }
    Json inspect_material(const Json& params) {
        fields(params,{"id","revision"},{"id"});current_revision(params);
        const auto& value=entity(doc_,params.at("id"));
        return inspect_entity_material(value.at("components"),params.at("id"),doc_.at("revision"),asset_directory(),model_cache_);
    }
    Json image_dispatch(const std::string& method,const Json& params) const {
        try {
            LoadedImage loaded;
            if(method=="asset.image.import") {
                fields(params,{"source","color_space"},{"source","color_space"});require(params.at("source").is_string(),"Image source path must be a string.");
                const auto text=params.at("source").get<std::string>();require(!text.empty() && text.find('\0')==std::string::npos,"Invalid image source path.");
                require(params.at("color_space")=="srgb" || params.at("color_space")=="linear","Image color space must be srgb or linear.");
                auto source=fs::path(std::u8string(text.begin(),text.end()));if(source.is_relative())source=path_.parent_path()/source;
                loaded=store_image_asset(asset_directory(),source,params.at("color_space")=="srgb");
            } else {
                fields(params,{"asset"},{"asset"});require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid image asset ID.");
                loaded=read_image_asset(asset_directory(),params.at("asset"));
            }
            Json mips=Json::array();for(const auto& mip:loaded.image->mips)mips.push_back({{"width",mip.width},{"height",mip.height},{"bytes",mip.rgba.size()}});
            return {{"asset",loaded.id},{"format","poima.image.v1"},{"bytes",loaded.bytes},{"color_space",loaded.image->srgb ? "srgb" : "linear"},{"mips",mips}};
        } catch(const Error&) { throw; }catch(const std::exception& error) { throw Error(-32050,error.what()); }
    }
    Json import_post_effect(const Json& params) const {
        fields(params,{"source","expected_sha256"},{"source","expected_sha256"});
        const auto& source=params.at("source");const auto& hash=params.at("expected_sha256");
        require(source.is_string(),"Post-effect source must be a file path.");
        const auto& name=source.get_ref<const std::string&>();
        require(!name.empty() && name.find('\0')==std::string::npos,"Invalid post-effect source path.");
        require(hash.is_string() && valid_asset_id(hash.get_ref<const std::string&>()),
            "Expected source SHA-256 must contain 64 lowercase hexadecimal characters.");
        const auto& digest=hash.get_ref<const std::string&>();
        try {
            auto path=fs::path(std::u8string(name.begin(),name.end()));
            require(path.extension()==".poimapost","Post-effect source must have a .poimapost extension.");
            if(path.is_relative())path=path_.parent_path()/path;
            const auto effect=import_post_effect_asset(asset_directory(),path,digest);
            return {{"asset",effect.digest},{"id",effect.id},{"bytes",effect.bytes},{"words",effect.words}};
        } catch(const Error&) { throw; }
        catch(const std::exception& error) { throw Error(-32050,error.what()); }
    }
    Json audio_asset_dispatch(const std::string& method,const Json& params) const {
        try {
            LoadedAudio loaded;
            if(method=="asset.audio.import") {
                fields(params,{"source"},{"source"});require(params.at("source").is_string(),"WAV source must be a path.");
                const auto text=params.at("source").get<std::string>();require(!text.empty() && text.find('\0')==std::string::npos,"Invalid WAV source path.");
                auto source=fs::path(std::u8string(text.begin(),text.end()));if(source.is_relative())source=path_.parent_path()/source;loaded=store_audio_asset(asset_directory(),source);
            } else { fields(params,{"asset"},{"asset"});require(params.at("asset").is_string(),"Audio asset must be a hash.");loaded=read_audio_asset(asset_directory(),params.at("asset")); }
            return {{"asset",loaded.id},{"format","poima.audio.v1"},{"bytes",loaded.bytes},{"sample_rate",audio_rate},{"channels",1},{"frames",loaded.clip->samples.size()},{"duration_seconds",double(loaded.clip->samples.size())/audio_rate}};
        }catch(const Error&) { throw; }catch(const std::exception& e) { throw Error(-32050,e.what()); }
    }
    Json animation_dispatch(const std::string& method,const Json& params) const {
        if(method=="asset.animation.author")return author_animation(params,asset_directory());
        if(method=="asset.animation.physical_limits") {
            fields(params,{"entity","revision","clip","time","loop"},{"entity","revision","clip","time"});
            current_revision(params);
            return inspect_animation_physical_limits(params,doc_,asset_directory());
        }
        return inspect_animation_asset(method,params,asset_directory());
    }
#include "world_rigid_mesh.inc"
#include "world_terrain_service.inc"
#include "world_foliage_service.inc"
#include "world_water_service.inc"
#include "world_particles.inc"
#include "world_surface_marks.inc"
#include "world_snow_service.inc"
    Json asset_dispatch(const std::string& method,const Json& params) const {
        if(method=="asset.catalog" || method=="asset.label") {
            const bool label=method=="asset.label";
            if(label)fields(params,{"asset","kind","label"},{"asset","kind","label"});
            else fields(params,{"kind","after","limit"});
            const asset_catalog::Kind* kind=nullptr;
            if(params.contains("kind")) {
                require(params.at("kind").is_string(),"Asset catalog kind must be a string.");
                kind=asset_catalog::kind_named(params.at("kind").get_ref<const std::string&>());
                require(kind,"Choose model, image or audio.");
            }
            if(label) {
                require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get_ref<const std::string&>()),"Invalid asset ID.");
                require(params.at("label").is_string() && asset_catalog::valid_label(params.at("label").get_ref<const std::string&>()),
                    "Asset label must be 1..1024 UTF-8 bytes without path separators or control characters.");
                try { return asset_catalog::remember(asset_directory(),params.at("asset"),*kind,params.at("label")); }
                catch(const std::exception& error) { throw Error(-32050,asset_diagnostic(error.what())); }
            }
            const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;
            require(limit>=1 && limit<=64,"Asset catalog page limit must be 1..64.");
            std::string after;
            if(params.contains("after")) {
                require(params.at("after").is_string(),"Asset catalog cursor must be a string.");
                after=params.at("after");
                const auto* cursor_kind=asset_catalog::package_kind(after);
                require(cursor_kind && (!kind || kind==cursor_kind),"Invalid asset catalog cursor for this kind.");
            }
            try { return asset_catalog::list(asset_directory(),kind,after,static_cast<std::size_t>(limit)); }
            catch(const std::exception& error) { throw Error(-32050,asset_diagnostic(error.what())); }
        }
        if(method=="asset.mesh.extract_rigid")return extract_rigid_mesh(params);
        if(method=="asset.animation.capture")return capture(params,false,true);
        if(method.starts_with("asset.animation."))return animation_dispatch(method,params);
        if(method=="asset.audio.import" || method=="asset.audio.inspect")return audio_asset_dispatch(method,params);
        if(method=="asset.post-effect.import")return import_post_effect(params);
        if(method=="asset.image.import" || method=="asset.image.inspect")return image_dispatch(method,params);
        if(method=="asset.source.inspect")return source_model_inspection(params,path_);
        return model_asset_dispatch(method,params,path_,asset_directory());
    }
    PreferencePaths preference_paths() const {
        return {path_,asset_directory(),protected_root_,read_only_};
    }
    fs::path input_profile_path(const Json& value) const {
        return resolve_preference_profile_path(value,preference_paths(),PreferenceKind::input);
    }
    Json apply_player_settings(const Json& params,PlayerOptions& options) const {
        return world_detail::apply_player_settings(params,options,preference_paths(),*runtime_);
    }
    Json capture(const Json& params, bool live=false,bool asset_preview=false) const {
        std::optional<float> ui_scale;
        bool reuse_renderer = false;
        if(live) {
            fields(params, {"session_id","tick","structure_revision","material_revision","ui_revision","ui_scale","camera","path","width","height","gpu","samples","viewmodel_samples","culling","draw_instancing","gpu_visibility","clustered_lighting","frames_in_flight","scene_debug_view","scene_product_probes","lighting_path","ambient_occlusion","reconstruction","shadow_filter","post_aa","capture_frames","profile","reuse_renderer"}, {"session_id","tick","camera","path"});
            require(!params.contains("reuse_renderer") || params.at("reuse_renderer").is_boolean(),
                "Capture reuse_renderer must be Boolean.");
            reuse_renderer = params.value("reuse_renderer", false);
            runtime_guard(params); require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            structure_read_guard(params);
            material_guard(params,"material_revision");
            require(runtime_->ui_model().definition().empty() || params.contains("ui_revision"),"UI-bearing captures require ui_revision.");
            if(params.contains("ui_revision"))require(revision(params.at("ui_revision"))==runtime_->ui_model().revision(),"UI revision conflict.",-32009);
            if(params.contains("ui_scale")) {
                require(params.at("ui_scale").is_number(),"Capture UI scale must be numeric.");
                const auto density=params.at("ui_scale").get<double>();
                require(std::isfinite(density) && density>=.25 && density<=8,"Capture UI scale must be .25..8.");
                ui_scale=static_cast<float>(density);
            }
        } else {
            if(asset_preview)fields(params,{"revision","camera","path","width","height","gpu","samples","viewmodel_samples","culling","draw_instancing","gpu_visibility","clustered_lighting","frames_in_flight","scene_debug_view","scene_product_probes","lighting_path","ambient_occlusion","reconstruction","shadow_filter","post_aa","capture_frames","profile","asset","clip","time","loop","skinning"},{"revision","camera","path","asset","time"});
            else fields(params, {"revision", "camera", "path", "width", "height", "gpu", "samples", "viewmodel_samples", "culling", "draw_instancing","gpu_visibility", "clustered_lighting", "frames_in_flight", "scene_debug_view", "scene_product_probes", "lighting_path", "ambient_occlusion", "reconstruction", "shadow_filter", "post_aa", "capture_frames", "profile"}, {"revision", "camera", "path"});
            current_revision(params);
        }
        const auto camera_id = identifier(params.at("camera"));
        if(live) {
            const auto found=std::find_if(runtime_->live_definition().entities.begin(),runtime_->live_definition().entities.end(),
                [&](const auto& e){return e.id==camera_id && e.camera.has_value();});
            require(found!=runtime_->live_definition().entities.end(),"Runtime camera entity/component does not exist.",-32004);
        }
        if(!live) require(doc_.at("entities").contains(camera_id) && doc_.at("entities").at(camera_id).at("components").contains("Camera"), "Camera entity/component does not exist.", -32004);
        const auto options=decode_render_options(params,path_,asset_directory());
        if (reuse_renderer && read_only_) {
            const fs::path output(std::u8string(options.capture.begin(), options.capture.end()));
            require(!path_components_contain(output, protected_root_),
                "Retained capture output must be outside the immutable game bundle.");
        }
        SceneSnapshot snapshot;Json animation_info=nullptr;
        snapshot.world_id = doc_.at("world_id"); snapshot.revision = revision(doc_.at("revision")); snapshot.camera_id = camera_id;
        try {
            if(live) snapshot=runtime_->snapshot(camera_id);
            else {
                const auto& lens=doc_.at("entities").at(camera_id).at("components").at("Camera");
                snapshot.vertical_fov=lens.at("vertical_fov");snapshot.near_plane=lens.at("near");snapshot.far_plane=lens.at("far");
                const auto matrices=world_matrices(doc_.at("entities"));
                snapshot.camera_world=matrices.at(camera_id);snapshot.lighting=authored_lighting(doc_,matrices);
                if(lens.contains("focus_target")) {
                    const auto target=focus_target_value(lens.at("focus_target"));
                    snapshot.camera_focus_anchor=focus_anchor(target,matrices.at(target.entity));
                }
                resolve_scene_post_effect(snapshot,asset_directory());
                require(rigid_transform(snapshot.camera_world),"Camera hierarchy must not scale or shear the camera.");
                if(asset_preview)animation_info=append_asset_animation_preview(snapshot,params,asset_directory());
                else append_captured_authored_geometry(snapshot,doc_,matrices,asset_directory(),model_cache_,authored_presentation_);
            }
        } catch(const Error&) { throw; }
        catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
        if(!live && asset_preview) { snapshot.presentation_source_id=new_presentation_source_id();for(auto& object:snapshot.objects)object.incarnation=1; }
        if(!live && !asset_preview) {
            const auto& authored_lens=doc_.at("entities").at(camera_id).at("components").at("Camera");
            select_scene_camera_draw(snapshot,authored_lens.contains("viewmodel") ? std::optional<SceneLens>(scene_lens_value(authored_lens.at("viewmodel"))) : std::nullopt,
                viewmodel_projection_value(authored_lens));
        }
        const Json lens={{"vertical_fov",snapshot.vertical_fov},{"near",snapshot.near_plane},{"far",snapshot.far_plane}};
        if(ui_scale && snapshot.logical_ui && !snapshot.logical_ui->elements.empty()) {
#if POIMA_GAME_UI
            // Capture-only density is independent of the hidden window's monitor.
            // Reuse the existing native packet boundary; live view/input state is
            // not involved, and omitting the option retains legacy window DPI.
            UiPresenter presenter;
            snapshot.ui=presenter.frame(snapshot.logical_ui,options.width,options.height,*ui_scale);
            snapshot.logical_ui.reset();
#else
            throw Error(-32003,"Native UI presentation is not available in this build.");
#endif
        }
        bool renderer_reused = false;
        RenderReport report;
        if (reuse_renderer) {
            auto key = params;
            for (const auto* field : {"tick","path","capture_frames","structure_revision","material_revision","ui_revision","reuse_renderer"})
                key.erase(field);
            renderer_reused = capture_renderer_ && capture_renderer_key_==key;
            if (!renderer_reused) {
                capture_renderer_.reset();
                capture_renderer_key_ = std::move(key);
                capture_renderer_ = std::make_unique<SceneCaptureSession>();
            }
            try {
                report = capture_renderer_->capture(options, snapshot);
                if (!report.success || !report.diagnostics.scene_presented ||
                    !same_renderer_snapshot(*report.diagnostics.scene_presented,renderer_scene_result(snapshot)))
                    capture_renderer_.reset();
            } catch (...) {
                capture_renderer_.reset();
                throw;
            }
        } else {
            capture_renderer_.reset();
            report = run_render_scene(options, snapshot);
        }
        require(report.available, report.detail, -32003);
        require(report.success, report.detail, -32020);
        require(report.diagnostics.scene_presented && same_renderer_snapshot(*report.diagnostics.scene_presented,renderer_scene_result(snapshot)),"Capture presentation receipt differs from its immutable scene snapshot.",-32020);
        Json result = {{"world_id", snapshot.world_id}, {"revision", snapshot.revision}, {"camera", camera_id},
            {"source",asset_preview ? "asset_animation" : live ? "runtime" : "authored"}, {"animation",animation_info}, {"tick",live ? Json(runtime_->inspect().tick) : Json(nullptr)},
            {"session_id",live ? Json(runtime_id_) : Json(nullptr)}, {"material_revision",snapshot.material_revision}, {"ui_revision",live ? Json(runtime_->ui_model().revision()) : Json(nullptr)}, {"camera_world", snapshot.camera_world}, {"lens", lens}, {"object_count", snapshot.world_object_count()},{"lighting",lighting_json(snapshot.lighting)},
            {"path", options.capture}, {"format", "BMP"}, {"width", report.width}, {"height", report.height},
            {"samples", report.samples}, {"gpu", report.gpu_name}, {"hardware", report.hardware},
            {"ui_scale",ui_scale ? params.at("ui_scale") : Json(nullptr)},
            {"frames_presented", report.frames_presented}, {"capture_written", report.capture_written},
            {"nvrhi_errors", report.validation_errors}, {"build_version", build_metadata().version},{"render_diagnostics",render_diagnostics(report.diagnostics)},
            {"renderer", "forward static and skinned geometry; legacy preview or GGX metallic/roughness with PNG/JPEG material maps; authored lighting with optional cascaded directional, point and spot shadow maps; explicit preview fallback"}};
        if (reuse_renderer) result["renderer_reused"] = renderer_reused;
        return result;
    }
    static Json describe_water_audio(const AudioWaterMedium& medium) {
        return {{"submersion_m", medium.submersion_m}, {"target_blend", medium.blend},
            {"cutoff_hz", medium.cutoff_hz}, {"gain", medium.gain}};
    }
    Json audio_dispatch(const std::string& method,const Json& params,bool live) const {
        const bool capture_audio=std::string_view(method).ends_with("capture");
        if(live) {
            if(capture_audio)fields(params,{"session_id","tick","listener","path","frames","early_reflections"},{"session_id","tick","listener","path"});
            else fields(params,{"session_id","tick","listener","early_reflections"},{"session_id","tick","listener"});
            runtime_guard(params);require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        } else {
            if(capture_audio)fields(params,{"revision","listener","path","frames","early_reflections"},{"revision","listener","path"});
            else fields(params,{"revision","listener","early_reflections"},{"revision","listener"});
            current_revision(params);
        }
        require(audio_available(),"Audio observation is not built. Configure POIMA_ENABLE_AUDIO=ON.",-32003);
        const auto listener=identifier(params.at("listener"));const auto frame_count=capture_audio ? revision(params.value("frames",Json(48000))) : 0;
        require(!capture_audio || (frame_count>=1 && frame_count<=480000),"Audio capture frames must be 1..480000.");
        const auto reflections=params.value("early_reflections",Json(false));
        require(reflections.is_boolean(),"early_reflections must be Boolean.");
        std::string output;if(capture_audio)output=capture_output_path(params.at("path"),path_,asset_directory());
        try {
            AudioSnapshot snapshot;RuntimeDefinition definition;std::map<std::string,Matrix4> matrices;
            snapshot.early_reflections=reflections.get<bool>();
            if(!live) { definition=resolve_runtime_definition(doc_, asset_directory(), model_cache_, AuthoredRuntimeView::audio_only);matrices=world_matrices(doc_.at("entities")); }
            const auto& source_definition=live ? runtime_->live_definition() : definition;
            auto pose=[&](const std::string& id) { return live ? runtime_->entity(id).world : matrices.at(id); };
            require(std::any_of(source_definition.entities.begin(),source_definition.entities.end(),[&](const auto& e){return e.id==listener;}),"Audio listener entity does not exist.",-32004);
            snapshot.listener=pose(listener);
            if (live) snapshot.water_medium=audio_detail::listener_water_medium(runtime_->water_region(),snapshot.listener);
            else if (doc_.contains("water")) {
                const auto asset=resolve_water_binding(doc_,asset_directory()).first;
                const water::Region region(asset.definition);
                snapshot.water_medium=audio_detail::listener_water_medium(&region,snapshot.listener);
            }
            Json geometry=Json::array();
            for(const auto& e:source_definition.entities) {
                if(e.emitter && e.emitter->enabled)snapshot.sources.push_back({e.id,*e.emitter,pose(e.id)});
                if(!e.acoustics || !e.acoustics->enabled)continue;
                AcousticGeometry g;g.entity=e.id;g.world=pose(e.id);g.material=*e.acoustics;
                std::string shape;
                if(e.collider) { for(std::size_t c=0;c<3;++c)for(std::size_t r=0;r<3;++r)g.world[c*4+r]*=2*e.collider->half_extents[c];shape="box_collider"; }
                else if(e.mesh) { g.mesh=e.mesh->mesh;shape=g.mesh ? "static_mesh" : "box_renderer"; }
                else throw Error(-32602,"Acoustic material lacks geometry.");
                geometry.push_back({{"entity",e.id},{"shape",shape},{"world_matrix",g.world},{"absorption",g.material.absorption},{"scattering",g.material.scattering},{"transmission",g.material.transmission}});snapshot.geometry.push_back(std::move(g));
            }
            auto report=observe_audio(snapshot,static_cast<std::uint32_t>(frame_count));Json paths=Json::array();
            for(const auto& p:report.paths) {
                Json path={{"entity",p.entity},{"asset",p.asset},{"source_position",p.source},{"listener_position",p.listener},{"listener_local_direction",p.direction},{"distance_m",p.distance},{"propagation_delay_samples",p.propagation_delay_samples},{"distance_gain",p.distance_gain},{"direct_visibility",p.occlusion},{"air_absorption",p.air},{"transmission",p.transmission}};
                if(snapshot.early_reflections) {
                    path["early_reflection"]=nullptr;
                    if(p.reflection) {
                        const auto& reflection=*p.reflection;
                        path["early_reflection"]={{"entity",reflection.entity},{"face",reflection.face},
                            {"point_world",reflection.point},{"listener_local_direction",reflection.direction},
                            {"distance_m",reflection.distance},{"propagation_delay_samples",reflection.propagation_delay_samples},
                            {"distance_gain",reflection.distance_gain},{"bands",reflection.bands}};
                    }
                }
                paths.push_back(std::move(path));
            }
            for(std::size_t i=0;i<snapshot.sources.size();++i) {
                paths[i]["gain"]=snapshot.sources[i].emitter.gain;paths[i]["loop"]=snapshot.sources[i].emitter.loop;paths[i]["clip_frames"]=snapshot.sources[i].emitter.clip->samples.size();
                describe_audio_routing(paths[i],snapshot.sources[i].emitter);
            }
            Json result={{"world_id",source_definition.world_id},{"revision",source_definition.authored_revision},{"tick",live ? Json(runtime_->inspect().tick) : Json(nullptr)},{"session_id",live ? Json(runtime_id_) : Json(nullptr)},
                {"source",live ? "runtime" : "authored"},{"listener",listener},{"listener_world",snapshot.listener},{"backend","Steam Audio 4.8.1; CPU direct paths; default HRTF"},{"build_version",build_metadata().version},
                {"geometry",geometry},{"triangles",report.triangles},{"sources",paths},{"scene_ms",report.scene_ms},{"simulation_ms",report.simulation_ms},{"dsp_ms",report.dsp_ms},
                {"reflections",snapshot.early_reflections},{"diffraction",false},{"device_playback",false},{"snapshot_policy","fresh synchronous frozen geometry/poses; capture restarts clips at sample zero"}};
            if(snapshot.early_reflections)result["reflection_budget_exceeded"]=report.reflection_budget_exceeded;
            if(snapshot.water_medium)result["listener_water"]=describe_water_audio(*snapshot.water_medium);
            if(capture_audio) {
                const auto bytes=audio_wave(report.samples,2);const auto utf8=fs::path(std::u8string(output.begin(),output.end()));write_flushed(utf8,bytes);
                result["capture"]={{"path",output},{"format","WAV IEEE float32"},{"sample_rate",audio_rate},{"channels",2},{"frames",frame_count},{"sha256",content_hash(bytes)},{"peak",report.peak},{"rms",report.rms},{"over_range_samples",report.over_range_samples}};
            }
            return result;
        }catch(const Error&) { throw; }catch(const std::exception& e) { throw Error(-32070,e.what()); }
    }
    void runtime_guard(const Json& params) const {
        const auto id=identifier(params.at("session_id"));
        require(runtime_ && id==runtime_id_,"Runtime session is absent or does not match.",-32030);
        if(params.contains("request_id"))for(const auto& receipt:playback_receipts_)
            require(receipt.at("params").at("session_id")!=id || receipt.at("params").at("request_id")!=params.at("request_id"),
                "Runtime request ID was already used by playback.",-32010);
        if(params.contains("request_id"))for(const auto& receipt:advance_receipts_)
            require(!receipt || receipt->params.at("session_id")!=id || receipt->params.at("request_id")!=params.at("request_id"),
                "Runtime request ID was already used by a step.",-32010);
    }
    static Json inspect_templates(const Json& document,const Json& params,bool single) {
        const Json empty=Json::object();const auto& catalog=document.contains("templates") ? document.at("templates") : empty;
        Json result={{"revision",document.at("revision")}};
        if(single) {
            const auto id=identifier(params.at("id"));require(catalog.contains(id),"Template does not exist.",-32004);
            result["id"]=id;result["template"]=catalog.at(id);return result;
        }
        const auto after=params.contains("after") ? identifier(params.at("after")) : std::string{};
        const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;require(limit>=1 && limit<=256,"Query limit must be 1..256.");
        Json rows=Json::array(),next=nullptr;
        for(const auto& [id,recipe]:catalog.items()) {
            if(id<=after)continue;
            if(rows.size()==limit) {next=rows.back().at("id");break;}
            const auto& bag=recipe.contains("entities") ? recipe.at("entities").at(recipe.at("root").get<std::string>()).at("components"):recipe.at("components");
            Json types=Json::array();for(const auto& [type,value]:bag.items()) {(void)value;types.push_back(type);}
            Json row={{"id",id},{"name",recipe.at("name")},{"components",types}};
            if(recipe.contains("entities")) {row["root"]=recipe.at("root");row["entity_count"]=recipe.at("entities").size();}
            rows.push_back(std::move(row));
        }
        result["templates"]=std::move(rows);result["next_after"]=next;return result;
    }
#include "world_save.inc"
#include "world_gameplay_save.inc"
#include "world_material.inc"
#include "world_texture.inc"
    Json runtime_summary() const {
        const auto state=runtime_->inspect();
        return {{"session_id",runtime_id_},{"world_id",runtime_definition_.world_id},{"authored_revision",runtime_definition_.authored_revision},
            {"current_authored_revision",doc_.at("revision")},{"source_stale",runtime_source_stale()},
            {"tick",state.tick},{"structure_revision",runtime_->structure_revision()},{"material_revision",runtime_->material_revision()},{"content_sha256",runtime_content_hash_},{"ui_revision",runtime_->ui_model().revision()},{"control_sequence",runtime_->control_sequence()},{"fixed_dt",Runtime::fixed_dt},{"entities",state.entities},{"bodies",state.bodies},{"characters",state.characters},
            {"scheduler","single_threaded_fixed_60_hz"},{"physics","Jolt 5.4.0; double positions; SSE2 baseline"}};
    }
    Json sound_voices(const Json& params) const {
        fields(params,{"session_id","tick","after","limit"},{"session_id","tick"});runtime_guard(params);
        const auto tick=runtime_->inspect().tick;require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        const auto after=revision(params.value("after",Json(0))),limit=revision(params.value("limit",Json(64)));require(limit>=1 && limit<=256,"Voice limit must be 1..256.");
        const auto& state=runtime_->sound_state();Json voices=Json::array();bool more=false;std::size_t emitting=0;
        for(const auto& voice:state.voices()) {
            const bool active=voice.emitting(tick*audio_tick_frames);emitting+=active ? 1 : 0;
            if(voice.id<=after)continue;
            if(voices.size()==limit) { more=true;continue; }
            const auto end=voice.end_sample();
            voices.push_back({{"voice",voice.id},{"emitter",voice.emitter},{"asset",voice.sound.asset},{"start_tick",voice.start_tick},
                {"stop_sample",voice.stop_sample ? Json(std::to_string(*voice.stop_sample)) : Json(nullptr)},{"end_sample",end==UINT64_MAX ? Json(nullptr) : Json(std::to_string(end))},
                {"gain",voice.gain},{"emitter_gain",voice.sound.gain},{"loop",voice.sound.loop},{"emitting",active},
                {"clip_frame",active ? Json((tick*audio_tick_frames-voice.start_tick*audio_tick_frames)%voice.sound.clip->samples.size()) : Json(nullptr)}});
            describe_audio_routing(voices.back(),voice.sound,voice.gain);
        }
        return {{"session_id",runtime_id_},{"tick",tick},{"next_voice",state.next_id()},{"emitting",emitting},{"retained",state.voices().size()},{"has_more",more},{"voices",voices}};
    }
    Json audio_replay(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","listener","path","sequence","expected_structure_revision","early_reflections"},{"session_id","request_id","expected_tick","listener","path","sequence"});
        identifier(params.at("session_id"));identifier(params.at("request_id"));revision(params.at("expected_tick"));auto normalized=params;normalized["method"]="runtime.audio.replay";
        for(const auto& receipt:playback_receipts_)if(receipt["params"]["session_id"]==params.at("session_id") && receipt["params"]["request_id"]==params.at("request_id")) {
            require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);auto result=receipt["result"];result["replayed"]=true;return result;
        }
        runtime_guard(params);
        const auto expected=revision(params.at("expected_tick"));structure_mutation_guard(params);require(expected==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(audio_available(),"Audio replay is not built. Configure POIMA_ENABLE_AUDIO=ON.",-32003);
        const auto reflections=params.value("early_reflections",Json(false));
        require(reflections.is_boolean(),"early_reflections must be Boolean.");
        const auto listener=identifier(params.at("listener"));const auto output=capture_output_path(params.at("path"),path_,asset_directory());
        struct Segment { std::uint32_t ticks;std::vector<RuntimeInput> inputs;std::vector<KinematicTarget> motions;std::vector<SoundCommand> sounds; };
        const auto& sequence=params.at("sequence");require(sequence.is_array() && !sequence.empty() && sequence.size()<=256,"Audio replay requires 1..256 segments.");
        std::vector<Segment> segments;std::uint64_t total=0;
        for(const auto& value:sequence) {
            fields(value,{"ticks","inputs","motions","sounds"},{"ticks"});const auto ticks=revision(value.at("ticks"));require(ticks>=1 && ticks<=600,"Audio replay segment must be 1..600 ticks.");total+=ticks;require(total<=3600 && expected+total<=max_revision,"Audio replay exceeds 3600 ticks or runtime tick range.");
            Segment segment;segment.ticks=static_cast<std::uint32_t>(ticks);const auto inputs=value.value("inputs",Json::array());require(inputs.is_array() && inputs.size()<=32,"At most 32 replay inputs.");for(const auto& input:inputs)segment.inputs.push_back(parse_input(input));
            segment.motions=parse_motions(value.value("motions",Json::array()));segment.sounds=parse_sounds(value.value("sounds",Json::array()));segments.push_back(std::move(segment));
        }
        const auto snapshot=[&] {
            auto audio=runtime_->audio_snapshot(listener);
            audio.early_reflections=reflections.get<bool>();
            return audio;
        };
        AudioSnapshot initial_audio;
        std::unique_ptr<AudioStream> stream;
        try {
            initial_audio=snapshot();
            stream=std::make_unique<AudioStream>(expected,initial_audio);
        }catch(const std::exception& e) { throw Error(-32070,e.what()); }
        std::vector<float> pcm;pcm.reserve(static_cast<std::size_t>(total*audio_tick_frames*2));
        auto receipts=playback_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());receipts.push_back({{"params",normalized},{"result",Json::object()}});
        Json result={{"session_id",runtime_id_},{"previous_tick",expected},{"listener",listener},{"replayed",false},{"success",false},{"capture",nullptr}};
        if(reflections.get<bool>())result["early_reflections"]=true;
        if (initial_audio.water_medium) {
            result["listener_water"]={{"initial",describe_water_audio(*initial_audio.water_medium)},{"final",nullptr},
                {"transition_frames",audio_detail::WaterFilter::transition_frames}};
        }
        try {
            auto medium=initial_audio.water_medium;
            auto append=[&](bool finish=false) {
                const auto audio=snapshot();
                auto block=stream->advance(runtime_->inspect().tick,audio,runtime_->sound_state().voices(),finish);
                pcm.insert(pcm.end(),block.begin(),block.end());
                medium=audio.water_medium;
            };
            bool replaced=false;
            for(auto& segment:segments) {
              for(std::uint32_t i=0;i<segment.ticks;++i) {
                const auto advanced=advance_runtime(1,segment.inputs,i==0 ? segment.motions : std::vector<KinematicTarget>{},i==0 ? segment.sounds : std::vector<SoundCommand>{});
                if(advanced.replaced) { replaced=true;result["stop_reason"]="runtime_replaced";break; }
                append();
                for(auto& input:segment.inputs)clear_runtime_input_edges(input);
            }
              if(replaced)break;
            }
            if(!replaced)append(true);
            if (medium && initial_audio.water_medium)
                result["listener_water"]["final"]=describe_water_audio(*medium);
            const auto bytes=audio_wave(pcm,2);write_flushed(fs::path(std::u8string(output.begin(),output.end())),bytes);
            result["capture"]={{"path",output},{"sha256",content_hash(bytes)},{"frames",pcm.size()/2},{"channels",2},{"sample_rate",audio_rate},{"format","WAV IEEE float32"}};result["success"]=true;result["detail"]="Committed simulation recorded with persistent direct/HRTF voices; no audio device.";
        }catch(const std::exception& e) { result["detail"]=e.what(); }
        result["current_session_id"]=runtime_id_;const auto stats=stream->stats();result["tick"]=runtime_->inspect().tick;result["stream"]={{"frames",stats.frames},{"blocks",stats.blocks},{"voices_started",stats.voices_started},{"path_updates",stats.path_updates},{"peak",stats.peak},{"over_range_samples",stats.over_range_samples},{"dsp_ms",stats.dsp_ms}};
        receipts.back()["result"]=result;playback_receipts_.swap(receipts);return result;
    }
#include "world_player_service.inc"
    Json play(const Json& params) {
        validate_player_parameters(params);
        identifier(params.at("session_id"));identifier(params.at("request_id"));revision(params.at("expected_tick"));
        auto normalized=params;normalized["method"]="runtime.play";
        for(const auto& receipt:playback_receipts_)if(receipt["params"]["session_id"]==params.at("session_id") && receipt["params"]["request_id"]==params.at("request_id")) {
            require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);
            auto result=receipt["result"];result["replayed"]=true;return result;
        }
        auto prepared=prepare_player(params);
        prepared.options.startup_splash=startup_splash_ && !prepared.options.replay && !prepared.options.background;
        startup_splash_=false;
        // Reserve the retry slot before any operation that may advance state.
        auto receipts=playback_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());
        receipts.push_back({{"params",normalized},{"result",Json::object()}});
        PlayerSessionAdapter session(*this);
        BlockingPreferenceScope preference_scope(*this,prepared.gameplay_preferences);
        capture_renderer_.reset();
        const auto report=run_player(prepared.options,session,player_progress_);
        // A final Tick can accept a write immediately before playback ends.
        // Observe completed work without adding storage waits to the player.
        session.owner_poll();
        require(report.render.available,report.render.detail,-32003);
        auto result=player_report_json(prepared.options,std::move(prepared.input_info),std::move(prepared.settings_application),report,params.at("mode"));
        const auto* pending=runtime_->gameplay_saves().pending();
        result["pending_gameplay_save"]=pending ? gameplay_save_json(runtime_->gameplay_saves().query(pending->ticket,&gameplay_save_ledger_)) : Json(nullptr);
        receipts.back()["result"]=result;playback_receipts_.swap(receipts);return result;
    }

#include "world_ui.inc"
    const components::Schema& runtime_component_schema(const std::string& type) const {
        return authored_schema(runtime_->component_schemas(),type);
    }
    Json component_runtime_info() const {
        return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"component_revision",runtime_->component_revision()},{"structure_revision",runtime_->structure_revision()}};
    }
#include "world_observe.inc"
#include "world_pose_frames.inc"
#include "world_joint_pose.inc"
#include "world_surface_measure.inc"
#include "world_attachments.inc"
#include "world_physical.inc"
    Json component_dispatch(const std::string& method,const Json& params) {
        require(params.is_object() && params.contains("session_id"),"Runtime component requests require session_id.");runtime_guard(params);
        if(method=="runtime.components") {
            fields(params,{"session_id"},{"session_id"});auto result=component_runtime_info();result["schemas"]=Json::array();
            for(const auto& schema:runtime_->component_schemas())result["schemas"].push_back(Json::parse(components::schema_json(schema)));
            return result;
        }
        if(method=="runtime.component.get") {
            fields(params,{"session_id","tick","structure_revision","id","type"},{"session_id","tick","id","type"});
            structure_read_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            const auto id=identifier(params.at("id")),type=identifier(params.at("type"));const auto& schema=runtime_component_schema(type);
            const auto value=component_checked([&]{return runtime_->component_read(type,id);});auto result=component_runtime_info();result["id"]=id;result["type"]=type;
            result["schema"]=Json::parse(components::schema_json(schema));result["values"]=value ? Json::parse(components::values_json(schema,*value)) : Json(nullptr);return result;
        }
        if(method=="runtime.component.query") {
            fields(params,{"session_id","tick","structure_revision","type","after","limit"},{"session_id","tick","type"});
            structure_read_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            const auto type=identifier(params.at("type"));const auto after=params.contains("after") ? identifier(params.at("after")) : std::string{};
            const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;require(limit>=1 && limit<=256,"Component query limit must be 1..256.");
            auto entities=component_checked([&]{return runtime_->component_query(type,after,static_cast<std::uint32_t>(limit+1));});const bool more=entities.size()>limit;if(more)entities.pop_back();
            auto result=component_runtime_info();result["type"]=type;result["entities"]=entities;result["next_after"]=more ? Json(entities.back()) : Json(nullptr);return result;
        }
        require(method=="runtime.component.edit","Unknown runtime component operation.",-32601);
        fields(params,{"session_id","request_id","expected_tick","expected_revision","id","type","values","expected_structure_revision"},{"session_id","request_id","expected_tick","expected_revision","id","type","values"});
        const auto request=identifier(params.at("request_id")),id=identifier(params.at("id")),type=identifier(params.at("type"));revision(params.at("expected_tick"));revision(params.at("expected_revision"));
        const auto& schema=runtime_component_schema(type);
        const auto value=component_checked([&]{return components::parse_values(schema,params.at("values").dump());});
        auto normalized=params;normalized["method"]=method;
        for(const auto& receipt:runtime_receipts_)if(receipt.at("params").at("request_id")==request) {
            require(receipt.at("params")==normalized,"Runtime request ID reused with different parameters.",-32010);auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        for(const auto& receipt:advance_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_ && receipt->params.at("request_id")==request)
            throw Error(-32010,"Runtime request ID already belongs to a simulation advance.");
        structure_mutation_guard(params);
        require(params.at("expected_tick")==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(params.at("expected_revision")==runtime_->component_revision(),"Component revision conflict.",-32009);
        require(runtime_->component_revision()<max_revision,"Component revision limit reached.");
        // Allocate response and receipt before native publication. Validation or
        // allocation failure leaves both component bytes and revision unchanged.
        auto result=component_runtime_info();result["component_revision"]=runtime_->component_revision()+1;result["id"]=id;result["type"]=type;result["values"]=Json::parse(components::values_json(schema,value));result["replayed"]=false;
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());receipts.push_back({{"params",normalized},{"result",result}});
        component_checked([&]{runtime_->component_edit(type,id,value);});runtime_receipts_.swap(receipts);return result;
    }
    Json gameplay_info() const {
        return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"revision",runtime_->gameplay_revision()},{"structure_revision",runtime_->structure_revision()},{"module",Json::parse(runtime_->gameplay_inspect())}};
    }
    Json gameplay_dispatch(const std::string& method,const Json& params) {
        require(params.is_object() && params.contains("session_id"),"Gameplay requests require session_id.");runtime_guard(params);
        if(method=="runtime.gameplay.metrics" || method=="runtime.gameplay.memory") {
            fields(params,{"session_id","tick"},{"session_id"});
            if(params.contains("tick"))require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            try {
                return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"revision",runtime_->gameplay_revision()},
                    {"structure_revision",runtime_->structure_revision()},{"module",Json::parse(method=="runtime.gameplay.memory" ?
                        runtime_->gameplay_memory_snapshot() : runtime_->gameplay_callback_metrics())}};
            }catch(const std::exception& e) {throw Error(-32060,e.what());}
        }
        if(method=="runtime.gameplay.inspect") {
            fields(params,{"session_id","tick","include_schema","fields"},{"session_id"});
            if(params.contains("tick"))require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            require(!params.contains("include_schema") || params.at("include_schema").is_boolean(),"include_schema must be Boolean.");
            auto result=gameplay_info();auto& module=result["module"];
            if(params.contains("fields")) {
                const auto& names=params.at("fields");require(!module.is_null() && names.is_array() && names.size()<=128,"fields needs a loaded module and at most 128 names.");Json values=Json::object();
                for(const auto& name:names) { require(name.is_string(),"Field names must be strings.");const auto key=name.get<std::string>();require(module["values"].contains(key) && !values.contains(key),"Unknown or duplicate gameplay field.");values[key]=module["values"][key]; }module["values"]=std::move(values);
            }
            if(!module.is_null() && !params.value("include_schema",false))module={{"identity",module["schema"]["identity"]},{"assembly_sha256",module["assembly_sha256"]},{"values",module["values"]}};
            return result;
        }
        if(method=="runtime.gameplay.collect") {
            fields(params,{"session_id"},{"session_id"});require(Gameplay::available() || Gameplay::native_available(),"Gameplay support is not built.",-32003);
            try { return Json::parse(Gameplay::collect()); }catch(const std::exception& e) { throw Error(-32060,e.what()); }
        }
        const bool managed_load=method=="runtime.gameplay.load",native_load=method=="runtime.gameplay.load_native",load=managed_load || native_load;
        require(load || method=="runtime.gameplay.edit","Unknown gameplay method.",-32601);
        if(managed_load)fields(params,{"session_id","request_id","expected_tick","expected_revision","hostfxr","bridge","assembly","type","values","expected_structure_revision","profile_callbacks"},{"session_id","request_id","expected_tick","expected_revision","hostfxr","bridge","assembly","type"});
        else if(native_load)fields(params,{"session_id","request_id","expected_tick","expected_revision","descriptor","expected_descriptor_sha256","values","expected_structure_revision"},{"session_id","request_id","expected_tick","expected_revision","descriptor"});
        else fields(params,{"session_id","request_id","expected_tick","expected_revision","values","expected_structure_revision"},{"session_id","request_id","expected_tick","expected_revision","values"});
        identifier(params.at("request_id"));auto normalized=params;normalized["method"]=method;
        if(managed_load)require(!params.contains("profile_callbacks") || params.at("profile_callbacks").is_boolean(),"profile_callbacks must be Boolean.");
        if(!normalized.contains("values"))normalized["values"]=Json::object();
        require(normalized.at("values").is_object() && normalized.at("values").size()<=128,"Gameplay values must be a bounded field object.");
        // Validate wire types before receipt equality: JSON numeric comparison
        // can equate 0.0 with 0, but historical retries still need integer guards.
        const auto expected_tick=revision(params.at("expected_tick")),expected_revision=revision(params.at("expected_revision"));
        for(const auto& receipt:runtime_receipts_)if(receipt["params"]["request_id"]==params.at("request_id")) {
            require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);auto result=receipt["result"];result["replayed"]=true;return result;
        }
        structure_mutation_guard(params);
        require(expected_tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(expected_revision==runtime_->gameplay_revision(),"Gameplay revision conflict.",-32009);
        require(managed_load ? Gameplay::available() : native_load ? Gameplay::native_available() : (Gameplay::available() || Gameplay::native_available()),
            native_load ? "Native gameplay is not built. Configure POIMA_ENABLE_NATIVE_GAMEPLAY=ON." : "Requested gameplay support is not built.",-32003);
        GameplayConfig config;
        auto path=[&](const char* key) { const auto text=params.at(key).get<std::string>();auto value=fs::path(std::u8string(text.begin(),text.end()));if(value.is_relative())value=path_.parent_path()/value;const auto bytes=fs::absolute(value).lexically_normal().u8string();return std::string(bytes.begin(),bytes.end()); };
        if(managed_load) {
            for(const auto* key:{"hostfxr","bridge","assembly","type"})require(params.at(key).is_string() && !params.at(key).get_ref<const std::string&>().empty() && params.at(key).get_ref<const std::string&>().size()<=4096,"Gameplay paths/type must contain 1..4096 UTF-8 bytes.");
            config.hostfxr=path("hostfxr");config.bridge=path("bridge");config.assembly=path("assembly");config.type=params.at("type").get<std::string>();
            config.profile_callbacks=params.value("profile_callbacks",false);
        }
        if(native_load)require(params.at("descriptor").is_string() && !params.at("descriptor").get_ref<const std::string&>().empty() && params.at("descriptor").get_ref<const std::string&>().size()<=4096,"Gameplay descriptor must contain 1..4096 UTF-8 bytes.");
        if(native_load && params.contains("expected_descriptor_sha256")) {
            const auto& hash=params.at("expected_descriptor_sha256");
            require(hash.is_string() && hash.get_ref<const std::string&>().size()==64 && hash.get_ref<const std::string&>().find_first_not_of("0123456789abcdef")==std::string::npos,"Expected descriptor hash must be 64 lowercase hexadecimal characters.");
        }
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());receipts.push_back({{"params",normalized},{"result",nullptr}});
        try {
            if(native_load) {
                const auto artifact=load_native_gameplay_artifact(path("descriptor"));
                if(params.contains("expected_descriptor_sha256") && params.at("expected_descriptor_sha256").get<std::string>()!=artifact.descriptor_sha256)throw std::runtime_error("Native gameplay descriptor differs from its previously verified hash.");
                if(artifact.target_os!=build_metadata().target_os || artifact.target_arch!=build_metadata().target_arch)throw std::runtime_error("Native gameplay target differs from this runtime.");
                config.native_aot=true;config.native_library=artifact.library;config.native_sha256=artifact.library_sha256;
                config.native_schema=artifact.schema;config.type=artifact.type;
                config.native_requirements=artifact.requirements;
                validate_gameplay_values(config.native_schema,normalized.at("values").dump());
            }
            if(load) { runtime_->gameplay_load(config,normalized.at("values").dump());runtime_gameplay_config_=std::move(config); }else runtime_->gameplay_edit(normalized.at("values").dump());
        }
        catch(const std::exception& e) { throw Error(-32060,e.what()); }
        auto result=gameplay_info();result["replayed"]=false;receipts.back()["result"]=result;runtime_receipts_.swap(receipts);return result;
    }
    void structure_mutation_guard(const Json& params) const {
        require(runtime_->structure_revision()==0 || params.contains("expected_structure_revision"),"Changed runtime structure requires expected_structure_revision.");
        if(params.contains("expected_structure_revision"))require(revision(params.at("expected_structure_revision"))==runtime_->structure_revision(),"Runtime structure revision conflict.",-32009);
    }
    void structure_read_guard(const Json& params) const {
        if(params.contains("structure_revision"))require(revision(params.at("structure_revision"))==runtime_->structure_revision(),"Runtime structure revision conflict.",-32009);
    }
    static Json structure_receipt_json(const StructureReceipt& receipt,bool replayed) {
        return {{"session_id",receipt.params.at("session_id")},{"tick",receipt.params.at("expected_tick")},
            {"structure_revision",receipt.outcome.revision},{"component_revision",receipt.component_revision},
            {"spawned",receipt.outcome.spawned},{"despawned",receipt.params.at("despawns")},{"replayed",replayed}};
    }
    Json structure_dispatch(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","expected_structure_revision","spawns","despawns"},
            {"session_id","request_id","expected_tick","expected_structure_revision"});
        runtime_guard(params);const auto request=identifier(params.at("request_id"));
        const auto tick=revision(params.at("expected_tick")),expected=revision(params.at("expected_structure_revision"));
        auto normalized=params;normalized["method"]="runtime.structure.transact";
        for(const auto* key:{"spawns","despawns"})if(!normalized.contains(key))normalized[key]=Json::array();
        const auto& births=normalized.at("spawns");const auto& deaths=normalized.at("despawns");
        require(births.is_array() && deaths.is_array() && births.size()+deaths.size()>=1 && births.size()+deaths.size()<=4096,
            "Structural transaction needs 1..4096 total spawn/removal commands.");
        std::vector<RuntimeSpawnRequest> spawns;spawns.reserve(births.size());
        std::vector<std::string> despawns;despawns.reserve(deaths.size());
        for(const auto& value:births) {
            fields(value,{"template_id","transform"},{"template_id"});RuntimeSpawnRequest spawn;spawn.template_id=identifier(value.at("template_id"));
            if(value.contains("transform")) {
                const auto& t=value.at("transform");validate_transform(t);RuntimeTransform transform;
                transform.position=t.at("position").get<std::array<double,3>>();transform.rotation=t.at("rotation").get<std::array<double,4>>();transform.scale=t.at("scale").get<std::array<double,3>>();spawn.transform=transform;
            }
            spawns.push_back(std::move(spawn));
        }
        for(const auto& value:deaths)despawns.push_back(identifier(value));
        for(const auto& receipt:structure_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_ && receipt->params.at("request_id")==request) {
            require(receipt->params==normalized,"Runtime request ID reused with different parameters.",-32010);
            return structure_receipt_json(*receipt,true);
        }
        for(const auto& receipt:runtime_receipts_)require(receipt.at("params").at("request_id")!=request,"Runtime request ID already belongs to another operation.",-32010);
        for(const auto& receipt:advance_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_)
            require(receipt->params.at("request_id")!=request,"Runtime request ID already belongs to a simulation advance.",-32010);
        require(tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(expected==runtime_->structure_revision(),"Runtime structure revision conflict.",-32009);
        auto receipt=std::make_unique<StructureReceipt>();receipt->params=std::move(normalized);
        static_assert(std::is_nothrow_move_assignable_v<RuntimeStructureResult>);
        try { receipt->outcome=runtime_->change_structure(expected,spawns,despawns); }
        catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        receipt->component_revision=runtime_->component_revision();
        const auto index=structure_receipt_next_;structure_receipts_[index].swap(receipt);
        structure_receipt_next_=(index+1)%structure_receipts_.size();
        return structure_receipt_json(*structure_receipts_[index],false);
    }
#include "world_wetness_service.inc"
    Json runtime_dispatch(const std::string& method,const Json& params) {
        if(method=="runtime.observe")return observe_runtime(params);
        if(method=="runtime.navigation.path")return runtime_navigation_dispatch(params);
        if(method=="runtime.pose.compare")return compare_runtime_pose_frames(params);
        if(method=="runtime.pose.joints")return check_runtime_joint_pose(params);
        if(method=="runtime.pose.clearance")return check_runtime_pose_clearance(params);
        if(method=="runtime.surface.measure")return measure_runtime_surfaces(params);
        if(method=="runtime.attachment.inspect")return inspect_runtime_attachments(params);
        if(method=="runtime.attachment.outcomes")return inspect_attachment_outcomes(params);
        if(method=="runtime.attachment.apply")return apply_runtime_attachments(params);
        if(method=="runtime.attachment.release")return release_runtime_attachment(params);
        // Validate guard representation before receipt equality (JSON considers
        // integer and floating numeric values equal).
        if(params.is_object() && params.contains("expected_structure_revision"))revision(params.at("expected_structure_revision"));
        if(method=="runtime.ui.activate")return control_dispatch(params);
        if(params.is_object() && params.contains("session_id") && params.contains("request_id"))
            for(const auto& receipt:control_receipts_)if(receipt && receipt->params.at("session_id")==params.at("session_id") && receipt->params.at("request_id")==params.at("request_id"))
                throw Error(-32010,"Runtime request ID already belongs to a UI activation.");
        if(method=="runtime.structure.transact")return structure_dispatch(params);
        if(params.is_object() && params.contains("session_id") && params.contains("request_id"))
            for(const auto& receipt:structure_receipts_)if(receipt && receipt->params.at("session_id")==params.at("session_id") && receipt->params.at("request_id")==params.at("request_id"))
                throw Error(-32010,"Runtime request ID already belongs to a structural transaction.");
        if(method=="runtime.instance") {
            fields(params,{"session_id","id","tick","expected_structure_revision"},{"session_id","id","tick"});
            runtime_guard(params);require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            if(params.contains("expected_structure_revision"))require(revision(params.at("expected_structure_revision"))==runtime_->structure_revision(),"Runtime structure revision conflict.",-32009);
            try {
                const auto instance=runtime_->instance(identifier(params.at("id")));const auto& t=instance.initial;
                return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"structure_revision",runtime_->structure_revision()},
                    {"root",instance.root},{"template_id",instance.template_id},
                    {"initial_transform",{{"position",t.position},{"rotation",t.rotation},{"scale",t.scale}}},{"nodes",instance.nodes}};
            }catch(const Error&) {throw;}catch(const std::exception& error) {throw Error(-32004,error.what());}
        }
        if(method=="runtime.template.get" || method=="runtime.template.query") {
            if(method=="runtime.template.get")fields(params,{"session_id","tick","revision","id"},{"session_id","tick","id"});
            else fields(params,{"session_id","tick","revision","after","limit"},{"session_id","tick"});
            runtime_guard(params);require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            if(params.contains("revision"))require(revision(params.at("revision"))==runtime_definition_.authored_revision,"Frozen template revision conflict.",-32009);
            auto result=inspect_templates(runtime_document_,params,method=="runtime.template.get");result["session_id"]=runtime_id_;result["tick"]=runtime_->inspect().tick;return result;
        }
        if(method.starts_with("runtime.ui."))return ui_dispatch(method,params);
        if(method.starts_with("runtime.material."))return material_dispatch(method,params);
        if(method.starts_with("runtime.texture."))return texture_dispatch(method,params);
        if(method=="runtime.water.inspect")return water_inspect(params);
        if(method=="runtime.foliage.inspect")return foliage_inspect(params);
        if(method=="runtime.snow.inspect")return inspect_snow(params);
        if(method=="runtime.wetness.inspect")return inspect_wetness(params);
        if(method=="runtime.particles.inspect")return inspect_particles(params);
        if(method=="runtime.surface_marks.inspect")return inspect_surface_marks(params);
        if(method=="runtime.components" || method.starts_with("runtime.component."))return component_dispatch(method,params);
        if(method=="runtime.status") {
            fields(params,{});const auto s=runtime_status();
            return {{"available",s.available},{"active",s.active},{"session_id",s.active ? Json(s.session_id) : Json(nullptr)},
                    {"tick",s.active ? Json(s.tick) : Json(nullptr)},{"structure_revision",s.active ? Json(s.structure_revision) : Json(nullptr)},{"material_revision",s.active ? Json(s.material_revision) : Json(nullptr)},{"ui_revision",s.active ? Json(s.ui_revision) : Json(nullptr)},{"control_sequence",s.active ? Json(s.control_sequence) : Json(nullptr)},{"authored_revision",s.active ? Json(s.authored_revision) : Json(nullptr)}};
        }
        if(method=="runtime.save.status") {
            fields(params,{"session_id"},{"session_id"});runtime_guard(params);const auto& queue=runtime_->gameplay_saves();
            Json result={{"epoch",epoch_text(queue.epoch())},{"enabled",queue.enabled()},{"configuration_generation",queue.configuration_generation()},
                {"pending",queue.pending() ? gameplay_save_json(queue.query(queue.pending()->ticket,&gameplay_save_ledger_)) : Json(nullptr)},{"last_restore",nullptr}};
            if(const auto restored=gameplay_save_ledger_.last_restore(queue.epoch()))result["last_restore"]={
                {"initiating_epoch",epoch_text(restored->initiating_ticket.epoch)},{"initiating_sequence",restored->initiating_ticket.sequence},
                {"source_tick",restored->committed_source_tick},{"restored_tick",restored->restored_tick},{"generation",restored->generation},{"recovered",restored->recovered}};
            return result;
        }
        if(method=="runtime.save.result") {
            fields(params,{"epoch","sequence"},{"epoch","sequence"});GameplaySaveTicket ticket{save_epoch(identifier(params.at("epoch"))),revision(params.at("sequence"))};
            require(ticket.valid(),"Save ticket must have a nonzero epoch and positive sequence.");
            return gameplay_save_json(runtime_ ? runtime_->gameplay_saves().query(ticket,&gameplay_save_ledger_) : GameplaySaveQueue{}.query(ticket,&gameplay_save_ledger_));
        }
        if(method=="runtime.audio.voices")return sound_voices(params);
        if(method=="runtime.audio.replay")return audio_replay(params);
        if(method=="runtime.audio.inspect" || method=="runtime.audio.capture")return audio_dispatch(method,params,true);
        if(method.starts_with("runtime.gameplay."))return gameplay_dispatch(method,params);
        if(method=="runtime.capture") return capture(params,true);
        if(method=="runtime.capture.release") {
            fields(params,{"session_id"},{"session_id"});
            runtime_guard(params);
            const bool released = bool(capture_renderer_);
            capture_renderer_.reset();
            return {{"session_id",runtime_id_},{"released",released}};
        }
        if(method=="runtime.play") return play(params);
        if(method=="runtime.start") {
            fields(params,{"session_id","revision"},{"session_id","revision"});
            const auto id=identifier(params.at("session_id")); revision(params.at("revision"));
            require(Runtime::available(),"Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON.",-32003);
            if(runtime_) {
                require(runtime_id_==id && params==runtime_start_params_,"Stop the current runtime before starting another session.",-32031);
                auto result=runtime_start_result_; result["replayed"]=true; return result;
            }
            require(!used_runtime_ids_.contains(id),"Runtime session ID was already used; supply a fresh ID.",-32010);
            require(used_runtime_ids_.size()<10000,"Runtime session count limit reached; reopen the authoring process.");
            current_revision(params); auto frozen=freeze_content(doc_);auto& definition=frozen.definition;
            std::unique_ptr<Runtime> candidate;
            try { candidate=std::make_unique<Runtime>(definition); }
            catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
            configure_runtime_saves(*candidate);
            Json result={{"session_id",id},{"authored_revision",definition.authored_revision},{"tick",0},{"started",true},{"replayed",false}};
            auto start_params=params,start_result=result;auto session_id=id;auto used=used_runtime_ids_;used.insert(id);Json receipts=Json::array();
            runtime_start_params_.swap(start_params);runtime_start_result_.swap(start_result);runtime_id_.swap(session_id);
            used_runtime_ids_.swap(used);runtime_receipts_.swap(receipts);runtime_definition_=std::move(definition);runtime_document_.swap(frozen.document);
            invalidate_runtime_source_stale();
            runtime_content_hash_.swap(frozen.hash);runtime_gameplay_config_.reset();runtime_.swap(candidate);
            return result;
        }
        if(method=="runtime.stop") {
            fields(params,{"session_id"},{"session_id"}); const auto id=identifier(params.at("session_id"));
            if(!runtime_ && stopped_runtime_id_==id) return {{"session_id",id},{"stopped",true},{"replayed",true}};
            runtime_guard(params);require(!runtime_->gameplay_saves().pending(),"Resolve pending gameplay save before stopping the runtime.",-32070); stopped_runtime_id_=id; capture_renderer_.reset();runtime_.reset();runtime_gameplay_config_.reset(); runtime_receipts_.clear();
            invalidate_runtime_source_stale();
            return {{"session_id",id},{"stopped",true},{"replayed",false}};
        }
        if(method=="runtime.inspect") { fields(params,{"session_id"},{"session_id"}); runtime_guard(params); return runtime_summary(); }
        if(method=="runtime.lighting") {
            fields(params,{"session_id","tick"},{"session_id"});runtime_guard(params);
            if(params.contains("tick"))require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            try { auto result=lighting_json(runtime_->lighting());result["session_id"]=runtime_id_;result["tick"]=runtime_->inspect().tick;return result; }
            catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        }
        if(method=="runtime.capsule_cast") {
            fields(params,{"session_id","tick","position","rotation","radius","half_height","direction","distance","ignore"},
                {"session_id","tick","position","rotation","radius","half_height","direction","distance"});
            runtime_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            return query_runtime_capsule_cast(*runtime_,runtime_id_,params);
        }
        if(method=="runtime.raycast" || method=="runtime.attachment.aim") {
            const bool picking=method=="runtime.attachment.aim";
            if(picking)fields(params,{"session_id","tick","structure_revision","origin","direction","distance","ignore"},
                {"session_id","tick","structure_revision","origin","direction","distance"});
            else fields(params,{"session_id","tick","origin","direction","distance","ignore"},{"session_id","tick","origin","direction","distance"});
            runtime_guard(params);
            if(picking)structure_read_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            return query_runtime_ray(*runtime_,runtime_id_,params,picking);
        }
        if(method=="runtime.hit_test") {
            fields(params,{"session_id","tick","origin","direction","distance","ignore"},{"session_id","tick","origin","direction","distance"});
            runtime_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            return query_runtime_hit_regions(*runtime_,runtime_id_,params);
        }
        if(method=="runtime.character_stance" || method=="runtime.camera_pose" || method=="runtime.camera_lens" || method=="runtime.camera_focus") {
            fields(params,{"session_id","tick","entity"},{"session_id","tick","entity"});
            runtime_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            if(method=="runtime.character_stance")return query_runtime_character_stance(*runtime_,runtime_id_,params);
            if(method=="runtime.camera_pose")return query_runtime_camera_pose(*runtime_,runtime_id_,params);
            if(method=="runtime.camera_focus")return query_runtime_camera_focus(*runtime_,runtime_id_,params);
            return query_runtime_camera_lens(*runtime_,runtime_id_,params);
        }
        if(method=="runtime.physical_rig")return physical_runtime_query(params);
        if(method=="runtime.physical_animation")return physical_animation_runtime_query(params);
        if(method=="runtime.controller_handoff")return controller_handoff_runtime_query(params);
        if(method=="runtime.physical_recovery")return physical_recovery_runtime_query(params);
        if(method=="runtime.procedural_pose")return procedural_pose_runtime_query(params);
        if(method=="runtime.root_motion") {
            fields(params,{"session_id","tick","entity"},{"session_id","tick","entity"});runtime_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            const auto entity=identifier(params.at("entity"));
            try{(void)runtime_->entity(entity);}catch(const std::runtime_error& error){throw Error(-32004,error.what());}
            try {
                const auto state=runtime_->root_motion(entity);Json value=nullptr;
                if(state)value={{"entity",state->entity},{"actor",state->actor.empty()?Json(nullptr):Json(state->actor)},{"node",state->node},
                    {"mode",state->mode==RootMotionMode::Character?"character":"extract"},{"enabled",state->enabled},{"yaw",state->yaw},
                    {"from_tick",state->from_tick},{"to_tick",state->to_tick},{"local_delta",state->local_delta},{"yaw_delta",state->yaw_delta},
                    {"world_requested",state->world_requested},{"world_applied",state->world_applied},{"applied_yaw",state->applied_yaw},{"translation_error",state->translation_error}};
                return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"source","runtime"},{"state",value}};
            }catch(const std::runtime_error& error){throw Error(-32602,error.what());}
        }
        if(method=="runtime.grounding") {
            fields(params,{"session_id","tick","entity","slot"},{"session_id","tick","entity","slot"});runtime_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            const auto entity=identifier(params.at("entity"));const auto slot=revision(params.at("slot"));
            require(slot>=1 && slot<=8,"Grounding slot must be 1..8.");
            try{(void)runtime_->entity(entity);}catch(const std::runtime_error& error){throw Error(-32004,error.what());}
            try {
                const auto state=runtime_->grounding(entity,static_cast<std::uint32_t>(slot));Json value=nullptr;
                if(state)value={{"entity",state->entity},{"slot",state->slot},{"enabled",state->enabled},{"weight",state->weight}};
                return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"source","runtime"},{"state",value}};
            }catch(const std::runtime_error& error){throw Error(-32602,error.what());}
        }
        if(method=="runtime.ik.clearance") {
            fields(params,{"session_id","tick","structure_revision","goal","radii","ignore"},
                {"session_id","tick","structure_revision","goal","radii"});
            runtime_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            require(revision(params.at("structure_revision"))==runtime_->structure_revision(),
                "Runtime structure revision conflict.",-32009);
            const auto& radii=params.at("radii");
            require(radii.is_array() && radii.size()==2 && radii[0].is_number() && radii[1].is_number(),
                "IK clearance requires two numeric radii.");
            RuntimeIkClearanceRequest request{parse_ik(Json::array({params.at("goal")})).front(),
                radii.get<std::array<double,2>>(),{}};
            if(params.contains("ignore")) {
                const auto& ids=params.at("ignore");
                require(ids.is_array() && ids.size()<=128,"Ignore must contain at most 128 entity IDs.");
                for(const auto& id:ids)request.ignore.push_back(identifier(id));
            }
            try{(void)runtime_->entity(request.goal.entity);}catch(const std::runtime_error& error){throw Error(-32004,error.what());}
            try {
                const auto result=runtime_->ik_clearance(request);
                Json blocker=nullptr;
                if(result.blocker) {
                    const auto& b=*result.blocker;
                    blocker={{"segment",b.segment},{"interval",b.interval},{"hit",capsule_cast_hit_json(b.hit)}};
                }
                return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},
                    {"structure_revision",runtime_->structure_revision()},{"current",result.current},
                    {"candidate",result.candidate},{"blocker",std::move(blocker)}};
            } catch(const std::invalid_argument& error){throw Error(-32602,error.what());}
            catch(const std::runtime_error& error){throw Error(-32602,error.what());}
        }
        if(method=="runtime.ik") {
            fields(params,{"session_id","tick","entity","slot","include_end_orientation","include_metric","include_grounding","include_planting","include_target_node"},{"session_id","tick","entity","slot"});runtime_guard(params);
            if(params.contains("include_end_orientation"))require(params.at("include_end_orientation").is_boolean(),"include_end_orientation must be boolean.");
            if(params.contains("include_metric"))require(params.at("include_metric").is_boolean(),"include_metric must be boolean.");
            if(params.contains("include_grounding"))require(params.at("include_grounding").is_boolean(),"include_grounding must be boolean.");
            if(params.contains("include_planting"))require(params.at("include_planting").is_boolean(),"include_planting must be boolean.");
            if(params.contains("include_target_node"))require(params.at("include_target_node").is_boolean(),"include_target_node must be boolean.");
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            const auto entity=identifier(params.at("entity"));const auto slot=revision(params.at("slot"));require(slot>=1 && slot<=8,"IK slot must be1..8.");
            try { (void)runtime_->entity(entity); }catch(const std::runtime_error& error){throw Error(-32004,error.what());}
            try {
                const auto state=runtime_->ik(entity,static_cast<std::uint32_t>(slot));Json value=nullptr;
                if(state)value={{"entity",state->entity},{"slot",state->slot},{"root",state->root},{"middle",state->middle},{"end",state->end},
                    {"enabled",state->enabled},{"space",state->space==IkSpace::Rig ? "rig" : "world"},{"target",state->target},{"pole",state->pole},{"weight",state->weight},
                    {"end_position",state->end_position},{"target_error",state->target_error},{"clamped",state->clamped},{"pole_fallback",state->pole_fallback}};
                if(state && params.value("include_target_node",false))
                    value["target_node"]=state->target_node?Json(*state->target_node):Json(nullptr);
                if(state && params.value("include_end_orientation",false)) {
                    value["end_orientation"]=nullptr;
                    if(state->end_orientation) {
                        const auto& orientation=*state->end_orientation;
                        value["end_orientation"]={{"rotation",orientation.goal.rotation},{"weight",orientation.goal.weight},
                            {"actual_rotation",orientation.actual_rotation},{"angular_error",orientation.angular_error}};
                    }
                }
                if(state && params.value("include_metric",false)) {
                    value["metric"]=nullptr;
                    if(state->metric) {
                        const auto& metric=*state->metric;
                        value["metric"]={{"profile","near-uniform-v1"},{"evaluated",metric.evaluated},
                            {"relative_limit",ik_near_uniform_metric_limit},
                            {"local_scale_error",metric.evaluated ? Json(metric.local_scale_error) : Json(nullptr)},
                            {"frame_error",metric.evaluated ? Json(metric.frame_error) : Json(nullptr)},
                            {"gram_error",metric.evaluated ? Json(metric.gram_error) : Json(nullptr)},
                            {"unweighted_position_error",metric.unweighted_position_error ? Json(*metric.unweighted_position_error) : Json(nullptr)},
                            {"unweighted_position_limit",metric.unweighted_position_limit ? Json(*metric.unweighted_position_limit) : Json(nullptr)}};
                    }
                }
                if(state && params.value("include_grounding",false)) {
                    value["grounding"]=nullptr;
                    if(state->grounding) {
                        const auto& g=*state->grounding;
                        Json hit=nullptr;
                        if(g.hit)hit={{"entity",g.hit->entity},{"position",g.hit->position},{"normal",g.hit->normal},
                            {"triangle",g.hit->triangle ? Json(*g.hit->triangle) : Json(nullptr)}};
                        value["grounding"]={{"decision_tick",g.decision_tick},{"casts",g.casts},{"status",g.status},{"cast",g.cast},{"weight",g.weight},
                            {"input_sole",g.input_sole},{"ray_origin",g.ray_origin},{"hit",hit},{"actual_sole",g.actual_sole},{"actual_normal",g.actual_normal},
                            {"position_error",g.position_error ? Json(*g.position_error) : Json(nullptr)},
                            {"normal_error",g.normal_error ? Json(*g.normal_error) : Json(nullptr)}};
                    }
                }
                if(state && params.value("include_planting",false)) {
                    value["planting"]=nullptr;
                    if(state->grounding && state->grounding->planting) {
                        const auto& plant=*state->grounding->planting;
                        Json anchor=nullptr;
                        if(plant.anchor) {
                            const auto& a=*plant.anchor;
                            anchor={{"acquired_tick",a.acquired_tick},{"support",a.support},
                                {"triangle",a.triangle ? Json(*a.triangle) : Json(nullptr)},
                                {"local_point",a.local_point},{"local_normal",a.local_normal},{"local_forward",a.local_forward}};
                        }
                        value["planting"]={{"armed",plant.armed},{"status",plant.status},{"anchor",anchor},
                            {"target_sole",plant.target_sole ? Json(*plant.target_sole) : Json(nullptr)},
                            {"target_normal",plant.target_normal ? Json(*plant.target_normal) : Json(nullptr)},
                            {"target_forward",plant.target_forward ? Json(*plant.target_forward) : Json(nullptr)},
                            {"position_error",plant.position_error ? Json(*plant.position_error) : Json(nullptr)},
                            {"normal_error",plant.normal_error ? Json(*plant.normal_error) : Json(nullptr)}};
                    }
                }
                return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"source","runtime"},{"state",value}};
            }catch(const std::runtime_error& error){throw Error(-32602,error.what());}
        }
        if(method=="runtime.entity") {
            fields(params,{"session_id","id","tick","structure_revision"},{"session_id","id"}); runtime_guard(params);structure_read_guard(params);
            if(params.contains("tick")) require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            RuntimeEntityState e;
            try { e=runtime_->entity(identifier(params.at("id"))); }
            catch(const std::runtime_error& error) { throw Error(-32004,error.what()); }
            auto result=runtime_entity_value(e);result["session_id"]=runtime_id_;result["tick"]=runtime_->inspect().tick;
            result["structure_revision"]=runtime_->structure_revision();result["id"]=e.id;return result;
        }
        if(method=="runtime.step" || method=="runtime.snow.step" || method=="runtime.wetness.step") {
            fields(params,{"session_id","request_id","expected_tick","ticks","inputs","motions","sounds","animations","animation_phase_matches","animation_rates","ik","root_motion","camera_lenses","camera_poses","character_stances","grounding","physical_rigs","surface_marks","snow_stamp","wetness_dose","expected_structure_revision"},{"session_id","request_id","expected_tick","ticks"});
            const bool stamp_snow=method=="runtime.snow.step";
            const bool dose_wetness=method=="runtime.wetness.step";
            require(params.contains("snow_stamp")==stamp_snow,"Use runtime.snow.step with a snow_stamp to compact snow.");
            require(params.contains("wetness_dose")==dose_wetness,
                "Use runtime.wetness.step with a wetness_dose to advance surface wetness.");
            identifier(params.at("session_id"));identifier(params.at("request_id"));
            auto normalized=params; normalized["method"]=method; if(!normalized.contains("inputs")) normalized["inputs"]=Json::array();if(!normalized.contains("motions"))normalized["motions"]=Json::array();if(!normalized.contains("sounds"))normalized["sounds"]=Json::array();
            if(!normalized.contains("animations"))normalized["animations"]=Json::array();
            if(!normalized.contains("animation_phase_matches"))normalized["animation_phase_matches"]=Json::array();
            if(!normalized.contains("animation_rates"))normalized["animation_rates"]=Json::array();
            const auto animation_rates=parse_animation_rates(normalized.at("animation_rates"));
            if(!normalized.contains("ik"))normalized["ik"]=Json::array();
            const auto ik=parse_ik(normalized.at("ik"));
            if(!normalized.contains("root_motion"))normalized["root_motion"]=Json::array();
            const auto root_motion=parse_root_motion(normalized.at("root_motion"));
            if(!normalized.contains("camera_lenses"))normalized["camera_lenses"]=Json::array();
            const auto camera_lenses=parse_camera_lenses(normalized.at("camera_lenses"));
            if(!normalized.contains("camera_poses"))normalized["camera_poses"]=Json::array();
            const auto camera_poses=parse_camera_poses(normalized.at("camera_poses"));
            if(!normalized.contains("character_stances"))normalized["character_stances"]=Json::array();
            const auto character_stances=parse_character_stances(normalized.at("character_stances"));
            if(!normalized.contains("grounding"))normalized["grounding"]=Json::array();
            const auto grounding=parse_grounding(normalized.at("grounding"));
            if(!normalized.contains("physical_rigs"))normalized["physical_rigs"]=Json::array();
            const auto physical_rigs=parse_physical_rigs(normalized.at("physical_rigs"));
            if(!normalized.contains("surface_marks"))normalized["surface_marks"]=Json::array();
            const auto surface_marks=parse_surface_marks(normalized.at("surface_marks"));
            const auto snow_stamp=stamp_snow
                ? std::optional{parse_snow_stamp(normalized.at("snow_stamp"))} : std::nullopt;
            const auto wetness_dose=dose_wetness
                ? std::optional{parse_wetness_dose(normalized.at("wetness_dose"))} : std::nullopt;
            for(auto& command:normalized["physical_rigs"])if(!command.contains("activate"))command["activate"]=false;
            for(auto& command:normalized["root_motion"])if(!command.contains("enabled"))command["enabled"]=true;
            for(auto& command:normalized["ik"]) {
                if(!command.contains("enabled"))command["enabled"]=true;
                if(!command.contains("space"))command["space"]="world";
                if(!command.contains("weight"))command["weight"]=1;
            }
            normalize_animation_receipts(normalized["animations"]);
            normalize_animation_receipts(normalized["animation_phase_matches"]);
            const auto expected=revision(params.at("expected_tick"));
            const auto ticks=revision(params.at("ticks")); require(ticks>=1 && ticks<=600 && expected+ticks<=max_revision,"Runtime step must contain 1..600 ticks within the tick range.");
            const auto& raw=normalized.at("inputs"); require(raw.is_array() && raw.size()<=32,"Runtime inputs must be an array of at most 32 characters.");
            std::vector<RuntimeInput> inputs;
            for(const auto& i:raw) {
                auto input=parse_input(i);
                inputs.push_back(std::move(input));
            }
            const auto motions=parse_motions(normalized.at("motions"));const auto sounds=parse_sounds(normalized.at("sounds"));
            auto animations=parse_animations(normalized.at("animations"));
            auto phase_matches=parse_animations(normalized.at("animation_phase_matches"),true);
            require(animations.size()+animation_rates.size()+phase_matches.size()<=64,"Caller exceeds64 combined animation writes in one Tick.");
            std::set<std::pair<std::string,std::optional<std::uint32_t>>> playback_targets;
            for(const auto& command:animations)playback_targets.emplace(command.entity,command.layer);
            for(auto& command:phase_matches) {
                require(command.phase_sync.has_value(),"A phase-matched animation command needs phase_sync landmarks.");
                require(playback_targets.emplace(command.entity,command.layer).second,"Duplicate animation command target.");
                animations.push_back(std::move(command));
            }
            for(const auto& rate:animation_rates)
                require(std::none_of(animations.begin(),animations.end(),[&](const auto& command) {
                    return command.entity==rate.entity && command.layer==rate.layer;
                }),"Replacement and rate command target the same animation clock in one Tick.");
            for(const auto& receipt:advance_receipts_)if(receipt && receipt->params.at("session_id")==params.at("session_id") && receipt->params.at("request_id")==params.at("request_id")) {
                require(receipt->params==normalized,"Runtime request ID reused with different parameters.",-32010);return advance_json(receipt->outcome,true);
            }
            runtime_guard(params);
            for(const auto& receipt:runtime_receipts_) if(receipt["params"]["request_id"]==params.at("request_id")) {
                require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);
                auto result=receipt["result"]; result["replayed"]=true; return result;
            }
            structure_mutation_guard(params);require(expected==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            auto receipt=std::make_unique<AdvanceReceipt>();receipt->params=normalized;
            receipt->outcome=advance_runtime(static_cast<std::uint32_t>(ticks),inputs,motions,sounds,animations,ik,
                root_motion,camera_lenses,character_stances,grounding,animation_rates,camera_poses,physical_rigs,
                surface_marks,snow_stamp,wetness_dose);
            const auto index=advance_receipt_next_;advance_receipts_[index].swap(receipt);advance_receipt_next_=(index+1)%advance_receipts_.size();
            return advance_json(advance_receipts_[index]->outcome);
        }
        throw Error(-32601,"Unknown runtime method.");
    }
    Json restore_history(const std::string& method,Json params) {
        fields(params,{"request_id","base_revision"},{"request_id","base_revision"});identifier(params.at("request_id"));revision(params.at("base_revision"));
        params["method"]=method;
        for(const auto& receipt:doc_.at("receipts"))if(receipt.at("params").at("request_id")==params.at("request_id")) {
            require(receipt.at("params")==params,"Transaction ID was already used by another operation.",-32010);
            auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        require(params.at("base_revision")==doc_.at("revision"),"Revision conflict; inspect the world before undo/redo.",-32009);
        require(revision(doc_.at("revision"))<max_revision,"World revision limit reached.");
        const bool undo=method=="world.undo";const auto& source=undo ? undo_ : redo_;
        require(!source.empty(),"No matching history remains in this session.",-32004);
        const auto& edit=*source.back();const auto& target_state=undo ? edit.before : edit.after;const auto& target=target_state.at("entities");
        require(history_state(doc_)==(undo ? edit.after : edit.before),"History no longer matches the authored state.",-32009);
        auto staged=doc_;staged["entities"]=target;staged["revision"]=revision(doc_.at("revision"))+1;
        const bool navigation_changed=doc_.contains("navigation")!=target_state.contains("navigation") ||
            (doc_.contains("navigation") && target_state.contains("navigation") && doc_.at("navigation")!=target_state.at("navigation"));
        if(target_state.contains("navigation"))staged["navigation"]=target_state.at("navigation");else staged.erase("navigation");
        const bool water_changed=doc_.contains("water")!=target_state.contains("water") ||
            (doc_.contains("water") && target_state.contains("water") && doc_.at("water")!=target_state.at("water"));
        if(target_state.contains("water"))staged["water"]=target_state.at("water");else staged.erase("water");
        const bool snow_changed=optional_authored_value_differs(doc_,target_state,"snow");
        if(target_state.contains("snow"))staged["snow"]=target_state.at("snow");else staged.erase("snow");
        const bool checkpoint_changed=optional_authored_value_differs(doc_,target_state,"physics_checkpoint");
        if(target_state.contains("physics_checkpoint"))staged["physics_checkpoint"]=target_state.at("physics_checkpoint");
        else staged.erase("physics_checkpoint");
        const bool provenance_changed=doc_.contains("asset_provenance")!=target_state.contains("asset_provenance") ||
            (doc_.contains("asset_provenance") && target_state.contains("asset_provenance") && doc_.at("asset_provenance")!=target_state.at("asset_provenance"));
        if(target_state.contains("asset_provenance"))staged["asset_provenance"]=target_state.at("asset_provenance");else staged.erase("asset_provenance");
        const bool mark_profiles_changed=optional_authored_value_differs(doc_,target_state,"surface_mark_profiles");
        if(target_state.contains("surface_mark_profiles"))staged["surface_mark_profiles"]=target_state.at("surface_mark_profiles");
        else staged.erase("surface_mark_profiles");
        const bool audio_buses_changed=optional_authored_value_differs(doc_,target_state,"audio_buses");
        if(target_state.contains("audio_buses"))staged["audio_buses"]=target_state.at("audio_buses");else staged.erase("audio_buses");
        if(staged.at("version")>=2) {
            std::set<std::string> retired;for(const auto& id:staged.at("retired_component_schemas"))retired.insert(id.get<std::string>());
            const auto& schemas=target_state.at("component_schemas");
            for(const auto& [id,value]:staged.at("component_schemas").items()) { (void)value;if(!schemas.contains(id))retired.insert(id); }
            for(const auto& [id,value]:schemas.items()) { (void)value;retired.erase(id); }
            staged["component_schemas"]=schemas;staged["retired_component_schemas"]=retired;
        }
        std::set<std::string> changed_templates;
        if(staged.at("version")>=3) {
            std::set<std::string> retired;for(const auto& id:staged.at("retired_template_ids"))retired.insert(id.get<std::string>());
            const auto& templates=target_state.at("templates");
            for(const auto& [id,value]:staged.at("templates").items()) {
                if(!templates.contains(id)) {retired.insert(id);changed_templates.insert(id);}
                else if(templates.at(id)!=value)changed_templates.insert(id);
            }
            for(const auto& [id,value]:templates.items()) { (void)value;retired.erase(id);if(!staged.at("templates").contains(id))changed_templates.insert(id); }
            staged["templates"]=templates;staged["retired_template_ids"]=retired;
        }
        std::set<std::string> changed_ui;
        if(staged.at("version")>=4) {
            std::set<std::string> retired;for(const auto& id:staged.at("retired_ui_ids"))retired.insert(id.get<std::string>());
            const auto& target_ui=target_state.at("ui");
            for(const auto& [id,value]:staged.at("ui").items()) {
                if(!target_ui.contains(id)) {retired.insert(id);changed_ui.insert(id);}
                else if(target_ui.at(id)!=value)changed_ui.insert(id);
            }
            for(const auto& [id,value]:target_ui.items()) {(void)value;retired.erase(id);if(!staged.at("ui").contains(id))changed_ui.insert(id);}
            staged["ui"]=target_ui;staged["retired_ui_ids"]=retired;
        }
        std::set<std::string> retired,changed;
        for(const auto& id:doc_.at("retired_ids"))retired.insert(id.get<std::string>());
        for(const auto& [id,value]:doc_.at("entities").items()) {
            if(!target.contains(id)) { retired.insert(id);changed.insert(id); }
            else if(target.at(id)!=value)changed.insert(id);
        }
        for(const auto& [id,value]:target.items()) { (void)value;retired.erase(id);if(!doc_.at("entities").contains(id))changed.insert(id); }
        staged["retired_ids"]=retired;validate(staged);validate_animation_document(staged,asset_directory(),model_cache_);
        validate_snow_edit(staged,changed,snow_changed,water_changed);
        validate_mesh_lod_edit(staged,changed);
        if(mark_profiles_changed)validate_surface_mark_assets(staged,asset_directory());
        Json result={{"revision",staged.at("revision")},{"committed",true},{"replayed",false},{"changed_ids",changed},{"history_recorded",true},{"history_action",undo ? "undo" : "redo"}};
        if(!changed_templates.empty())result["changed_template_ids"]=changed_templates;
        if(!changed_ui.empty())result["changed_ui_ids"]=changed_ui;
        if(navigation_changed)result["changed_world_fields"]=Json::array({"navigation"});
        if(water_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("water");}
        if(snow_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("snow");}
        if(checkpoint_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("physics_checkpoint");}
        if(provenance_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("asset_provenance");}
        if(mark_profiles_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("surface_mark_profiles");}
        if(audio_buses_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("audio_buses");}
        auto& receipts=staged["receipts"];if(receipts.size()==128)receipts.erase(receipts.begin());receipts.push_back({{"params",params},{"result",result}});
        auto next_undo=undo_,next_redo=redo_;
        if(undo) { next_redo.push_back(next_undo.back());next_undo.pop_back(); }
        else { next_undo.push_back(next_redo.back());next_redo.pop_back(); }
        authored_presentation_.require_next_generation();
        persist(std::move(staged));authored_presentation_.advance_generation();undo_.swap(next_undo);redo_.swap(next_redo);return result;
    }
    Json transact(Json params,const std::string& origin="world.transact") {
        fields(params, {"request_id", "base_revision", "ops", "preview"}, {"request_id", "base_revision", "ops"});
        identifier(params.at("request_id")); revision(params.at("base_revision"));
        require(!params.contains("preview") || params.at("preview").is_boolean(), "Preview must be boolean.");
        if (!params.contains("preview")) params["preview"] = false;
        if(origin!="world.transact")params["method"]=origin;
        for (const auto& receipt : doc_.at("receipts")) {
            if (receipt.at("params").at("request_id") != params.at("request_id")) continue;
            bool custom=false;for(const auto& op:params.at("ops"))if(op.is_object() && op.contains("op") &&
                (op.at("op")=="ui.element.set" || op.at("op")=="ui.element.remove" || op.at("op")=="template.set" || op.at("op")=="template.remove" || op.at("op")=="component.schema.set" || op.at("op")=="component.schema.remove" ||
                    op.at("op")=="physics_checkpoint.set" || (op.contains("type") && op.at("type").is_string() && custom_component(op.at("type").get<std::string>()))))custom=true;
            require(custom ? receipt.at("params").dump()==params.dump() : receipt.at("params")==params,"Transaction ID was already used with different parameters.",-32010);
            auto result = receipt.at("result"); result["replayed"] = true; return result;
        }
        require(params.at("base_revision") == doc_.at("revision"), "Revision conflict; inspect the current world and retry.", -32009);
        require(revision(doc_.at("revision")) < max_revision, "World revision limit reached.");
        const auto& ops = params.at("ops");
        require(ops.is_array() && !ops.empty() && ops.size() <= 256, "Transaction needs 1..256 operations.");
        Json staged = doc_;
        const auto changes = stage_transaction_operations(staged, ops, path_);
        staged["revision"] = revision(doc_.at("revision")) + 1;
        validate(staged);
        validate_animation_document(staged,asset_directory(),model_cache_);
        if(changes.navigation_touched && staged.contains("navigation"))(void)resolve_navigation_binding(staged,asset_directory());
        if(changes.water_touched && staged.contains("water") && !staged.contains("snow"))(void)resolve_water_binding(staged,asset_directory());
        validate_snow_edit(staged,changes.entity_ids,changes.snow_touched,changes.water_touched);
        validate_mesh_lod_edit(staged,changes.entity_ids);
        if(changes.mark_profiles_touched)validate_surface_mark_assets(staged,asset_directory());
        for(const auto& asset:changes.provenance_assets)if(staged.contains("asset_provenance") && staged.at("asset_provenance").contains(asset))
            validate_provenance_binding(asset_directory(),asset,staged.at("asset_provenance").at(asset));
        const bool provenance_changed=doc_.contains("asset_provenance")!=staged.contains("asset_provenance") ||
            (doc_.contains("asset_provenance") && staged.contains("asset_provenance") && doc_.at("asset_provenance")!=staged.at("asset_provenance"));
        const bool navigation_changed=doc_.contains("navigation")!=staged.contains("navigation") ||
            (doc_.contains("navigation") && staged.contains("navigation") && doc_.at("navigation")!=staged.at("navigation"));
        const bool water_changed=doc_.contains("water")!=staged.contains("water") ||
            (doc_.contains("water") && staged.contains("water") && doc_.at("water")!=staged.at("water"));
        const bool snow_changed=optional_authored_value_differs(doc_,staged,"snow");
        const bool checkpoint_changed=optional_authored_value_differs(doc_,staged,"physics_checkpoint");
        const bool mark_profiles_changed=optional_authored_value_differs(doc_,staged,"surface_mark_profiles");
        const bool audio_buses_changed=optional_authored_value_differs(doc_,staged,"audio_buses");
        Json result = {{"revision", staged["revision"]}, {"committed", !params["preview"].get<bool>()},
                       {"replayed", false}, {"changed_ids", changes.entity_ids}};
        if(!changes.template_ids.empty())result["changed_template_ids"]=changes.template_ids;
        if(!changes.ui_ids.empty())result["changed_ui_ids"]=changes.ui_ids;
        if(navigation_changed)result["changed_world_fields"]=Json::array({"navigation"});
        if(water_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("water");}
        if(snow_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("snow");}
        if(checkpoint_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("physics_checkpoint");}
        if(provenance_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("asset_provenance");}
        if(mark_profiles_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("surface_mark_profiles");}
        if(audio_buses_changed) {if(!result.contains("changed_world_fields"))result["changed_world_fields"]=Json::array();result["changed_world_fields"].push_back("audio_buses");}
        if (!params["preview"].get<bool>()) {
            auto next_undo=undo_;History next_redo;
            auto edit=std::make_shared<Edit>(Edit{history_state(doc_),history_state(staged),0,params.at("request_id").get<std::string>()});
            edit->bytes=edit->before.dump().size()+edit->after.dump().size();
            const bool recorded=edit->bytes<=max_document_bytes;
            result["history_recorded"]=recorded;
            if(recorded) {
                next_undo.push_back(std::move(edit));
                while(next_undo.size()>32 || history_bytes(next_undo,next_redo)>max_document_bytes)next_undo.erase(next_undo.begin());
            }else { next_undo.clear();result["history_reason"]="Edit exceeds the 16 MiB history budget; prior history was cleared."; }
            auto& receipts = staged["receipts"];
            if (receipts.size() == 128) receipts.erase(receipts.begin());
            receipts.push_back({{"params", params}, {"result", result}});
            persist(std::move(staged));
            undo_.swap(next_undo);redo_.swap(next_redo);if(!recorded)++skipped_large_edits_;
        }
        return result;
    }
};
}

struct WorldSession::Impl {
    World world;
    bool closed=false;
    explicit Impl(const std::string& path,WorldOpenMode mode,const std::string& protected_root):world(path,mode,protected_root) {}
};
WorldSession::WorldSession(const std::string& path,WorldOpenMode mode,const std::string& protected_root):impl_(std::make_unique<Impl>(path,mode,protected_root)) {}
WorldSession::~WorldSession()=default;
void WorldSession::observe_blocking_player(PlayerProgressObserver observer) {
    require(!closed(),"World session is closed.",-32001);
    impl_->world.observe_blocking_player(std::move(observer));
}
void WorldSession::enable_startup_splash() {
    require(!closed(),"World session is closed.",-32001);
    impl_->world.enable_startup_splash();
}
void WorldSession::configure_development_profiles(const std::string& filename) {
    require(!closed(),"World session is closed.",-32001);
    impl_->world.configure_development_profiles(filename);
}
void WorldSession::begin_development_shutdown() { impl_->world.begin_development_shutdown(); }
bool WorldSession::development_shutdown_complete() const noexcept { return impl_->world.development_shutdown_complete(); }
bool WorldSession::poll_player() {
    require(!closed(),"World session is closed.",-32001);
    profiling::Binding trace(&impl_->world.profiler(),profiling::Source::player);
    return impl_->world.poll_player();
}
PlayerPollResult WorldSession::poll_player_cooperative() {
    require(!closed(),"World session is closed.",-32001);
    profiling::Binding trace(&impl_->world.profiler(),profiling::Source::player);
    return impl_->world.poll_player_cooperative();
}
bool WorldSession::player_active() const {
    return !closed() && impl_->world.player_active();
}
void WorldSession::stop_player() {
    require(!closed(),"World session is closed.",-32001);
    profiling::Binding trace(&impl_->world.profiler(),profiling::Source::player);
    impl_->world.stop_player();
}

bool WorldSession::closed() const { return impl_->closed; }
profiling::Recorder& WorldSession::profiler() noexcept { return impl_->world.profiler(); }
WorldProfilerContext WorldSession::profiler_context() const { return impl_->world.profiler_context(); }
WorldPackageContent WorldSession::package_content() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.package_content();
}
WorldRuntimeStatus WorldSession::runtime_status() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.runtime_status();
}
std::shared_ptr<const ui::Presentation> WorldSession::runtime_ui_presentation() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.runtime_ui_presentation();
}
WorldTickAdvance WorldSession::advance_tick(const std::string& expected_session,std::uint64_t expected_tick,
    const std::vector<RuntimeInput>& inputs) {
    require(!closed(),"World session is closed.",-32001);
    profiling::Binding trace(&impl_->world.profiler());
    impl_->world.guard_player_request("runtime.step");
    const auto result=impl_->world.advance_tick(expected_session,expected_tick,inputs);
    impl_->world.synchronize_player_request("runtime.step",Json::object());
    return result;
}
WorldAudioState WorldSession::audio_state(const std::string& expected_session,std::uint64_t expected_tick,
    const std::string& listener) const {
    require(!closed(),"World session is closed.",-32001);
    profiling::Binding trace(&impl_->world.profiler());profiling::SessionScope session(expected_session);
    profiling::Scope sample("world.audio.snapshot",expected_tick<=max_revision ? static_cast<std::int64_t>(expected_tick) : -1);
    return impl_->world.audio_state(expected_session,expected_tick,listener);
}
std::vector<std::pair<std::string,std::string>> WorldSession::runtime_hierarchy() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.runtime_hierarchy();
}
WorldGameplayStatus WorldSession::gameplay_status() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.gameplay_status();
}
WorldComponentStatus WorldSession::component_status() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.component_status();
}
WorldSaveStatus WorldSession::save_status() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.save_configuration_status();
}
SceneSnapshot WorldSession::authored_snapshot(const EditorCamera& camera) const {
    profiling::Binding trace(&impl_->world.profiler());profiling::Scope sample("world.snapshot");
    require(!closed(),"World session is closed.",-32001);return impl_->world.editor_snapshot(camera,false);
}
SceneSnapshot WorldSession::authored_preview(const EditorCamera& camera,const std::string& entity,
    const std::array<double,3>& position,const std::array<double,4>& rotation,const std::array<double,3>& scale) const {
    require(!closed(),"World session is closed.",-32001);
    return impl_->world.editor_preview(camera,entity,{{"position",position},{"rotation",rotation},{"scale",scale}});
}
SceneSnapshot WorldSession::runtime_snapshot(const EditorCamera& camera) const {
    profiling::Binding trace(&impl_->world.profiler());profiling::Scope sample("world.snapshot");
    require(!closed(),"World session is closed.",-32001);return impl_->world.editor_snapshot(camera,true);
}
std::vector<WorldCameraInfo> WorldSession::cameras(bool live) const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.cameras(live);
}
std::vector<WorldControllerInfo> WorldSession::controllers(bool live) const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.controllers(live);
}
SceneSnapshot WorldSession::authored_camera_snapshot(const std::string& id) const {
    profiling::Binding trace(&impl_->world.profiler());profiling::Scope sample("world.snapshot");
    require(!closed(),"World session is closed.",-32001);return impl_->world.camera_snapshot(id,false);
}
SceneSnapshot WorldSession::runtime_camera_snapshot(const std::string& id) const {
    profiling::Binding trace(&impl_->world.profiler());profiling::Scope sample("world.snapshot");
    require(!closed(),"World session is closed.",-32001);return impl_->world.camera_snapshot(id,true);
}
std::string WorldSession::request(std::string_view line,WorldRequestScope scope) {
    Json id=nullptr,response;bool notification=false;
    try {
        require(line.size()<=1024*1024,"Request exceeds 1 MiB.",-32700);
        Json request;
        try { request=parse(std::string(line)); }
        catch(const Json::exception&) { throw Error(-32700,"Invalid JSON."); }
        require(request.is_object() && request.value("jsonrpc",Json{})=="2.0" && request.contains("method") && request.at("method").is_string(),"Invalid JSON-RPC request.",-32600);
        if(request.contains("id")) {
            require(request["id"].is_null() || request["id"].is_string() || request["id"].is_number_integer(),"Invalid request ID.",-32600);id=request["id"];
        }else notification=true;
        require(!closed(),"World session is closed.",-32001);
        const auto method=request["method"].get<std::string>();
        const bool shared=scope!=WorldRequestScope::standalone;
        const bool discovery=method=="world.describe" || method=="world.describe.select";
        require(!shared || method!="session.close","Disconnect the client to detach; host.shutdown stops a headless shared host.",-32080);
        if(scope==WorldRequestScope::shared_editor)
            require(method!="world.capture" && method!="runtime.capture" && method!="runtime.capture.release" && method!="asset.animation.capture" && method!="runtime.play",
                    "This operation creates a graphics lifetime; use editor.capture or the editor's runtime controls in a shared editor.",-32080);
        if(method=="host.shutdown")require(scope==WorldRequestScope::shared_headless,"Only a shared headless host supports host.shutdown.",-32080);
        if(method=="player.start" || method=="player.control" || method=="player.capture" || method=="player.capture_current" || method=="player.settings.transact")
            require(scope==WorldRequestScope::shared_headless,"Player service mutations require a pumped shared headless owner.",-32080);
        impl_->world.guard_player_request(method);
        const bool trace=!method.starts_with("profiler.");
        profiling::Binding trace_binding(trace ? &impl_->world.profiler() : nullptr,profiling::Source::request);
        const auto runtime=trace && profiling::active() ? impl_->world.profiler_context() : WorldProfilerContext{};
        profiling::SessionScope trace_session(runtime.session.data());
        const bool trace_name_safe=std::all_of(method.begin(),method.end(),[](unsigned char c){return c>=32 && c<=126;});
        profiling::Scope trace_request(trace_name_safe ? std::string_view(method) : std::string_view("request.invalid_name"),runtime.tick);
        auto result=impl_->world.dispatch(method=="host.shutdown" ? "session.close" : method,request.value("params",Json::object()));
        impl_->world.synchronize_player_request(method,result);
        if(discovery && shared) {
            result["session_scope"]=scope==WorldRequestScope::shared_editor ? "shared_editor" : "shared_headless";
            result["methods"].erase("session.close");
            if(scope==WorldRequestScope::shared_headless)result["methods"]["host.shutdown"]=object_schema(Json::object());
            else {
                result["unavailable_methods"]={"world.capture","runtime.capture","runtime.capture.release","asset.animation.capture","runtime.play"};
                for(const auto& name:result["unavailable_methods"])result["methods"].erase(name.get<std::string>());
                result["editor_discovery"]="editor.describe";
            }
        }
        if(discovery && scope!=WorldRequestScope::shared_headless)
            for(const auto* name:{"player.start","player.control","player.capture","player.capture_current","player.settings.transact"})result["methods"].erase(name);
        // Project only after scope restrictions, so focused discovery cannot
        // advertise an operation hidden from the full session descriptor.
        if(discovery) {
            // Resolve the real owner scope before preserving this reserved header
            // in full and focused discovery. Qualification is not call readiness.
            result["runtime_game_session_contract"]=runtime_game_session_contract_identity(result.at("read_only").get<bool>(),scope);
            const auto params=request.value("params",Json::object());
            const auto view=method=="world.describe.select" ? std::string("selection") : discovery_view(params);
            result=project_discovery(std::move(result),params,view,
                [&](const std::string& type){impl_->world.require_registered_custom_type(type);});
        }
        response={{"jsonrpc","2.0"},{"id",id},{"result",result}};
        if(method=="session.close" || method=="host.shutdown")impl_->closed=true;
    }catch(const Error& error) {
        response={{"jsonrpc","2.0"},{"id",id},{"error",{{"code",error.code},{"message",error.what()}}}};
        if(!error.data.is_null())response["error"]["data"]=error.data;
    }catch(const Json::exception&) {
        response={{"jsonrpc","2.0"},{"id",id},{"error",{{"code",-32602},{"message","Invalid operation parameters."}}}};
    }catch(const std::exception& error) {
        std::cerr<<"World operation failed: "<<error.what();
        if(const auto* storage=dynamic_cast<const world_detail::StorageFailure*>(&error))
            world_detail::append_storage_diagnostic(std::cerr,*storage);
        std::cerr<<'\n';
        response={{"jsonrpc","2.0"},{"id",id},{"error",{{"code",-32000},{"message","World storage failure; inspect stderr."}}}};
    }
    return notification ? std::string{} : response.dump();
}
int run_world_stdio(WorldSession& session) {
    while(!session.closed()) {
        std::string line;bool oversized=false;char c=0;
        while(std::cin.get(c) && c!='\n') { if(line.size()<1024*1024)line+=c;else oversized=true; }
        if(line.empty() && !oversized && !std::cin)break;
        if(oversized)line.push_back(' '); // Preserve the original bounded framing error.
        const auto response=session.request(line);
        if(!response.empty())std::cout<<response<<'\n'<<std::flush;
    }
    return 0;
}
int run_world_session(const std::string& utf8_path,const std::string& development_profiles) {
    WorldSession session(utf8_path);
    if(!development_profiles.empty())session.configure_development_profiles(development_profiles);
    return run_world_stdio(session);
}
}
