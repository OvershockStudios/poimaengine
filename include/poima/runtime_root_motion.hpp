// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include "poima/animation.hpp"

namespace poima::root_motion {
// Column-vector SE(2), embedded in the engine's XZ plane, radians about +Y.
struct Planar { double x=0,z=0,yaw=0; };
Planar compose(const Planar&,const Planar&);
Planar inverse(const Planar&);
Planar power(Planar,std::uint64_t);
Planar planar(const Matrix4&,bool yaw);
Matrix4 matrix(const Planar&);
void validate_metric(const Matrix4&,bool upright=false);
void validate_path(std::span<const int>,std::span<const NodePose>,std::uint32_t node);
// Yaw tier: upright rest path and LINEAR pure-yaw rotation curves. Summed
// angular-rate bound covers every ancestry segment at the maximum speed 8.
void validate_yaw_profiles(const ModelAsset&,std::span<const NodePose>,std::span<const int>,std::uint32_t node);
Planar selected(const ModelPose&,std::span<const int>,std::uint32_t node,bool yaw);
// Root velocity is independent of local-pose histories and visual IK.
Planar interval(const CompiledAnimation&,std::span<const NodePose>,std::span<const int>,
    std::uint32_t node,bool yaw,const Planar& rest,double duration,const AnimationCommand&,
    std::uint64_t anchor_tick,std::uint64_t from_tick);
Planar mix(const Planar&,const Planar&,double weight);
double decay_average(double from,double to);
void validate_delta(const Planar&);
void strip(ModelPose&,std::span<const int>,std::uint32_t node,bool yaw,const Planar& rest);
}
