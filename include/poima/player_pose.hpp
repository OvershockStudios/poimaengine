// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"

namespace poima {
// Owned fixed-Tick effects only. The current native base is never interpolated.
struct PlayerPoseSample {
    std::string session_id,source_id,camera_id;
    std::uint64_t tick=0,material_revision=0;
    RuntimeCameraPose world;
    std::optional<RuntimeCameraPose> viewmodel;
    Matrix4 base_world=identity_matrix();
    friend bool operator==(const PlayerPoseSample&,const PlayerPoseSample&)=default;
};
struct PlayerPosePresentation {
    bool enabled=false,interpolating=false;
    std::uint64_t previous_tick=0,target_tick=0;
    double alpha=1;
    std::string reset_reason;
    RuntimeCameraPose previous_world,target_world,rendered_world;
    std::optional<RuntimeCameraPose> previous_viewmodel,target_viewmodel,rendered_viewmodel;
    Matrix4 target_base_world=identity_matrix();
    friend bool operator==(const PlayerPosePresentation&,const PlayerPosePresentation&)=default;
};
// Only the last consecutive committed pair is retained. Repeated presentation
// queries do not advance either the simulation or this history.
class PlayerPoseHistory {
    std::optional<PlayerPoseSample> previous_,target_;
    std::string reset_reason_;
public:
    void reset(const PlayerPoseSample& sample,const std::string& reason);
    void commit(const PlayerPoseSample& sample);
    void reconcile(const PlayerPoseSample& sample);
    PlayerPosePresentation presentation(double alpha,bool enabled) const;
};
}
