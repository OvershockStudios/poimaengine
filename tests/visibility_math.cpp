// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include "../src/render_shadow_bounds.hpp"
#include "../src/render_shadow_index_ranges.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

void check(bool yes,const char* why) { if(!yes)throw std::runtime_error(why); }

namespace {
using poima::Bounds;
using poima::Matrix4;
namespace shadow=poima::render_shadow_bounds;
namespace ranges=poima::render_shadow_index_ranges;

constexpr float identity_clip[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
constexpr float identity_model[3][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};

enum class DotOrder { serial, pairwise, fused };
constexpr std::array dot_orders{DotOrder::serial,DotOrder::pairwise,DotOrder::fused};

float rounded_sum(float a,float b) {
    const volatile float value=a+b;
    return value;
}

float shader_dot(const std::array<float,4>& a,const std::array<float,4>& b,DotOrder order) {
    if(order==DotOrder::fused) {
        float sum=0;
        for(std::size_t i=4;i!=0;--i)sum=std::fma(a[i-1],b[i-1],sum);
        return sum;
    }
    std::array<float,4> products{};
    for(std::size_t i=0;i<4;++i) {
        const volatile float product=a[i]*b[i];
        products[i]=product;
    }
    if(order==DotOrder::pairwise)
        return rounded_sum(rounded_sum(products[0],products[1]),rounded_sum(products[2],products[3]));
    return rounded_sum(rounded_sum(rounded_sum(products[0],products[1]),products[2]),products[3]);
}

std::array<float,4> shader_clip(const std::array<float,4>& local,const float (&model)[3][4],
    const float (&projection)[16],DotOrder model_order,DotOrder projection_order) {
    // The shader rounds XYZ in model space before the projection dot products.
    std::array<float,4> world{0,0,0,1};
    for(std::size_t row=0;row<3;++row)
        world[row]=shader_dot({model[row][0],model[row][1],model[row][2],model[row][3]},local,model_order);
    std::array<float,4> clip{};
    for(std::size_t row=0;row<4;++row)
        clip[row]=shader_dot({projection[row],projection[4+row],projection[8+row],projection[12+row]},world,projection_order);
    return clip;
}

unsigned outside_float_planes(const std::array<float,4>& clip) {
    for(const auto coordinate:clip)if(!std::isfinite(coordinate))return 0;
    const auto [x,y,z,w]=clip;
    // Compare the rounded coordinates using homogeneous Vulkan clipping.
    const std::array outside{x < -w,x > w,y < -w,y > w,z < 0,z > w};
    unsigned result=0;
    for(std::size_t plane=0;plane<outside.size();++plane)if(outside[plane])result|=1u<<plane;
    return result;
}

bool float_corners_prove_outside(const Bounds& local,const float (&model)[3][4],const float (&projection)[16]) {
    unsigned common_planes=0x3f;
    for(unsigned corner=0;corner<8;++corner) {
        std::array<float,4> point{0,0,0,1};
        for(std::size_t axis=0;axis<3;++axis)
            point[axis]=static_cast<float>((corner&(1u<<axis)) ? local.maximum[axis] : local.minimum[axis]);
        for(const auto first:dot_orders)for(const auto second:dot_orders)
            common_planes&=outside_float_planes(shader_clip(point,model,projection,first,second));
    }
    return common_planes!=0;
}

void rounded_projection(const Matrix4& matrix,float (&projection)[16]) {
    for(std::size_t i=0;i<16;++i)projection[i]=static_cast<float>(matrix[i]);
}

void precise_shadow_boxes() {
    constexpr float diagonal=.70710678118f;
    const float rotated_clip[16]{diagonal,diagonal,0,0, -diagonal,diagonal,0,0, 0,0,1,0, 0,0,0,1};
    const float rod_model[3][4]{{diagonal,-diagonal,0,1},{diagonal,diagonal,0,-1},{0,0,1,.5f}};
    const Bounds rod{{-5,-.025,-.1},{5,.025,.1}};
    Matrix4 world=poima::identity_matrix();
    Matrix4 old_projection{};
    for(std::size_t row=0;row<3;++row)for(std::size_t col=0;col<4;++col)
        world[col*4+row]=rod_model[row][col];
    for(std::size_t i=0;i<16;++i)old_projection[i]=rotated_clip[i];
    check(poima::intersects(poima::transform_bounds(rod,world),poima::make_frustum(old_projection)),
        "Fixture's conservative world AABB did not overlap the rotated clip volume.");
    check(!shadow::intersects(rod,rod_model,shadow::frustum(rotated_clip)) &&
          float_corners_prove_outside(rod,rod_model,rotated_clip),
        "Local shadow box retained a separated rotated rod or disagreed with its float clip corners.");

    const auto identity=shadow::frustum(identity_clip);
    const Bounds box{{-.3,-.5,-.3},{.3,.5,.3}};
    float affine[3][4]{{-1.5f,.4f,0,.1f},{.1f,.8f,.1f,.1f},{0,.2f,.2f,.5f}};
    check(shadow::intersects(box,affine,identity),"Visible reflected and sheared shadow box rejected.");
    affine[0][3]=5;
    check(!shadow::intersects(box,affine,identity) && float_corners_prove_outside(box,affine,identity_clip),
        "Exterior reflected and sheared shadow box did not cull conservatively.");

    const float half_w[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,.5f};
    const auto at_half_w=shadow::frustum(half_w);
    const Bounds behind_near{{-.1,-.1,-.2},{.1,.1,-.1}}, above_far{{-.1,-.1,.6},{.1,.1,.7}};
    check(!shadow::intersects(behind_near,identity_model,at_half_w),"Vulkan z >= 0 near plane omitted.");
    check(!shadow::intersects(above_far,identity_model,at_half_w),"Vulkan z <= w far plane omitted.");
    check(shadow::intersects({{-.1,-.1,0},{.1,.1,0}},identity_model,at_half_w),"Shadow box touching z=0 rejected.");
    check(shadow::intersects({{-.1,-.1,.5},{.1,.1,.5}},identity_model,at_half_w),"Shadow box touching z=w rejected.");

    float perspective[16];
    rounded_projection(poima::perspective(90,1,1,10),perspective);
    float eye_model[3][4]{{1,0,0,0},{0,1,0,0},{0,0,1,3}};
    check(!shadow::intersects(box,eye_model,shadow::frustum(perspective)) &&
          float_corners_prove_outside(box,eye_model,perspective),"Shadow box behind the perspective eye retained.");
}

void shadow_float_cancellation_and_invalid_input() {
    constexpr float big=16777216;
    const float cancellation_model[3][4]{{big,1,-big,1},{0,0,0,0},{0,0,0,.5f}};
    const float cancellation_projection[16]{big,0,0,big, 1,1,0,0, -big,0,0,-big, 1,0,.5f,1};
    const Bounds point{{1,1,1},{1,1,1}};
    const auto rounded_model=shader_clip({1,1,1,1},cancellation_model,identity_clip,DotOrder::serial,DotOrder::serial);
    const auto rounded_clip=shader_clip({1,1,1,1},identity_model,cancellation_projection,DotOrder::serial,DotOrder::serial);
    check(rounded_model[0]==1 && rounded_clip[0]==rounded_clip[3],"Float guard fixture did not cancel to the right clip plane.");
    check(shadow::intersects(point,cancellation_model,shadow::frustum(identity_clip)),"Shadow box rounded onto a clip plane by model arithmetic was rejected.");
    check(shadow::intersects(point,identity_model,shadow::frustum(cancellation_projection)),"Clip-plane guard lost cancellation between independent float x and w evaluations.");

    constexpr auto infinity=std::numeric_limits<float>::infinity();
    constexpr auto nan=std::numeric_limits<float>::quiet_NaN();
    const Bounds exterior{{3,-.1,.2},{4,.1,.4}};
    const auto finite=shadow::frustum(identity_clip);
    const float invalid_projection[16]{1,0,0,0, 0,infinity,0,0, 0,0,1,0, 0,0,0,1};
    const float invalid_model[3][4]{{1,0,0,0},{0,nan,0,0},{0,0,1,0}};
    check(shadow::intersects(exterior,identity_model,shadow::frustum(invalid_projection)),"Nonfinite shadow projection performed a rejection.");
    check(shadow::intersects(exterior,invalid_model,finite),"Nonfinite shadow model performed a rejection.");
    check(shadow::intersects({{3,1,.2},{4,-1,.4}},identity_model,finite),"Inverted local shadow bounds performed a rejection.");
    check(shadow::intersects({{3,-.1,.2},{4,infinity,.4}},identity_model,finite),"Nonfinite local shadow bounds performed a rejection.");
    constexpr auto huge=std::numeric_limits<float>::max();
    const float huge_projection[16]{1,0,0,0, 0,huge,0,0, 0,0,1,0, 0,0,0,1};
    const float huge_model[3][4]{{1,0,0,0},{0,huge,0,0},{0,0,1,0}};
    const Bounds huge_bounds{{3,-2,.2},{4,huge,.4}};
    check(shadow::intersects(exterior,identity_model,shadow::frustum(huge_projection)),"Uncertain extreme projection performed a rejection.");
    check(shadow::intersects(exterior,huge_model,finite),"Uncertain extreme model performed a rejection.");
    check(shadow::intersects(huge_bounds,identity_model,finite),"Uncertain extreme local bounds performed a rejection.");
}

void shadow_affine_clip_cohort() {
    std::mt19937 random(116389);
    const auto number=[&](int scale) {return static_cast<float>((static_cast<int>(random()%2001)-1000)*scale)/1000.f;};
    float clip[16];
    unsigned rejections=0,retained=0;
    for(unsigned sample=0;sample<2048;++sample) {
        float model[3][4];
        for(auto& row:model)for(std::size_t col=0;col<4;++col)row[col]=number(col==3 ? 12 : 2);
        Bounds local;
        for(std::size_t axis=0;axis<3;++axis) {
            const auto center=number(2), radius=.01f+std::abs(number(2));
            local.minimum[axis]=center-radius;
            local.maximum[axis]=center+radius;
        }
        auto projection=sample%2 ? poima::perspective(65,1.25,.5,14) : poima::identity_matrix();
        projection[4]+=number(1)*.2f;
        projection[1]+=number(1)*.2f;
        rounded_projection(projection,clip);
        if(shadow::intersects(local,model,shadow::frustum(clip)))++retained;
        else {
            ++rejections;
            check(float_corners_prove_outside(local,model,clip),"Affine shadow rejection had no common outside plane in the float clip oracle.");
        }
    }
    check(rejections>100 && retained>100,"Randomized shadow cohort did not exercise admitted and rejected affine boxes.");
}

void append_indexed_triangles(poima::MeshAsset& mesh,float x,unsigned triangles=1024) {
    const auto first=static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({{x,-.1f,.4f}});
    mesh.vertices.push_back({{x,.1f,.4f}});
    mesh.vertices.push_back({{x,0,.6f}});
    for(unsigned i=0;i<triangles;++i) {
        mesh.indices.push_back(first+1);
        mesh.indices.push_back(first+2);
        mesh.indices.push_back(first);
    }
}

void verify_culled_triangles(const poima::MeshAsset& mesh,ranges::Range retained,
    const float (&model)[3][4],const float (&projection)[16]) {
    const std::uint64_t end=std::uint64_t{retained.first}+retained.count;
    check(retained.first%3==0 && retained.count%3==0 && end<=mesh.indices.size(),"Shadow range is outside the original triangle stream.");
    for(std::size_t i=0;i<mesh.indices.size();i+=3) {
        if(retained.first<=i && i<end)continue;
        unsigned common_planes=0x3f;
        for(std::size_t j=0;j<3;++j) {
            const auto& p=mesh.vertices.at(mesh.indices[i+j]).position;
            const std::array point{p[0],p[1],p[2],1.f};
            for(const auto first:dot_orders)for(const auto second:dot_orders)
                common_planes&=outside_float_planes(shader_clip(point,model,projection,first,second));
        }
        check(common_planes!=0,"Omitted source triangle could be visible under the float clip oracle.");
    }
}

void shadow_index_ranges() {
    constexpr std::uint32_t elements_per_leaf=3072;
    const auto clip=shadow::frustum(identity_clip);
    const auto local=shadow::local_frustum(identity_model,clip);
    const auto expect=[&](const poima::MeshAsset& mesh,const shadow::LocalFrustum& frustum,
                          std::uint32_t first,std::uint32_t count) {
        const auto source=mesh.indices;
        const ranges::Hierarchy hierarchy(mesh);
        check(!hierarchy.empty(),"Valid indexed rigid mesh did not produce a hierarchy.");
        const auto retained=hierarchy.visible_range(frustum);
        check(retained.first==first && retained.count==count,"Shadow hierarchy selected the wrong contiguous index span.");
        check(mesh.indices==source,"Shadow hierarchy rewrote the original index order.");
        return retained;
    };

    poima::MeshAsset mixed;
    for(const float x:{4.f,.25f,-4.f,-.25f,4.f})append_indexed_triangles(mixed,x);
    const auto middle=expect(mixed,local,elements_per_leaf,3*elements_per_leaf);
    verify_culled_triangles(mixed,middle,identity_model,identity_clip);
    // The culled middle leaf remains between the first and last admitted leaves.
    check(middle.first<2*elements_per_leaf && middle.first+middle.count>3*elements_per_leaf,
        "Shadow admission compacted a gap in the original index buffer.");

    poima::MeshAsset outside;
    for(int leaf=0;leaf<4;++leaf)append_indexed_triangles(outside,4);
    const auto none=expect(outside,local,0,0);
    verify_culled_triangles(outside,none,identity_model,identity_clip);

    poima::MeshAsset inside;
    for(int leaf=0;leaf<4;++leaf)append_indexed_triangles(inside,0);
    expect(inside,local,0,4*elements_per_leaf);
    const float missing_projection[16]{0,0,0,0, 0,std::numeric_limits<float>::infinity(),0,0};
    expect(outside,shadow::local_frustum(identity_model,shadow::frustum(missing_projection)),0,4*elements_per_leaf);

    // The final partial leaf touches x=w, keeping exactly its 17 triangles.
    auto edge=outside;
    append_indexed_triangles(edge,1,17);
    const auto tail=expect(edge,local,4*elements_per_leaf,17*3);
    verify_culled_triangles(edge,tail,identity_model,identity_clip);

    const float reflected[3][4]{{-1,.2f,0,0},{0,1,.1f,0},{0,0,1,0}};
    const auto reflected_range=expect(mixed,shadow::local_frustum(reflected,clip),elements_per_leaf,3*elements_per_leaf);
    verify_culled_triangles(mixed,reflected_range,reflected,identity_clip);

    const auto expect_empty=[&](const poima::MeshAsset& invalid) {
        const ranges::Hierarchy hierarchy(invalid);
        check(hierarchy.empty(),"Unsupported indexed geometry was admitted to the shadow hierarchy.");
        const auto no_range=hierarchy.visible_range(local);
        check(no_range.first==0 && no_range.count==0,"Empty shadow hierarchy did not return its absent range.");
    };
    auto malformed=inside;
    malformed.indices.resize(malformed.indices.size()-3);
    expect_empty(malformed); // 4,095 triangles is below the admission threshold.
    malformed=inside;
    malformed.indices.push_back(0);
    expect_empty(malformed);
    malformed=inside;
    malformed.indices.back()=static_cast<std::uint32_t>(malformed.vertices.size());
    expect_empty(malformed);
    malformed=inside;
    malformed.vertices.front().position[1]=std::numeric_limits<float>::quiet_NaN();
    expect_empty(malformed);
    malformed=inside;
    malformed.indices.clear();
    expect_empty(malformed);
    malformed=inside;
    malformed.influences.resize(malformed.vertices.size());
    expect_empty(malformed);
}
}

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
        precise_shadow_boxes();shadow_float_cancellation_and_invalid_input();shadow_affine_clip_cohort();shadow_index_ranges();
        std::cout<<"Six clip planes, perspective depth, touching/enclosing bounds and 10000 affine float-rounding cases passed.\n";
        std::cout<<"Precise rigid shadow bounds passed the Vulkan clip and 2048 affine two-stage float cases.\n";
        std::cout<<"Contiguous shadow index ranges passed full, partial, empty, boundary and malformed mesh cases.\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
