// SPDX-License-Identifier: Apache-2.0
// Reviewable two-bone analytic content; no importer, GPU or external art needed.
// This tests the joined Runtime workflow, not active balance/recovery or a crowd.
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace poima;using Json=nlohmann::json;namespace pr=physical_rig;
const std::string hash(64,'a');
std::string id(unsigned n){const auto s=std::to_string(n);return std::string(32-s.size(),'0')+s;}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double actual,double expected,double error=2e-5){check(std::isfinite(actual)&&std::abs(actual-expected)<=error,"Physical integration analytic oracle differs.");}
template<class F>void rejects(F&& operation){bool failed=false;try{operation();}catch(const std::exception&){failed=true;}check(failed,"Invalid integrated physical operation succeeded.");}
std::shared_ptr<ModelAsset> model() {
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(2);m->roots={0};
    m->nodes[0].name="pelvis";m->nodes[0].skin=0;m->nodes[0].primitives={0};
    m->nodes[1].name="tip";m->nodes[1].parent=0;m->nodes[1].position={0,.5,0};
    auto mesh=std::make_shared<MeshAsset>();mesh->vertices={
        {{0,0,0},{0,0,1},{0,0},{1,0,0,1}},{{.2f,0,0},{0,0,1},{1,0},{1,0,0,1}},{{0,.5f,0},{0,0,1},{0,1},{1,0,0,1}}};
    mesh->indices={0,1,2};mesh->has_uv=true;mesh->influences.assign(3,SkinWeight{{0,1,0,0},{.5f,.5f,0,0}});m->primitives={mesh};
    auto inverse_child=identity_matrix();inverse_child[13]=-.5;m->skins.push_back({"two-bone bind",0,{0,1},{identity_matrix(),inverse_child}});
    m->animations.push_back({"one metre per second",2,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,0,0,0},{2,0,0,0}}}}});
    return m;
}
pr::Configuration physical() {
    pr::Configuration c;c.profile.root="pelvis";
    pr::Part p;p.name="pelvis";p.node=0;p.shape=pr::Sphere{.2};p.mass=3;p.body_in_bone.position={.125,0,0};p.linear_damping=0;p.angular_damping=.02;
    auto tip=p;tip.name="tip";tip.node=1;tip.shape=pr::Sphere{.1};tip.mass=1;c.profile.parts={p,tip};
    pr::Joint j;j.name="spine";j.parent="pelvis";j.child="tip";j.parent_frame.position={0,.25,0};j.child_frame.position={0,-.25,0};
    j.limits=pr::SwingTwist{.8,1,-.7,.7};j.rest_stiffness=1;j.rest_damping=.2;j.soft_limit_stiffness=1;j.friction_torque=.01;c.profile.joints={j};return c;
}
std::vector<RuntimeEntityDefinition> actor(std::shared_ptr<const ModelAsset> m,unsigned first,std::string parent={}) {
    RuntimeEntityDefinition rig;rig.id=id(first);rig.parent=std::move(parent);rig.transform.position={0,3,0};
    rig.animation_rig=RuntimeAnimationRig{m,0,0,1,false,true,{}};rig.physical_rig=physical();
    RuntimeEntityDefinition pelvis;pelvis.id=id(first+1);pelvis.parent=rig.id;pelvis.rig_node=RuntimeRigNode{rig.id,0};
    RuntimeEntityDefinition tip;tip.id=id(first+2);tip.parent=pelvis.id;tip.transform.position={0,.5,0};tip.rig_node=RuntimeRigNode{rig.id,1};
    RuntimeEntityDefinition skin;skin.id=id(first+3);skin.parent=pelvis.id;skin.mesh=RuntimeMesh{};skin.mesh->mesh=m->primitives[0];skin.skinned_mesh=RuntimeSkinnedMesh{rig.id,0};
    return {rig,pelvis,tip,skin};
}
RuntimeDefinition definition() {
    RuntimeDefinition d;d.world_id="native-physical-integration";d.authored_revision=13;
    RuntimeEntityDefinition root;root.id=id(1);
    RuntimeEntityDefinition floor;floor.id=id(2);floor.transform.position={0,-.5,0};floor.transform.scale={30,1,30};floor.collider=BoxCollider{};
    RuntimeEntityDefinition camera;camera.id=id(3);camera.transform.position={0,4,8};camera.camera=RuntimeCamera{};
    RuntimeEntityDefinition prop;prop.id=id(4);prop.transform.position={-3,1.5,0};prop.collider=BoxCollider{};prop.collider->motion=BodyMotion::Dynamic;prop.collider->mass=2;
    d.entities={root,floor,camera,prop};const auto m=model();auto authored=actor(m,10,id(1));d.entities.insert(d.entities.end(),authored.begin(),authored.end());
    RuntimeSpawnTemplate recipe;recipe.id=id(900);recipe.name="calibrated physical actor";recipe.root=id(1000);recipe.entities=actor(m,1000);d.templates={recipe};return d;
}
void physical_step(Runtime& r,const std::vector<PhysicalRigCommand>& commands,std::uint32_t ticks=1) {
    r.step(ticks,{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},commands);
}
bool same(const PhysicalRigPartObservation& a,const PhysicalRigPartObservation& b) {
    return a.name==b.name&&a.node_entity==b.node_entity&&a.node==b.node&&a.body_id==b.body_id&&a.pose.position==b.pose.position&&a.pose.rotation==b.pose.rotation&&
        a.linear_velocity==b.linear_velocity&&a.angular_velocity==b.angular_velocity&&a.mass==b.mass&&a.inertia==b.inertia&&a.enrolled==b.enrolled&&a.active==b.active&&a.sleeping==b.sleeping&&
        a.max_linear_velocity==b.max_linear_velocity&&a.max_angular_velocity==b.max_angular_velocity&&a.linear_damping==b.linear_damping&&a.angular_damping==b.angular_damping&&
        a.friction==b.friction&&a.restitution==b.restitution&&a.at_linear_velocity_limit==b.at_linear_velocity_limit&&a.at_angular_velocity_limit==b.at_angular_velocity_limit;
}
void projections(const Runtime& r,const std::string& rig) {
    const auto summary=r.physical_rig(rig);const auto full=r.physical_rig(rig,true,true);
    check(summary&&full&&summary->body_count==2&&summary->joint_count==1&&summary->parts.empty()&&summary->joints.empty(),"Runtime summary missing or evaluated full projections.");
    check(full->parts.size()==2&&full->joints.size()==1,"Runtime full projection missing rows.");
    for(const auto& p:full->parts){const auto selected=r.physical_part(rig,p.name);check(selected&&same(p,*selected),"Full/selective Runtime part DTO fields differ.");}
    near(full->parts[0].mass,3,1e-6);check(!r.physical_part(rig,"absent"),"Absent part acquired an invented observation.");
}
std::vector<Matrix4> palette(const Runtime& r,const std::string& skin=id(13)) {
    const auto scene=r.snapshot(id(3));for(const auto& object:scene.objects)if(object.entity_id==skin){check(object.skin&&object.skin->palette.size()==2,"Runtime publication lost the calibrated skin palette.");return object.skin->palette;}
    throw std::runtime_error("Runtime publication lost the skinned drawable.");
}
double momentum_x(const Runtime& r) {
    const auto p=r.physical_rig(id(10),true,false);check(bool(p),"Momentum rig absent.");double value=0;for(const auto& b:p->parts)value+=b.mass*b.linear_velocity[0];return value;
}
PhysicalRigCommand hit(const Runtime& r,const std::string& part,pr::Vec3 impulse,bool offset) {
    const auto p=r.physical_part(id(10),part);check(bool(p),"Impulse part absent.");auto point=p->pose.position;if(offset)point[0]+=.05;
    return {id(10),false,PhysicalRigImpulse{part,point,impulse}};
}
void workflow_save_and_future() {
    const auto d=definition();Runtime r(d);r.step(12,{});projections(r,id(10));
    const auto before_queries=r.save_snapshot(hash);(void)palette(r);projections(r,id(10));check(r.save_snapshot(hash)==before_queries,"Presentation/selective reads changed complete Runtime archive.");
    near(r.entity(id(11)).world[12],.2,1e-8);
    physical_step(r,{{id(10),true,{}}});projections(r,id(10));
    check(r.physical_rig(id(10))->mode==PhysicalRigMode::Passive,"Committed activation did not transfer ownership.");
    near(momentum_x(r),4,1e-4); // 3kg+1kg, final animation translated at 1m/s.
    const auto p=r.physical_part(id(10),"pelvis");near(p->pose.position[0]-r.entity(id(11)).world[12],.125,1e-6);
    const auto activated=r.save_snapshot(hash);auto first_only=Runtime::from_snapshot(d,hash,activated);
    check(first_only->save_snapshot(hash)==activated,"Activation archive did not restore byte-identically.");
    const auto before_skin=palette(r);const auto first=hit(r,"tip",{0,0,.05},true),second=hit(r,"pelvis",{.03,0,0},false);
    physical_step(*first_only,{first});physical_step(r,{first,second});
    near(momentum_x(r)-momentum_x(*first_only),.03,1e-4);check(r.save_snapshot(hash)!=first_only->save_snapshot(hash),"Distinct localized second impulse was dropped.");
    check(palette(r)!=before_skin,"Solved physical locals were not published into the skin palette.");
    const auto checkpoint=r.save_snapshot(hash);Runtime::validate_snapshot(d,hash,checkpoint);check(r.save_snapshot(hash)==checkpoint,"Detached validation changed the source Runtime.");
    auto cold=Runtime::from_snapshot(d,hash,checkpoint);check(cold->save_snapshot(hash)==checkpoint,"Whole joined Runtime archive did not round-trip exactly.");
    bool physical_contact=false;const auto pelvis_id=r.physical_part(id(10),"pelvis")->body_id,tip_id=r.physical_part(id(10),"tip")->body_id;
    for(unsigned i=0;i<90;++i) {
        r.step(1,{});cold->step(1,{});const auto actual=r.save_snapshot(hash);check(cold->save_snapshot(hash)==actual,"Cold continuation changed a complete Runtime archive field.");
        check(palette(r)==palette(*cold),"Cold continuation changed published skin matrices.");
        const auto saved=Json::parse(actual);
        for(const auto& pair:saved.at("payload").at("physical").at("physics").at("pairs")) {
            const auto a=pair.at("body_a").get<std::uint32_t>(),b=pair.at("body_b").get<std::uint32_t>();
            physical_contact=physical_contact||a==pelvis_id||a==tip_id||b==pelvis_id||b==tip_id;
        }
    }
    check(physical_contact,"Integrated continuation never exercised a real physical-rig contact cache.");
}
RuntimeTransform spawn_at(double x){RuntimeTransform t;t.position={x,3,0};return t;}
void rollback_and_lifecycle() {
    const auto d=definition();Runtime r(d);r.step(3,{});const auto before=r.save_snapshot(hash);const auto summary=r.inspect();
    auto predictor=Runtime::from_snapshot(d,hash,before);const auto born=predictor->change_structure(0,{{id(900),spawn_at(4)}},{}).spawned.at(0);
    // Real birth, real retirement and physics ticks precede the stale revision.
    rejects([&]{r.step(4,{}, {},{}, {},{{0,0,{{id(900),spawn_at(4)}},{}},{1,1,{}, {born}},{3,0,{{id(900),spawn_at(7)}},{}}});});
    check(r.save_snapshot(hash)==before,"Rejected multi-tick structural batch changed complete physical/animation/body-ledger archive.");
    const auto root=r.change_structure(0,{{id(900),spawn_at(4)}},{}).spawned.at(0);check(root==born&&r.inspect().bodies==summary.bodies+2,"Rollback consumed IDs or hierarchical spawn omitted physical proxies.");
    r.step(12,{});physical_step(r,{{root,true,{}}});projections(r,root);check(r.physical_rig(root)->mode==PhysicalRigMode::Passive,"Spawned calibrated rig did not activate.");
    const auto active=r.save_snapshot(hash);auto cold=Runtime::from_snapshot(d,hash,active);check(cold->save_snapshot(hash)==active,"Spawned passive owner archive did not restore exactly.");
    for(unsigned i=0;i<8;++i){r.step(1,{});cold->step(1,{});check(r.save_snapshot(hash)==cold->save_snapshot(hash),"Spawned owner cold continuation diverged.");}
    const auto survivor=*r.physical_part(id(10),"pelvis");r.change_structure(r.structure_revision(),{}, {root});
    check(r.inspect().bodies==summary.bodies&&same(*r.physical_part(id(10),"pelvis"),survivor),"Despawn leaked proxies or changed surviving physical owner.");rejects([&]{(void)r.physical_rig(root);});
    const auto retired=r.save_snapshot(hash);auto restored=Runtime::from_snapshot(d,hash,retired);check(restored->save_snapshot(hash)==retired,"Retired body-ID history did not restore exactly.");
    const auto next=r.change_structure(r.structure_revision(),{{id(900),spawn_at(7)}},{}).spawned.at(0);
    const auto other=restored->change_structure(restored->structure_revision(),{{id(900),spawn_at(7)}},{}).spawned.at(0);check(next==other&&next!=root,"Post-restore lifecycle allocation frontier changed or reused retired public IDs.");
}
void malformed_archives_and_commands() {
    const auto d=definition();Runtime r(d);r.step(12,{});physical_step(r,{{id(10),true,{}}});const auto before=r.save_snapshot(hash);
    rejects([&]{physical_step(r,{{id(10),true,{}}});});check(r.save_snapshot(hash)==before,"Duplicate activation rejection changed full archive.");
    rejects([&]{physical_step(r,{{id(10),false,PhysicalRigImpulse{"absent",{0,3,0},{1,0,0}}}});});check(r.save_snapshot(hash)==before,"Unknown impulse rejection changed full archive.");
    for(unsigned mutation=0;mutation<4;++mutation) {
        auto bad=Json::parse(before);auto& physical=bad["payload"]["physical"];
        if(mutation==0)physical["physics"]["gravity"][0]=.1; // Finite, but not exactly native float32.
        if(mutation==1)physical["rigs"][0]["membership"]["poses"][0]["position"][0]=42;
        if(mutation==2)physical["constraint_order"]=Json::array();
        if(mutation==3)physical["rigs"][0]["membership"]["warm_starts"][0]["name"]="unknown";
        const auto payload=bad.at("payload").dump();bad["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));
        rejects([&]{(void)Runtime::from_snapshot(d,hash,bad.dump());});check(r.save_snapshot(hash)==before,"Rejected detached archive modified source Runtime.");
    }
}
}
int main() {
    try{check(Runtime::available(),"Native simulation is required.");workflow_save_and_future();rollback_and_lifecycle();malformed_archives_and_commands();std::cout<<"Runtime physical integration: 3 native groups passed.\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
