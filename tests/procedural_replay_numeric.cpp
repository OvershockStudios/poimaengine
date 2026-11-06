// SPDX-License-Identifier: Apache-2.0
// Tests the reader's numeric policy directly, independently of engine/native
// execution. These minimal path fixtures are not valid standalone save files;
// the production typed codec still validates whole saves and quaternion bounds.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using StateJson=nlohmann::json;
#include "../src/runtime_animation_procedural_compare.inc"
using Scope=ProceduralReplayScope;

void expect(bool result,const std::string& message) {
    if(!result)throw std::runtime_error(message);
}
StateJson leaf(const std::string& path,const StateJson& value) {
    StateJson result;
    result[StateJson::json_pointer(path)]=value;
    return result;
}
std::string difference(Scope scope,const std::string& path,double stored,double replay) {
    return procedural_replay_difference(leaf(path,stored),leaf(path,replay),scope);
}
void accepts(Scope scope,const std::string& path,double stored,double replay) {
    const auto result=difference(scope,path,stored,replay);
    expect(result.empty(),"Expected admitted reader resolution at "+path+": "+result);
}
void rejects(Scope scope,const std::string& path,double stored,double replay,
    const char* category="exact") {
    const auto result=difference(scope,path,stored,replay);
    expect(!result.empty(),"Unexpected numeric admission at "+path);
    expect(result.find("$"+path)!=std::string::npos,"Missing first leaf path: "+result);
    expect(result.find(std::string("category=")+category)!=std::string::npos,"Wrong numeric category: "+result);
    expect(result.find("stored=")!=std::string::npos&&result.find("replay=")!=std::string::npos&&
        result.find("bound=")!=std::string::npos,"Missing diagnostic values or budget: "+result);
}

struct Allowed {Scope scope;const char* path;const char* category;};
void path_allowlist() {
    const Allowed allowed[]{
        {Scope::GeneratedControls,"/0/end_orientation/rotation/0","angular"},
        {Scope::TerminalPoses,"/0/position/2","linear"},
        {Scope::TerminalPoses,"/0/rotation/3","angular"},
        {Scope::TerminalStates,"/0/end_position/0","linear"},
        {Scope::TerminalStates,"/0/target_error","linear"},
        {Scope::TerminalStates,"/0/metric/unweighted_position_error","linear"},
        {Scope::TerminalStates,"/0/metric/unweighted_position_limit","linear"},
        {Scope::TerminalStates,"/0/end_orientation/actual_rotation/0","angular"},
        {Scope::TerminalStates,"/0/end_orientation/angular_error","angular"},
        {Scope::TerminalStates,"/0/metric/local_scale_error","dimensionless"},
        {Scope::TerminalStates,"/0/metric/frame_error","dimensionless"},
        {Scope::TerminalStates,"/0/metric/gram_error","dimensionless"},
        {Scope::TerminalStates,"/0/grounding/state/input_sole/0","linear"},
        {Scope::TerminalStates,"/0/grounding/state/ray_origin/1","linear"},
        {Scope::TerminalStates,"/0/grounding/state/actual_sole/2","linear"},
        {Scope::TerminalStates,"/0/grounding/state/position_error","linear"},
        {Scope::TerminalStates,"/0/grounding/state/actual_normal/0","dimensionless"},
        {Scope::TerminalStates,"/0/grounding/state/normal_error","angular"},
        {Scope::TerminalStates,"/0/grounding/planting/target_sole/1","linear"},
        {Scope::TerminalStates,"/0/grounding/planting/position_error","linear"},
        {Scope::TerminalStates,"/0/grounding/planting/target_normal/2","dimensionless"},
        {Scope::TerminalStates,"/0/grounding/planting/target_forward/0","dimensionless"},
        {Scope::TerminalStates,"/0/grounding/planting/normal_error","angular"}
    };
    // Independent binary64 expectation: 128 * 2^-52, not a production constant.
    const double resolution=0x1p-45;
    for(const auto& entry:allowed) {
        accepts(entry.scope,entry.path,0.,resolution);
        accepts(entry.scope,entry.path,0.,-resolution);
        rejects(entry.scope,entry.path,0.,std::nextafter(resolution,1.),entry.category);
        rejects(entry.scope,entry.path,0.,std::nextafter(-resolution,-1.),entry.category);
        // The identical path has no permission under any exact-record scope.
        rejects(Scope::Exact,entry.path,0.,resolution);
        rejects(Scope::QueryRay,entry.path,0.,resolution);
    }
    // Real serializer paths: history={current,previous}, each={tick,poses}.
    for(const auto* phase:{"current","previous"})for(const bool layer:{false,true}) {
        const std::string prefix=std::string("/rigs/2/")+(layer?"layers/3/":"")+
            "history/"+phase+"/poses/4/";
        for(const auto* kind:{"position","rotation","scale"}) {
            const auto path=prefix+kind+"/0";
            const auto* category=std::string(kind)=="position"?"linear":
                std::string(kind)=="rotation"?"angular":"dimensionless";
            accepts(Scope::PassiveSourceHistory,path,0.,resolution);
            rejects(Scope::PassiveSourceHistory,path,0.,std::nextafter(resolution,1.),category);
        }
    }
}

