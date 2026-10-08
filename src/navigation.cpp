// SPDX-License-Identifier: Apache-2.0
#include "poima/navigation.hpp"
#include "navigation_memory.hpp"
#include <atomic>
#include <Recast.h>
#include <RecastAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourAlloc.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>

namespace poima::navigation {
namespace {
using Json=nlohmann::json;
constexpr const char* upstream="6dc1667f580357e8a2154c28b7867bea7e8ad3a7";
void check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
Json parse_package(const std::string& text) {
    std::vector<std::set<std::string>> seen;
    return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value) {
        check(depth<=16,"Navigation JSON nesting exceeds 16 levels.");
        if(event==Json::parse_event_t::object_start)seen.emplace_back();
        if(event==Json::parse_event_t::key)check(seen.back().insert(value.get<std::string>()).second,"Duplicate navigation JSON field.");
        if(event==Json::parse_event_t::object_end)seen.pop_back();
        return true;
    });
}
void keys(const Json& value,std::initializer_list<const char*> allowed) {
    check(value.is_object(),"Navigation value must be an object.");
    for(const auto& [key,unused]:value.items())check(std::any_of(allowed.begin(),allowed.end(),[&](auto k){return key==k;}),"Unknown navigation field.");
}
double number(const Json& value,double lo,double hi) {
    check(value.is_number() && !value.is_boolean(),"Navigation number has the wrong type.");
    const auto v=value.get<double>();check(std::isfinite(v) && v>=lo && v<=hi,"Navigation number exceeds bounds.");return v;
}
void validate(const Profile& p) {
    for(double v:{p.radius,p.height,p.climb,p.slope,p.cell_size,p.cell_height})check(std::isfinite(v),"Nonfinite navigation profile.");
    check(p.radius>=.05 && p.radius<=2 && p.height>2*p.radius && p.height<=4,"Invalid navigation capsule dimensions.");
    check(p.climb>=0 && p.climb<=2 && p.climb<=p.height && p.slope>=0 && p.slope<=60,"Invalid navigation slope/climb.");
    check(p.cell_size>=.025 && p.cell_size<=1 && p.cell_height>=.025 && p.cell_height<=.5,"Invalid navigation voxel dimensions.");
}
Json profile(const Profile& p) {validate(p);return {{"radius",p.radius},{"height",p.height},{"climb",p.climb},{"slope",p.slope},{"cell_size",p.cell_size},{"cell_height",p.cell_height}};}
bool hash(const std::string& value) {return value.size()==64 && std::all_of(value.begin(),value.end(),[](char c){return (c>='0' && c<='9')||(c>='a' && c<='f');});}
// Recast's vectors assume allocations succeed. Throwing from the allocator
// unwinds our RAII objects safely instead of returning null to those vectors.
// The hooks are installed once for this private linked dependency. Per-bake
// counters are thread-local; no background work or global timer is introduced.
struct Allocation;
struct Budget {std::size_t used=0,limit=max_bake_memory,peak=0,attempts=0;Allocation* head=nullptr;};
thread_local Budget* active_budget=nullptr;
thread_local std::size_t last_peak=0,last_attempts=0;
std::atomic<std::size_t> live_allocations{0};
struct alignas(std::max_align_t) Allocation {std::size_t bytes;Budget* budget;Allocation* previous;Allocation* next;};
void* allocate(std::size_t bytes,rcAllocHint) {
    if(active_budget)++active_budget->attempts;
    const auto limit=active_budget ? active_budget->limit : max_bake_memory;
    check(bytes<=limit && (!active_budget || active_budget->used<=limit-bytes),"Navigation Recast allocation budget exceeded.");
    auto* header=static_cast<Allocation*>(std::malloc(sizeof(Allocation)+bytes));if(!header)throw std::bad_alloc();
    header->bytes=bytes;header->budget=active_budget;header->previous=nullptr;header->next=nullptr;
    if(active_budget) {
        active_budget->used+=bytes;active_budget->peak=std::max(active_budget->peak,active_budget->used);header->next=active_budget->head;
        if(header->next)header->next->previous=header;
        active_budget->head=header;
    }
    ++live_allocations;return header+1;
}
void release(void* pointer) {
    if(!pointer)return;
    auto* header=static_cast<Allocation*>(pointer)-1;
    if(header->budget) {
        header->budget->used-=header->bytes;
        if(header->previous)header->previous->next=header->next;else header->budget->head=header->next;
        if(header->next)header->next->previous=header->previous;
    }
    --live_allocations;std::free(header);
}
struct BudgetScope {
    Budget value;Budget* previous;
    explicit BudgetScope(std::size_t limit):previous(active_budget){value.limit=limit;active_budget=&value;}
    ~BudgetScope(){
        // Upstream stages also allocate temporary arrays with manual frees.
        // Their normal frees unlink entries; leftovers on exceptional unwind
        // are reclaimed after our rc object owners have already destructed.
        while(value.head)release(value.head+1);
        last_peak=value.peak;last_attempts=value.attempts;active_budget=previous;
    }
};
// Detour's placement-new pool constructors also assume allocations succeed.
// Temporary constructor/manual allocations are tracked separately. Successful
// mesh allocations escape only after their stack-budget pointers are detached.
struct DtAllocation;
struct DtBudget {std::size_t used=0,limit=max_detour_memory,peak=0,attempts=0,fail_at=0;DtAllocation* head=nullptr;};
thread_local DtBudget* active_dt=nullptr;
thread_local std::size_t last_dt_peak=0,last_dt_attempts=0,next_dt_failure=0;
std::atomic<std::size_t> live_dt_allocations{0};
struct alignas(std::max_align_t) DtAllocation {std::size_t bytes;DtBudget* budget;DtAllocation* previous;DtAllocation* next;};
void* dt_allocate(std::size_t bytes,dtAllocHint) {
    if(active_dt) {
        ++active_dt->attempts;
        check(active_dt->attempts!=active_dt->fail_at,"Injected navigation Detour allocation failure.");
    }
    const auto limit=active_dt ? active_dt->limit : max_detour_memory;
    check(bytes<=limit && (!active_dt || active_dt->used<=limit-bytes),"Navigation Detour allocation budget exceeded.");
    auto* header=static_cast<DtAllocation*>(std::malloc(sizeof(DtAllocation)+bytes));if(!header)throw std::bad_alloc();
    header->bytes=bytes;header->budget=active_dt;header->previous=nullptr;header->next=nullptr;
    if(active_dt) {
        active_dt->used+=bytes;active_dt->peak=std::max(active_dt->peak,active_dt->used);header->next=active_dt->head;
        if(header->next)header->next->previous=header;
        active_dt->head=header;
    }
    // dtNavMesh::init publishes its tiles pointer before later allocations.
    // Once tiles exist, zero records make later-init destructor inspection safe.
    std::memset(header+1,0,bytes);
    ++live_dt_allocations;return header+1;
}
void dt_release(void* pointer) {
    if(!pointer)return;
    auto* header=static_cast<DtAllocation*>(pointer)-1;
    if(header->budget) {
        header->budget->used-=header->bytes;
        if(header->previous)header->previous->next=header->next;else header->budget->head=header->next;
        if(header->next)header->next->previous=header->previous;
    }
    --live_dt_allocations;std::free(header);
}
struct DetourScope {
    DtBudget value;DtBudget* previous;
    explicit DetourScope(std::size_t limit):previous(active_dt) {
        check(limit>=1 && limit<=max_detour_memory,"Invalid navigation Detour allocation limit.");
        static std::once_flag hooks;std::call_once(hooks,[]{dtAllocSetCustom(&dt_allocate,&dt_release);});
        value.limit=limit;value.fail_at=next_dt_failure;next_dt_failure=0;active_dt=&value;
    }
    void detach_owned() noexcept {
        while(value.head) {
            auto* header=value.head;value.head=header->next;
            header->budget=nullptr;header->previous=nullptr;header->next=nullptr;
        }
        value.used=0;
    }
    ~DetourScope() {
        while(value.head)dt_release(value.head+1);
        last_dt_peak=value.peak;last_dt_attempts=value.attempts;active_dt=previous;
    }
};
struct NavFree {void operator()(dtNavMesh* value) const {dtFreeNavMesh(value);}};
struct QueryFree {void operator()(dtNavMeshQuery* value) const {dtFreeNavMeshQuery(value);}};
template<class T>std::vector<T> integers(const Json& value,std::size_t limit,std::uint64_t maximum) {
    check(value.is_array() && value.size()<=limit,"Navigation integer array exceeds bounds.");std::vector<T> result;result.reserve(value.size());
    for(const auto& v:value) {check(v.is_number_integer() && !v.is_boolean() && (!v.is_number_unsigned() ? v.get<std::int64_t>()>=0 : true),"Invalid navigation integer array.");const auto n=v.get<std::uint64_t>();check(n<=maximum,"Navigation integer exceeds bounds.");result.push_back(static_cast<T>(n));}return result;
}
std::vector<float> floats(const Json& value,std::size_t limit) {
    check(value.is_array() && value.size()<=limit,"Navigation float array exceeds bounds.");std::vector<float> result;result.reserve(value.size());
    for(const auto& v:value)result.push_back(static_cast<float>(number(v,-1000000,1000000)));
    return result;
}
Point point(const Json& value) {auto data=floats(value,3);check(data.size()==3,"Navigation point needs three coordinates.");return {data[0],data[1],data[2]};}
struct Tile {
    std::vector<unsigned short> vertices,polygons;
    std::vector<unsigned int> detail_meshes;
    std::vector<float> detail_vertices;
    std::vector<unsigned char> detail_triangles;
    Point minimum{},maximum{};
};
std::unique_ptr<dtNavMesh,NavFree> build_nav(const Tile& t,const Profile& p,std::size_t allocation_limit=max_detour_memory) {
    DetourScope allocation(allocation_limit);
    check(t.vertices.size()%3==0 && t.vertices.size()>=9 && t.vertices.size()/3<65535,"Invalid navigation vertex count.");
    check(t.polygons.size()%12==0 && !t.polygons.empty() && t.polygons.size()/12<=max_mesh_polygons,"Invalid navigation polygon count.");
    const auto count=t.polygons.size()/12;
    check(t.detail_meshes.size()==count*4 && t.detail_vertices.size()%3==0 && t.detail_triangles.size()%4==0,"Invalid navigation detail counts.");
    for(std::size_t k=0;k<3;++k)check(std::isfinite(t.minimum[k]) && std::isfinite(t.maximum[k]) && t.minimum[k]<=t.maximum[k],"Invalid navigation bounds.");
    const auto cs=static_cast<float>(p.cell_size),ch=static_cast<float>(p.cell_height);
    const auto width=std::ceil((double(t.maximum[0])-t.minimum[0])/cs),depth=std::ceil((double(t.maximum[2])-t.minimum[2])/cs);
    check(width>=1 && depth>=1 && width*depth<=max_cells && (double(t.maximum[1])-t.minimum[1])/ch<=1024.01,"Navigation tile voxel dimensions exceed bounds.");
    for(std::size_t i=0;i<t.vertices.size();i+=3)for(std::size_t k=0;k<3;++k) {
        const auto spacing=k==1 ? ch:cs;const auto extent=(double(t.maximum[k])-t.minimum[k])/spacing;
        check(t.vertices[i+k]<=std::ceil(extent)+1,"Navigation quantized vertex exceeds tile bounds.");
    }
    for(std::size_t i=0;i<t.detail_vertices.size();++i) {
        const auto k=i%3;const auto v=t.detail_vertices[i];
        check(std::isfinite(v) && v>=t.minimum[k]-ch*3 && v<=t.maximum[k]+ch*3,"Navigation detail vertex exceeds tile bounds.");
    }
    std::size_t next_vertex=0,next_triangle=0;
    for(std::size_t i=0;i<count;++i) {
        auto* poly=t.polygons.data()+i*12;std::size_t nv=0;
        for(;nv<6 && poly[nv]!=RC_MESH_NULL_IDX;++nv)check(poly[nv]<t.vertices.size()/3,"Navigation polygon references absent vertex.");
        check(nv>=3,"Navigation polygon needs three vertices.");
        std::set<unsigned short> unique;double winding=0;
        for(std::size_t j=0;j<nv;++j) {
            check(unique.insert(poly[j]).second,"Navigation polygon repeats a vertex.");
            auto* a=t.vertices.data()+poly[j]*3;auto* b=t.vertices.data()+poly[(j+1)%nv]*3;auto* c=t.vertices.data()+poly[(j+2)%nv]*3;
            const auto cross=(double(b[0])-a[0])*(double(c[2])-b[2])-(double(b[2])-a[2])*(double(c[0])-b[0]);
            if(cross!=0){check(winding==0 || winding*cross>0,"Navigation polygon is not convex.");winding=cross;}
        }
        check(winding!=0,"Navigation polygon has zero XZ area.");
        for(std::size_t j=nv;j<6;++j)check(poly[j]==RC_MESH_NULL_IDX,"Navigation polygon has noncontiguous vertices.");
        for(std::size_t j=0;j<nv;++j)check(poly[6+j]==RC_MESH_NULL_IDX || poly[6+j]<count,"Navigation neighbor is out of bounds; external links are unsupported.");
        for(std::size_t j=0;j<nv;++j)if(poly[6+j]!=RC_MESH_NULL_IDX) {
            const auto neighbor=poly[6+j];check(neighbor!=i,"Navigation polygon links itself.");auto* other=t.polygons.data()+neighbor*12;bool match=false;
            for(std::size_t k=0;k<6 && other[k]!=RC_MESH_NULL_IDX;++k) {
                const auto end=k+1<6 && other[k+1]!=RC_MESH_NULL_IDX ? k+1:0;
                if(other[k]==poly[(j+1)%nv] && other[end]==poly[j] && other[6+k]==i)match=true;
            }
            check(match,"Navigation neighbor does not share a reciprocal edge.");
        }
        auto* d=t.detail_meshes.data()+i*4;
        check(d[0]==next_vertex && d[2]==next_triangle && d[1]>=nv && d[1]<=255 && d[3]>=1 && d[3]<=255,"Invalid navigation detail ranges.");
        check(std::size_t(d[0])+d[1]<=t.detail_vertices.size()/3 && std::size_t(d[2])+d[3]<=t.detail_triangles.size()/4,"Navigation detail range is outside its array.");
        for(std::size_t j=d[2];j<std::size_t(d[2])+d[3];++j) {
            auto* tri=t.detail_triangles.data()+j*4;check(tri[0]<d[1] && tri[1]<d[1] && tri[2]<d[1] && (tri[3]&0xc0)==0,"Invalid navigation detail triangle.");
        }
        next_vertex+=d[1];next_triangle+=d[3];
    }
    check(next_vertex==t.detail_vertices.size()/3 && next_triangle==t.detail_triangles.size()/4,"Unreferenced navigation detail storage.");
    std::vector<unsigned short> flags(count,1);std::vector<unsigned char> areas(count,0);
    dtNavMeshCreateParams params{};params.verts=t.vertices.data();params.vertCount=static_cast<int>(t.vertices.size()/3);
    params.polys=t.polygons.data();params.polyFlags=flags.data();params.polyAreas=areas.data();params.polyCount=static_cast<int>(count);params.nvp=6;
    params.detailMeshes=t.detail_meshes.data();params.detailVerts=t.detail_vertices.data();params.detailVertsCount=static_cast<int>(t.detail_vertices.size()/3);
    params.detailTris=t.detail_triangles.data();params.detailTriCount=static_cast<int>(t.detail_triangles.size()/4);
    std::copy(t.minimum.begin(),t.minimum.end(),params.bmin);std::copy(t.maximum.begin(),t.maximum.end(),params.bmax);
    params.cs=static_cast<float>(p.cell_size);params.ch=static_cast<float>(p.cell_height);
    params.walkableHeight=static_cast<float>(p.height);params.walkableRadius=static_cast<float>(p.radius);params.walkableClimb=static_cast<float>(p.climb);// The pinned BV builder dereferences a failed temporary allocation.
    // Keep endpoint lookup bounded and linear until a separately reviewed
    // BV-tree builder can offer the same safe allocation behavior.
    params.buildBvTree=false;
    unsigned char* data=nullptr;int bytes=0;check(dtCreateNavMeshData(&params,&data,&bytes),"Cannot build checked Detour navigation data.");
    std::unique_ptr<unsigned char,decltype(&dtFree)> owner(data,&dtFree);
    check(bytes>0 && static_cast<std::size_t>(bytes)<=max_package_bytes,"Detour tile exceeds 16 MiB.");
    // init sets maxTiles before allocating tiles; an allocation failure can
    // leave its destructor unable to run. Keep this POD/pointer-only object
    // scope-owned until init completes; the tracked scope frees every raw
    // allocation on failure without invoking the incomplete destructor.
    auto* raw_nav=dtAllocNavMesh();check(raw_nav!=nullptr,"Cannot allocate Detour mesh.");
    check(dtStatusSucceed(raw_nav->init(data,bytes,DT_TILE_FREE_DATA)),"Cannot initialize Detour mesh.");
    std::unique_ptr<dtNavMesh,NavFree> nav(raw_nav);owner.release();allocation.detach_owned();return nav;
}
double distance(Point a,Point b) {double squared=0;for(std::size_t k=0;k<3;++k)squared+=(double(a[k])-b[k])*(double(a[k])-b[k]);return std::sqrt(squared);}
}
struct Mesh::Impl {Json document;Profile settings;Tile tile;std::unique_ptr<dtNavMesh,NavFree> mesh;};
Mesh::Mesh(std::unique_ptr<Impl> value):impl_(std::move(value)) {}
Mesh::~Mesh()=default;
bool available() noexcept {return true;}
namespace detail {
std::size_t live_recast_allocations() noexcept {return live_allocations.load();}
std::size_t last_recast_peak_bytes() noexcept {return last_peak;}
std::size_t last_recast_allocation_attempts() noexcept {return last_attempts;}
std::size_t live_detour_allocations() noexcept {return live_dt_allocations.load();}
std::size_t last_detour_peak_bytes() noexcept {return last_dt_peak;}
std::size_t last_detour_allocation_attempts() noexcept {return last_dt_attempts;}
void fail_next_detour_allocation(std::size_t ordinal) noexcept {next_dt_failure=ordinal;}
}
std::string profile_json(const Profile& p) {return profile(p).dump();}
Profile parse_profile(const std::string& text) {
    const auto value=parse_package(text);keys(value,{"radius","height","climb","slope","cell_size","cell_height"});Profile p;
    for(auto [key,destination]:{std::pair{"radius",&p.radius},{"height",&p.height},{"climb",&p.climb},{"slope",&p.slope},{"cell_size",&p.cell_size},{"cell_height",&p.cell_height}})if(value.contains(key))*destination=number(value.at(key),0,60);
    validate(p);return p;
}
std::shared_ptr<const Mesh> Mesh::bake(const Geometry& geometry,const Profile& p,const std::string& source,std::size_t allocation_limit) {
    check(allocation_limit>=1 && allocation_limit<=max_bake_memory,"Invalid navigation Recast allocation limit.");
    validate(p);check(hash(source),"Invalid navigation source fingerprint.");
    check(geometry.vertices.size()>=3 && geometry.vertices.size()<=max_vertices && geometry.triangles.size()%3==0 && !geometry.triangles.empty() && geometry.triangles.size()/3<=max_triangles,"Navigation collision geometry exceeds bounds or is empty.");
    std::vector<float> vertices;vertices.reserve(geometry.vertices.size()*3);
    for(const auto& v:geometry.vertices)for(float x:v){check(std::isfinite(x) && std::abs(x)<=1000000,"Navigation geometry position exceeds bounds.");vertices.push_back(x);}
    for(std::size_t i=0;i<geometry.triangles.size();i+=3) {
        for(std::size_t k=0;k<3;++k)check(geometry.triangles[i+k]>=0 && static_cast<std::size_t>(geometry.triangles[i+k])<geometry.vertices.size(),"Navigation triangle index is invalid.");
        const auto a=geometry.vertices[static_cast<std::size_t>(geometry.triangles[i])],b=geometry.vertices[static_cast<std::size_t>(geometry.triangles[i+1])],c=geometry.vertices[static_cast<std::size_t>(geometry.triangles[i+2])];
        const double ux=double(b[0])-a[0],uy=double(b[1])-a[1],uz=double(b[2])-a[2],vx=double(c[0])-a[0],vy=double(c[1])-a[1],vz=double(c[2])-a[2];
        const auto x=uy*vz-uz*vy,y=uz*vx-ux*vz,z=ux*vy-uy*vx;check(x*x+y*y+z*z>1e-12,"Navigation triangle is degenerate at world float precision.");
    }
    rcConfig cfg{};cfg.cs=static_cast<float>(p.cell_size);cfg.ch=static_cast<float>(p.cell_height);cfg.walkableSlopeAngle=static_cast<float>(p.slope);
    cfg.walkableHeight=static_cast<int>(std::ceil(p.height/cfg.ch));cfg.walkableClimb=static_cast<int>(std::floor(p.climb/cfg.ch));cfg.walkableRadius=static_cast<int>(std::ceil(p.radius/cfg.cs));
    cfg.maxEdgeLen=static_cast<int>(12/cfg.cs);cfg.maxSimplificationError=.5f;cfg.minRegionArea=0;cfg.mergeRegionArea=0;cfg.maxVertsPerPoly=6;
    cfg.detailSampleDist=cfg.cs*6;cfg.detailSampleMaxError=cfg.ch;
    rcCalcBounds(vertices.data(),static_cast<int>(geometry.vertices.size()),cfg.bmin,cfg.bmax);
    cfg.bmin[1]-=cfg.ch*2;cfg.bmax[1]+=static_cast<float>(p.height)+cfg.ch*2;
    rcCalcGridSize(cfg.bmin,cfg.bmax,cfg.cs,&cfg.width,&cfg.height);
    check(cfg.width>=1 && cfg.height>=1 && std::uint64_t(cfg.width)*std::uint64_t(cfg.height)<=max_cells && cfg.width<65535 && cfg.height<65535,"Navigation XZ grid exceeds 4 million cells or quantized dimension limits.");
    check((cfg.bmax[1]-cfg.bmin[1])/cfg.ch<=1024,"Navigation vertical voxel extent exceeds 1024.");
    static std::once_flag allocation_hooks;std::call_once(allocation_hooks,[]{rcAllocSetCustom(&allocate,&release);});BudgetScope budget(allocation_limit);
    rcContext context(false);
    auto solid=std::unique_ptr<rcHeightfield,decltype(&rcFreeHeightField)>(rcAllocHeightfield(),&rcFreeHeightField);
    check(bool(solid) && rcCreateHeightfield(&context,*solid,cfg.width,cfg.height,cfg.bmin,cfg.bmax,cfg.cs,cfg.ch),"Cannot allocate navigation heightfield.");
    const auto triangles=static_cast<int>(geometry.triangles.size()/3);std::vector<unsigned char> areas(static_cast<std::size_t>(triangles),0);
    rcMarkWalkableTriangles(&context,cfg.walkableSlopeAngle,vertices.data(),static_cast<int>(geometry.vertices.size()),geometry.triangles.data(),triangles,areas.data());
    check(rcRasterizeTriangles(&context,vertices.data(),static_cast<int>(geometry.vertices.size()),geometry.triangles.data(),areas.data(),triangles,*solid,cfg.walkableClimb),"Cannot rasterize navigation collision triangles.");
    rcFilterLowHangingWalkableObstacles(&context,cfg.walkableClimb,*solid);rcFilterLedgeSpans(&context,cfg.walkableHeight,cfg.walkableClimb,*solid);rcFilterWalkableLowHeightSpans(&context,cfg.walkableHeight,*solid);
    auto compact=std::unique_ptr<rcCompactHeightfield,decltype(&rcFreeCompactHeightfield)>(rcAllocCompactHeightfield(),&rcFreeCompactHeightfield);
    check(bool(compact) && rcBuildCompactHeightfield(&context,cfg.walkableHeight,cfg.walkableClimb,*solid,*compact),"Cannot compact navigation heightfield.");solid.reset();
    check(rcErodeWalkableArea(&context,cfg.walkableRadius,*compact) && rcBuildRegionsMonotone(&context,*compact,0,0,0),"Cannot construct clearance-aware navigation regions.");
    auto contours=std::unique_ptr<rcContourSet,decltype(&rcFreeContourSet)>(rcAllocContourSet(),&rcFreeContourSet);
    check(bool(contours) && rcBuildContours(&context,*compact,cfg.maxSimplificationError,cfg.maxEdgeLen,*contours),"Cannot build navigation contours.");
    auto polygons=std::unique_ptr<rcPolyMesh,decltype(&rcFreePolyMesh)>(rcAllocPolyMesh(),&rcFreePolyMesh);
    check(bool(polygons) && rcBuildPolyMesh(&context,*contours,6,*polygons),"Cannot build navigation polygons.");
    check(polygons->npolys>0 && polygons->npolys<=max_mesh_polygons && polygons->nverts<65535,"Navigation bake has no walkable surface or exceeds polygon limits.");
    auto detail=std::unique_ptr<rcPolyMeshDetail,decltype(&rcFreePolyMeshDetail)>(rcAllocPolyMeshDetail(),&rcFreePolyMeshDetail);
    check(bool(detail) && rcBuildPolyMeshDetail(&context,*polygons,*compact,cfg.detailSampleDist,cfg.detailSampleMaxError,*detail),"Cannot build navigation surface detail.");
    auto result=std::make_unique<Impl>();result->settings=p;auto& tile=result->tile;
    tile.vertices.assign(polygons->verts,polygons->verts+polygons->nverts*3);tile.polygons.assign(polygons->polys,polygons->polys+polygons->npolys*12);
    tile.detail_meshes.assign(detail->meshes,detail->meshes+detail->nmeshes*4);tile.detail_vertices.assign(detail->verts,detail->verts+detail->nverts*3);tile.detail_triangles.assign(detail->tris,detail->tris+detail->ntris*4);
    std::copy_n(polygons->bmin,3,tile.minimum.begin());std::copy_n(polygons->bmax,3,tile.maximum.begin());
    result->mesh=build_nav(tile,p);
    result->document={{"format","poima.navigation"},{"version",1},{"upstream",upstream},{"source_fingerprint",source},{"profile",profile(p)},
        {"clearance",{{"radius",static_cast<float>(cfg.walkableRadius)*cfg.cs},{"height",static_cast<float>(cfg.walkableHeight)*cfg.ch},{"climb",static_cast<float>(cfg.walkableClimb)*cfg.ch}}},
        {"statistics",{{"source_vertices",geometry.vertices.size()},{"source_triangles",triangles},{"grid_width",cfg.width},{"grid_height",cfg.height},{"polygons",polygons->npolys}}},
        {"tile",{{"minimum",tile.minimum},{"maximum",tile.maximum},{"vertices",tile.vertices},{"polygons",tile.polygons},{"detail_meshes",tile.detail_meshes},{"detail_vertices",tile.detail_vertices},{"detail_triangles",tile.detail_triangles}}}};
    check(result->document.dump().size()<=max_package_bytes,"Navigation package exceeds 16 MiB.");
    return std::shared_ptr<const Mesh>(new Mesh(std::move(result)));
}
std::shared_ptr<const Mesh> Mesh::decode(const std::string& text,std::size_t allocation_limit) {
    check(text.size()<=max_package_bytes,"Navigation package exceeds 16 MiB.");auto value=parse_package(text);keys(value,{"format","version","upstream","source_fingerprint","profile","clearance","statistics","tile"});
    check(value.at("format")=="poima.navigation" && value.at("version")==1 && value.at("upstream")==upstream,"Unsupported navigation package format/backend.");
    check(value.at("source_fingerprint").is_string() && hash(value.at("source_fingerprint")),"Invalid navigation package fingerprint.");
    auto result=std::make_unique<Impl>();result->settings=parse_profile(value.at("profile").dump());auto& tile=result->tile;const auto& data=value.at("tile");
    keys(data,{"minimum","maximum","vertices","polygons","detail_meshes","detail_vertices","detail_triangles"});tile.minimum=point(data.at("minimum"));tile.maximum=point(data.at("maximum"));
    tile.vertices=integers<unsigned short>(data.at("vertices"),65534*3,65535);tile.polygons=integers<unsigned short>(data.at("polygons"),max_mesh_polygons*12,65535);
    tile.detail_meshes=integers<unsigned int>(data.at("detail_meshes"),max_mesh_polygons*4,16777216);tile.detail_vertices=floats(data.at("detail_vertices"),1048576*3);tile.detail_triangles=integers<unsigned char>(data.at("detail_triangles"),1048576*4,255);
    const auto& p=result->settings;const auto cs=static_cast<float>(p.cell_size),ch=static_cast<float>(p.cell_height);
    const Json clearance={{"radius",static_cast<float>(static_cast<int>(std::ceil(p.radius/cs)))*cs},{"height",static_cast<float>(static_cast<int>(std::ceil(p.height/ch)))*ch},{"climb",static_cast<float>(static_cast<int>(std::floor(p.climb/ch)))*ch}};
    check(value.at("clearance")==clearance,"Navigation clearance metadata disagrees with profile.");
    const auto& stats=value.at("statistics");keys(stats,{"source_vertices","source_triangles","grid_width","grid_height","polygons"});
    auto sv=number(stats.at("source_vertices"),3,max_vertices),st=number(stats.at("source_triangles"),1,max_triangles),w=number(stats.at("grid_width"),1,65534),h=number(stats.at("grid_height"),1,65534);
    check(std::floor(sv)==sv && std::floor(st)==st && std::floor(w)==w && std::floor(h)==h && w*h<=max_cells && number(stats.at("polygons"),1,max_mesh_polygons)==static_cast<double>(tile.polygons.size()/12),"Invalid navigation statistics.");
    result->mesh=build_nav(tile,p,allocation_limit);result->document=std::move(value);return std::shared_ptr<const Mesh>(new Mesh(std::move(result)));
}
std::string Mesh::encode() const {return impl_->document.dump();}
std::string Mesh::metadata() const {
    Json value=Json::object();
    for(const auto* key:{"format","version","upstream","source_fingerprint","profile","clearance","statistics"})value[key]=impl_->document.at(key);
    return value.dump();
}
std::string Mesh::source_fingerprint() const {return impl_->document.at("source_fingerprint");}
Path Mesh::path(const PathRequest& request,std::size_t allocation_limit) const {
    DetourScope allocation(allocation_limit);
    for(auto v:request.start)check(std::isfinite(v) && std::abs(v)<=1000000,"Invalid navigation start.");
    for(auto v:request.end)check(std::isfinite(v) && std::abs(v)<=1000000,"Invalid navigation end.");
    for(auto v:request.extents)check(std::isfinite(v) && v>=.01f && v<=100,"Navigation projection extents must be .01..100 meters.");
    check(request.max_polygons>=1 && request.max_polygons<=4096 && request.max_corners>=2 && request.max_corners<=4096 && request.max_nodes>=32 && request.max_nodes<=4096,"Navigation path/query budget exceeds bounds.");
    std::unique_ptr<dtNavMeshQuery,QueryFree> query(dtAllocNavMeshQuery());check(bool(query) && dtStatusSucceed(query->init(impl_->mesh.get(),static_cast<int>(request.max_nodes))),"Cannot allocate navigation query.");
    dtQueryFilter filter;filter.setIncludeFlags(1);filter.setExcludeFlags(0);Path result;result.requested_start=request.start;result.requested_end=request.end;
    dtPolyRef start=0,end=0;check(dtStatusSucceed(query->findNearestPoly(request.start.data(),request.extents.data(),&filter,&start,result.projected_start.data())) && dtStatusSucceed(query->findNearestPoly(request.end.data(),request.extents.data(),&filter,&end,result.projected_end.data())),"Navigation endpoint query failed.");
    result.start_found=start!=0;result.end_found=end!=0;
    result.start_distance=result.start_found ? distance(request.start,result.projected_start) : 0;result.end_distance=result.end_found ? distance(request.end,result.projected_end) : 0;
    if(!start || !end){result.status="unreachable";return result;}
    std::vector<dtPolyRef> corridor(request.max_polygons);int count=0;
    const auto status=query->findPath(start,end,result.projected_start.data(),result.projected_end.data(),&filter,corridor.data(),&count,static_cast<int>(request.max_polygons));
    check(!dtStatusFailed(status) && count>=0 && static_cast<std::uint32_t>(count)<=request.max_polygons,"Navigation path query failed or returned an invalid count.");result.polygons=static_cast<std::uint32_t>(count);
    if(!count){result.status="unreachable";return result;}
    result.reachable_end=result.projected_end;
    const auto last=static_cast<std::size_t>(count)-1;
    if(corridor[last]!=end)check(dtStatusSucceed(query->closestPointOnPoly(corridor[last],result.projected_end.data(),result.reachable_end.data(),nullptr)),"Cannot project partial navigation endpoint.");
    std::vector<float> corners(request.max_corners*3);int corners_count=0;
    const auto straight=query->findStraightPath(result.projected_start.data(),result.reachable_end.data(),corridor.data(),count,corners.data(),nullptr,nullptr,&corners_count,static_cast<int>(request.max_corners));
    check(!dtStatusFailed(straight) && corners_count>=0 && static_cast<std::uint32_t>(corners_count)<=request.max_corners,"Navigation corner query failed or returned an invalid count.");
    for(std::size_t i=0;i<static_cast<std::size_t>(corners_count);++i)result.corners.push_back({corners[i*3],corners[i*3+1],corners[i*3+2]});
    result.status=dtStatusDetail(status,DT_OUT_OF_NODES) ? "out_of_nodes" : dtStatusDetail(status,DT_BUFFER_TOO_SMALL)||dtStatusDetail(straight,DT_BUFFER_TOO_SMALL) ? "buffer_limit" : dtStatusDetail(status,DT_PARTIAL_RESULT)||dtStatusDetail(straight,DT_PARTIAL_RESULT)||corridor[last]!=end ? "partial" : "complete";
    return result;
}
}
