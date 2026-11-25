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
            .cells_x=config.cells_x,.cells_z=config.cells_z,.optics=*optics,.columns={}};
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
            if (region.optics())
                volume.columns.push_back({static_cast<float>(sample.floor_height_m-origin_y),
                    static_cast<float>(sample.surface_height_m-origin_y)});
            if (sample.depth_m == 0) continue;
            // Normalize depth over one meter between the color texel centers.
            const auto palette_u = static_cast<float>(0.25 + 0.5 * std::min(sample.depth_m, 1.0));
            wet[count++] = {x, z, static_cast<float>(sample.surface_height_m - origin_y), palette_u};
        }
    }
    if (!count) return;
    if (region.optics()) scene.water_volume=std::move(volume);

    SceneObject object{};
    object.entity_id = "water-region/" + region.plan_sha256();
    object.world = identity_matrix();
    object.world[12] = config.origin_x_m;
    object.world[13] = origin_y;
    object.world[14] = config.origin_z_m;
    object.albedo = {0.016f, 0.30f, 0.46f};
    object.mesh = surface_mesh(config.cell_size_m, std::span(wet).first(count));
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