void relative_and_absolute_bounds() {
    const double relative=0x1p-45;
    accepts(Scope::TerminalPoses,"/0/position/0",2.,2.+2.*relative);
    rejects(Scope::TerminalPoses,"/0/position/0",2.,std::nextafter(2.+2.*relative,3.),"linear");
    struct Budget {Scope scope;const char* path;double baseline;double ceiling;const char* category;};
    const Budget cases[]{
        {Scope::TerminalPoses,"/0/position/0",1e6,1e-8,"linear"},
        {Scope::TerminalStates,"/0/end_orientation/angular_error",100.,1e-12,"angular"},
        {Scope::TerminalStates,"/0/metric/frame_error",100.,1e-12,"dimensionless"}
    };
    for(const auto& entry:cases) {
        // Choose adjacent representable doubles straddling the absolute cap.
        double inside=entry.baseline+entry.ceiling;
        if(inside-entry.baseline>entry.ceiling)inside=std::nextafter(inside,entry.baseline);
        const double outside=std::nextafter(inside,std::numeric_limits<double>::infinity());
        expect(inside-entry.baseline<=entry.ceiling&&outside-entry.baseline>entry.ceiling,
            "Test failed to straddle its independently specified ceiling.");
        accepts(entry.scope,entry.path,entry.baseline,inside);
        rejects(entry.scope,entry.path,entry.baseline,outside,entry.category);
    }
    rejects(Scope::GeneratedControls,"/0/end_orientation/rotation/3",1.,-1.,"angular");
    rejects(Scope::TerminalPoses,"/0/position/0",1e100, std::nextafter(1e100,2e100),"linear");
}

void exact_inputs_and_context() {
    const struct {Scope scope;const char* path;} exact[]{
        {Scope::GeneratedControls,"/0/target/0"},
        {Scope::GeneratedControls,"/0/pole/1"},
        {Scope::GeneratedControls,"/0/weight"},
        {Scope::GeneratedControls,"/0/end_orientation/weight"},
        {Scope::TerminalPoses,"/0/scale/0"},
        {Scope::TerminalStates,"/0/target/0"},
        {Scope::TerminalStates,"/0/pole/1"},
        {Scope::TerminalStates,"/0/weight"},
        {Scope::TerminalStates,"/0/end_orientation/goal/rotation/2"},
        {Scope::TerminalStates,"/0/end_orientation/goal/weight"},
        {Scope::TerminalStates,"/0/grounding/state/weight"},
        {Scope::TerminalStates,"/0/grounding/state/hit/position/0"},
        {Scope::TerminalStates,"/0/grounding/state/hit/normal/2"},
        {Scope::TerminalStates,"/0/grounding/planting/anchor/local_point/0"},
        {Scope::TerminalStates,"/0/grounding/planting/anchor/local_normal/1"},
        {Scope::TerminalStates,"/0/grounding/planting/anchor/local_forward/2"},
        {Scope::QueryRay,"/origin/0"},
        {Scope::QueryRay,"/direction/1"},
        {Scope::QueryRay,"/distance"},
        {Scope::PassiveSourceHistory,"/rigs/0/control/weight"},
        {Scope::PassiveSourceHistory,"/rigs/0/layers/0/weight"},
        {Scope::PassiveSourceHistory,"/rigs/0/fade/frozen/0/position/0"},
        {Scope::PassiveSourceHistory,"/rigs/0/history/current/poses/0/custom/0"},
        {Scope::PassiveSourceHistory,"/physical/0/history/current/poses/0/position/0"},
        {Scope::PassiveSourceHistory,"/rigs/0/history/future/poses/0/position/0"},
        {Scope::PassiveSourceHistory,"/rigs/0/history/current/poses/0/position/3"},
        {Scope::GeneratedControls,"/0/end_orientation/rotation/4"},
        {Scope::TerminalPoses,"/0/position/3"},
        {Scope::TerminalStates,"/0/end_position/3"}
    };
    for(const auto& entry:exact)rejects(entry.scope,entry.path,1.,std::nextafter(1.,2.));
    // Object keys that happen to be digits cannot masquerade as array indices.
    const auto object=StateJson{{"0",{{"position",StateJson::array({0.,0.,0.})}}}};
    auto changed=object;changed["0"]["position"][0]=0x1p-46;
    expect(!procedural_replay_difference(object,changed,Scope::TerminalPoses).empty(),"Numeric object key acquired array permission.");
}

