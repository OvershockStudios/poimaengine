// SPDX-License-Identifier: Apache-2.0
#include "poima/foliage_motion.hpp"
#include "poima/scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace poima {
namespace {
bool between(double value, double low, double high) {
    return std::isfinite(value) && value>=low && value<=high;
}
}

void validate_foliage_motion(const FoliageMotion& motion) {
    const double height=motion.tip_y-motion.root_y;
    if(!between(motion.root_y,-1024,1024) || !between(motion.tip_y,-1024,1024) ||
        !between(height,.05,128) || (!motion.wind && !motion.touch))
        throw std::invalid_argument("Invalid foliage bending extent or missing profile.");
    if(const auto& w=motion.wind) {
        if(!between(w->direction[0],-1,1) || !between(w->direction[1],-1,1) ||
            std::hypot(w->direction[0],w->direction[1])<1e-4 || !between(w->tip_m,0,2) ||
            w->period_ticks<2 || w->period_ticks>3600 || !between(w->wavelength_m,.5,256))
            throw std::invalid_argument("Invalid foliage wind profile.");
    }
    if(const auto& t=motion.touch) {
        if(t->actor.empty() || !between(t->radius_m,.05,8) || !between(t->tip_m,0,2))
            throw std::invalid_argument("Invalid foliage touch profile.");
    }
}

FoliageMotionSample sample_foliage_motion(const FoliageMotion& motion, const Matrix4& world,
    std::uint64_t tick, const std::optional<FoliageActor>& actor) {
    const auto stem=[&](double y) {
        return std::array{world[4]*y+world[12],world[5]*y+world[13],world[6]*y+world[14]};
    };
    const auto root=stem(motion.root_y);
    const auto tip=stem(motion.tip_y);
    FoliageMotionSample result;
    double x=0, z=0;
    if(const auto& wind=motion.wind;wind && wind->tip_m>0) {
        const auto& direction=wind->direction;
        const double len=std::hypot(direction[0],direction[1]);
        const double nx=direction[0]/len,nz=direction[1]/len;
        const double spatial=std::remainder(nx*root[0]+nz*root[2],wind->wavelength_m)/wind->wavelength_m;
        const double time=static_cast<double>(tick%wind->period_ticks)/wind->period_ticks;
        const double offset=wind->tip_m*std::sin(2*std::numbers::pi*(time-spatial));
        x=nx*offset;
        z=nz*offset;
    }
    if(const auto& touch=motion.touch;touch && touch->tip_m>0 && actor &&
        actor->feet[1]<=std::max(root[1],tip[1]) &&
        actor->feet[1]+actor->height>=std::min(root[1],tip[1])) {
        const double dx=root[0]-actor->feet[0],dz=root[2]-actor->feet[2];
        const double distance=std::hypot(dx,dz);
        const double t=1-std::clamp(distance/touch->radius_m,0.,1.);
        result.touch_weight=t*t*(3-2*t);
        // Soften the center so a crossing changes direction continuously.
        const double scale=touch->tip_m*result.touch_weight/std::hypot(distance,touch->radius_m*.125);
        x+=dx*scale;
        z+=dz*scale;
    }
    const double requested=std::hypot(x,z);
    if(requested==0)return result;

    // Project the world direction onto the plant's local bending plane.
    const auto inverse=inverse_affine(world);
    double local_x=inverse[0]*x+inverse[8]*z;
    double local_z=inverse[2]*x+inverse[10]*z;
    const auto world_length=std::hypot(world[0]*local_x+world[8]*local_z,
        world[1]*local_x+world[9]*local_z,world[2]*local_x+world[10]*local_z);
    if(world_length>requested) {
        local_x*=requested/world_length;
        local_z*=requested/world_length;
    }
    if(!between(local_x,-std::numeric_limits<float>::max(),std::numeric_limits<float>::max()) ||
        !between(local_z,-std::numeric_limits<float>::max(),std::numeric_limits<float>::max()))
        throw std::runtime_error("Foliage displacement exceeds GPU coordinate range.");
    result.deformation={static_cast<float>(local_x),static_cast<float>(local_z),
        static_cast<float>(motion.root_y),static_cast<float>(1/(motion.tip_y-motion.root_y))};
    for(std::size_t axis=0;axis<3;++axis)
        result.world_tip_offset_m[axis]=world[axis]*result.deformation[0]+world[8+axis]*result.deformation[1];
    return result;
}

Bounds foliage_displaced_bounds(Bounds bounds, const Matrix4& world, const FoliageMotionSample& pose) {
    const auto& [x,z,root,inverse_height]=pose.deformation;
    if(x==0 && z==0)return bounds;
    if(!std::isfinite(x) || !std::isfinite(z) || !between(root,-1024,1024) ||
        !between(inverse_height,1./128,20))
        throw std::runtime_error("Invalid foliage presentation pose.");
    std::array<double,3> offset;
    for(std::size_t axis=0;axis<3;++axis)
        offset[axis]=world[axis]*x+world[8+axis]*z;
    if(!between(std::hypot(offset[0],offset[1],offset[2]),0,4.00001))
        throw std::runtime_error("Foliage presentation displacement exceeds its bound.");
    for(std::size_t axis=0;axis<3;++axis) {
        bounds.minimum[axis]+=std::min(0.0,offset[axis]);
        bounds.maximum[axis]+=std::max(0.0,offset[axis]);
    }
    return bounds;
}
}
