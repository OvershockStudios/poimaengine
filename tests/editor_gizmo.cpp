// SPDX-License-Identifier: Apache-2.0
#include "poima/editor_gizmo.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
using namespace poima;
namespace {
using V3=std::array<double,3>;
constexpr std::array<V3,3> axes{{{1,0,0},{0,1,0},{0,0,1}}};
void check(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
void near(double actual,double expected,double tolerance=1e-8) { check(std::isfinite(actual)&&std::abs(actual-expected)<tolerance,"Gizmo analytic value differs."); }
SceneSnapshot camera() { SceneSnapshot c;c.camera_world=identity_matrix();c.camera_world[14]=8;return c; }
double focal() { return 300/std::tan(std::numbers::pi_v<double>/6); }
std::array<double,2> project(V3 p) { return {400+focal()*p[0]/(8-p[2]),300-focal()*p[1]/(8-p[2])}; }
void bounded(const EditorGizmo& g) {
    check(g.triangles.size()%3==0&&g.triangles.size()<=max_editor_overlay_vertices,"Overlay triangle budget exceeded.");
    for(const auto& v:g.triangles) {
        check(std::isfinite(v.x)&&std::isfinite(v.y)&&v.x>=0&&v.x<=1&&v.y>=0&&v.y<=1,"Overlay escapes viewport clip.");
        for(auto c:v.color)check(std::isfinite(c)&&c>=0&&c<=1,"Overlay color invalid.");
    }
}
void move_scale() {
    auto g=make_editor_gizmo(camera(),800,600,{},axes,EditorGizmoMode::move);
    check(g.handles[0].visible&&g.handles[1].visible&&!g.handles[2].visible,"Parallel movement handle visibility differs.");
    check(hit_test_editor_gizmo(g,450,300)==EditorGizmoAxis::x,"X handle hit failed.");
    check(hit_test_editor_gizmo(g,400,250)==EditorGizmoAxis::y,"Y handle hit failed.");
    check(!hit_test_editor_gizmo(g,200,200),"Empty space hit a handle.");
    check(hit_test_editor_gizmo(g,450,300,0)==EditorGizmoAxis::x,"Exact zero-tolerance hit failed.");
    auto drag=begin_editor_gizmo_drag(g,EditorGizmoAxis::x,440,300);check(bool(drag),"Move gesture did not begin.");
    const auto moved=update_editor_gizmo_drag(*drag,520,300);check(bool(moved),"Move update failed.");near(*moved,8*80/focal());
    check(!begin_editor_gizmo_drag(g,EditorGizmoAxis::z,400,300),"View-parallel drag accepted.");
    bounded(g);
    g=make_editor_gizmo(camera(),800,600,{},axes,EditorGizmoMode::scale);
    drag=begin_editor_gizmo_drag(g,EditorGizmoAxis::x,480,300);check(bool(drag),"Scale gesture did not begin.");
    near(*update_editor_gizmo_drag(*drag,560,300),2);
    check(!update_editor_gizmo_drag(*drag,390,300),"Negative scale ratio accepted.");
    near(*update_editor_gizmo_drag(*drag,480,300),1);bounded(g);
}
void rotation() {
    const auto g=make_editor_gizmo(camera(),800,600,{},axes,EditorGizmoMode::rotate,EditorGizmoAxis::z);
    check(!g.handles[0].visible&&!g.handles[1].visible&&g.handles[2].visible,"Edge-on rotation rings were not hidden.");
    check(hit_test_editor_gizmo(g,400,220)==EditorGizmoAxis::z,"Ring hit failed.");
    auto drag=begin_editor_gizmo_drag(g,EditorGizmoAxis::z,400,220);check(bool(drag),"Ring gesture did not begin.");
    constexpr double pi=std::numbers::pi_v<double>;
    near(*update_editor_gizmo_drag(*drag,320,300),pi/2);
    near(*update_editor_gizmo_drag(*drag,400,380),pi);
    const auto copy=*drag;
    check(!update_editor_gizmo_drag(*drag,400,300),"Ring center singularity accepted.");
    check(drag->last_angle==copy.last_angle&&drag->angle==copy.angle,"Failed rotation advanced state.");
    near(*update_editor_gizmo_drag(*drag,480,300),3*pi/2);
    near(*update_editor_gizmo_drag(*drag,400,220),2*pi);
    near(*update_editor_gizmo_drag(*drag,320,300),5*pi/2);
    auto independent=copy;near(*update_editor_gizmo_drag(independent,320,300),pi/2);
    check(drag->angle!=independent.angle,"Copied gesture shares mutable state.");bounded(g);
}
void affine_basis_and_clipping() {
    const std::array<V3,3> affine{{{2,2,1},{1,3,0},{0,0,2}}};
    auto g=make_editor_gizmo(camera(),800,600,{},affine,EditorGizmoMode::move);
    check(g.handles[0].visible&&g.handles[1].visible,"Nonorthogonal basis hidden.");
    // First axis has length3: scalar parameters refer to normalized world axis.
    const auto p=project({1.0/3,1.0/3,1.0/6}),q=project({2.0/3,2.0/3,1.0/3});
    auto drag=begin_editor_gizmo_drag(g,EditorGizmoAxis::x,p[0],p[1]);check(bool(drag),"Affine basis drag failed.");
    near(*update_editor_gizmo_drag(*drag,q[0],q[1]),.5);bounded(g);
    g=make_editor_gizmo(camera(),800,600,{6,0,0},axes,EditorGizmoMode::move);
    check(!g.triangles.empty(),"Partly offscreen handles disappeared.");bounded(g);
    bool clipped=false;for(const auto& v:g.triangles)clipped=clipped||v.x==1;
    check(clipped,"Viewport edge did not clip a crossing handle.");
    for(const auto mode:{EditorGizmoMode::move,EditorGizmoMode::rotate,EditorGizmoMode::scale})
        for(int x=-8;x<=8;++x)bounded(make_editor_gizmo(camera(),800,600,{static_cast<double>(x),2,0},affine,mode));
}
void affine_rotation() {
    const std::array<V3,3> stretched{{{2,0,0},{0,1,0},{0,0,1}}};
    const auto ellipse=make_editor_gizmo(camera(),800,600,{},stretched,EditorGizmoMode::rotate);
    check(ellipse.handles[2].visible,"Affine rotation ring hidden.");
    near(ellipse.handles[2].screen_points[0][0],480);
    near(ellipse.handles[2].screen_points[24][1],260);
    auto drag=begin_editor_gizmo_drag(ellipse,EditorGizmoAxis::z,480,300);check(bool(drag),"Affine ring drag did not begin.");
    const double root=std::sqrt(.5),quarter=std::numbers::pi_v<double>/4;
    // Screen angle is atan(0.5), but local rotation is 45 degrees.
    near(*update_editor_gizmo_drag(*drag,400+80*root,300-40*root),quarter);
    near(*update_editor_gizmo_drag(*drag,400,260),2*quarter);bounded(ellipse);
    const std::array<V3,3> sheared{{{2,0,0},{1,1,0},{0,0,1}}};
    const auto skew=make_editor_gizmo(camera(),800,600,{},sheared,EditorGizmoMode::rotate);
    drag=begin_editor_gizmo_drag(skew,EditorGizmoAxis::z,480,300);check(bool(drag),"Nonorthogonal rotation drag did not begin.");
    near(*update_editor_gizmo_drag(*drag,400+120*root,300-40*root),quarter);bounded(skew);
    auto singular=sheared;singular[1]={4,0,0};
    check(!make_editor_gizmo(camera(),800,600,{},singular,EditorGizmoMode::rotate).handles[2].visible,"Degenerate ring plane accepted.");
}
void invalid_inputs() {
    auto c=camera();
    check(make_editor_gizmo(c,800,600,{0,0,9},axes,EditorGizmoMode::move).triangles.empty(),"Behind-camera gizmo rendered.");
    check(make_editor_gizmo(c,800,600,{0,0,7.95},axes,EditorGizmoMode::move).triangles.empty(),"Near-plane gizmo rendered.");
    check(make_editor_gizmo(c,0,600,{},axes,EditorGizmoMode::move).triangles.empty(),"Zero extent accepted.");
    auto bad_axes=axes;bad_axes[0]={0,0,0};
    check(!make_editor_gizmo(c,800,600,{},bad_axes,EditorGizmoMode::move).handles[0].visible,"Zero-length basis accepted.");
    const double nan=std::numeric_limits<double>::quiet_NaN();
    c.camera_world[12]=nan;
    check(make_editor_gizmo(c,800,600,{},axes,EditorGizmoMode::move).triangles.empty(),"NaN camera accepted.");
    c=camera();c.camera_world[15]=2;
    check(make_editor_gizmo(c,800,600,{},axes,EditorGizmoMode::move).triangles.empty(),"Nonaffine camera accepted.");
    c=camera();c.camera_world[0]=-1;
    check(make_editor_gizmo(c,800,600,{},axes,EditorGizmoMode::move).triangles.empty(),"Reflected camera accepted.");
    const auto g=make_editor_gizmo(camera(),800,600,{},axes,EditorGizmoMode::move);
    check(!hit_test_editor_gizmo(g,nan,0)&&!hit_test_editor_gizmo(g,440,300,-1),"Invalid hit-test accepted.");
    check(!begin_editor_gizmo_drag(g,static_cast<EditorGizmoAxis>(99),440,300),"Invalid axis accepted.");
    auto drag=begin_editor_gizmo_drag(g,EditorGizmoAxis::x,440,300);check(bool(drag),"Valid gesture missing.");
    check(!update_editor_gizmo_drag(*drag,nan,300),"NaN pointer accepted.");
    near(*update_editor_gizmo_drag(*drag,440,300),0);
}
}
int main() {
    try { move_scale();rotation();affine_basis_and_clipping();affine_rotation();invalid_inputs();std::cout<<"Editor gizmo geometry and drag tests passed.\n";return 0; }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