void structure_and_scalar_types() {
    const auto base=StateJson::parse(R"([{"entity":"rig","slot":2,"enabled":true,"space":"world","weight":1.0,"target":[0.0,0.0,0.0],"end_orientation":{"rotation":[0.0,0.0,0.0,1.0],"weight":1.0}}])");
    const auto before=base.dump();
    auto tiny=base;tiny[0]["end_orientation"]["rotation"][0]=0x1p-46;
    expect(procedural_replay_difference(base,tiny,Scope::GeneratedControls).empty(),"Valid generated rotation was rejected.");
    expect(base.dump()==before&&tiny[0]["end_orientation"]["rotation"][0]==StateJson(0x1p-46),"Comparator changed an input.");
    const struct {const char* path;StateJson value;} mutations[]{
        {"/0/entity","other"},{"/0/slot",3},{"/0/enabled",false},{"/0/space","rig"},
        {"/0/weight",0.5},{"/0/end_orientation",nullptr},
        {"/0/end_orientation/rotation/0",0},{"/0/end_orientation/rotation/0",false},
        {"/0/end_orientation/rotation/0","0.0"},
        {"/0/slot",StateJson::number_integer_t(2)}
    };
    for(const auto& mutation:mutations) {
        auto changed=base;changed[StateJson::json_pointer(mutation.path)]=mutation.value;
        expect(!procedural_replay_difference(base,changed,Scope::GeneratedControls).empty(),
            std::string("Typed input changed without rejection: ")+mutation.path);
    }
    auto missing=base;missing[0].erase("enabled");
    auto extra=base;extra[0]["unrecognized"]=nullptr;
    auto longer=base;longer.push_back(base[0]);
    auto vector_longer=base;vector_longer[0]["end_orientation"]["rotation"].push_back(0.);
    for(const auto& changed:{missing,extra,longer,vector_longer})
        expect(!procedural_replay_difference(base,changed,Scope::GeneratedControls).empty(),"Changed structure was admitted.");
    auto pair=base;auto second=base[0];second["entity"]="other";pair.push_back(second);
    auto reordered=pair;std::swap(reordered[0],reordered[1]);
    expect(!procedural_replay_difference(pair,reordered,Scope::GeneratedControls).empty(),"Reordered membership was admitted.");
    const auto status=leaf("/0/grounding/state/status","contact");
    expect(!procedural_replay_difference(status,leaf("/0/grounding/state/status","miss"),Scope::TerminalStates).empty(),"Branch status changed.");
    for(const auto* path:{"/0/grounding/state/hit/entity","/0/grounding/planting/anchor/support"})
        expect(!procedural_replay_difference(leaf(path,"a"),leaf(path,"b"),Scope::TerminalStates).empty(),"Support identity changed.");
    for(const auto* path:{"/0/grounding/state/decision_tick","/0/grounding/state/casts","/0/grounding/state/cast",
                         "/0/grounding/state/hit/triangle","/0/grounding/planting/anchor/acquired_tick"})
        expect(!procedural_replay_difference(leaf(path,2),leaf(path,3),Scope::TerminalStates).empty(),"Discrete state changed.");
    for(const auto* path:{"/rigs/0/history/current/tick","/rigs/0/layers/0/history/previous/tick"})
        expect(!procedural_replay_difference(leaf(path,2),leaf(path,3),Scope::PassiveSourceHistory).empty(),"History tick changed.");
}

void finite_zero_and_diagnostics() {
    const auto path="/0/position/0";
    for(const double bad:{std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()}) {
        expect(!difference(Scope::TerminalPoses,path,bad,bad).empty(),"Identical nonfinite values passed.");
        expect(!difference(Scope::TerminalPoses,path,0.,bad).empty(),"Nonfinite replay passed.");
    }
    rejects(Scope::Exact,path,0.,-0.);
    rejects(Scope::GeneratedControls,"/0/target/0",0.,-0.);
    accepts(Scope::TerminalPoses,path,0.,-0.);
    const auto escaped=procedural_replay_difference(StateJson{{"a/b~c",1}},StateJson{{"a/b~c",2}},Scope::Exact);
    expect(escaped.find("$/a~1b~0c")!=std::string::npos,"Diagnostic key escaping is ambiguous.");
    bool threw=false;
    try {require_procedural_replay_equal(leaf(path,0.),leaf(path,1.),Scope::TerminalPoses,"Witness rejected.");}
    catch(const std::runtime_error& error) {
        const std::string message=error.what();
        threw=message.find("Witness rejected. First difference at $/0/position/0")!=std::string::npos&&
            message.find("absolute_error=")!=std::string::npos;
    }
    expect(threw,"Required-equality wrapper lost its error context.");
    StateJson deep=0.;for(int i=0;i<70;++i)deep=StateJson{{"child",deep}};
    expect(!procedural_replay_difference(deep,deep,Scope::Exact).empty(),"Unbounded nesting was accepted.");
}
}

int main() {
    try {
        path_allowlist();relative_and_absolute_bounds();exact_inputs_and_context();
        structure_and_scalar_types();finite_zero_and_diagnostics();
        std::cout<<"Procedural replay numeric policy passed.\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
