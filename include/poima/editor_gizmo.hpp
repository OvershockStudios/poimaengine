// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/editor_overlay.hpp"
#include "poima/scene.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace poima {
enum class EditorGizmoMode { move, rotate, scale };
enum class EditorGizmoAxis { x, y, z };
struct EditorGizmoData;
struct EditorGizmoHandle {
    bool visible=false;
    // Physical client pixels. Open axis segment, or closed rotation ring.
    std::vector<std::array<double,2>> screen_points;
};
struct EditorGizmo {
    // Unlit overlay triangles, normalized top-left coordinates. Not depth-tested.
    std::vector<EditorOverlayVertex> triangles;
    std::array<EditorGizmoHandle,3> handles;
    std::shared_ptr<const EditorGizmoData> data;
};
struct EditorGizmoDrag {
    // Treat as opaque state; copying creates an independent drag clock.
    std::shared_ptr<const EditorGizmoData> data;
    EditorGizmoAxis axis=EditorGizmoAxis::x;
    double anchor=0,last_angle=0,angle=0;
};
// Move/scale axes are normalized independently. Rotation rings retain the affine
// relationship between the other two basis columns: X spans Y/Z, Y spans Z/X,
// Z spans X/Y. For local rotation pass parent_world * local_rotation, excluding
// the selected entity's own local scale (the edited transform is R*Q*S).
// Parent-affine/nonorthogonal bases are supported, degenerate planes hidden.
// Camera must be rigid. Invalid
// camera/input, near-plane crossing and unusable projected handles yield an
// empty gizmo. Geometry is bounded (96 segments/ring, three axes).
EditorGizmo make_editor_gizmo(const SceneSnapshot& camera,std::uint32_t width,std::uint32_t height,
    const std::array<double,3>& origin,const std::array<std::array<double,3>,3>& basis,
    EditorGizmoMode mode,std::optional<EditorGizmoAxis> selected={});
std::optional<EditorGizmoAxis> hit_test_editor_gizmo(const EditorGizmo& gizmo,
    double x,double y,double tolerance_pixels=8);
// Freeze gizmo camera/origin/basis at gesture start. Do not regenerate drag state
// from preview transforms. Pointer positions use physical client pixels.
std::optional<EditorGizmoDrag> begin_editor_gizmo_drag(const EditorGizmo& gizmo,
    EditorGizmoAxis axis,double x,double y);
// Cumulative move world distance along the normalized axis; cumulative rotation
// radians; or scale ratio relative to gesture start. Invalid updates return null
// without advancing the drag. Rotation unwrap requires successive angular samples
// less than pi apart. Scale ratios outside [0.001,1000] are rejected, not clamped.
std::optional<double> update_editor_gizmo_drag(EditorGizmoDrag& drag,double x,double y);
}
