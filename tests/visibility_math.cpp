// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

void check(bool yes,const char* why) { if(!yes)throw std::runtime_error(why); }
int main() {
    try {
        using namespace poima;
        const auto clip=make_frustum(identity_matrix());
        check(intersects({{-.5,-.5,.2},{.5,.5,.8}},clip),"Interior box rejected.");
        for(const auto outside:std::array<Bounds,6>{{{{-3,-.5,.2},{-2,.5,.8}},{{2,-.5,.2},{3,.5,.8}},{{-.5,-3,.2},{.5,-2,.8}},{{-.5,2,.2},{.5,3,.8}},{{-.5,-.5,-2},{.5,.5,-1}},{{-.5,-.5,2},{.5,.5,3}}}})
            check(!intersects(outside,clip),"Exterior box accepted.");
        check(intersects({{1,-.5,.2},{2,.5,.8}},clip),"Touching plane rejected.");
        check(intersects({{-20,-20,-20},{20,20,20}},clip),"Frustum-containing box rejected.");
        const auto perspective_frustum=make_frustum(perspective(90,1,1,10));
        check(intersects(transform_bounds(mesh_bounds(nullptr),local_matrix({0,0,-5},{0,0,0,1},{1,1,1})),perspective_frustum),"Visible perspective box rejected.");
        check(!intersects(transform_bounds(mesh_bounds(nullptr),local_matrix({0,0,5},{0,0,0,1},{1,1,1})),perspective_frustum),"Behind-camera box retained.");
        check(!intersects(transform_bounds(mesh_bounds(nullptr),local_matrix({0,0,-12},{0,0,0,1},{1,1,1})),perspective_frustum),"Beyond-far box retained.");
        MeshAsset mesh;mesh.vertices.resize(2);mesh.vertices[0].position={-2,1,-3};mesh.vertices[1].position={4,5,6};
        const auto local=mesh_bounds(&mesh);check(local.minimum[0]==-2 && local.maximum[2]==6,"Imported bounds mismatch.");
        std::mt19937 random(7341);std::uniform_real_distribution<double> distribution(-10,10);
        // General affine transforms include negative entries, nonuniform scale
        // and shear. Check every transformed corner using GPU-like float math.
        for(int sample=0;sample<10000;++sample) {
            auto world=identity_matrix();for(std::size_t col=0;col<4;++col)for(std::size_t row=0;row<3;++row)world[col*4+row]=distribution(random);
            const auto result=transform_bounds(local,world);
            for(int corner=0;corner<8;++corner)for(std::size_t row=0;row<3;++row) {
                float coordinate=static_cast<float>(world[12+row]);
                for(std::size_t col=0;col<3;++col)coordinate+=static_cast<float>(world[col*4+row])*static_cast<float>((corner&(1<<col)) ? local.maximum[col] : local.minimum[col]);
                check(coordinate>=result.minimum[row] && coordinate<=result.maximum[row],"Rounded GPU corner escaped conservative bounds.");
            }
        }
        bool rejected=false;try { (void)transform_bounds({{1,0,0},{0,1,1}},identity_matrix()); }catch(const std::runtime_error&) { rejected=true; }check(rejected,"Inverted bounds accepted.");
        std::cout<<"Six clip planes, perspective depth, touching/enclosing bounds and 10000 affine float-rounding cases passed.\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
