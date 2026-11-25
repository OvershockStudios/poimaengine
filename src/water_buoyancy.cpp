// SPDX-License-Identifier: Apache-2.0
#include "poima/water_region.hpp"

#include <cmath>
#include <span>

namespace poima::water {
namespace {

using Vec = std::array<double,3>;

Vec add(Vec a,Vec b) { return {a[0]+b[0],a[1]+b[1],a[2]+b[2]}; }
Vec sub(Vec a,Vec b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
Vec mul(Vec a,double k) { return {k*a[0],k*a[1],k*a[2]}; }
double dot(Vec a,Vec b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Vec cross(Vec a,Vec b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}

struct Plane { Vec normal; double limit; };

// A box starts with six faces. Each water prism adds at most six planes,
// and each face can have at most one edge for every other face.
constexpr std::size_t max_faces=12;
struct Face {
    std::array<Vec,max_faces> vertices{};
    std::size_t count=0;
    std::span<const Vec> points() const { return {vertices.data(),count}; }
    void push(Vec point) { vertices.at(count++)=point; }
};
struct Hull {
    std::array<Face,max_faces> faces;
    std::size_t count=0;
};

Hull box(const std::array<Vec,3>& axes) {
    const auto& [x,y,z]=axes;
    const auto a=mul(add(add(x,y),z),-1.);
    const std::array vertices{a,add(a,mul(x,2)),add(a,mul(y,2)),add(a,mul(add(x,y),2)),
        add(a,mul(z,2)),add(a,mul(add(x,z),2)),add(a,mul(add(y,z),2)),mul(a,-1)};
    constexpr std::array<std::array<std::size_t,4>,6> indices{{
        {0,2,3,1},{4,5,7,6},{0,1,5,4},{2,6,7,3},{0,4,6,2},{1,3,7,5}}};
    Hull result{};
    for (const auto& face : indices) {
        auto& polygon=result.faces[result.count++];
        for (const auto index : face) polygon.push(vertices[index]);
    }
    return result;
}

bool clip(Hull& hull,Plane plane) {
    Hull output{};
    Face cap;
    const auto remember=[&](Vec point) {
        const auto same=[&](Vec previous) {
            const auto offset=sub(previous,point);
            return std::max({std::abs(offset[0]),std::abs(offset[1]),std::abs(offset[2])})<1e-9;
        };
        if (!std::ranges::any_of(cap.points(),same)) cap.push(point);
    };
    bool cut=false;
    for (std::size_t i=0;i<hull.count;++i) {
        const auto points=hull.faces[i].points();
        Face polygon;
        auto start=points.back();
        auto start_distance=dot(plane.normal,start)-plane.limit;
        for (auto end : points) {
            const auto end_distance=dot(plane.normal,end)-plane.limit;
            if ((start_distance<0 && end_distance>0) || (start_distance>0 && end_distance<0)) {
                const auto edge=add(start,mul(sub(end,start),start_distance/(start_distance-end_distance)));
                polygon.push(edge);
                remember(edge);
            }
            if (end_distance<=0) polygon.push(end);
            if (end_distance==0) remember(end);
            cut|=end_distance>0;
            start=end;
            start_distance=end_distance;
        }
        if (polygon.count>=3) output.faces.at(output.count++)=polygon;
    }
    if (!cut) return true;
    if (output.count<3 || cap.count<3) return false;
    Vec center{};
    for (const auto vertex : cap.points()) center=add(center,vertex);
    center=mul(center,1./cap.count);
    const auto normal=plane.normal;
    const Vec seed=std::abs(normal[0])>std::hypot(normal[1],normal[2]) ? Vec{0,1,0} : Vec{1,0,0};
    const auto tangent=cross(seed,plane.normal);
    const auto bitangent=cross(plane.normal,tangent);
    struct Turn { double angle; Vec point; };
    std::array<Turn,max_faces> ordered;
    for (std::size_t i=0;i<cap.count;++i) {
        const auto offset=sub(cap.vertices[i],center);
        ordered[i]={std::atan2(dot(offset,bitangent),dot(offset,tangent)),cap.vertices[i]};
    }
    std::sort(ordered.begin(),ordered.begin()+static_cast<std::ptrdiff_t>(cap.count),
        [](const Turn& a,const Turn& b){return a.angle<b.angle;});
    for (std::size_t i=0;i<cap.count;++i) cap.vertices[i]=ordered[i].point;
    output.faces.at(output.count++)=cap;
    hull=output;
    return true;
}

struct VolumeMoment {
    double volume=0;
    Vec moment{};

    void include(const Hull& box,std::span<const Plane> planes,VolumeMoment* alongside=nullptr) {
        auto polyhedron=box;
        for (const auto plane : planes) if (!clip(polyhedron,plane)) return;

        // Keep the reference point inside the clipped solid. The resulting
        // tetrahedra have positive volumes even near a very shallow edge.
        Vec interior{};
        std::size_t vertices=0;
        for (std::size_t i=0;i<polyhedron.count;++i)
            for (const auto vertex : polyhedron.faces[i].points()) {
                interior=add(interior,vertex);
                ++vertices;
            }
        interior=mul(interior,1./vertices);
        for (std::size_t i=0;i<polyhedron.count;++i) {
            const auto points=polyhedron.faces[i].points();
            const auto a=sub(points.front(),interior);
            for (std::size_t j=1;j+1<points.size();++j) {
                const auto b=sub(points[j],interior),c=sub(points[j+1],interior);
                const auto tetra=std::abs(dot(a,cross(b,c)))/6;
                const auto weighted=mul(add(interior,mul(add(add(a,b),c),.25)),tetra);
                volume+=tetra;
                moment=add(moment,weighted);
                if (alongside) {
                    alongside->volume+=tetra;
                    alongside->moment=add(alongside->moment,weighted);
                }
            }
        }
    }
};

Plane inside_edge(Vec from,Vec to) {
    const auto side=sub(to,from);
    const Vec outward{side[2],0,-side[0]};
    return {outward,dot(outward,from)};
}

}

Displacement Region::displaced_by_box(const Vec& center,const std::array<Vec,3>& axes) const {
    // All clipping is relative to the body center, even for distant tiles.
    const auto finite=[](Vec v) { return std::ranges::all_of(v,[](double n){return std::isfinite(n);}); };
    if (!finite(center) || !std::ranges::all_of(axes,finite)) return {};
    const auto extent=[&](std::size_t i) {
        return std::abs(axes[0][i])+std::abs(axes[1][i])+std::abs(axes[2][i]);
    };
    const Vec radius{extent(0),extent(1),extent(2)};
    const auto& config=grid_.config();
    const auto size=config.cell_size_m;
    const auto x=config.origin_x_m-center[0],z=config.origin_z_m-center[2];
    const auto x_end=x+size*config.cells_x,z_end=z+size*config.cells_z;
    const auto initial=box(axes);
    VolumeMoment total;
    Vec current_moment{};
    if (optics_ && optics_->offshore) {
        const auto& sea=*optics_->offshore;
        const auto stage=sea.surface_height_m-center[1];
        const std::array edge_planes{Plane{{0,0,1},z},Plane{{-1,0,0},-x_end},
            Plane{{0,0,-1},-z_end},Plane{{1,0,0},x}};
        const auto edge=edge_planes.at(static_cast<std::size_t>(sea.edge));
        const auto reach=std::abs(edge.normal[0])*radius[0]+std::abs(edge.normal[2])*radius[2];
        if (stage>-radius[1] && stage-sea.depth_m<radius[1] && edge.limit>-reach) {
            const std::array planes{edge,Plane{{0,1,0},stage},Plane{{0,-1,0},sea.depth_m-stage}};
            total.include(initial,planes);
        }
    }
    if (x<radius[0] && x_end>-radius[0] && z<radius[2] && z_end>-radius[2]) {
        const auto index=[&](double offset,std::uint32_t count) {
            return static_cast<std::uint32_t>(std::clamp(std::floor(offset/size),0.,double(count-1)));
        };
        for (auto iz=index(-radius[2]-z,config.cells_z);iz<=index(radius[2]-z,config.cells_z);++iz) {
            for (auto ix=index(-radius[0]-x,config.cells_x);ix<=index(radius[0]-x,config.cells_x);++ix) {
                const auto left=x+size*ix,near=z+size*iz;
                const Cell cell{ix,iz};
                VolumeMoment per_cell;
                auto* tracking=currents_ ? &per_cell : nullptr;
                if (shore_corner_heights_m_.empty()) {
                    const auto sample=grid_.sample(cell);
                    const auto bottom=sample.floor_height_m-center[1],top=sample.surface_height_m-center[1];
                    if (sample.depth_m<=0 || top<=-radius[1] || bottom>=radius[1]) continue;
                    const std::array planes{
                        Plane{{-1,0,0},-left},Plane{{1,0,0},left+size},
                        Plane{{0,0,-1},-near},Plane{{0,0,1},near+size},
                        Plane{{0,-1,0},-bottom},Plane{{0,1,0},top}};
                    total.include(initial,planes,tracking);
                } else {
                    const auto bed=fitted_bed(cell);
                    const Vec midpoint{left+.5*size,0,near+.5*size};
                    const std::array<Vec,4> corners{{{left,0,near},{left+size,0,near},
                        {left+size,0,near+size},{left,0,near+size}}};
                    for (std::size_t side=0;side<corners.size();++side) {
                        if (bed.inactive_wedges & (1u<<side)) continue;
                        const auto next=(side+1)%corners.size();
                        const auto top=bed.stages_m[side]-center[1];
                        if (top+std::max({bed.swell_center_m,bed.swell_corners_m[side],bed.swell_corners_m[next]})<=-radius[1] ||
                            std::min({bed.center_m,bed.corners_m[side],bed.corners_m[next]})-center[1]>=radius[1]) continue;
                        const auto top_gradient=fitted_water_gradient(bed.swell_center_m,bed.swell_corners_m,side,size);
                        const auto bed_gradient=fitted_water_gradient(bed.center_m,bed.corners_m,side,size);
                        const Vec up{-top_gradient[0],1,-top_gradient[1]},down{bed_gradient[0],-1,bed_gradient[1]};
                        const std::array planes{inside_edge(corners[side],corners[next]),
                            inside_edge(corners[next],midpoint),inside_edge(midpoint,corners[side]),
                            Plane{up,dot(up,midpoint)+top+bed.swell_center_m},
                            Plane{down,dot(down,midpoint)+center[1]-bed.center_m}};
                        total.include(initial,planes,tracking);
                    }
                }
                if (per_cell.volume>0) {
                    const auto centroid=add(center,mul(per_cell.moment,1./per_cell.volume));
                    // Within this cell the velocity field is affine, so its
                    // submerged-volume average is the value at the centroid.
                    const auto velocity=current_at(cell,centroid[0],centroid[2]);
                    current_moment=add(current_moment,mul(velocity,per_cell.volume));
                }
            }
        }
    }
    if (total.volume<=0) return {};
    return {total.volume,mul(total.moment,1./total.volume),mul(current_moment,1./total.volume)};
}

} // namespace poima::water
