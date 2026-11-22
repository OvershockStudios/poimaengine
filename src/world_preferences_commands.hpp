// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "world_json.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace poima {
class GamepadHost;
class Runtime;
struct PlayerOptions;
struct PlayerReport;
struct RuntimeCameraState;

namespace world_detail {

enum class PreferenceKind { input, settings };

// World supplies the selected filesystem roots and retains the shared gamepad host.
struct PreferencePaths {
    const std::filesystem::path& world;
    std::filesystem::path assets;
    const std::filesystem::path& protected_root;
    bool read_only;
};

std::filesystem::path resolve_preference_profile_path(const Json& value,const PreferencePaths& paths,
    PreferenceKind kind);
Json dispatch_input_profile(const std::string& method,const Json& params,const PreferencePaths& paths,
    std::shared_ptr<GamepadHost>& gamepad_host);
Json dispatch_saved_player_settings(const std::string& method,const Json& params,const PreferencePaths& paths);
Json apply_player_settings(const Json& params,PlayerOptions& options,const PreferencePaths& paths,
    const Runtime& runtime);
Json player_live_settings(const PlayerOptions& options,const PlayerReport& report,
    const std::optional<RuntimeCameraState>& final_camera);
void finish_player_settings(Json& application,const PlayerOptions& options,const PlayerReport& report,
    const std::optional<RuntimeCameraState>& final_camera);

} // namespace world_detail
} // namespace poima
