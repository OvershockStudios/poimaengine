// SPDX-License-Identifier: Apache-2.0
#include "poima/scene.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void near(double actual, double expected) {
    if (!std::isfinite(actual) || std::abs(actual-expected) > 1e-9)
        throw std::runtime_error("Unexpected matrix result.");
}
void identity(const poima::Matrix4& value) {
    const auto expected = poima::identity_matrix();
    for (std::size_t i=0;i<16;++i) near(value[i], expected[i]);
}
}
int main() {
    try {
        const double half = std::sqrt(0.5);
        const auto parent = poima::local_matrix({10,0,0}, {0,0,half,half}, {2,3,4});
        const auto child = poima::local_matrix({1,0,0}, {half,0,0,half}, {1,2,1});
        const auto world = poima::multiply(parent, child);
        near(world[12],10); near(world[13],2); near(world[14],0);
        identity(poima::multiply(world, poima::inverse_affine(world)));
        identity(poima::multiply(poima::inverse_affine(world), world));
        // Non-uniform parent scale and a non-axis-aligned child yield shear.
        const auto sheared = poima::multiply(parent, poima::local_matrix({1,2,3}, {0,0,std::sin(0.3),std::cos(0.3)}, {1,1,1}));
        identity(poima::multiply(sheared, poima::inverse_affine(sheared)));
        if (poima::rigid_transform(sheared)) throw std::runtime_error("Scaled camera accepted.");
        const auto camera = poima::local_matrix({5,-3,8}, {0,half,0,half}, {1,1,1});
        if (!poima::rigid_transform(camera)) throw std::runtime_error("Rigid camera rejected.");
        identity(poima::multiply(camera, poima::inverse_affine(camera)));
        const auto projection = poima::perspective(90,2,0.5,100);
        auto depth = [&](double z) { return (projection[10]*z+projection[14]) / (-z); };
        near(depth(-0.5),0); near(depth(-100),1); near(projection[0],0.5); near(projection[5],1);
        bool rejected = false;
        try { (void)poima::inverse_affine(poima::local_matrix({0,0,0},{0,0,0,1},{1,0,1})); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("Singular inverse accepted.");
        std::cout << "Hierarchy, affine inverse, camera rigidity and [0,1] perspective depth passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
