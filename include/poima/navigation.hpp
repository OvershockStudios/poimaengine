// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace poima::navigation {
inline constexpr std::size_t max_triangles=250000,max_vertices=750000,max_cells=4000000;
inline constexpr std::size_t max_package_bytes=16*1024*1024,max_bake_memory=512*1024*1024,max_detour_memory=32*1024*1024;
// Detour adds one to neighbor indices and reserves bit 15 for external links.
inline constexpr int max_mesh_polygons=0x7fff;
using Point=std::array<float,3>;
struct Profile {
    double radius=.3,height=1.8,climb=.3,slope=45,cell_size=.15,cell_height=.1;
};
struct Geometry {
    std::vector<Point> vertices;
    std::vector<int> triangles;
};
struct PathRequest {
    Point start{},end{},extents{2,4,2};
    std::uint32_t max_polygons=256,max_corners=256,max_nodes=4096;
};
struct Path {
    std::string status;
    Point requested_start{},requested_end{},projected_start{},projected_end{},reachable_end{};
    double start_distance=0,end_distance=0;
    std::vector<Point> corners;
    std::uint32_t polygons=0;
    bool start_found=false,end_found=false;
};
bool available() noexcept;
std::string profile_json(const Profile&);
Profile parse_profile(const std::string&);
// Immutable one-tile static mesh. Calls allocate bounded independent query
// scratch; there is no shared path cursor or mutable NPC decision state.
class Mesh {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Mesh(std::unique_ptr<Impl>);
public:
    ~Mesh();
    Mesh(const Mesh&)=delete;
    Mesh& operator=(const Mesh&)=delete;
    static std::shared_ptr<const Mesh> bake(const Geometry&,const Profile&,const std::string& source_fingerprint,std::size_t allocation_limit=max_bake_memory);
    static std::shared_ptr<const Mesh> decode(const std::string& package,std::size_t allocation_limit=max_detour_memory);
    std::string encode() const;
    std::string metadata() const;
    std::string source_fingerprint() const;
    Path path(const PathRequest&,std::size_t allocation_limit=max_detour_memory) const;
};
}
