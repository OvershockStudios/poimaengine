// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace poima {
Bounds mesh_bounds(const MeshAsset* mesh) {
    if(!mesh)return {{-.5,-.5,-.5},{.5,.5,.5}};
    if(mesh->vertices.empty())throw std::runtime_error("Cannot bound empty mesh geometry.");
    Bounds result;for(std::size_t k=0;k<3;++k)result.minimum[k]=result.maximum[k]=mesh->vertices.front().position[k];
    for(const auto& vertex:mesh->vertices)for(std::size_t k=0;k<3;++k) {
        const double v=vertex.position[k];if(!std::isfinite(v))throw std::runtime_error("Mesh bounds require finite positions.");
        result.minimum[k]=std::min(result.minimum[k],v);result.maximum[k]=std::max(result.maximum[k],v);
    }
    return result;
}
Bounds transform_bounds(const Bounds& local,const Matrix4& world) {
    Bounds result;
    for(std::size_t row=0;row<3;++row) {
        double low=world[12+row],high=low,magnitude=std::abs(low)+1;
        for(std::size_t col=0;col<3;++col) {
            if(local.minimum[col]>local.maximum[col])throw std::runtime_error("Inverted geometry bounds.");
            const double a=world[col*4+row]*local.minimum[col],b=world[col*4+row]*local.maximum[col];
            low+=std::min(a,b);high+=std::max(a,b);magnitude+=std::max(std::abs(a),std::abs(b));
        }
        if(!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(magnitude))throw std::runtime_error("Transformed bounds exceed numeric range.");
        // GPU model transforms use floats. Widen the double-precision bound to
        // retain geometry whose rounded shader coordinates touch a clip plane.
        const double guard=32*std::numeric_limits<float>::epsilon()*magnitude;
        result.minimum[row]=low-guard;result.maximum[row]=high+guard;
    }
    return result;
}
Frustum make_frustum(const Matrix4& m) {
    Frustum result;
    for(std::size_t col=0;col<4;++col) {
        const auto n=col*4;
        result[0][col]=m[n+3]+m[n];result[1][col]=m[n+3]-m[n];
        result[2][col]=m[n+3]+m[n+1];result[3][col]=m[n+3]-m[n+1];
        result[4][col]=m[n+2];result[5][col]=m[n+3]-m[n+2]; // Vulkan depth [0,w]
    }
    return result;
}
bool intersects(const Bounds& bounds,const Frustum& frustum) {
    for(const auto& p:frustum) {
        double support=p[3],magnitude=std::abs(p[3])+1;
        for(std::size_t k=0;k<3;++k) {
            const double value=p[k]*(p[k]>=0 ? bounds.maximum[k] : bounds.minimum[k]);
            support+=value;magnitude+=std::abs(value);
        }
        // Invalid/uncertain planes cannot prove an object is outside. The
        // renderer separately validates its matrices before GPU submission.
        if(std::isfinite(support) && std::isfinite(magnitude) && support < -64*std::numeric_limits<float>::epsilon()*magnitude)return false;
    }
    return true;
}
}
