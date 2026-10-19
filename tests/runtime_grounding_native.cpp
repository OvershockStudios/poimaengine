// SPDX-License-Identifier: Apache-2.0
// Real Jolt supports plus independent sole geometry and complete native saves.
#include "poima/runtime.hpp"
#include "poima/runtime_animation.hpp"
#include "poima/runtime_grounding.hpp"
#include "poima/jobs.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace poima;
namespace {
using V=std::array<double,3>;using Q=std::array<double,4>;using Json=nlohmann::json;
const std::string hash(64,'b');std::filesystem::path output;unsigned archives=0;
void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
std::string id(unsigned v){auto s=std::to_string(v);return std::string(32-s.size(),'0')+s;}
double length(const V& v){return std::hypot(v[0],v[1],v[2]);}
V sub(V a,const V& b){for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
V cross(const V& a,const V& b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
V unit(V v){const auto n=length(v);require(std::isfinite(n)&&n>0,"Independent direction is invalid.");for(auto& x:v)x/=n;return v;}
void near(double a,double b,double e=3e-5){require(std::isfinite(a)&&std::abs(a-b)<=e,"Independent contact scalar differs.");}
void near(const V& a,const V& b,double e=3e-5){for(unsigned i=0;i<3;++i)near(a[i],b[i],e);}
template<class F>void rejects(F&& f,const char* text=nullptr){bool rejected=false;try{f();}catch(const std::exception& e){rejected=!text||std::string(e.what()).find(text)!=std::string::npos;}require(rejected,"Expected original native rejection did not occur.");}
V point(const Matrix4& m,const V& p){V v{};for(unsigned r=0;r<3;++r)v[r]=m[12+r]+m[r]*p[0]+m[4+r]*p[1]+m[8+r]*p[2];return v;}
V normal(const Matrix4& m,const V& p){
    // Independent cofactor inverse transpose, rather than solver quaternion.
    const V a{m[0],m[1],m[2]},b{m[4],m[5],m[6]},c{m[8],m[9],m[10]};
    const auto x=cross(b,c),y=cross(c,a),z=cross(a,b);V n{};
    const auto determinant=a[0]*x[0]+a[1]*x[1]+a[2]*x[2];require(determinant>0,"Independent sole frame is not positive.");
    for(unsigned r=0;r<3;++r)n[r]=(p[0]*x[r]+p[1]*y[r]+p[2]*z[r])/determinant;return unit(n);
}
Q around_z(double angle){return {0,0,std::sin(angle/2),std::cos(angle/2)};}
std::string retain(const std::string& label,const std::string& bytes){
    if(!output.empty()){const auto path=output/(std::to_string(++archives)+"-"+label+".json");std::ofstream stream(path,std::ios::binary);require(bool(stream),"Cannot open original checkpoint archive.");stream.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));stream.close();require(bool(stream),"Cannot retain complete checkpoint bytes.");}return bytes;
}
RuntimeDefinition definition(unsigned feet=1,double lift=0,double foot_angle=0){
    RuntimeDefinition d;d.world_id="grounding-native";d.authored_revision=4;
    auto model=std::make_shared<ModelAsset>();model->nodes.resize(3*feet);
    RuntimeEntityDefinition rig;rig.id=id(1);rig.animation_rig=RuntimeAnimationRig{model};
    for(unsigned foot=0;foot<feet;++foot){
        const unsigned base=3*foot;model->roots.push_back(base);
        for(unsigned j=0;j<3;++j){auto& n=model->nodes[base+j];n.name="Original analytic foot "+std::to_string(base+j);n.parent=j?static_cast<int>(base+j-1):-1;}
        model->nodes[base].position={feet==1?0.:(foot==0?-.3:.3),1.8,0};model->nodes[base+1].position={.5,-.7,0};
        // Choose the literal animated sole height independently of end tilt.
        model->nodes[base+2].position={-.5,-.7+lift+.2*std::cos(foot_angle)-.2,0};model->nodes[base+2].rotation=around_z(foot_angle);
        RuntimeTwoBoneIk chain;chain.slot=foot+1;chain.root=base;chain.middle=base+1;chain.end=base+2;chain.space=IkSpace::Rig;chain.target={0,.4,0};chain.pole={1,1,0};
        chain.metric_policy=RuntimeIkMetricPolicy::NearUniformV1;chain.grounding=RuntimeGroundingDefinition{};chain.grounding->sole_offset={0,-.2,0};chain.grounding->reference_height=.2;chain.grounding->max_correction=.6;
        rig.animation_rig->ik.push_back(chain);
    }
    // A deliberately invalid evaluated metric only after the first tick.
    model->animations.push_back({"Late metric failure",1./32,{{0,AnimationPath::scale,AnimationInterpolation::step,{0,1./64,1./32},{{1,1,1,0},{1,1,1,0},{1.01,1,1,0}}}}});
    d.entities.push_back(rig);
    for(unsigned n=0;n<model->nodes.size();++n){RuntimeEntityDefinition node;node.id=id(10+n);node.parent=model->nodes[n].parent<0?id(1):id(10+static_cast<unsigned>(model->nodes[n].parent));node.transform={model->nodes[n].position,model->nodes[n].rotation,model->nodes[n].scale};node.rig_node=RuntimeRigNode{id(1),n};d.entities.push_back(node);}
    RuntimeEntityDefinition floor;floor.id=id(100);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{};floor.collider->half_extents={5,.5,5};floor.collider->motion=BodyMotion::Static;d.entities.push_back(floor);return d;
}
RuntimeEntityDefinition& entity(RuntimeDefinition& d,const std::string& key){auto i=std::find_if(d.entities.begin(),d.entities.end(),[&](const auto& e){return e.id==key;});require(i!=d.entities.end(),"Fixture entity missing.");return *i;}
RuntimeGroundingState state(Runtime& r,unsigned slot=1){const auto v=r.ik(id(1),slot);require(v&&v->grounding,"Grounding state missing.");return *v->grounding;}
RuntimeGroundingState state(RuntimeAnimations& r,unsigned slot=1){const auto v=r.ik_state(id(1),slot);require(v&&v->grounding,"Animation grounding state missing.");return *v->grounding;}
void achieved(Runtime& r,const RuntimeDefinition& d,unsigned slot,const V& expected_point,const V& expected_normal){
    const auto& chain=d.entities[0].animation_rig->ik[slot-1];const auto& profile=*chain.grounding;const auto actual=r.entity(id(10+chain.end)).world;const auto sole=point(actual,profile.sole_offset),up=normal(actual,profile.sole_up);const auto s=state(r,slot);
    require(s.status=="contact"&&s.hit&&s.cast&&s.weight>0,"Actual contact decision absent.");near(sole,expected_point);near(up,expected_normal);near(s.actual_sole,sole,2e-12);near(s.actual_normal,up,2e-12);require(s.position_error&&s.normal_error,"Fresh hit has no achieved residual.");near(*s.position_error,length(sub(sole,s.hit->position)),2e-12);near(*s.normal_error,std::atan2(length(cross(up,s.hit->normal)),std::clamp(up[0]*s.hit->normal[0]+up[1]*s.hit->normal[1]+up[2]*s.hit->normal[2],-1.,1.)),2e-12);
}
void phase_queries_history_and_failure(){
    const auto d=definition(2);RuntimeAnimations animation(d);auto wrapper=identity_matrix();const auto owner=std::this_thread::get_id();unsigned calls=0;std::vector<RuntimeRay> rays;
    const grounding::Query query=[&](const std::string& rig,const RuntimeRay& ray)->std::optional<RuntimeRayHit>{
        require(rig==id(1)&&std::this_thread::get_id()==owner,"Contact query ran on a job or wrong rig.");
        // Both feet must use the same original uncorrected frame. Earlier
        // successful foot correction cannot feed a later input ray.
        near(ray.origin,{calls==0?-.3:.3,.5,0},2e-12);near(ray.direction,{0,-1,0},0);near(ray.distance,.9,2e-12);rays.push_back(ray);++calls;
        RuntimeRayHit hit;hit.entity=id(100);hit.position={ray.origin[0],0,ray.origin[2]};hit.normal=V{0,1,0};hit.distance=.5;hit.fraction=.5/.9;return hit;
    };
    auto world=[&](const std::string& rig)->const Matrix4&{require(rig==id(1),"Wrong physical wrapper.");return wrapper;};
    require(animation.has_grounding()&&animation.owns_grounding(id(1),1)&&animation.owns_grounding(id(1),2),"Frozen grounding ownership absent.");
    const auto initial=animation.save_state(0);require(Json::parse(initial)["version"]==7,"Grounding needs complete animation version7.");
    animation.sample(0,world);require(calls==0&&state(animation).status=="uninitialized","Initialization performed a contact query.");
    animation.sample(1,world,query);require(calls==2&&state(animation,1).casts==1&&state(animation,2).casts==1,"Exactly one contact query per eligible foot required.");
    const auto saved=retain("animation-two-feet",animation.save_state(1));
    for(unsigned n=0;n<3;++n){(void)animation.ik_state(id(1),1);(void)animation.state(id(1),1);animation.sample(1,world);require(animation.save_state(1)==saved,"Queryless sample/read changed retained decision or complete animation state.");}
    require(calls==2,"Read/sample redundantly queried supports.");
    RuntimeAnimations cold(d);cold.load_state(saved,1);cold.sample(1,world);require(cold.save_state(1)==saved&&state(cold).casts==1&&calls==2,"Restore refreshed rather than retaining the exact decision.");
    auto cp=animation.checkpoint();unsigned attempted=0;
    const grounding::Query failed=[&](const std::string&,const RuntimeRay& ray)->std::optional<RuntimeRayHit>{if(++attempted==2)throw std::runtime_error("actual second query failure");RuntimeRayHit hit;hit.entity=id(100);hit.position={ray.origin[0],.05,0};hit.normal=V{0,1,0};return hit;};
    rejects([&]{animation.sample(2,world,failed);},"second query failure");require(attempted==2&&animation.save_state(1)==saved,"Second-foot failure published an earlier decision/history.");
    animation.restore(cp);require(animation.save_state(1)==saved,"Checkpoint changed complete grounding state.");
    RuntimeIkCommand manual{id(1),1,true,IkSpace::Rig,{0,.3,0},{1,1,0},1};rejects([&]{animation.apply_ik({manual});},"owns");require(animation.save_state(1)==saved,"Manual/automatic conflict published controls.");
    auto bad=Json::parse(saved);bad["rigs"][0]["ik"][0]["grounding"]["state"]["status"]="invented";rejects([&]{animation.load_state(bad.dump(),1);});require(animation.save_state(1)==saved,"Invalid grounding cache load changed state.");
    auto ordinary=d;for(auto& c:ordinary.entities[0].animation_rig->ik)c.grounding.reset();RuntimeAnimations legacy(ordinary);require(Json::parse(legacy.save_state(0))["version"]==4,"Omitted grounding changed old animation version.");
}
void malformed_and_wrapper_atomicity(){
    for(unsigned kind=0;kind<11;++kind){auto d=definition();auto& g=*d.entities[0].animation_rig->ik[0].grounding;
        if(kind==0)g.sole_up={0,0,0};if(kind==1)g.sole_forward={0,1,0};if(kind==2)g.sole_offset[0]=std::numeric_limits<double>::infinity();if(kind==3)g.reference_height=std::numeric_limits<double>::quiet_NaN();if(kind==4)g.plant_height=g.lift_height;if(kind==5)g.probe_above=0;if(kind==6)g.max_correction=0;if(kind==7)g.max_slope_degrees=81;if(kind==8)g.sole_up={0,2,0};if(kind==9)g.probe_below=11;if(kind==10)d.entities[0].animation_rig->ik[0].end_orientation=RuntimeIkEndOrientation{};
        rejects([&]{RuntimeAnimations invalid(d);});
    }
    auto d=definition();auto third=d.entities[0].animation_rig->ik[0];third.slot=2;d.entities[0].animation_rig->ik.push_back(third);third.slot=3;d.entities[0].animation_rig->ik.push_back(third);rejects([&]{RuntimeAnimations invalid(d);},"two feet");
    // Independent disjoint mapped rigs isolate the world cap from per-rig
    // topology/overlap checks. Exactly64 feet is accepted; the65th rejects.
    const auto pair=definition(2);RuntimeDefinition population;population.world_id="grounding-capacity";population.authored_revision=1;
    for(unsigned rig=0;rig<33;++rig){
        const auto root=id(1000+100*rig);
        for(unsigned index=0;index<7;++index){auto copy=pair.entities[index];copy.id=index? id(1000+100*rig+index):root;
            if(index){const auto model_node=index-1;const auto parent=pair.entities[0].animation_rig->model->nodes[model_node].parent;copy.parent=parent<0?root:id(1001+100*rig+static_cast<unsigned>(parent));copy.rig_node->rig=root;}
            population.entities.push_back(std::move(copy));
        }
        if(rig==31){RuntimeAnimations exact_capacity(population);require(exact_capacity.has_grounding(),"Exactly64 disjoint automatic feet rejected.");}
    }
    rejects([&]{RuntimeAnimations excessive(population);},"64 per world");
    RuntimeAnimations animation(definition());const auto original=animation.save_state(0);unsigned calls=0;grounding::Query query=[&](const std::string&,const RuntimeRay&)->std::optional<RuntimeRayHit>{++calls;return {};};
    for(unsigned k=0;k<3;++k){auto matrix=identity_matrix();if(k==0)matrix[0]=1.1;if(k==1){matrix[0]=-1;matrix[10]=-1;matrix[5]=-1;}if(k==2){const auto angle=.1;matrix[0]=std::cos(angle);matrix[1]=std::sin(angle);matrix[4]=-std::sin(angle);matrix[5]=std::cos(angle);}rejects([&]{animation.sample(1,[&](const std::string&)->const Matrix4&{return matrix;},query);});require(calls==0&&animation.save_state(0)==original,"Invalid wrapper queried/published before admission.");}
}
void flat_slope_and_query_filters(){
    auto d=definition();
    // A real self kinematic collider and a closer unrelated dynamic collider
    // would mask the static floor if filtering occurred after closest-hit.
    d.entities[0].collider=BoxCollider{};d.entities[0].collider->half_extents={.03,.03,.03};d.entities[0].collider->motion=BodyMotion::Kinematic;
    RuntimeEntityDefinition dynamic;dynamic.id=id(101);dynamic.transform.position={0,.15,0};dynamic.collider=BoxCollider{};dynamic.collider->half_extents={.4,.04,.4};dynamic.collider->motion=BodyMotion::Dynamic;d.entities.push_back(dynamic);
    Runtime flat(d);require(state(flat).casts==0,"Runtime construction queried supports.");flat.step(1,{});require(state(flat).decision_tick==1&&state(flat).casts==1&&state(flat).hit->entity==id(100),"Nearest eligible static support/self-ignore failed.");achieved(flat,d,1,{0,0,0},{0,1,0});
    const auto saved=retain("flat-contact",flat.save_snapshot(hash));for(unsigned i=0;i<3;++i){(void)flat.entity(id(12));(void)flat.ik(id(1),1);(void)flat.inspect();require(flat.save_snapshot(hash)==saved,"Read/save altered contact state.");}require(state(flat).casts==1,"Read refreshed support decision.");
    const double angle=.3;auto slope=definition();auto& floor=entity(slope,id(100));floor.transform.rotation=around_z(angle);const V n{-std::sin(angle),std::cos(angle),0};for(unsigned i=0;i<3;++i)floor.transform.position[i]=-.5*n[i];Runtime tilted(slope);tilted.step(1,{});achieved(tilted,slope,1,{0,0,0},n);require(state(tilted).hit->entity==id(100),"Real rotated support absent.");
    // Uniform scaled and yawed wrappers retain a predictable physical sole.
    auto transformed=definition();transformed.entities[0].transform.scale={1.25,1.25,1.25};transformed.entities[0].transform.rotation={0,std::sin(.4),0,std::cos(.4)};transformed.entities[0].transform.position={2,0,1};Runtime scaled(transformed);scaled.step(1,{});achieved(scaled,transformed,1,{2,0,1},{0,1,0});
    // End scale is outside the root/middle metric restriction. Non-axis sole
    // calibration must align the actual inverse-transpose normal, not just the
    // logical quaternion's image of the unscaled local normal.
    for(const auto angle:{0.,.3}) {
        auto affine=definition();auto model=std::make_shared<ModelAsset>(*affine.entities[0].animation_rig->model);
        const V original_scale{2,1,.5};model->nodes[2].scale=original_scale;
        affine.entities[0].animation_rig->model=model;entity(affine,id(12)).transform.scale=original_scale;
        auto& profile=*affine.entities[0].animation_rig->ik[0].grounding;
        profile.sole_offset={-.1,-.2,.04};profile.sole_up={std::sqrt(.5),std::sqrt(.5),0};profile.sole_forward={0,0,1};
        const V n{-std::sin(angle),std::cos(angle),0};auto& support=entity(affine,id(100));support.transform.rotation=around_z(angle);
        for(unsigned i=0;i<3;++i)support.transform.position[i]=-.5*n[i];
        Runtime end_scaled(affine);end_scaled.step(1,{});
        // Prepared sole=(-.2,.2,.02); the vertical cast meets this real plane.
        achieved(end_scaled,affine,1,{-.2,-.2*std::tan(angle),.02},n);
        require(entity(affine,id(12)).transform.scale==original_scale&&model->nodes[2].scale==original_scale&&
            end_scaled.entity(id(12)).local.scale==original_scale,"Grounding changed original nonuniform end scale.");
        const auto saved=retain(angle==0?"affine-end-flat":"affine-end-slope",end_scaled.save_snapshot(hash));
        auto restored=Runtime::from_snapshot(affine,hash,saved);
        require(restored->save_snapshot(hash)==saved,"Nonuniform end contact changed complete cold snapshot.");
    }
}
void live_platform_and_exact_cold_continuation(){
    auto d=definition();entity(d,id(100)).collider->motion=BodyMotion::Kinematic;Runtime warm(d);KinematicTarget move;move.entity=id(100);move.position={0,-.4,0};move.duration_ticks=1;
    warm.step(1,{}, {move});require(state(warm).casts==1&&state(warm).decision_tick==1,"Final phase omitted current tick query.");achieved(warm,d,1,{0,.1,0},{0,1,0});near(state(warm).hit->position[1],.1);
    const auto saved=retain("platform-before-cold",warm.save_snapshot(hash));auto cold=Runtime::from_snapshot(d,hash,saved);require(cold->save_snapshot(hash)==saved&&state(*cold).casts==1,"Cold reconstruction changed cached decision/full state.");
    // Current postphysics platform rotation, not a previous cached support.
    KinematicTarget rotate;rotate.entity=id(100);rotate.position={0,-.4,0};rotate.rotation=around_z(.12);rotate.duration_ticks=2;
    warm.step(2,{}, {rotate});cold->step(1,{}, {rotate});cold->step(1,{});const auto continued=retain("platform-warm-continuation",warm.save_snapshot(hash));require(continued==retain("platform-cold-continuation",cold->save_snapshot(hash)),"Whole platform/grounding continuation differs across batch/cold restore.");
    const V n{-std::sin(.12),std::cos(.12),0};const double y=-.4+.5/std::cos(.12);achieved(warm,d,1,{0,y,0},n);require(state(warm).casts==3,"One postphysics query per actual tick required.");
    // A new support height must be queried; no world-space lock is retained.
    move.position={0,-.45,0};move.rotation=around_z(.12);warm.step(1,{}, {move});near(state(warm).hit->position[1],-.45+.5/std::cos(.12));require(state(warm).casts==4,"Moving support was not requeried.");
}
void fallbacks_and_lift_fade(){
    for(unsigned kind=0;kind<7;++kind){auto d=kind==2?definition(1,.2):definition();const char* expected="miss";auto& g=*d.entities[0].animation_rig->ik[0].grounding;
        if(kind==0)entity(d,id(100)).transform.position[0]=20;
        if(kind==1){d.entities[0].animation_rig->ik[0].enabled=false;expected="disabled";}
        if(kind==2)expected="lifted";
        if(kind==3){g.max_correction=.1;expected="correction-limit";}
        if(kind==4){entity(d,id(100)).transform.position[1]=-.7;expected="unreachable";}
        if(kind==5){entity(d,id(100)).transform.position[1]=.1;expected="no-normal";}
        if(kind==6){auto& floor=entity(d,id(100));floor.transform.rotation=around_z(.5);floor.transform.position={.5*std::sin(.5),-.5*std::cos(.5),0};g.max_slope_degrees=10;expected="slope";}
        Runtime r(d);const auto before=r.entity(id(12)).local;r.step(1,{});const auto s=state(r);require(s.status==expected&&s.weight==0&&!r.ik(id(1),1)->enabled,"Ineligible support did not preserve the incoming animation.");require(r.entity(id(12)).local.position==before.position&&r.entity(id(12)).local.rotation==before.rotation&&r.entity(id(12)).local.scale==before.scale,"Fallback snapped the original foot.");require(s.casts==((kind==1||kind==2)?0u:1u),"Fallback query count differs.");
        if(kind==3||kind==4||kind==6)require(s.hit&&s.position_error&&s.normal_error,"Fresh rejected support diagnostics were erased.");else require(!s.hit&&!s.position_error&&!s.normal_error,"Miss/inactive retained stale or fabricated support.");
        retain(std::string("fallback-")+expected,r.save_snapshot(hash));
    }
    const double lift=.09;auto d=definition(1,lift,.4);Runtime partial(d);partial.step(1,{});const auto s=state(partial);require(s.status=="contact","Fade sample unexpectedly rejected.");near(s.weight,.5,2e-12);near(partial.ik(id(1),1)->weight,.5,2e-12);near(partial.ik(id(1),1)->end_orientation->goal.weight,.5,2e-12);require(*s.position_error>1e-3&&*s.normal_error>.05,"Partial contact falsely claimed exact plane conformance.");
    auto edge=definition(1,.15-1e-6,.4),lifted=definition(1,.15+1e-6,.4);Runtime near_lift(edge),over_lift(lifted);near_lift.step(1,{});over_lift.step(1,{});require(state(near_lift).status=="contact"&&state(near_lift).weight<1e-8&&state(over_lift).status=="lifted"&&state(over_lift).casts==0,"Lift cutoff contribution/cast behavior differs.");const auto a=near_lift.entity(id(12)).world,b=over_lift.entity(id(12)).world;near(normal(a,{0,1,0}),normal(b,{0,1,0}),1e-7);near(point(a,{0,-.2,0})[1],point(b,{0,-.2,0})[1],3e-6);
}
void complete_late_rollback_and_ownership(){
    auto d=definition();RuntimeEntityDefinition body;body.id=id(200);body.transform.position={3,5,0};body.collider=BoxCollider{};body.collider->motion=BodyMotion::Dynamic;d.entities.push_back(body);
    Runtime value(d),witness(d);const auto original=retain("before-late-failure",value.save_snapshot(hash));const auto initial_body=value.entity(id(200)).world;AnimationCommand invalid{id(1),0,0,1,false,true};witness.step(1,{}, {}, {},{invalid});require(state(witness).status=="contact"&&state(witness).casts==1&&witness.entity(id(200)).world!=initial_body,"First tick did not exercise real accepted contact/physics.");
    rejects([&]{value.step(2,{}, {}, {},{invalid});});require(retain("after-late-failure",value.save_snapshot(hash))==original&&value.entity(id(200)).world==initial_body&&state(value).casts==0,"Late metric failure lost whole physics/control/decision/history rollback.");
    RuntimeIkCommand manual{id(1),1,true,IkSpace::Rig,{0,.2,0},{1,1,0},1};rejects([&]{value.step(1,{}, {}, {}, {}, {},{manual});},"owns");require(value.save_snapshot(hash)==original,"Manual/automatic ownership rejection mutated full native state.");
    value.step(1,{});const auto completed=retain("warm-source",value.save_snapshot(hash));value.step(2,{});auto cold=Runtime::from_snapshot(d,hash,completed);require(retain("cold-restored",cold->save_snapshot(hash))==completed,"Cold full snapshot differs.");cold->step(2,{});require(retain("warm-continued",value.save_snapshot(hash))==retain("cold-continued",cold->save_snapshot(hash)),"Complete cold continuation differs.");
}
}
int main(int argc,char** argv){try{
    if(argc!=1){require(argc==3&&std::string(argv[1])=="--output","Use optional --output FRESH.");output=std::filesystem::absolute(argv[2]);require(!std::filesystem::exists(output)&&std::filesystem::is_directory(output.parent_path()),"Output must be fresh with existing parent.");require(std::filesystem::create_directory(output),"Cannot create owned checkpoint directory.");}
    phase_queries_history_and_failure();malformed_and_wrapper_atomicity();
    if(!Runtime::available()){std::cout<<"2 portable grounding groups passed; native physics unavailable\n";return 0;}
    flat_slope_and_query_filters();live_platform_and_exact_cold_continuation();fallbacks_and_lift_fade();complete_late_rollback_and_ownership();std::cout<<"6 native grounding groups passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
