// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
const std::string digest(64,'a');
std::string id(unsigned n){auto value=std::to_string(n);return std::string(32-value.size(),'0')+value;}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double a,double b,double tolerance=4e-5){check(std::isfinite(a) && std::abs(a-b)<=tolerance,"Capsule analytic contact differs.");}
template<class F>void rejects(F&& f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Invalid capsule query was accepted.");}
RuntimeEntityDefinition box(unsigned n,std::array<double,3> p,std::array<float,3> half={1,1,.05f}){RuntimeEntityDefinition e;e.id=id(n);e.transform.position=p;e.collider=BoxCollider{half,BodyMotion::Static};return e;}
RuntimeDefinition walls(){RuntimeDefinition d;d.world_id="capsule-query-native";d.authored_revision=1;d.entities={box(1,{0,0,-2})};return d;}
CapsuleCastRequest query(){CapsuleCastRequest q;q.radius=.1;q.half_height=0;q.distance=4;return q;}
std::shared_ptr<MeshAsset> fence(bool reversed){
    auto mesh=std::make_shared<MeshAsset>();
    const auto rectangle=[&](float left,float right,float bottom,float top){
        const auto start=static_cast<std::uint32_t>(mesh->vertices.size());
        for(const auto p:std::array<std::array<float,3>,4>{{{left,bottom,-2},{right,bottom,-2},{right,top,-2},{left,top,-2}}}){MeshVertex v;v.position=p;v.normal={0,0,1};v.tangent={1,0,0,1};mesh->vertices.push_back(v);}
        if(reversed)mesh->indices.insert(mesh->indices.end(),{start,start+2,start+1,start,start+3,start+2});
        else mesh->indices.insert(mesh->indices.end(),{start,start+1,start+2,start,start+2,start+3});
    };
    rectangle(-1,-.2f,-1,1);rectangle(.2f,1,-1,1);rectangle(-.2f,.2f,.4f,1);rectangle(-.2f,.2f,-1,-.4f);return mesh;
}
void primitive_sweep_and_zero_height(){
    Runtime r(walls());const auto saved=r.save_snapshot(digest);auto q=query();const auto sphere=r.capsule_cast(q);
    check(sphere && sphere->entity==id(1) && !sphere->initial_overlap && !sphere->triangle && sphere->normal,"Sphere did not return an ordinary physical hit.");
    near(sphere->distance,1.85);near(sphere->fraction,1.85/4);near(sphere->point_on_obstacle[2],-1.95);near(sphere->point_on_query[2],-1.95);near((*sphere->normal)[2],1);check(sphere->penetration_depth==0,"Sweep invented penetration state.");
    q.half_height=.4;const auto upright=r.capsule_cast(q);check(upright.has_value(),"Upright capsule missed wall.");near(upright->distance,sphere->distance);
    q.rotation={std::sqrt(.5),0,0,std::sqrt(.5)};const auto lengthwise=r.capsule_cast(q);check(lengthwise.has_value(),"Lengthwise capsule missed wall.");near(lengthwise->distance,1.45);
    q.position={0,0,-4};q.direction={0,0,1};const auto back=r.capsule_cast(q);check(back && back->normal,"Back-side convex hit absent.");near((*back->normal)[2],-1);
    check(r.save_snapshot(digest)==saved,"Volume queries changed complete native state.");
}
void triangle_apertures_and_winding(){
    for(bool reversed:{false,true}){
        RuntimeDefinition d;d.world_id="capsule-fence";d.authored_revision=1;RuntimeEntityDefinition rail;rail.id=id(1);rail.mesh_collider=MeshCollider{fence(reversed)};d.entities={rail};Runtime r(d);auto q=query();
        check(!r.raycast({q.position,q.direction,q.distance,{}}),"Fence fixture center ray unexpectedly hit.");
        check(!r.capsule_cast(q),"A fitting sphere could not pass the actual mesh opening.");
        q.radius=.25;const auto thick=r.capsule_cast(q);check(thick && thick->triangle && *thick->triangle<8 && thick->normal && !thick->initial_overlap,"Volume did not hit the thin rail missed by its center ray.");
        check(thick->distance>1.8 && thick->distance<2,"Triangle capsule contact is not on the thin plane.");
        q.radius=.05;q.half_height=.5;check(r.capsule_cast(q).has_value(),"Tall capsule passed through the top/bottom rail.");
        q.half_height=.2;check(!r.capsule_cast(q),"Short narrow capsule did not fit the geometric aperture.");
        q.position={0,0,-4};q.direction={0,0,1};q.radius=.25;q.half_height=0;const auto back=r.capsule_cast(q);check(back && back->triangle,"Reverse-direction/winding mesh hit absent.");
        near(back->distance,thick->distance);check(r.inspect().tick==0,"Mesh query advanced simulation.");
    }
}
void initial_contacts_and_overlap_only(){
    Runtime r(walls());auto q=query();q.position={0,0,-2};const auto initial=r.capsule_cast(q);
    check(initial && initial->initial_overlap && initial->fraction==0 && initial->distance==0 && initial->penetration_depth>=0,"Start-inside shape was reported as free motion.");
    q.distance=0;const auto overlap=r.capsule_cast(q);check(overlap && overlap->initial_overlap && overlap->fraction==0 && overlap->distance==0,"Zero distance bypassed overlap detection.");
    q.position={0,0,0};check(!r.capsule_cast(q),"Separated zero-distance shape was reported overlapping.");
    q.position={0,0,-1.951};q.radius=.002;const auto touching=r.capsule_cast(q);check(touching && touching->initial_overlap,"Boundary overlap was lost at zero displacement.");
}
void stable_ties_and_filters(){
    auto d=walls();d.entities.push_back(box(2,{0,0,-2}));RuntimeEntityDefinition cosmetic;cosmetic.id=id(3);d.entities.push_back(cosmetic);Runtime r(d);auto q=query();
    for(unsigned i=0;i<4;++i)check(r.capsule_cast(q)->entity==id(1),"Equal-fraction hit ignored stable entity tie order.");
    q.ignore={id(1)};check(r.capsule_cast(q)->entity==id(2),"Explicit body ignore was not applied.");q.ignore={id(1),id(2),id(3)};check(!r.capsule_cast(q),"Ignored body or valid cosmetic identity fabricated a hit.");
    q.ignore={id(99)};rejects([&]{r.capsule_cast(q);});q.ignore={id(1),id(1)};rejects([&]{r.capsule_cast(q);});
    auto shifted=walls();shifted.entities[0].transform.position={900000,0,-2};Runtime far(shifted);q=query();q.position[0]=900000;const auto hit=far.capsule_cast(q);check(hit.has_value(),"Far-origin query lost its local narrow-phase contact.");near(hit->point_on_obstacle[0],900000,1e-3);near(hit->distance,1.85);
    auto boundary=walls();boundary.entities[0]=box(1,{1e6-.04,0,0},{.1f,1,1});Runtime edge(boundary);q=query();q.position={1e6,0,0};q.radius=.2;q.distance=0;
    const auto support=edge.capsule_cast(q);check(support && support->initial_overlap && support->point_on_obstacle[0]>1e6,"Valid boundary-center contact was rejected by the input-only position domain.");
}
void invalid_requests_and_candidate_exhaustion(){
    Runtime r(walls());const auto saved=r.save_snapshot(digest);
    for(unsigned kind=0;kind<14;++kind){auto q=query();
        if(kind==0)q.radius=0;if(kind==1)q.half_height=-.01;if(kind==2)q.radius=10.01;if(kind==3)q.half_height=10.01;
        if(kind==4)q.rotation={0,0,0,0};if(kind==5)q.position[0]=1e6+1;if(kind==6)q.direction={0,0,0};if(kind==7){q.distance=0;q.direction={0,0,0};}
        if(kind==8)q.distance=-1;if(kind==9)q.distance=10001;if(kind==10)q.direction[0]=1e6+1;if(kind==11)q.rotation[2]=std::numeric_limits<double>::quiet_NaN();
        if(kind==12)q.position[1]=std::numeric_limits<double>::infinity();if(kind==13){q.position[2]=-1e6;q.direction={0,0,-1};q.distance=1;}
        rejects([&]{r.capsule_cast(q);});check(r.save_snapshot(digest)==saved,"Rejected capsule request changed the complete native archive.");
    }
    RuntimeDefinition dense;dense.world_id="capsule-budget";dense.authored_revision=1;
    for(unsigned n=1;n<=capsule_query_body_budget+1;++n)dense.entities.push_back(box(n,{0,0,0},{.01f,.01f,.01f}));
    Runtime many(dense);const auto original=many.save_snapshot(digest);auto q=query();q.radius=1;q.distance=0;
    rejects([&]{many.capsule_cast(q);});check(many.save_snapshot(digest)==original,"Exhausted query mutated state or silently reported clearance.");
}
}
int main(){try{
    if(!Runtime::available()){std::cout<<"Native capsule queries unavailable; simulation groups skipped.\n";return 0;}
    primitive_sweep_and_zero_height();triangle_apertures_and_winding();initial_contacts_and_overlap_only();stable_ties_and_filters();invalid_requests_and_candidate_exhaustion();
    std::cout<<"5 native capsule query groups passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
