// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "render_shadow_bounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace poima::render_shadow_index_ranges {

inline constexpr std::uint32_t minimum_index_count = 4'096 * 3;

struct Range {
    std::uint32_t first = 0, count = 0;
    friend bool operator==(const Range&, const Range&) = default;
};

class Hierarchy {
public:
    explicit Hierarchy(const MeshAsset& mesh) {
        const auto count = mesh.indices.size();
        if (!mesh.influences.empty() || count < minimum_index_count || count > maximum_index_count || count % 3 ||
            mesh.vertices.empty() || mesh.vertices.size() > maximum_vertex_count)
            return;
        if (!std::ranges::all_of(mesh.vertices, [](const MeshVertex& vertex) {
            return std::ranges::all_of(vertex.position, [](float value) {
                return std::isfinite(value) && std::abs(value) <= render_shadow_bounds::coefficient_limit;
            });
        }) || !std::ranges::all_of(mesh.indices, [&](std::uint32_t index) {
            return index < mesh.vertices.size();
        })) return;

        index_count_ = static_cast<std::uint32_t>(count);
        const auto blocks = (index_count_ + block_index_count - 1) / block_index_count;
        nodes_.reserve(2 * blocks - 1);
        build(mesh, 0, blocks);
    }

    bool empty() const noexcept { return nodes_.empty(); }

    // Empty hierarchies leave the full draw to the caller. An empty result
    // from a populated hierarchy means every block was proved outside.
    Range visible_range(const render_shadow_bounds::LocalFrustum& frustum) const {
        if (empty()) return {};
        if (!frustum.usable) return {0, index_count_};
        const auto* first = visible_leaf(0, frustum, false);
        if (!first) return {};
        const auto* last = visible_leaf(0, frustum, true);
        const auto end = last->range.first + last->range.count;
        return {first->range.first, end - first->range.first};
    }

private:
    static constexpr std::uint32_t block_index_count = 1'024 * 3;
    static constexpr std::uint32_t maximum_index_count = 3'000'000;
    static constexpr std::size_t maximum_vertex_count = 1'000'000;

    struct Node {
        Bounds bounds{};
        Range range;
        // Child layout in the tree's preorder storage. Zero denotes a leaf.
        std::size_t right_child = 0;
    };

    std::size_t build(const MeshAsset& mesh, std::uint32_t first, std::uint32_t end) {
        const auto here = nodes_.size();
        nodes_.emplace_back();
        if (end - first == 1) {
            auto& leaf = nodes_[here];
            const auto begin = first * block_index_count;
            const auto stop = std::min(end * block_index_count, index_count_);
            leaf.range = {begin, stop - begin};
            const auto& origin = mesh.vertices[mesh.indices[begin]].position;
            for (std::size_t axis = 0; axis < 3; ++axis)
                leaf.bounds.minimum[axis] = leaf.bounds.maximum[axis] = origin[axis];
            for (auto i = begin + 1; i < stop; ++i) {
                const auto& point = mesh.vertices[mesh.indices[i]].position;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const double coordinate = point[axis];
                    leaf.bounds.minimum[axis] = std::min(leaf.bounds.minimum[axis], coordinate);
                    leaf.bounds.maximum[axis] = std::max(leaf.bounds.maximum[axis], coordinate);
                }
            }
        } else {
            const auto middle = first + (end - first) / 2;
            const auto left = build(mesh, first, middle);
            const auto right = build(mesh, middle, end);
            auto& node = nodes_[here];
            node.right_child = right;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                node.bounds.minimum[axis] = std::min(nodes_[left].bounds.minimum[axis], nodes_[right].bounds.minimum[axis]);
                node.bounds.maximum[axis] = std::max(nodes_[left].bounds.maximum[axis], nodes_[right].bounds.maximum[axis]);
            }
        }
        return here;
    }

    const Node* visible_leaf(std::size_t index, const render_shadow_bounds::LocalFrustum& frustum,
                             bool from_end) const {
        const auto& node = nodes_[index];
        if (!render_shadow_bounds::intersects(node.bounds, frustum)) return nullptr;
        if (!node.right_child) return &node;
        const auto left = index + 1;
        const auto primary = from_end ? node.right_child : left;
        const auto secondary = from_end ? left : node.right_child;
        if (const auto* leaf = visible_leaf(primary, frustum, from_end)) return leaf;
        return visible_leaf(secondary, frustum, from_end);
    }

    std::vector<Node> nodes_;
    std::uint32_t index_count_ = 0;
};

} // namespace poima::render_shadow_index_ranges
