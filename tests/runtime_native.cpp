// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace poima;
namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
RuntimeDefinition fixture() {
    RuntimeDefinition d; d.world_id="fixture"; d.authored_revision=7;
    RuntimeEntityDefinition floor; floor.id="floor"; floor.transform.position={0,-0.5,0}; floor.transform.scale={20,1,20}; floor.collider=BoxCollider{};
    RuntimeEntityDefinition wall; wall.id="wall"; wall.transform.position={0,1.5,-3}; wall.transform.scale={8,3,0.5}; wall.collider=BoxCollider{};
    RuntimeEntityDefinition box; box.id="box"; box.transform.position={2,3,0}; box.collider=BoxCollider{}; box.collider->dynamic=true;
    RuntimeEntityDefinition player; player.id="player"; player.transform.position={0,1,2}; player.character=CharacterController{}; player.character->camera="camera";
    RuntimeEntityDefinition camera; camera.id="camera"; camera.parent="player"; camera.transform.position={0,1.6,0}; camera.camera=RuntimeCamera{};
    d.entities={player,box,camera,wall,floor}; return d;
}
void equal(const RuntimeEntityState& a,const RuntimeEntityState& b) {
    if(a.world!=b.world || a.velocity!=b.velocity || a.ground!=b.ground)
        std::cerr<<"Mismatch for "<<a.id<<": positions "<<a.world[12]<<','<<a.world[13]<<','<<a.world[14]
            <<" / "<<b.world[12]<<','<<b.world[13]<<','<<b.world[14]<<" ground "<<a.ground<<" / "<<b.ground<<'\n';
    require(a.world==b.world && a.velocity==b.velocity && a.ground==b.ground && a.yaw==b.yaw && a.pitch==b.pitch,"Same-build deterministic state differs.");
}
}
int main() {
    try {
        const auto definition=fixture(); Runtime a(definition), b(definition);
        a.step(120,{}); b.step(60,{}); b.step(60,{});
        equal(a.entity("player"),b.entity("player")); equal(a.entity("box"),b.entity("box"));
        const auto landed=a.entity("player");
        require(landed.ground=="on_ground" && std::abs(landed.world[13])<0.05,"Player did not land.");
        require(std::abs(a.entity("box").world[13]-0.48)<0.04,"Box did not settle.");
        RuntimeInput walk; walk.entity="player"; walk.move={0,1};
        a.step(180,{walk}); b.step(90,{walk}); b.step(90,{walk});
        equal(a.entity("player"),b.entity("player"));
        require(a.entity("player").world[14]>-2.5 && a.entity("player").world[14]<-2.3,"Wall collision failed.");
        RuntimeInput jump; jump.entity="player"; jump.jump=true;
        a.step(10,{jump}); b.step(1,{jump}); b.step(9,{});
        equal(a.entity("player"),b.entity("player")); require(a.entity("player").world[13]>0.5,"Jump failed.");
        a.step(120,{}); b.step(120,{}); equal(a.entity("player"),b.entity("player"));
        RuntimeInput look; look.entity="player"; look.look={45,20};
        a.step(1,{look}); b.step(1,{look}); equal(a.entity("camera"),b.entity("camera"));
        require(a.entity("player").yaw==45 && a.entity("player").pitch==20,"Look input failed.");
        const auto snapshot=a.snapshot("camera");
        require(snapshot.revision==7 && snapshot.camera_world==a.entity("camera").world,"Snapshot lost source identity or camera pose.");
        const auto before=a.entity("player"); const auto tick=a.inspect().tick;
        bool rejected=false;
        try { a.step(60,{walk,walk}); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected && a.inspect().tick==tick,"Duplicate inputs were not rejected before advancing."); equal(before,a.entity("player"));
        auto invalid=definition; invalid.entities.front().character->camera="absent";
        rejected=false; try { Runtime bad(invalid); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected,"Invalid camera reference accepted.");
        // Exercise the rollback path after Jolt has started a real update:
        // overlapping bodies exceed the configured contact/pair capacities.
        RuntimeDefinition crowded; crowded.world_id="capacity-fixture";
        for(int i=0;i<150;++i) { RuntimeEntityDefinition e; e.id="body-"+std::to_string(i); e.collider=BoxCollider{}; e.collider->dynamic=true; crowded.entities.push_back(e); }
        Runtime overflow(crowded);
        std::vector<RuntimeEntityState> original;
        for(const auto& e:crowded.entities) original.push_back(overflow.entity(e.id));
        rejected=false; try { overflow.step(2,{}); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected,"Capacity fixture did not reach the expected Jolt update failure.");
        require(overflow.inspect().tick==0,"Failed physics batch advanced the clock.");
        for(const auto& state:original) equal(state,overflow.entity(state.id));
        // A new runtime still works after failed construction and update.
        Runtime c(definition); c.step(120,{}); equal(landed,c.entity("player"));
        std::cout << "Physics landing, wall collision, jump, look, chunked deterministic replay, snapshot identity, input guards and real capacity rollback passed.\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
