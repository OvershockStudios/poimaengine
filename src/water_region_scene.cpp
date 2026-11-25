// SPDX-License-Identifier: Apache-2.0
#include "water_region_scene.hpp"
#include "poima/assets.hpp"
#include "poima/water_region.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace poima::water {
namespace {
struct SurfaceCell {
    std::uint32_t x, z;
    float height, palette_u;
    friend bool operator==(const SurfaceCell&, const SurfaceCell&) = default;
};

static_assert(max_scene_water_wave_layers == max_surface_wave_layers);

SceneWaterRipples project_ripple(const SurfaceRipples& ripple, std::uint64_t tick) {
    constexpr auto tau = 2 * std::numbers::pi;
    const auto heading = ripple.heading_degrees * std::numbers::pi / 180;
    // Reduce the integer tick before conversion so long-lived saves retain
    // the same phase as a short run. Camera and presentation time are irrelevant.
    const auto phase_tick = tick % ripple.period_ticks;
    return {
        {static_cast<float>(std::cos(heading)), static_cast<float>(std::sin(heading))},
        static_cast<float>(ripple.slope),
        static_cast<float>(tau / ripple.wavelength_m),
        static_cast<float>(tau * static_cast<double>(phase_tick) / ripple.period_ticks),
    };
}

// Linear RGB. Cell depth blends between these two swatches in one draw.
std::shared_ptr<const MaterialTextures> depth_palette() {
    static const auto palette = [] {
        auto image = std::make_shared<TextureImage>();
        image->mips.push_back({2, 1, {4, 116, 144, 255, 3, 27, 86, 255}});
        auto textures = std::make_shared<MaterialTextures>();
        textures->maps[0] = {std::move(image), 33071, 33071, 9729, 9729};
        return std::shared_ptr<const MaterialTextures>(std::move(textures));
    }();
    return palette;
}

std::shared_ptr<const MeshAsset> surface_mesh(double cell_size, std::span<const SurfaceCell> cells) {
    struct SurfaceCache {
        double cell_size = 0;
        std::vector<SurfaceCell> cells;
        std::weak_ptr<const MeshAsset> mesh;
    };
    // Reuse unchanged projected geometry without owning another world's mesh
    // or modifying the simulation. Exact comparison makes cross-camera reuse safe.
    thread_local SurfaceCache cache;
    if (cache.cell_size == cell_size && std::ranges::equal(cells, cache.cells)) {
        if (auto existing = cache.mesh.lock()) return existing;
    }

    auto mesh = std::make_shared<MeshAsset>();
    mesh->vertices.reserve(4 * cells.size());
    mesh->indices.reserve(6 * cells.size());
    mesh->has_uv = true;
    mesh->material.metallic = 0.0f;
    mesh->material.roughness = 0.3f;
    for (const auto& cell : cells) {
        const auto x0 = static_cast<float>(cell.x * cell_size);
        const auto x1 = static_cast<float>((cell.x + 1) * cell_size);
        const auto z0 = static_cast<float>(cell.z * cell_size);
        const auto z1 = static_cast<float>((cell.z + 1) * cell_size);
        const std::array<float, 2> uv{cell.palette_u, 0.5f};
        const std::array<float, 3> up{0, 1, 0};
        const std::array<float, 4> tangent{1, 0, 0, 1};
        const auto first = static_cast<std::uint32_t>(mesh->vertices.size());
        mesh->vertices.insert(mesh->vertices.end(), {
            {{x0, cell.height, z0}, up, uv, tangent},
            {{x0, cell.height, z1}, up, uv, tangent},
            {{x1, cell.height, z1}, up, uv, tangent},
            {{x1, cell.height, z0}, up, uv, tangent},
        });
        mesh->indices.insert(mesh->indices.end(),
            {first, first + 1, first + 2, first, first + 2, first + 3});
    }
    cache.cells.assign(cells.begin(), cells.end());
    cache.cell_size = cell_size;
    cache.mesh = mesh;
    return mesh;
}

std::shared_ptr<const MeshAsset> clipped_surface_mesh(const SceneWaterVolume& water) {
    struct SurfaceCache {
        double size=0;
        std::uint32_t width=0;
        std::vector<SceneWaterColumn> cells;
        std::weak_ptr<const MeshAsset> mesh;
    };
    thread_local SurfaceCache cache;
    if (cache.size==water.cell_size_m && cache.width==water.cells_x && cache.cells==water.columns)
        if (auto mesh=cache.mesh.lock()) return mesh;

    auto mesh=std::make_shared<MeshAsset>();
    mesh->has_uv=true;
    mesh->material.metallic=0;
    mesh->material.roughness=.3f;
    // Reversed perimeter order when emitting each fan keeps the upward
    // triangle winding used by the ordinary water surface.
    constexpr std::array<std::array<float,2>,4> perimeter{{{0,0},{1,0},{1,1},{0,1}}};
    const float size=static_cast<float>(water.cell_size_m);
    for (std::uint32_t z=0;z<water.cells_z;++z) for (std::uint32_t x=0;x<water.cells_x;++x) {
        const auto& column=water.columns[z*water.cells_x+x];
        for (std::size_t side=0;side<perimeter.size();++side) {
            if (column.inactive_wedges() & (1u<<side)) continue;
            const auto next=(side+1)%perimeter.size();
            const float stage=column.stages[side];
            const std::array heights{column.floor_m,column.corners[next],column.corners[side]};
            const std::array tops{stage+column.swell_center_m,stage+column.swell_corners[next],
                                  stage+column.swell_corners[side]};
            const std::array depth{tops[0]-heights[0],tops[1]-heights[1],tops[2]-heights[2]};
            if (*std::ranges::max_element(depth)<=0) continue;
            const std::array<std::array<float,2>,3> triangle{{{.5f,.5f},perimeter[next],perimeter[side]}};
            const auto gradient=fitted_water_gradient(column.swell_center_m,column.swell_corners,side,size);
            const auto factor=1/std::sqrt(1+gradient[0]*gradient[0]+gradient[1]*gradient[1]);
            const std::array<float,3> normal{-gradient[0]*factor,factor,-gradient[1]*factor};
            const auto along_x=1/std::sqrt(1+gradient[0]*gradient[0]);
            const std::array<float,4> tangent{along_x,gradient[0]*along_x,0,1};
            const auto first=static_cast<std::uint32_t>(mesh->vertices.size());
            const auto add=[&](float u,float v,float top,float depth_m) {
                const std::array<float,2> uv{.25f+.5f*std::min(depth_m,1.f),.5f};
                mesh->vertices.push_back({{size*(static_cast<float>(x)+u),top,size*(static_cast<float>(z)+v)},
                    normal,uv,tangent});
            };
            for (std::size_t a=0;a<triangle.size();++a) {
                const auto b=(a+1)%triangle.size();
                const float da=depth[a],db=depth[b];
                if (da>=0) add(triangle[a][0],triangle[a][1],tops[a],da);
                if ((da<0 && db>0) || (da>0 && db<0)) {
                    const float t=da/(da-db);
                    add(std::lerp(triangle[a][0],triangle[b][0],t),std::lerp(triangle[a][1],triangle[b][1],t),
                        std::lerp(tops[a],tops[b],t),0);
                }
            }
            const auto end=static_cast<std::uint32_t>(mesh->vertices.size());
            for (auto index=first+1;index+1<end;++index)
                mesh->indices.insert(mesh->indices.end(),{first,index,index+1});
        }
    }
    cache.size=water.cell_size_m;
    cache.width=water.cells_x;
    cache.cells=water.columns;
    cache.mesh=mesh;
    return mesh;
}

void append_water_sheet(const WaterSheetSample& sheet,std::uint64_t tick,SceneWaterVolume& volume,SceneSnapshot& scene) {
    volume.sheets.push_back({sheet.lip_m,sheet.forward_xz,sheet.width_m,sheet.length_m,sheet.drop_per_m2,
        sheet.vertical_depth_m,sheet.launch_speed_m_s,static_cast<double>(tick%960u)/60.,sheet.aeration.value_or(0.)});
    constexpr std::uint32_t segments=12;
    const auto forward=sheet.forward_xz;
    const std::array side{-forward[1],forward[0]};
    auto mesh=std::make_shared<MeshAsset>();
    mesh->vertices.reserve(2*(segments+1));
    mesh->indices.reserve(6*segments);
    mesh->material.metallic=0;
    mesh->material.roughness=.18f;
    mesh->material.double_sided=true;
    for (std::uint32_t i=0;i<=segments;++i) {
        const auto s=sheet.length_m*i/segments;
        const auto height=sheet.vertical_depth_m-sheet.drop_per_m2*s*s;
        const auto slope=2*sheet.drop_per_m2*s;
        const auto inv=1/std::hypot(slope,1.);
        const std::array normal{static_cast<float>(forward[0]*slope*inv),static_cast<float>(inv),
            static_cast<float>(forward[1]*slope*inv)};
        for (const double offset : {-.5*sheet.width_m,.5*sheet.width_m})
            mesh->vertices.push_back({{static_cast<float>(s*forward[0]+offset*side[0]),static_cast<float>(height),
                static_cast<float>(s*forward[1]+offset*side[1])},normal});
        if(i>0) {
            const auto base=(i-1)*2;
            mesh->indices.insert(mesh->indices.end(),{base,base+1,base+2,base+1,base+3,base+2});
        }
    }
    SceneObject object{};
    object.entity_id="water-sheet/"+std::string(sheet.id);
    object.world=identity_matrix();
    for (std::size_t axis=0;axis<3;++axis) object.world[12+axis]=sheet.lip_m[axis];
    object.albedo={.055f,.32f,.42f};
    object.material=mesh->material;
    object.mesh=std::move(mesh);
    object.cast_shadows=false;
    object.stream_geometry=true;
    object.water_region_surface=true;
    scene.objects.push_back(std::move(object));
}
}

