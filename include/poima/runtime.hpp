// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include <memory>
#include <optional>

namespace poima {
struct RuntimeTransform {
    std::array<double,3> position{0,0,0};
    std::array<double,4> rotation{0,0,0,1};
    std::array<double,3> scale{1,1,1};
};
struct BoxCollider {
    std::array<float,3> half_extents{0.5f,0.5f,0.5f};
    bool dynamic = false;
    float mass = 1, friction = 0.5f, restitution = 0;
};
struct CharacterController {
    float radius = 0.3f, height = 1.8f, speed = 4, jump_speed = 5;
    std::string camera;
};
struct RuntimeCamera { double vertical_fov=60, near_plane=0.1, far_plane=1000; };
struct RuntimeMesh {
    std::array<float,3> albedo{}; bool visible=true;
    std::shared_ptr<const MeshAsset> mesh;
    std::optional<PbrMaterial> material;
    std::shared_ptr<const MaterialTextures> textures;
};
struct RuntimeEntityDefinition {
    std::string id, parent;
    RuntimeTransform transform;
    std::optional<BoxCollider> collider;
    std::optional<CharacterController> character;
    std::optional<RuntimeCamera> camera;
    std::optional<RuntimeMesh> mesh;
    std::optional<Light> light;
    std::optional<LightingEnvironment> environment;
};
struct RuntimeDefinition {
    std::string world_id;
    std::uint64_t authored_revision=0;
    std::vector<RuntimeEntityDefinition> entities;
};
struct RuntimeInput {
    std::string entity;
    std::array<float,2> move{0,0}; // right, forward; diagonal magnitude clamped to one
    std::array<float,2> look{0,0}; // yaw-left, pitch-up degrees; applied on first tick
    bool jump=false;              // rising action on first tick only
};
struct RuntimeEntityState {
    std::string id;
    Matrix4 world;
    std::array<double,3> velocity{};
    bool has_body=false, is_character=false;
    std::string ground;
    double yaw=0, pitch=0;
};
struct RuntimeSummary {
    std::uint64_t tick=0;
    std::size_t entities=0, bodies=0, characters=0;
};
// One single-threaded world, no GUI/graphics dependency. Structural definition
// is immutable while running; presentation snapshots own copies of their data.
class Runtime {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    static constexpr double fixed_dt=1.0/60.0;
    static bool available();
    explicit Runtime(const RuntimeDefinition& definition);
    ~Runtime();
    Runtime(const Runtime&)=delete;
    Runtime& operator=(const Runtime&)=delete;
    RuntimeSummary inspect() const;
    RuntimeEntityState entity(const std::string& id) const;
    void step(std::uint32_t ticks, const std::vector<RuntimeInput>& inputs);
    SceneLighting lighting() const;
    SceneSnapshot snapshot(const std::string& camera) const;
};
}
