// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include <nlohmann/json.hpp>

namespace poima {
inline nlohmann::json attachment_outcome_rows(const std::map<std::string,RuntimeAttachmentOutcome>& outcomes) {
    using Json=nlohmann::json;
    auto rows=Json::array();
    for(const auto& [id,outcome]:outcomes) {
        Json row={{"entity",id},{"anchor",outcome.anchor}};
        if(const auto* frame=std::get_if<RuntimeAttachmentFrameMiss>(&outcome.detail)) {
            row["status"]="frame_miss";
            row["position_error_m"]=frame->position_error_m;row["position_limit_m"]=frame->position_limit_m;
            row["rotation_error_deg"]=frame->rotation_error_deg ? Json(*frame->rotation_error_deg) : Json(nullptr);
            row["rotation_limit_deg"]=frame->rotation_limit_deg;
        } else if(const auto* surface=std::get_if<RuntimeAttachmentSurfaceMiss>(&outcome.detail)) {
            row["status"]="surface_miss";
            row["probe_index"]=surface->probe_index;row["target_triangle"]=surface->target_triangle;
            row["distance_m"]=surface->distance_m;row["distance_limit_m"]=surface->distance_limit_m;
        } else if(const auto* blocked=std::get_if<RuntimeAttachmentReleaseBlocked>(&outcome.detail)) {
            row["status"]="release_blocked";
            row["obstacle"]=blocked->obstacle;row["penetration_m"]=blocked->penetration_m;
        } else if(const auto* reach=std::get_if<RuntimeAttachmentReachBlocked>(&outcome.detail)) {
            row["status"]="reach_blocked";
            row["obstacle"]=reach->obstacle;row["segment"]=reach->segment;row["interval"]=reach->interval;
            row["fraction"]=reach->fraction;row["penetration_m"]=reach->penetration_m;
        } else row["status"]=std::holds_alternative<RuntimeAttachmentReleased>(outcome.detail) ? "released" : "transferred";
        rows.push_back(std::move(row));
    }
    return rows;
}
}
