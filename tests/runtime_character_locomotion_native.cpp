// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
using namespace poima;
using Json=nlohmann::json;
namespace {
const std::string hash(64,'a');
std::string id(unsigned value) {std::ostringstream out;out<<std::hex<<std::setw(32)<<std::setfill('0')<<value;return out.str();}
const auto floor_id=id(1),step_id=id(2),roof_id=id(3),ramp_id=id(4),platform_id=id(5),actor=id(100),camera=id(101);
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void near(double a,double b,double tolerance,const char* message) {
    check(std::isfinite(a) && std::isfinite(b) && std::abs(a-b)<=tolerance,message);
}
template<class F>void rejects(F action) {
    try {action();}catch(const std::exception&) {return;}
    throw std::runtime_error("Invalid locomotion definition, operation or snapshot was admitted.");
}
RuntimeEntityDefinition box(const std::string& identity,std::array<double,3> position,
    std::array<double,3> size,BodyMotion motion=BodyMotion::Static) {
    RuntimeEntityDefinition e;e.id=identity;e.transform.position=position;e.transform.scale=size;
    e.collider=BoxCollider{};e.collider->motion=motion;return e;
}
RuntimeDefinition flat(bool virtual_mode=true,bool with_camera=true,bool stance=false) {
    RuntimeDefinition d;d.world_id="locomotion-native";d.authored_revision=1;
    d.entities.push_back(box(floor_id,{0,-.5,0},{40,1,40}));
    RuntimeEntityDefinition e;e.id=actor;e.transform.position={-1,.1,0};e.character=CharacterController{};
    if(virtual_mode)e.character->locomotion=RuntimeCharacterLocomotionProfile{};
    if(stance)e.character->stance=RuntimeCharacterStanceProfile{};
    if(with_camera)e.character->camera=camera;
    d.entities.push_back(e);
    if(with_camera) {RuntimeEntityDefinition c;c.id=camera;c.parent=actor;c.transform.position={.05,1.6,.02};c.camera=RuntimeCamera{};d.entities.push_back(c);}
    return d;
}
RuntimeInput right() {RuntimeInput input;input.entity=actor;input.move={1,0};return input;}
void crouch(Runtime& runtime,bool requested) {runtime.step(1,{}, {}, {}, {}, {}, {}, {}, {},{{actor,requested}});}
std::array<double,3> foot(const Runtime& runtime,const std::string& identity=actor) {
    const auto world=runtime.entity(identity).world;return {world[12],world[13],world[14]};
}
Json& row(Json& envelope,const std::string& identity) {
    for(auto& entity:envelope["payload"]["entities"])if(entity["id"]==identity)return entity;
    throw std::runtime_error("Snapshot entity missing.");
}
std::string seal(Json value) {
    const auto data=value["payload"].dump();value["sha256"]=sha256(std::as_bytes(std::span(data.data(),data.size())));return value.dump();
}
void stairs_and_height_limit() {
    for(const double height:{.2,.4,.6}) {
        auto d=flat();d.entities.push_back(box(step_id,{1,height/2,0},{2,height,4}));
        Runtime r(d);r.step(120,{});const auto before=foot(r);r.step(32,{right()});const auto after=foot(r);
        check(r.entity(actor).locomotion.has_value(),"Virtual locomotion diagnostics absent.");
        if(height<=.4) {
            check(after[0]>.7,"Supported stair did not produce real horizontal progress.");
            near(after[1]-before[1],height,.06,"Actual stair foot rise differs from authored step height.");
            check(r.entity(actor).ground=="on_ground","Accepted stair lacks support.");
        }else {
            check(after[0]<.1 && after[1]-before[1]<.1,"Over-height wall was stepped through or climbed.");
            for(unsigned n=0;n<10;++n) {
                const auto before_tick=foot(r);r.step(1,{right()});const auto after_tick=foot(r);const auto observed=r.entity(actor);
                for(unsigned axis=0;axis<3;++axis)
                    near(observed.velocity[axis],(after_tick[axis]-before_tick[axis])/Runtime::fixed_dt,1e-5,
                        "Reported motor velocity differs from actual committed foot displacement.");
                near(observed.velocity[0],0,.02,"Blocked motor advertised desired horizontal speed as actual motion.");
                auto saved=Json::parse(r.save_snapshot(hash));const auto& motor=row(saved,actor)["controller"]["motor"];
                check(motor["integration_velocity"][0].get<double>()>3.9 && motor["linear_velocity"][0]==observed.velocity[0],
                    "Portable motor state lost separate integration intent and resolved velocity.");
            }
        }
    }
}
void ceiling_and_crouched_stair() {
    auto d=flat(true,true,true);d.entities.push_back(box(step_id,{1,.1,0},{2,.2,4}));
    d.entities.push_back(box(roof_id,{1,2.05,0},{2,.4,4})); // bottom1.85: standing cannot rise .2m.
    Runtime r(d);r.step(120,{});r.step(32,{right()});
    check(foot(r)[0]<.1,"Stair sweep moved the standing capsule through a low ceiling.");
    crouch(r,true);r.step(30,{right()});
    check(foot(r)[0]>.5 && foot(r)[1]>.12,"Crouched capsule failed the same valid low stair.");
    near(r.entity(camera).local.position[1],.8,1e-6,"Crouched stair failed owned eye height.");
    crouch(r,false);const auto s=r.character_stance(actor);
    check(s && s->crouched && s->stand_blocked && !s->requested_crouch,"Low stair ceiling did not retain sticky standing intent.");
}
void slopes_and_walkable_normal() {
    for(const double degrees:{20.0,60.0}) {
        const double angle=degrees*std::numbers::pi/180;
        auto d=flat();auto ramp=box(ramp_id,{2*std::cos(angle),2*std::sin(angle)-.1*std::cos(angle),0},{4,.2,4});
        ramp.transform.rotation={0,0,std::sin(angle/2),std::cos(angle/2)};d.entities.push_back(ramp);
        Runtime r(d);r.step(120,{});const double baseline=foot(r)[1];r.step(45,{right()});const auto p=foot(r);const auto e=r.entity(actor);
        if(degrees==20) {
            check(p[0]>.6 && p[1]-baseline>.2 && e.ground=="on_ground","Walkable ramp failed physical ascent/support.");
            // Independently derive the rotated box's upper plane and upright
            // capsule lower-sphere contact offset; no reported slope surrogate.
            const double plane=(p[0]+.1*std::sin(angle))*std::tan(angle);
            const double sphere=.3*(1/std::cos(angle)-1);
            near(p[1]-baseline,plane+sphere,.08,"Ramp foot differs from independent box/capsule geometry.");
            check(e.locomotion && e.locomotion->ground_normal[1]>std::cos(45*std::numbers::pi/180),"Walkable ramp normal differs.");
        }else check(p[0]<.5 && p[1]-baseline<.5,"Steep ramp bypassed the configured maximum slope.");
    }
}
void floor_following_and_jump() {
    auto d=flat();d.entities.front()=box(floor_id,{-1.25,-.25,0},{2.5,.5,4});
    d.entities.push_back(box(step_id,{1.25,-.45,0},{2.5,.5,4}));
    Runtime r(d);r.step(120,{});const double baseline=foot(r)[1];
    const double effective_radius=double(d.entities[1].character->radius)+.02;
    const double slope_limit=double(d.entities[1].character->locomotion->max_slope_degrees)*std::numbers::pi/180;
    for(unsigned n=0;n<40;++n) {
        r.step(1,{right()});const auto observed=r.entity(actor);const auto p=foot(r);
        if(observed.ground=="steep_ground") {
            // The lower capsule sphere remains supported by the upper tread's
            // corner while its contact normal rotates beyond the slope limit.
            // This exception is bounded by that independent circular geometry,
            // not by accepting arbitrary steep or airborne descent contacts.
            check(p[0]>effective_radius*std::sin(slope_limit) && p[0]<effective_radius && observed.locomotion,
                "Descending steep support lies outside the authored capsule edge.");
            near(p[1]-baseline,std::sqrt(effective_radius*effective_radius-p[0]*p[0])-effective_radius,.005,
                "Descending steep support differs from independent capsule edge geometry.");
            const auto& normal=observed.locomotion->ground_normal;
            check(normal[1]>0 && normal[1]<std::cos(slope_limit)+.01 && normal[0]>std::sin(slope_limit)-.04,
                "Descending edge contact does not have the independently steep upward normal.");
            near(normal[2],0,.01,"Descending capsule edge unexpectedly has lateral support.");
        }else check(observed.ground=="on_ground","Small descending step intermittently lost support.");
    }
    near(foot(r)[1]-baseline,-.2,.05,"Floor following missed the independent lower tread.");
    RuntimeInput jump;jump.entity=actor;jump.jump=true;const double start=foot(r)[1];r.step(1,{jump});r.step(4,{});
    check(foot(r)[1]>start+.2 && r.entity(actor).velocity[1]>2 && r.entity(actor).ground=="in_air",
        "Floor following suppressed upward jump motion or snapped an airborne capsule down.");
    auto ledge=flat();ledge.entities.front()=box(floor_id,{-1.25,-.25,0},{2.5,.5,4});
    ledge.entities.push_back(box(step_id,{2,-1.25,0},{4,.5,4})); // 1m drop exceeds ground_snap .5.
    Runtime falling(ledge);falling.step(120,{});bool airborne=false;
    for(unsigned n=0;n<40;++n) {falling.step(1,{right()});airborne|=falling.entity(actor).ground=="in_air";}
    check(airborne && foot(falling)[1]<-.4,"Large ledge was treated as grounded or refused gravity.");
    auto roof_course=flat();roof_course.entities.push_back(box(roof_id,{-1,2.15,0},{3,.2,3}));
    Runtime roof(roof_course);roof.step(120,{});const double roof_start=foot(roof)[1];
    roof.step(1,{jump});double peak=foot(roof)[1];bool ceiling_stopped=false;
    for(unsigned n=0;n<12;++n) {
        const auto previous=foot(roof);roof.step(1,{});const auto current=foot(roof);const auto observed=roof.entity(actor);
        peak=std::max(peak,current[1]);
        near(observed.velocity[1],(current[1]-previous[1])/Runtime::fixed_dt,1e-5,
            "Roof collision reported velocity differs from actual vertical displacement.");
        // The independently authored roof underside is2.05; even the external
        // capsule proxy's top, offset .02 above logical feet, must stay below it.
        check(current[1]+double(roof.character_stance(actor)->height)+.02<=2.052,
            "Jumping capsule/proxy crossed the authored roof underside.");
        if(current[1]>roof_start+.1 && std::abs(current[1]-previous[1])<1e-4) {
            auto saved=Json::parse(roof.save_snapshot(hash));const auto& motor=row(saved,actor)["controller"]["motor"];
            check(motor["integration_velocity"][1].get<double>()<=.02,
                "Blocked upward jump retained ballistic intent pressing into the roof.");
            ceiling_stopped=true;
        }
    }
    check(ceiling_stopped && peak>roof_start+.1 && peak<roof_start+.27 && foot(roof)[1]<peak-.04,
        "Independent roof jump did not hit the ceiling and promptly resume downward motion.");
}
RuntimeDefinition platform_course() {
    auto d=flat();d.entities[1].transform.position={0,.8,0};
    d.entities.push_back(box(platform_id,{0,.25,0},{20,.5,20},BodyMotion::Kinematic));return d;
}
void translating_support_and_stop() {
    auto d=platform_course();Runtime r(d);r.step(120,{});const auto before=foot(r);
    KinematicTarget move;move.entity=platform_id;move.position={2,.25,0};move.duration_ticks=120;
    r.step(60,{}, {move});const auto half=foot(r);near(half[0]-before[0],1,.08,"Zero-input character did not follow translating support.");
    check(r.entity(actor).locomotion && r.entity(actor).locomotion->ground_velocity[0]>.9,"Platform point velocity absent from actual support.");
    r.step(60,{});near(foot(r)[0]-before[0],2,.1,"Support transport accumulated wrong displacement.");
    const auto stopped=foot(r);r.step(15,{});near(foot(r)[0],stopped[0],.03,"Stopped support left accumulating horizontal velocity.");
    Runtime walking(d);walking.step(120,{});const auto start=foot(walking);move.position={1,.25,0};move.duration_ticks=60;
    walking.step(60,{right()},{move});near(foot(walking)[0]-start[0],5,.15,"Relative movement failed to compose platform1m plus authored walking4m.");
}
void rotating_support_and_departure() {
    auto d=platform_course();d.entities[1].transform.position={2,.8,0};Runtime r(d);r.step(120,{});const auto before=foot(r);
    KinematicTarget turn;turn.entity=platform_id;turn.position={0,.25,0};turn.rotation={0,std::sin(std::numbers::pi/4),0,std::cos(std::numbers::pi/4)};turn.duration_ticks=120;
    r.step(120,{}, {turn});const auto after=foot(r);
    near(after[0],before[2],.1,"Rotating platform X transport differs from independent90-degree rigid transform.");
    near(after[2],-before[0],.1,"Rotating platform Z transport differs from independent90-degree rigid transform.");
    near(after[1],before[1],.05,"Rotating horizontal support changed foot height.");
    near(r.entity(actor).yaw,0,1e-6,"Support rotation silently took facing ownership.");
    RuntimeInput jump;jump.entity=actor;jump.jump=true;r.step(1,{jump});r.step(4,{});
    check(foot(r)[1]>after[1]+.2 && r.entity(actor).ground=="in_air","Rotating-support jump remained glued to the platform.");
}
void stance_proxy_and_portable_validation() {
    auto d=flat(true,true,true);Runtime r(d);r.step(120,{});const auto original=foot(r);
    auto ray=[&] {const auto p=foot(r);return r.raycast({{p[0]-2,p[1]+1.3,p[2]},{1,0,0},4,{floor_id}});};
    check(ray() && ray()->entity==actor,"Virtual actor is missing from ordinary query raycast.");
    crouch(r,true);check(!ray(),"Virtual query proxy did not adopt the actual crouched shape.");
    near(foot(r)[1],original[1],.03,"Virtual crouch moved foot origin.");near(r.entity(camera).local.position[1],.8,1e-6,"Virtual crouch did not own eye drop.");
    auto saved=Json::parse(r.save_snapshot(hash));check(saved["version"]==9,"Virtual definition failed to select version9.");
    auto& entity=row(saved,actor);const auto& body=entity["body"];const auto& motor=entity["controller"]["motor"];
    near(body["position"][1].get<double>()-motor["position"][1].get<double>(),.02,1e-6,"Saved query proxy offset differs from logical feet.");
    check(body["linear_velocity"]==Json::array({0.0,0.0,0.0}) && body["angular_velocity"]==Json::array({0.0,0.0,0.0}),"Query proxy has independent saved velocity.");
    auto restored=Runtime::from_snapshot(d,hash,saved.dump());check(restored->save_snapshot(hash)==saved.dump() && restored->character_stance(actor)->crouched,"Portable virtual shape/state reconstruction differs.");
    for(unsigned n=0;n<10;++n) {
        auto bad=saved;auto& changed=row(bad,actor);
        if(n==0)changed["controller"].erase("motor");
        if(n==1)changed["controller"]["motor"]["extra"]=0;
        if(n==2)changed["controller"]["motor"]["position"][0]=1;
        if(n==3)changed["controller"]["motor"]["rotation"]=Json::array({0,0,0,0});
        if(n==4)changed["controller"]["motor"]["integration_velocity"]=Json::array({501,0,0});
        if(n==5)changed["body"]["linear_velocity"]=Json::array({1,0,0});
        if(n==6)changed["body"]["position"][1]=changed["body"]["position"][1].get<double>()+.1;
        if(n==7)changed["controller"]["motor"]["position"]=Json::array({false,0,0});
        if(n==8)changed["controller"]["motor"]["linear_velocity"]=Json::array({700,0,0});
        if(n==9)changed["controller"]["motor"].erase("integration_velocity");
        rejects([&]{Runtime::validate_snapshot(d,hash,seal(bad));});
    }
    auto old=saved;old["version"]=8;rejects([&]{Runtime::validate_snapshot(d,hash,old.dump());});
    auto invalid=d;invalid.entities[1].character->locomotion->step_height=1.01f;rejects([&]{Runtime invalid_runtime(invalid);});
    invalid=d;invalid.entities[1].character->locomotion->ground_snap=-.01f;rejects([&]{Runtime invalid_runtime(invalid);});
    invalid=d;invalid.entities[1].character->locomotion->max_slope_degrees=std::numeric_limits<float>::infinity();rejects([&]{Runtime invalid_runtime(invalid);});
}
void rollback_and_two_cold_continuations() {
    auto d=platform_course();d.entities[1].character->stance=RuntimeCharacterStanceProfile{};Runtime r(d),control(d);r.step(120,{});control.step(120,{});
    const auto before=r.save_snapshot(hash);
    rejects([&]{r.step(2,{right()}, {}, {}, {},{{1,99,{},{}}}, {}, {}, {},{{actor,true}});});
    check(r.save_snapshot(hash)==before,"Later-Tick failure lost complete motor/shape/body rollback.");
    r.step(5,{right()});control.step(5,{right()});check(r.save_snapshot(hash)==control.save_snapshot(hash),"Rollback lost virtual contact history on subsequent Ticks.");
    KinematicTarget move;move.entity=platform_id;move.position={1,.25,0};move.duration_ticks=120;r.step(40,{}, {move});crouch(r,true);
    const auto saved=r.save_snapshot(hash);auto a=Runtime::from_snapshot(d,hash,saved),b=Runtime::from_snapshot(d,hash,saved);
    check(a->save_snapshot(hash)==saved && b->save_snapshot(hash)==saved,"Cold reconstruction changed complete selected v9 state.");
    a->step(20,{right()});b->step(20,{right()});check(a->save_snapshot(hash)==b->save_snapshot(hash),"Two cold virtual/platform continuations differ.");
    check(a->inspect().tick==r.inspect().tick+20 && a->entity(actor).locomotion.has_value(),"Cold continuation did not run20 actual motor Ticks.");
}
void templates_and_legacy_formats() {
    for(bool camera_present:{false,true}) {
        auto d=flat(false,camera_present);Runtime r(d);r.step(60,{});const auto old=r.save_snapshot(hash);
        check(Json::parse(old)["version"]==(camera_present ? 7 : 1) && !r.entity(actor).locomotion,"Omitted profile changed legacy controller/save behavior.");
        auto loaded=Runtime::from_snapshot(d,hash,old);check(loaded->save_snapshot(hash)==old,"Legacy portable checkpoint changed.");
        auto bad=Json::parse(old);row(bad,actor)["controller"]["motor"]={{"position",{0,0,0}},{"rotation",{0,0,0,1}},{"linear_velocity",{0,0,0}}};
        rejects([&]{Runtime::validate_snapshot(d,hash,seal(bad));});
    }
    auto physical_stance=flat(false,false,true);Runtime physical(physical_stance);check(Json::parse(physical.save_snapshot(hash))["version"]==8,"Physical stance no longer retains version8.");
    RuntimeDefinition d;d.world_id="virtual-template";d.authored_revision=1;d.entities={box(floor_id,{0,-.5,0},{40,1,40})};
    RuntimeSpawnTemplate recipe;recipe.id=std::string(32,'c');recipe.name="Virtual actor";recipe.root=id(9);
    RuntimeEntityDefinition local;local.id=recipe.root;local.character=CharacterController{};local.character->locomotion=RuntimeCharacterLocomotionProfile{};local.character->stance=RuntimeCharacterStanceProfile{};
    recipe.entities={local};d.templates={recipe};Runtime r(d);const auto unused=r.save_snapshot(hash);
    check(Json::parse(unused)["version"]==9 && Json::parse(unused)["payload"]["cameras"].empty(),"Unused frozen virtual template did not select camera-free version9.");
    auto empty=Runtime::from_snapshot(d,hash,unused);check(empty->save_snapshot(hash)==unused,"Unused virtual template failed portable roundtrip.");
    RuntimeTransform left,right_position;left.position={-2,.1,0};right_position.position={2,.1,0};
    const auto born=r.change_structure(0,{{recipe.id,left},{recipe.id,right_position}},{}).spawned;
    check(born.size()==2 && born[0]!=born[1],"Template motor births did not allocate independent identities.");r.step(120,{});
    r.step(1,{}, {}, {}, {}, {}, {}, {}, {},{{born[0],true}});check(r.character_stance(born[0])->crouched && !r.character_stance(born[1])->crouched,"Template motors share stance state.");
    const auto saved=r.save_snapshot(hash);auto loaded=Runtime::from_snapshot(d,hash,saved);check(loaded->save_snapshot(hash)==saved,"Template motor save lost ownership/allocator state.");
    loaded->change_structure(loaded->structure_revision(),{}, {born[0]});rejects([&]{loaded->entity(born[0]);});
    check(loaded->entity(born[1]).locomotion.has_value(),"Retiring one motor removed another instance.");
    const auto after=loaded->save_snapshot(hash);auto continued=Runtime::from_snapshot(d,hash,after);check(continued->save_snapshot(hash)==after,"Retired motor identity/proxy survived cold reconstruction.");
}
}
int main() {
    try {
        if(!Runtime::available()) {std::cout<<"Native character locomotion runtime unavailable; simulation checks skipped.\n";return 0;}
        stairs_and_height_limit();ceiling_and_crouched_stair();slopes_and_walkable_normal();floor_following_and_jump();
        translating_support_and_stop();rotating_support_and_departure();stance_proxy_and_portable_validation();
        rollback_and_two_cold_continuations();templates_and_legacy_formats();
        std::cout<<"9 native character locomotion groups passed\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
