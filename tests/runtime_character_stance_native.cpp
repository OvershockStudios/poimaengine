// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace poima;
using Json=nlohmann::json;
namespace {
const std::string hash(64,'a');
std::string id(unsigned value) {return std::string(31,'0')+char('0'+value);}
const auto actor=id(1),camera=id(2),floor_id=id(3),ceiling_id=id(4);
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void near(double a,double b,double tolerance,const char* message) {
    check(std::isfinite(a) && std::isfinite(b) && std::abs(a-b)<=tolerance,message);
}
template<class F>void rejects(F action) {
    try {action();}catch(const std::exception&) {return;}
    throw std::runtime_error("Invalid stance operation or snapshot was admitted.");
}
RuntimeDefinition definition(bool with_camera=true,bool configured=true) {
    RuntimeDefinition d;d.world_id="stance-native";d.authored_revision=9;
    RuntimeEntityDefinition floor;floor.id=floor_id;floor.transform.position={0,-.5,0};floor.transform.scale={40,1,40};floor.collider=BoxCollider{};
    RuntimeEntityDefinition ceiling;ceiling.id=ceiling_id;ceiling.transform.position={2.5,1.3,0};ceiling.transform.scale={2,.2,4};ceiling.collider=BoxCollider{};
    RuntimeEntityDefinition player;player.id=actor;player.character=CharacterController{};
    if(configured)player.character->stance=RuntimeCharacterStanceProfile{1,2,.8f};
    d.entities={floor,ceiling,player};
    if(with_camera) {
        d.entities.back().character->camera=camera;
        RuntimeEntityDefinition eye;eye.id=camera;eye.parent=actor;eye.transform.position={.05,1.6,.02};eye.camera=RuntimeCamera{};
        d.entities.push_back(eye);
    }
    return d;
}
void stance(Runtime& runtime,bool crouch,std::uint32_t ticks=1) {
    runtime.step(ticks,{}, {}, {}, {}, {}, {}, {}, {},{{actor,crouch}});
}
RuntimeCharacterStanceState state(const Runtime& r,const std::string& entity=actor) {
    auto s=r.character_stance(entity);check(s.has_value(),"Character stance state absent.");return *s;
}
RuntimeInput right() {RuntimeInput input;input.entity=actor;input.move={1,0};return input;}
std::optional<RuntimeRayHit> high_ray(const Runtime& r) {
    const auto p=r.entity(actor).world;
    return r.raycast({{p[12]-2,p[13]+1.3,p[14]},{1,0,0},4,{floor_id,ceiling_id}});
}
Json& row(Json& value,const std::string& entity) {
    for(auto& r:value["payload"]["entities"])if(r["id"]==entity)return r;
    throw std::runtime_error("Snapshot row absent.");
}
std::string seal(Json value) {
    const auto bytes=value["payload"].dump();value["sha256"]=sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));return value.dump();
}
void shrink_eye_and_speed() {
    auto d=definition();Runtime r(d);r.step(120,{});
    check(r.entity(actor).ground=="on_ground" && high_ray(r) && high_ray(r)->entity==actor,"Standing capsule witness missing.");
    const auto foot=r.entity(actor).world;
    stance(r,true);auto s=state(r);
    check(s.configured && s.requested_crouch && s.crouched && !s.stand_blocked && s.ground=="on_ground",
        "Shrink did not commit configured crouch and support.");
    near(s.height,1,1e-6,"Actual crouch height differs.");near(s.speed,2,1e-6,"Actual crouch speed differs.");
    near(r.entity(actor).world[13],foot[13],.03,"Shape change moved the foot origin.");
    near(r.entity(camera).local.position[1],.8,1e-6,"Crouch did not lower the camera from its initial eye height.");
    check(r.entity(camera).local.position[0]==.05 && r.entity(camera).local.position[2]==.02 && !high_ray(r),
        "Crouch changed eye X/Z or retained standing collision height.");
    const double start=r.entity(actor).world[12];r.step(15,{right()});
    near(r.entity(actor).world[12]-start,.5,.03,"Crouched movement does not use profile speed.");
    stance(r,true,3);near(r.entity(camera).local.position[1],.8,1e-6,"Repeated crouch accumulated camera drop.");
}
void blocked_and_sticky_recovery() {
    Runtime r(definition());r.step(120,{});stance(r,true);r.step(75,{right()});
    near(r.entity(actor).world[12],2.5,.04,"Low ceiling witness did not reach its interior.");
    stance(r,false);const auto blocked=state(r);
    check(!blocked.requested_crouch && blocked.crouched && blocked.stand_blocked && !high_ray(r),
        "Obstructed stand grew the capsule or lost sticky requested standing.");
    r.step(3,{});check(state(r).stand_blocked && state(r).crouched,"Neutral Ticks forgot the blocked stand request.");
    near(r.entity(camera).local.position[1],.8,1e-6,"Blocked stand raised the eye through the ceiling.");
    r.step(45,{right()});const auto standing=state(r);
    check(!standing.requested_crouch && !standing.crouched && !standing.stand_blocked && high_ray(r) && high_ray(r)->entity==actor,
        "Sticky stand did not retry and expand after obtaining clearance.");
    near(r.entity(actor).world[13],0,.05,"Auto-expansion changed the foot baseline.");
    near(r.entity(camera).local.position[1],1.6,1e-6,"Auto-expansion did not restore the initial eye height.");
    const auto x=r.entity(actor).world[12];r.step(15,{right()});near(r.entity(actor).world[12]-x,1,.04,"Standing speed was not restored.");
}
void floor_slop_and_millimeter_clearance() {
    auto d=definition();
    for(auto& entity:d.entities)if(entity.id==ceiling_id) {
        entity.transform.position={0,5,0};entity.transform.scale={8,.2,8};entity.collider->motion=BodyMotion::Kinematic;
    }
    Runtime r(d);r.step(120,{});stance(r,true);r.step(20,{});
    check(r.entity(actor).ground=="on_ground","Floor allowance fixture lacks settled support.");
    const auto initial_foot=r.entity(actor).world[13];
    stance(r,false);
    check(!state(r).crouched && !state(r).stand_blocked && high_ray(r),
        "Supporting floor contact falsely blocked an unobstructed expansion.");
    near(r.entity(actor).world[13],initial_foot,.03,"Floor-supported expansion moved its foot origin.");
    stance(r,true);r.step(20,{});
    const double foot=r.entity(actor).world[13];
    const double top=foot+state(r).standing_height;
    KinematicTarget lower;lower.entity=ceiling_id;lower.position={0,top-.004+.1,0};lower.duration_ticks=3;
    r.step(3,{}, {lower});
    // Measure the actual native box/capsule geometry before requesting growth.
    // Four millimeters is above the strict1mm non-support allowance and far
    // below ordinary floor penetration slop; no renderer-derived oracle.
    const double penetration=r.entity(actor).world[13]+state(r).standing_height-(r.entity(ceiling_id).world[13]-.1);
    check(penetration>.002 && penetration<.006,"Slightly low ceiling is not an independent millimeter-clearance witness.");
    stance(r,false);const auto blocked=state(r);
    check(!blocked.requested_crouch && blocked.crouched && blocked.stand_blocked && blocked.ground=="on_ground",
        "Ceiling penetration incorrectly received the supporting-floor slop allowance.");
    near(r.entity(camera).local.position[1],.8,1e-6,"Strict ceiling rejection raised the eye.");
    r.step(2,{});check(state(r).stand_blocked,"Slight ceiling obstruction did not persist across neutral Ticks.");
    KinematicTarget raise;raise.entity=ceiling_id;raise.position={0,5,0};raise.duration_ticks=3;
    r.step(3,{}, {raise});r.step(1,{});
    check(!state(r).crouched && !state(r).stand_blocked && !state(r).requested_crouch && high_ray(r),
        "Removing the millimeter obstruction left a false floor blocker or lost the sticky request.");
}
void invalid_and_later_tick_rollback() {
    auto d=definition();Runtime r(d),control(d);r.step(120,{});control.step(120,{});
    const auto before=r.save_snapshot(hash);
    rejects([&]{r.step(1,{}, {}, {}, {}, {}, {}, {}, {},{{actor,true},{actor,false}});});
    rejects([&]{r.step(1,{}, {}, {}, {}, {}, {}, {}, {},{{floor_id,true}});});
    rejects([&]{r.step(1,{}, {}, {}, {}, {}, {}, {}, {},{{id(9),true}});});
    check(r.save_snapshot(hash)==before,"Rejected stance command changed complete native state.");
    // Frame0 commits crouch and simulates; frame1 fails the actual structural
    // revision check, exercising shape rollback after real physics work.
    rejects([&]{r.step(2,{}, {}, {}, {},{{1,99,{},{}}}, {}, {}, {},{{actor,true}});});
    check(r.save_snapshot(hash)==before && !state(r).crouched && high_ray(r) && high_ray(r)->entity==actor,
        "Later-Tick failure retained shrunken shape, stance, camera or physics state.");
    r.step(5,{right()});control.step(5,{right()});
    check(r.save_snapshot(hash)==control.save_snapshot(hash),"Shape/support rollback changed subsequent controlled physics.");
}
void portable_shapes_and_cold_continuation() {
    auto d=definition();Runtime r(d);r.step(120,{});stance(r,true);r.step(75,{right()});stance(r,false);
    const auto bytes=r.save_snapshot(hash);auto parsed=Json::parse(bytes);
    check(parsed["version"]==8 && row(parsed,actor)["controller"]["stand_blocked"]==true,
        "Configured actor did not emit version8 blocked-stand state.");
    Runtime::validate_snapshot(d,hash,bytes);auto a=Runtime::from_snapshot(d,hash,bytes),b=Runtime::from_snapshot(d,hash,bytes);
    check(a->save_snapshot(hash)==b->save_snapshot(hash) && state(*a).stand_blocked && !high_ray(*a) &&
        state(*a).height==1,"Fresh restore did not install the saved crouched collision shape.");
    near(a->entity(camera).local.position[1],.8,1e-6,"Restored stance eye does not match saved state.");
    a->step(45,{right()});b->step(45,{right()});
    check(a->save_snapshot(hash)==b->save_snapshot(hash) && !state(*a).crouched && high_ray(*a) && high_ray(*a)->entity==actor,
        "Two independently cold-restored owners diverged or lost automatic standing continuation.");
    check(r.save_snapshot(hash)==bytes,"Portable reconstruction changed the original runtime.");
    for(const auto& flags:std::vector<std::array<bool,3>>{{true,false,false},{true,true,true},{false,true,false},{false,false,true}}) {
        auto malformed=parsed;auto& c=row(malformed,actor)["controller"];
        c["requested_crouch"]=flags[0];c["crouched"]=flags[1];c["stand_blocked"]=flags[2];
        rejects([&]{Runtime::validate_snapshot(d,hash,seal(malformed));});
    }
    auto malformed=parsed;row(malformed,actor)["controller"]["crouched"]=1;
    rejects([&]{Runtime::validate_snapshot(d,hash,seal(malformed));});
    malformed=parsed;row(malformed,actor)["controller"].erase("stand_blocked");
    rejects([&]{Runtime::validate_snapshot(d,hash,seal(malformed));});
    malformed=parsed;row(malformed,camera)["local"]["position"][1]=1.6;
    rejects([&]{Runtime::validate_snapshot(d,hash,seal(malformed));});
}
void legacy_and_camera_free() {
    for(bool camera_present:{false,true}) {
        const auto old=definition(camera_present,false);Runtime legacy(old);legacy.step(120,{});
        const auto saved=legacy.save_snapshot(hash);check(Json::parse(saved)["version"]==(camera_present?7:1),"No-profile world changed legacy format.");
        check(!state(legacy).configured,"No-profile actor reported configured stance.");
        rejects([&]{stance(legacy,true);});check(legacy.save_snapshot(hash)==saved,"Unsupported crouch changed legacy state.");
        auto restore=Runtime::from_snapshot(old,hash,saved);check(restore->save_snapshot(hash)==saved,"Legacy standing shape roundtrip changed state.");
        const auto next=definition(camera_present,true);Runtime configured(next);configured.step(120,{});
        auto compatible=Json::parse(configured.save_snapshot(hash));compatible["version"]=camera_present?7:1;
        auto& c=row(compatible,actor)["controller"];for(const auto* key:{"requested_crouch","crouched","stand_blocked"})c.erase(key);
        if(!camera_present)for(const auto* key:{"cameras","components","structure","ui","control_sequence"})compatible["payload"].erase(key);
        auto standing=Runtime::from_snapshot(next,hash,seal(compatible));
        check(!state(*standing).requested_crouch && !state(*standing).crouched && high_ray(*standing) && high_ray(*standing)->entity==actor,
            "Older snapshot did not default a configured actor to standing.");
        if(!camera_present) {stance(*standing,true);check(state(*standing).crouched && standing->snapshot().camera_id.empty(),"Camera-free stance required a camera.");}
    }
}
void frozen_template_detection() {
    RuntimeDefinition d;d.world_id="unused-stance-template";d.authored_revision=1;
    RuntimeSpawnTemplate recipe;recipe.id=std::string(32,'c');recipe.name="Stance template";recipe.root=id(9);
    RuntimeEntityDefinition local;local.id=recipe.root;local.character=CharacterController{};local.character->stance=RuntimeCharacterStanceProfile{};
    recipe.entities={local};d.templates={recipe};Runtime r(d);
    const auto unused=r.save_snapshot(hash);check(Json::parse(unused)["version"]==8 && Json::parse(unused)["payload"]["cameras"].empty(),
        "Unused frozen stance template failed version8/camera-free detection.");
    auto empty=Runtime::from_snapshot(d,hash,unused);check(empty->save_snapshot(hash)==unused,"Empty version8 template world failed roundtrip.");
    const auto born=r.change_structure(0,{{recipe.id,{}}},{}).spawned.front();
    r.step(1,{}, {}, {}, {}, {}, {}, {}, {},{{born,true}});const auto saved=r.save_snapshot(hash);
    auto restored=Runtime::from_snapshot(d,hash,saved);check(state(*restored,born).crouched,"Template-born saved stance was not restored.");
    restored->change_structure(restored->structure_revision(),{}, {born});
    rejects([&]{restored->character_stance(born);});
}
}
int main() {
    try {
        if(!Runtime::available()) {std::cout<<"Native character stance runtime unavailable; simulation checks skipped.\n";return 0;}
        shrink_eye_and_speed();blocked_and_sticky_recovery();floor_slop_and_millimeter_clearance();invalid_and_later_tick_rollback();
        portable_shapes_and_cold_continuation();legacy_and_camera_free();frozen_template_detection();
        std::cout<<"7 native character stance groups passed\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
