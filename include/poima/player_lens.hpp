// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"

namespace poima {
// Lightweight committed observations. No camera pose, scene or runtime borrow.
struct PlayerLensSample {
    std::string session_id,source_id,camera_id;
    std::uint64_t tick=0,material_revision=0;
    SceneLens world;
    std::optional<SceneLens> viewmodel;
    std::optional<double> authored_fov;
    friend bool operator==(const PlayerLensSample&,const PlayerLensSample&)=default;
};
struct PlayerLensPresentation {
    bool enabled=false,interpolating=false;
    std::uint64_t previous_tick=0,target_tick=0;
    double alpha=1;
    std::string reset_reason;
    SceneLens previous_world,target_world,rendered_world;
    std::optional<SceneLens> previous_viewmodel,target_viewmodel,rendered_viewmodel;
    std::optional<double> authored_fov,preference_fov;
    friend bool operator==(const PlayerLensPresentation&,const PlayerLensPresentation&)=default;
};
// Stores only the final two consecutive fixed-Tick samples. Presentation is a
// pure value query; reconciliation never invents a committed Tick interval.
class PlayerLensHistory {
    std::optional<PlayerLensSample> previous_,target_;
    std::string reset_reason_;
public:
    void reset(const PlayerLensSample& sample,const std::string& reason);
    void commit(const PlayerLensSample& sample);
    void reconcile(const PlayerLensSample& sample);
    PlayerLensPresentation presentation(double alpha,bool enabled,
        std::optional<double> pref={}) const;
};
}
