// SPDX-License-Identifier: Apache-2.0
#include "physical_pose_diagnostics.hpp"
#include "physical_rig_json.hpp"
#include "poima/physical_target.hpp"
#include <cmath>
#include <map>
#include <stdexcept>

namespace poima {
using Json = nlohmann::json;

Json physical_pose_diagnostics(const ModelAsset& model, const Json& physical,
    std::optional<std::uint32_t> clip, double time, bool loop) {
    const auto configuration = physical_rig::parse_configuration(physical);
    const auto pose = sample_model(model, clip, time, loop);
    std::map<std::string, const physical_rig::Part*> parts;
    std::vector<std::optional<physical_rig::Quat>> world(model.nodes.size());
    std::vector<std::size_t> chain;
    for (const auto& part : configuration.profile.parts) {
        parts.emplace(part.name, &part);
        if (part.node >= world.size()) throw std::invalid_argument("PhysicalRig part is outside the cooked model.");
        // sample_model has validated every parent and rejected cycles. Cache each
        // required ancestor once, even when the file lists children before parents.
        for (int node = static_cast<int>(part.node); node >= 0 && !world[node]; node = model.nodes[node].parent)
            chain.push_back(static_cast<std::size_t>(node));
        while (!chain.empty()) {
            const auto node = chain.back();
            chain.pop_back();
            const auto local = physical_rig::canonical_quaternion(pose.local[node].rotation);
            const int parent = model.nodes[node].parent;
            world[node] = parent < 0 ? local : physical_rig::multiply(*world[parent], local);
        }
    }

    auto frame = [&](const physical_rig::Part& part, const physical_rig::Pose& joint_frame) {
        return Json{{"part",part.name},{"node",part.node},{"bone_rotation_model",*world[part.node]},
            {"body_rotation_in_bone",part.body_in_bone.rotation},{"joint_rotation_in_body",joint_frame.rotation}};
    };
    Json joints = Json::array();
    std::size_t within = 0, outside = 0, indeterminate = 0;
    for (const auto& joint : configuration.profile.joints) {
        const auto& parent = *parts.at(joint.parent);
        const auto& child = *parts.at(joint.child);
        const auto requested = physical_rig::desired_joint_rotation(joint, parent, child, *world[parent.node], *world[child.node]);
        Json row = {{"joint",joint.name},{"parent",frame(parent,joint.parent_frame)},{"child",frame(child,joint.child_frame)},
            {"limits",physical.at("joints").at(joints.size()).at("limits")},{"requested_rotation",requested},
            {"chart_denominator",std::hypot(requested[0],requested[3])},{"fit",nullptr}};
        try {
            const auto fit = physical_rig::fit_target(joint, requested, physical_rig::TargetLimitPolicy::Fit);
            const bool adjusted = fit.hinge_projected || fit.twist_clamped || fit.swing_clamped;
            row["status"] = adjusted ? "outside_limits" : "within_limits";
            row["fit"] = {{"rotation",fit.target},{"correction_radians",fit.correction_radians},
                {"hinge_projected",fit.hinge_projected},{"twist_clamped",fit.twist_clamped},{"swing_clamped",fit.swing_clamped}};
            if (adjusted) ++outside;
            else ++within;
        } catch (const std::invalid_argument& error) {
            row["status"] = "indeterminate";
            row["reason"] = error.what();
            ++indeterminate;
        }
        joints.push_back(std::move(row));
    }
    const char* overall = "within_limits";
    if (joints.empty()) overall = "no_joints";
    else if (outside) overall = "outside_limits";
    else if (indeterminate) overall = "indeterminate";
    return {{"format","poima.physical-pose-diagnostics.v1"},{"source","cooked_model"},
        {"rotation_space","child_joint_relative_to_parent_joint"},{"rotation_order","xyzw"},{"angle_unit","radians"},
        {"clip",clip ? Json(*clip) : Json(nullptr)},{"requested_time",time},{"sample_time",pose.time},{"loop",loop},
        {"status",overall},{"counts",{{"within_limits",within},{"outside_limits",outside},{"indeterminate",indeterminate}}},
        {"joints",std::move(joints)}};
}
}
