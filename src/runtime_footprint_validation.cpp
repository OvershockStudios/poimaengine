// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include "pose_frames.hpp"
#include "surface_mark_profiles.hpp"
#include "surface_mark_validation.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace poima {
void validate_runtime_footprint_surfaces(const RuntimeDefinition& definition) {
    for(const auto& recipe:definition.templates)
        for(const auto& entity:recipe.entities)if(entity.footprint_surface)
            throw std::runtime_error("FootprintSurface is unavailable in runtime template '"+recipe.id+"'.");

    if(std::ranges::none_of(definition.entities,[](const auto& entity) {return entity.footprint_surface.has_value();}))return;

    std::map<std::string_view,const RuntimeEntityDefinition*> graph;
    for(const auto& entity:definition.entities)
        if(!graph.emplace(entity.id,&entity).second)
            throw std::runtime_error("Duplicate entity '"+entity.id+"' in FootprintSurface hierarchy.");

    std::map<std::string_view,Matrix4> world_transforms;
    for(const auto& entity:definition.entities) {
        if(!entity.footprint_surface)continue;
        const auto check=[&](bool condition,const char* message) {
            if(!condition)throw std::runtime_error("FootprintSurface '"+entity.id+"': "+message);
        };
        const auto& surface=*entity.footprint_surface;
        check(surface_mark_validation::style_in_range(surface.style),"Style is outside the surface mark ranges.");
        check(surface.lifetime_ticks>0 && surface.lifetime_ticks<=max_surface_mark_lifetime_ticks,
            "Lifetime must be 1..216000 ticks.");
        check(std::isfinite(surface.sole_tolerance_m) && surface.sole_tolerance_m>=.001 && surface.sole_tolerance_m<=.1,
            "Sole tolerance must be .001..0.1 metres.");
        if(!surface.profile.empty()) {
            const auto profile=definition.surface_mark_profiles.find(surface.profile);
            check(surface_mark_profiles::valid_id(surface.profile) && profile!=definition.surface_mark_profiles.end() && bool(profile->second.image),
                "Profile must name a mask in the frozen surface mark catalog.");
        }

        check(entity.mesh && entity.mesh->visible && entity.mesh->material &&
            entity.mesh->material->alpha_mode==MaterialAlphaMode::opaque && !entity.skinned_mesh && !entity.camera_draw,
            "Receiver must be a visible rigid opaque PBR world drawable.");
        check(!entity.character && bool(entity.collider)!=bool(entity.mesh_collider),
            "Receiver needs exactly one box or triangle mesh collider.");
        if(entity.mesh->mesh) {
            check(entity.mesh_collider && entity.mesh_collider->mesh==entity.mesh->mesh,
                "Triangle collision must use the same cooked primitive as rendering.");
            check(std::ranges::all_of(entity.mesh->mesh->vertices,[](const auto& vertex) {
                return std::ranges::all_of(vertex.position,[](float value) {return std::abs(value)<=10000.f;});
            }),"Triangle vertices must be within +/-10000 metres in receiver space.");
        }else {
            check(entity.collider && entity.collider->half_extents==std::array<float,3>{.5f,.5f,.5f},
                "A built-in box needs a collider with 0.5 m half extents.");
            check(entity.collider->motion==BodyMotion::Static ||
                (entity.collider->motion==BodyMotion::Kinematic && entity.parent.empty()),
                "A built-in box needs static collision or a kinematic hierarchy root.");
        }

        // Resolve each necessary ancestor once. This follows the same composed
        // transform used by rendering and physics, including ordinary parents.
        std::vector<const RuntimeEntityDefinition*> path;
        const auto* current=&entity;
        while(current && !world_transforms.contains(current->id)) {
            check(path.size()<graph.size(),"Receiver hierarchy contains a cycle.");
            check(!current->camera && !current->rig_node && !current->physical_rig,
                "Receiver cannot inherit camera or animation pose effects.");
            path.push_back(current);
            if(current->parent.empty()) {current=nullptr;break;}
            const auto parent=graph.find(current->parent);
            check(parent!=graph.end(),"Receiver ancestor is absent.");
            current=parent->second;
            check(!current->character && (!current->collider || current->collider->motion==BodyMotion::Static),
                "Receiver cannot inherit a moving body or character controller.");
        }
        auto world=current ? world_transforms.at(current->id) : identity_matrix();
        for(auto it=path.rbegin();it!=path.rend();++it) {
            const auto& transform=(*it)->transform;
            world=multiply(world,local_matrix(transform.position,transform.rotation,transform.scale));
            world_transforms.emplace((*it)->id,world);
        }
        check(rigid_transform(world) && pose_frames::axes(world).basis==pose_frames::Basis::regular,
            "Receiver needs an unscaled right-handed world transform.");
        check(std::all_of(world.begin()+12,world.begin()+15,[](double v) {return std::abs(v)<=1e6;}),
            "Receiver position exceeds the current 1000 km bound.");
    }
}
}
