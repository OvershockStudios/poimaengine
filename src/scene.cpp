// SPDX-License-Identifier: Apache-2.0
#include "poima/scene.hpp"
#include <cmath>
#include <numbers>
#include <random>
#include <stdexcept>

namespace poima {
namespace {
void finite(const Matrix4& value) {
    for (double item : value) if (!std::isfinite(item))
        throw std::runtime_error("Transform composition exceeds numeric range.");
}
}
std::string new_presentation_source_id() {
    std::random_device random;
    constexpr char hex[]="0123456789abcdef";
    std::string result(32,'0');
    for(auto& value:result)value=hex[random()&15];
    return result;
}
Matrix4 identity_matrix() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                result[static_cast<std::size_t>(col*4+row)] += a[static_cast<std::size_t>(k*4+row)] * b[static_cast<std::size_t>(col*4+k)];
    finite(result); return result;
}
Matrix4 local_matrix(const std::array<double,3>& p, const std::array<double,4>& q, const std::array<double,3>& s) {
    // Normalize within the authored quaternion validation tolerance.
    const double length = std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if (!(length > 0) || !std::isfinite(length)) throw std::runtime_error("Invalid rotation.");
    const double x=q[0]/length, y=q[1]/length, z=q[2]/length, w=q[3]/length;
    Matrix4 result = {
        (1-2*(y*y+z*z))*s[0], 2*(x*y+z*w)*s[0], 2*(x*z-y*w)*s[0], 0,
        2*(x*y-z*w)*s[1], (1-2*(x*x+z*z))*s[1], 2*(y*z+x*w)*s[1], 0,
        2*(x*z+y*w)*s[2], 2*(y*z-x*w)*s[2], (1-2*(x*x+y*y))*s[2], 0,
        p[0],p[1],p[2],1};
    finite(result); return result;
}
Matrix4 inverse_affine(const Matrix4& m) {
    finite(m);
    const double a=m[0], b=m[4], c=m[8], d=m[1], e=m[5], f=m[9], g=m[2], h=m[6], i=m[10];
    const double determinant = a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    if (!std::isfinite(determinant) || determinant == 0) throw std::runtime_error("Singular transform.");
    Matrix4 r = {(e*i-f*h)/determinant,(f*g-d*i)/determinant,(d*h-e*g)/determinant,0,
        (c*h-b*i)/determinant,(a*i-c*g)/determinant,(b*g-a*h)/determinant,0,
        (b*f-c*e)/determinant,(c*d-a*f)/determinant,(a*e-b*d)/determinant,0,0,0,0,1};
    for (int row=0;row<3;++row) r[static_cast<std::size_t>(12+row)] =
        -(r[static_cast<std::size_t>(row)]*m[12]+r[static_cast<std::size_t>(4+row)]*m[13]+r[static_cast<std::size_t>(8+row)]*m[14]);
    finite(r); return r;
}
bool rigid_transform(const Matrix4& m) {
    for (int a=0;a<3;++a) for (int b=0;b<3;++b) {
        double dot=0;
        for (int row=0;row<3;++row) dot+=m[static_cast<std::size_t>(a*4+row)]*m[static_cast<std::size_t>(b*4+row)];
        if (!std::isfinite(dot) || std::abs(dot-(a==b ? 1.0 : 0.0))>1e-6) return false;
    }
    return true;
}
Matrix4 perspective(double fov, double aspect, double near_plane, double far_plane) {
    if (!(fov>0 && fov<180 && aspect>0 && near_plane>0 && far_plane>near_plane))
        throw std::runtime_error("Invalid perspective projection.");
    const double y=1/std::tan(fov*std::numbers::pi/360);
    Matrix4 result = {y/aspect,0,0,0, 0,y,0,0, 0,0,far_plane/(near_plane-far_plane),-1,
        0,0,far_plane*near_plane/(near_plane-far_plane),0};
    finite(result); return result;
}
}