void append_region_scene(const Region& region, SceneSnapshot& scene) {
    const auto& grid = region.grid();
    const auto& config = grid.config();
    if (config.cells_x > max_region_side || config.cells_z > max_region_side)
        throw std::invalid_argument("Water region exceeds the scene projection bounds.");

    const auto origin_y = grid.sample({0, 0}).floor_height_m;
    SceneWaterVolume volume;
    if (const auto& optics = region.optics()) {
        volume={.origin_m={config.origin_x_m,origin_y,config.origin_z_m},.cell_size_m=config.cell_size_m,
            .cells_x=config.cells_x,.cells_z=config.cells_z,.optics=*optics,
            .surface_swell=!region.surface_swell().empty(),.surface_wakes=region.wakes().has_value(),
            .swell_variation=region.swell_variation(),.columns={}};
        if (optics->offshore) {
            const auto swell=region.surface_swell();
            for (std::size_t i=0;i<swell.size();++i)
                volume.offshore_swell[i]=project_ripple(swell[i],grid.tick());
        }
        if (optics->shore_foam) {
            constexpr std::uint64_t foam_cycle_ticks = 240;
            volume.shore_foam_phase = static_cast<float>(2 * std::numbers::pi *
                static_cast<double>(grid.tick() % foam_cycle_ticks) / foam_cycle_ticks);
        }
        volume.columns.reserve(static_cast<std::size_t>(config.cells_x)*config.cells_z);
    }
    std::array<SurfaceCell, max_region_side * max_region_side> wet;
    std::size_t count = 0;
    for (std::uint32_t z = 0; z < config.cells_z; ++z) {
        for (std::uint32_t x = 0; x < config.cells_x; ++x) {
            const auto sample = grid.sample({x, z});
            if (region.optics()) {
                auto& column=volume.columns.emplace_back();
                column.floor_m=static_cast<float>(sample.floor_height_m-origin_y);
                column.stage_m=static_cast<float>(sample.surface_height_m-origin_y);
                if (volume.optics.follow_terrain) {
                    const auto fitted=region.fitted_bed({x,z});
                    column.flags=1u|(fitted.inactive_wedges<<1u);
                    column.swell_center_m=static_cast<float>(fitted.swell_center_m);
                    for (std::size_t side=0;side<column.corners.size();++side) {
                        column.corners[side]=static_cast<float>(fitted.corners_m[side]-origin_y);
                        column.stages[side]=static_cast<float>(fitted.stages_m[side]-origin_y);
                        column.swell_corners[side]=static_cast<float>(fitted.swell_corners_m[side]);
                    }
                }
            }
            if (sample.depth_m == 0) continue;
            // Normalize depth over one meter between the color texel centers.
            const auto palette_u = static_cast<float>(0.25 + 0.5 * std::min(sample.depth_m, 1.0));
            wet[count++] = {x, z, static_cast<float>(sample.surface_height_m - origin_y), palette_u};
        }
    }
    if (const auto& storage=region.storage()) for (const auto i : storage->placed()) {
        const auto sample=storage->sample(i);
        const auto top=sample.surface_height_m;
        if (top<=sample.floor_height_m) continue;
        const auto& box=sample.footprint;
        volume.reservoirs.push_back({{box.min_xz_m[0],sample.floor_height_m,box.min_xz_m[1]},
            {box.edge(0),top,box.edge(1)}});
        static const auto patch=surface_mesh(1.,std::array{SurfaceCell{0,0,0,.5f}});
        SceneObject object{};
        object.entity_id="water-reservoir/"+std::string(sample.id);
        object.world=identity_matrix();
        object.world[0]=box.size_xz_m[0];
        object.world[10]=box.size_xz_m[1];
        object.world[12]=box.min_xz_m[0];
        object.world[13]=top;
        object.world[14]=box.min_xz_m[1];
        object.albedo={.016f,.30f,.46f};
        object.mesh=patch;
        object.material=patch->material;
        object.textures=depth_palette();
        object.cast_shadows=false;
        object.stream_geometry=true;
        object.water_region_surface=true;
        scene.objects.push_back(std::move(object));
    }
    if (const auto& storage=region.storage()) {
        for (const auto& sheet : storage->sheets(grid)) append_water_sheet(sheet,grid.tick(),volume,scene);
        for (const auto& foam : storage->foam()) {
            volume.impact_foam.push_back({foam.center_xz_m,foam.forward_xz,foam.footprint.min_xz_m,
                {foam.footprint.edge(0),foam.footprint.edge(1)},foam.radius_m,foam.core,foam.fringe,
                static_cast<double>(grid.tick()%960u)/60.});
        }
    }
    const bool has_storage_media=!volume.reservoirs.empty() || !volume.sheets.empty();
    if (!count && !volume.optics.offshore && has_storage_media) {
        scene.water_volume=std::move(volume);
        return;
    }
    if (!count && !volume.optics.offshore) return;
    auto surface=volume.optics.follow_terrain ? clipped_surface_mesh(volume) :
        surface_mesh(config.cell_size_m,std::span(wet).first(count));
    if (region.optics()) scene.water_volume=std::move(volume);
    if (surface->indices.empty()) return;

    SceneObject object{};
    object.entity_id = "water-region/" + region.plan_sha256();
    object.world = identity_matrix();
    object.world[12] = config.origin_x_m;
    object.world[13] = origin_y;
    object.world[14] = config.origin_z_m;
    object.albedo = {0.016f, 0.30f, 0.46f};
    object.mesh = std::move(surface);
    object.material = object.mesh->material;
    object.textures = depth_palette();
    object.cast_shadows = false;
    object.stream_geometry = true;
    object.water_region_surface = true;
    if (const auto& ripples = region.surface_ripples(); ripples && static_cast<float>(ripples->slope) > 0) {
        scene.water_ripples = project_ripple(*ripples, grid.tick());
        object.water_ripple_surface = true;
    }
    std::size_t layer_count = 0;
    for (const auto& layer : region.surface_wave_layers()) {
        if (static_cast<float>(layer.slope) > 0)
            scene.water_wave_layers[layer_count++] = project_ripple(layer, grid.tick());
    }
    scene.water_wave_layer_count = layer_count;
    object.water_ripple_surface |= layer_count > 0;
    // Leave incarnation zero because a cell's triangles can appear or disappear.
    scene.objects.push_back(std::move(object));
}
}
