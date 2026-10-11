// SPDX-License-Identifier: Apache-2.0
#include "runtime_hit_regions.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace poima {
namespace {
using V=std::array<double,3>;
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
bool bounded(double v,double low,double high) {return std::isfinite(v) && v>=low && v<=high;}
bool name(const std::string& text) {
    if(text.empty() || text.size()>64 || text[0]<'a' || text[0]>'z')return false;
    return std::all_of(text.begin(),text.end(),[](char c){return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='.' || c=='-';});
}
void shape(const RuntimeHitRegion& region) {
    check(name(region.name),"HitRegion name must be 1..64 ASCII lowercase action-style characters.");
    for(double v:region.center)check(bounded(v,-10000,10000),"HitRegion center must be finite within +/-10000 meters.");
    switch(region.shape) {
    case HitRegionShape::Sphere:
        check(bounded(region.radius,.001,10000),"HitRegion sphere radius must be .001..10000 meters.");break;
    case HitRegionShape::Box:
        for(double v:region.half_extents)check(bounded(v,.001,10000),"HitRegion box half extents must be .001..10000 meters.");break;
    case HitRegionShape::Capsule:
        check(bounded(region.radius,.001,10000) && bounded(region.half_height,0,10000),"HitRegion capsule radius/half height is invalid.");break;
    default:throw std::runtime_error("Unknown HitRegion shape.");
    }
}
V transform(const Matrix4& m,const V& v,bool point) {
    V out{};
    for(std::size_t row=0;row<3;++row) {
        out[row]=(point ? m[12+row] : 0)+m[row]*v[0]+m[4+row]*v[1]+m[8+row]*v[2];
        check(std::isfinite(out[row]),"HitRegion inverse-affine segment exceeds numeric range.");
    }
    return out;
}
double length(const V& v) {return std::hypot(v[0],v[1],v[2]);}
// Uniform scaling avoids squared overflow. Closest approach avoids subtracting
// two large quadratic coefficients (which loses a small distant sphere's r^2).
std::array<double,2> sphere_roots(const V& p,const V& d,double radius,bool cylinder=false) {
    double scale=radius;
    for(std::size_t i=0;i<3;++i)if(!cylinder || i!=1)scale=std::max({scale,std::abs(p[i]),std::abs(d[i])});
    V x{},y{};double a=0,projection=0;
    for(std::size_t i=0;i<3;++i)if(!cylinder || i!=1) {
        x[i]=p[i]/scale;y[i]=d[i]/scale;a+=y[i]*y[i];projection+=x[i]*y[i];
    }
    const auto miss=std::array<double,2>{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity()};
    if(a==0)return miss;
    const double middle=-projection/a;V closest{};
    for(std::size_t i=0;i<3;++i)closest[i]=std::fma(middle,y[i],x[i]);
    const double distance=length(closest),r=radius/scale;if(distance>r)return miss;
    const double half=std::sqrt(((r-distance)*(r+distance))/a);return {middle-half,middle+half};
}
}
void validate_runtime_hit_regions(const RuntimeDefinition& definition) {
    const auto count=std::count_if(definition.entities.begin(),definition.entities.end(),[](const auto& e){return e.hit_region.has_value();});
    if(count==0)return; // Preserve omitted-region native behavior/error precedence.
    check(static_cast<std::size_t>(count)<=max_runtime_hit_regions,"Runtime HitRegion limit is 4096 including disabled regions.");
    std::map<std::string,const RuntimeEntityDefinition*> graph;
    for(const auto& entity:definition.entities)check(graph.emplace(entity.id,&entity).second,"Duplicate entity in HitRegion hierarchy.");
    std::set<std::pair<std::string,std::string>> identities;
    for(const auto& entity:definition.entities)if(entity.hit_region) {
        const auto& region=*entity.hit_region;shape(region);
        check(graph.contains(region.owner),"HitRegion owner does not exist.");
        check(identities.emplace(region.owner,region.name).second,"HitRegion owner/name must be unique including disabled regions.");
        std::set<std::string> visited;const RuntimeEntityDefinition* current=&entity;bool owns=false;
        while(current) {
            check(visited.insert(current->id).second,"HitRegion ancestry contains a cycle.");
            owns|=current->id==region.owner;
            if(current->parent.empty())break;
            const auto found=graph.find(current->parent);check(found!=graph.end(),"HitRegion ancestor is absent.");current=found->second;
        }
        check(owns,"HitRegion owner must be self or an ancestor.");
    }
}
namespace hit_regions {
std::optional<Intersection> intersect(const RuntimeHitRegion& region,const Matrix4& world,const V& origin,const V& delta) {
    shape(region);
    for(double v:world)check(std::isfinite(v),"HitRegion query pose must be finite.");
    check(world[3]==0 && world[7]==0 && world[11]==0 && world[15]==1,"HitRegion query pose must be affine.");
    for(double v:origin)check(std::isfinite(v),"HitRegion ray origin must be finite.");
    for(double v:delta)check(std::isfinite(v),"HitRegion ray delta must be finite.");
    const auto inverse=inverse_affine(world);auto p=transform(inverse,origin,true);const auto d=transform(inverse,delta,false);
    for(std::size_t i=0;i<3;++i) {p[i]-=region.center[i];check(std::isfinite(p[i]),"HitRegion local origin exceeds numeric range.");}
    bool inside=false;
    if(region.shape==HitRegionShape::Box)inside=std::abs(p[0])<=region.half_extents[0] && std::abs(p[1])<=region.half_extents[1] && std::abs(p[2])<=region.half_extents[2];
    else {auto closest=p;if(region.shape==HitRegionShape::Capsule)closest[1]-=std::clamp(p[1],-region.half_height,region.half_height);inside=length(closest)<=region.radius;}
    if(inside)return Intersection{};
    double first=std::numeric_limits<double>::infinity();V local_normal{};
    const auto admit=[&](double t,const V& normal) {
        if(std::isfinite(t) && t>=0 && t<=1 && t<first){first=t;local_normal=normal;}
    };
    if(region.shape==HitRegionShape::Box) {
        double enter=0,leave=1;V normal{};
        for(std::size_t axis=0;axis<3;++axis) {
            if(d[axis]==0) {if(std::abs(p[axis])>region.half_extents[axis])return {};continue;}
            double a=(-region.half_extents[axis]-p[axis])/d[axis],b=(region.half_extents[axis]-p[axis])/d[axis];
            double sign=-1;if(b<a){std::swap(a,b);sign=1;}
            if(a>enter){enter=a;normal={};normal[axis]=sign;}
            leave=std::min(leave,b);if(enter>leave)return {};
        }
        admit(enter,normal);
    }else if(region.shape==HitRegionShape::Sphere || region.half_height==0) {
        for(double t:sphere_roots(p,d,region.radius)) {
            V normal{};for(std::size_t i=0;i<3;++i)normal[i]=std::fma(t,d[i],p[i]);admit(t,normal);
        }
    }else {
        for(double t:sphere_roots(p,d,region.radius,true))if(std::isfinite(t)) {
            const double y=p[1]+t*d[1];
            if(y>=-region.half_height && y<=region.half_height)admit(t,{p[0]+t*d[0],0,p[2]+t*d[2]});
        }
        for(double cap:{-region.half_height,region.half_height}) {
            auto center=p;center[1]-=cap;check(std::isfinite(center[1]),"HitRegion capsule origin exceeds numeric range.");
            for(double t:sphere_roots(center,d,region.radius))if(std::isfinite(t)) {
                const double y=p[1]+t*d[1];
                if((cap<0 && y<=cap) || (cap>0 && y>=cap))admit(t,{p[0]+t*d[0],y-cap,p[2]+t*d[2]});
            }
        }
    }
    if(!std::isfinite(first))return {};
    V normal{};
    // Column-major inverse transpose: local surface gradients, not positions.
    for(std::size_t row=0;row<3;++row)normal[row]=inverse[row*4]*local_normal[0]+inverse[row*4+1]*local_normal[1]+inverse[row*4+2]*local_normal[2];
    const double magnitude=length(normal);check(std::isfinite(magnitude) && magnitude>0,"HitRegion surface normal exceeds numeric range.");
    for(double& v:normal)v/=magnitude;return Intersection{first,normal};
}
}
}
