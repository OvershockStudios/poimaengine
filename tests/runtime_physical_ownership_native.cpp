// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace poima;
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
std::string id(unsigned value){char text[33];std::snprintf(text,sizeof(text),"%032x",value);return text;}
RuntimeDefinition scene(bool passive=false) {
    RuntimeDefinition d;d.world_id="physical-ownership-native";
    auto model=std::make_shared<ModelAsset>();model->nodes.resize(1);model->nodes[0].name="Pelvis";model->roots={0};
    RuntimeEntityDefinition rig;rig.id=id(1);rig.transform.position={0,3,0};rig.animation_rig=RuntimeAnimationRig{model};
    physical_rig::Configuration configuration;configuration.initially_passive=passive;configuration.profile.root="pelvis";
    physical_rig::Part part;part.name="pelvis";part.node=0;part.shape=physical_rig::Sphere{.2};configuration.profile.parts={part};rig.physical_rig=configuration;
    RuntimeEntityDefinition bone;bone.id=id(2);bone.parent=rig.id;bone.rig_node=RuntimeRigNode{rig.id,0};
    d.entities={rig,bone};return d;
}
void attach(RuntimeDefinition& d,unsigned kind,bool deep) {
    RuntimeEntityDefinition attachment;attachment.id=id(3);attachment.parent=id(2);
    if(deep){RuntimeEntityDefinition intermediate;intermediate.id=id(4);intermediate.parent=id(2);d.entities.push_back(intermediate);attachment.parent=id(4);}
    if(kind==0)attachment.collider=BoxCollider{{.1f,.1f,.1f},BodyMotion::Static};
    else if(kind==1)attachment.mesh_collider=MeshCollider{};
    else attachment.character=CharacterController{};
    d.entities.push_back(attachment);
}
void rejected(RuntimeDefinition d) {
    bool matched=false;try{Runtime runtime(d);}catch(const std::runtime_error& error){matched=std::string(error.what()).find("cannot descend from PhysicalRig")!=std::string::npos;}
    check(matched,"Physical descendant reached another validator/native allocation instead of the explicit attachment-ownership gate.");
}
void descendant_cases() {
    for(bool passive:{false,true})for(bool deep:{false,true})for(unsigned kind=0;kind<3;++kind){auto d=scene(passive);attach(d,kind,deep);rejected(std::move(d));}
}
void supported_separation() {
    for(bool passive:{false,true}) {
        auto d=scene(passive);RuntimeEntityDefinition floor;floor.id=id(10);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{{10,.5,10},BodyMotion::Static};d.entities.push_back(floor);
        // Static ancestry and unrelated colliders stay admitted; the prohibition
        // concerns ordinary bodies whose world poses would move with the rig.
        d.entities[0].parent=floor.id;Runtime runtime(d);runtime.step(1,{});
        const auto physical=runtime.physical_rig(id(1),true,false);check(physical&&physical->parts.size()==1,"Valid physical rig lost its owned proxy.");
        check(physical->parts[0].enrolled==passive,"Ownership admission changed initial physical mode.");
    }
}
}
int main() {
    try{descendant_cases();supported_separation();std::cout<<"Physical ownership PASS "<<checks<<" checks\n";return 0;}
    catch(const std::exception& error){std::cerr<<"Physical ownership FAIL: "<<error.what()<<'\n';return 1;}
}
