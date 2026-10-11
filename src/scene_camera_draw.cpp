// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace poima {
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
bool identity(const std::string& id) {
    return id.size()==32 && id.find_first_not_of("0123456789abcdef")==std::string::npos && id.find_first_not_of('0')!=std::string::npos;
}
void lens(const SceneLens& value) {
    check(std::isfinite(value.vertical_fov) && value.vertical_fov>=5 && value.vertical_fov<=150,
        "Viewmodel field of view must be finite and within 5..150 degrees.");
    check(std::isfinite(value.near_plane) && std::isfinite(value.far_plane) && value.near_plane>=.001 &&
        value.far_plane<=1e7 && value.far_plane>value.near_plane,"Viewmodel needs 0.001 <= near < far <= 10000000.");
}
void binding(const CameraDrawBinding& value) {
    check(identity(value.camera),"CameraDraw camera must be a nonzero canonical entity ID.");
    check(value.mode==CameraDrawMode::Viewmodel || value.mode==CameraDrawMode::Hidden,"CameraDraw mode is invalid.");
}
}
void validate_runtime_camera_draw(const RuntimeDefinition& definition) {
    const bool present=std::any_of(definition.entities.begin(),definition.entities.end(),[](const auto& e) {
        return e.camera_draw.has_value() || (e.camera && e.camera->viewmodel.has_value());
    });
    if(!present)return; // Preserve legacy native admission/error precedence.
    check(definition.entities.size()<=10000,"CameraDraw hierarchy exceeds the runtime entity budget.");
    std::map<std::string,const RuntimeEntityDefinition*> graph;
    for(const auto& entity:definition.entities) {
        check(graph.emplace(entity.id,&entity).second,"Duplicate entity in CameraDraw hierarchy.");
        if(entity.camera && entity.camera->viewmodel)lens(*entity.camera->viewmodel);
    }
    // Visit each edge once, with no recursion or quadratic ancestry walks for
    // the complete graph. A binding cannot hide a dangling or cyclic parent.
    std::map<std::string,unsigned> marks;
    for(const auto& [id,entity]:graph) {
        (void)entity;if(marks[id]==2)continue;
        std::vector<std::string> path;std::string current=id;
        while(!current.empty() && marks[current]!=2) {
            check(marks[current]!=1,"CameraDraw hierarchy contains a cycle.");
            const auto found=graph.find(current);check(found!=graph.end(),"CameraDraw ancestor is absent.");
            marks[current]=1;path.push_back(current);current=found->second->parent;
        }
        for(const auto& member:path)marks[member]=2;
    }
    std::map<std::pair<std::string,CameraDrawMode>,std::size_t> counts;
    for(const auto& entity:definition.entities)if(entity.camera_draw) {
        const auto& value=*entity.camera_draw;binding(value);
        check(entity.mesh.has_value(),"CameraDraw requires an existing drawable.");
        const auto camera=graph.find(value.camera);
        check(camera!=graph.end() && camera->second->camera.has_value(),"CameraDraw camera entity/component is absent.");
        check(++counts[{value.camera,value.mode}]<=32,"CameraDraw limit is 32 bindings per mode per camera including invisible drawables.");
        if(value.mode==CameraDrawMode::Viewmodel) {
            check(camera->second->camera->viewmodel.has_value(),"Viewmodel binding requires a Camera viewmodel lens.");
            bool descendant=false;std::string parent=entity.parent;
            while(!parent.empty()) {
                if(parent==value.camera) {descendant=true;break;}
                parent=graph.at(parent)->parent;
            }
            check(descendant,"Viewmodel drawable must be a strict descendant of its Camera.");
        }
    }
}
void select_scene_camera_draw(SceneSnapshot& scene,std::optional<SceneLens> viewmodel_lens) {
    check(!scene.viewmodel,"CameraDraw selector requires a generic unpartitioned snapshot.");
    const bool present=viewmodel_lens.has_value() || std::any_of(scene.objects.begin(),scene.objects.end(),[](const auto& e){return e.camera_draw.has_value();});
    if(!present)return; // No allocations or changes for omitted feature.
    check(identity(scene.camera_id),"CameraDraw selection requires a named canonical Camera.");
    if(viewmodel_lens)lens(*viewmodel_lens);
    std::vector<SceneObject> world;world.reserve(scene.objects.size());
    std::optional<SceneViewmodel> foreground;
    if(viewmodel_lens)foreground.emplace(SceneViewmodel{*viewmodel_lens,{}});
    std::size_t hidden=0;
    for(const auto& source:scene.objects) {
        check(source.camera_visible,"CameraDraw selector requires generic camera visibility.");
        if(source.camera_draw) {
            const auto& value=*source.camera_draw;binding(value);
            if(value.mode==CameraDrawMode::Viewmodel) {
                if(value.camera==scene.camera_id) {
                    check(foreground.has_value(),"Viewmodel selection requires its frozen lens.");
                    check(foreground->objects.size()<32,"Viewmodel selection exceeds 32 drawables.");
                    auto object=source;object.cast_shadows=false;foreground->objects.push_back(std::move(object));
                }
                continue; // Globally excluded from all named world views.
            }
        }
        auto object=source;
        if(object.camera_draw && object.camera_draw->camera==scene.camera_id) {
            check(++hidden<=32,"Hidden selection exceeds 32 drawables.");object.camera_visible=false;
        }
        world.push_back(std::move(object));
    }
    // String/vector allocations and admission have all completed on the
    // detached staging values. These swaps cannot expose a partial partition.
    scene.objects.swap(world);scene.viewmodel.swap(foreground);
}
}
