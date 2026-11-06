// SPDX-License-Identifier: Apache-2.0
// Included inside the animation codec's unnamed namespace, after StateJson.
// Reader validation only. Recording on the current owner, stored snapshots,
// commands, journal replies and the live physics state still use exact checks.
enum class ProceduralReplayScope {
    Exact, GeneratedControls, QueryRay, TerminalStates, TerminalPoses, PassiveSourceHistory
};
enum class ProceduralReplayNumber { Exact, Linear, Angular, Dimensionless };
struct ProceduralReplayPathPart {
    bool array=false;
    std::size_t index=0;
    std::string field;
};
using ProceduralReplayPath=std::vector<ProceduralReplayPathPart>;

// These are reader-validation resolutions, not guaranteed libm error bounds,
// a promise of cross-platform future trajectories, or gameplay tolerances.
// The relative/near-zero allowance is 128 binary64 epsilons. Absolute ceilings
// prevent large coordinates from turning that allowance into a gameplay-sized
// discrepancy. All bounds are per scalar; original values are never rounded,
// normalized, substituted or written back by this comparator.
constexpr double procedural_replay_roundoff=128*0x1p-52;
constexpr double procedural_replay_linear_ceiling=1e-8;
constexpr double procedural_replay_angular_ceiling=1e-12;
constexpr double procedural_replay_dimensionless_ceiling=1e-12;

bool procedural_replay_path(const ProceduralReplayPath& actual,
    std::initializer_list<const char*> expected) {
    if(actual.size()!=expected.size())return false;
    std::size_t i=0;
    for(const auto* field:expected) {
        const auto& part=actual[i++];
        const std::string token(field);
        if(token=="#") {if(!part.array)return false;}
        else if(token=="@3") {if(!part.array||part.index>=3)return false;}
        else if(token=="@4") {if(!part.array||part.index>=4)return false;}
        else if(part.array||part.field!=token)return false;
    }
    return true;
}

ProceduralReplayNumber procedural_replay_number(ProceduralReplayScope scope,
    const ProceduralReplayPath& path) {
    const auto matches=[&](std::initializer_list<const char*> p) {
        return procedural_replay_path(path,p);
    };
    if(scope==ProceduralReplayScope::GeneratedControls) {
        // Generated quaternion components alone: targets, poles, enabled,
        // space, slots and both weights remain exact, including signed zero.
        if(matches({"#","end_orientation","rotation","@4"}))return ProceduralReplayNumber::Angular;
    } else if(scope==ProceduralReplayScope::TerminalPoses) {
        if(matches({"#","position","@3"}))return ProceduralReplayNumber::Linear;
        if(matches({"#","rotation","@4"}))return ProceduralReplayNumber::Angular;
        // Terminal local scale is immutable captured input, never approximate.
    } else if(scope==ProceduralReplayScope::TerminalStates) {
        // Mirrors of saved commands (target/pole/weight/goal) stay exact.
        if(matches({"#","end_position","@3"})||matches({"#","target_error"})||
           matches({"#","metric","unweighted_position_error"})||
           matches({"#","metric","unweighted_position_limit"}))return ProceduralReplayNumber::Linear;
        if(matches({"#","end_orientation","actual_rotation","@4"})||
           matches({"#","end_orientation","angular_error"}))return ProceduralReplayNumber::Angular;
        if(matches({"#","metric","local_scale_error"})||matches({"#","metric","frame_error"})||
           matches({"#","metric","gram_error"}))return ProceduralReplayNumber::Dimensionless;
        if(matches({"#","grounding","state","input_sole","@3"})||
           matches({"#","grounding","state","ray_origin","@3"})||
           matches({"#","grounding","state","actual_sole","@3"})||
           matches({"#","grounding","state","position_error"})||
           matches({"#","grounding","planting","target_sole","@3"})||
           matches({"#","grounding","planting","position_error"}))return ProceduralReplayNumber::Linear;
        if(matches({"#","grounding","state","actual_normal","@3"})||
           matches({"#","grounding","planting","target_normal","@3"})||
           matches({"#","grounding","planting","target_forward","@3"}))return ProceduralReplayNumber::Dimensionless;
        if(matches({"#","grounding","state","normal_error"})||
           matches({"#","grounding","planting","normal_error"}))return ProceduralReplayNumber::Angular;
        // Literal hit/anchor payloads (including float32-origin positions and
        // normals), query replies, statuses, counters and flags remain exact.
    } else if(scope==ProceduralReplayScope::PassiveSourceHistory) {
        // Full animation context, not physical COM/body-history frames. Only
        // source-history poses sampled again during passive progression may
        // differ; clocks, layer slots/masks/weights and all other context do not.
        for(const auto* phase:{"current","previous"}) {
            if(matches({"rigs","#","history",phase,"poses","#","position","@3"})||
               matches({"rigs","#","layers","#","history",phase,"poses","#","position","@3"}))
                return ProceduralReplayNumber::Linear;
            if(matches({"rigs","#","history",phase,"poses","#","rotation","@4"})||
               matches({"rigs","#","layers","#","history",phase,"poses","#","rotation","@4"}))
                return ProceduralReplayNumber::Angular;
            if(matches({"rigs","#","history",phase,"poses","#","scale","@3"})||
               matches({"rigs","#","layers","#","history",phase,"poses","#","scale","@3"}))
                return ProceduralReplayNumber::Dimensionless;
        }
    }
    // QueryRay is intentionally exact. The retained journal is authoritative;
    // a future relaxation needs separate evidence and an explicit new policy.
    return ProceduralReplayNumber::Exact;
}

const char* procedural_replay_category(ProceduralReplayNumber category) {
    switch(category) {
    case ProceduralReplayNumber::Linear:return "linear";
    case ProceduralReplayNumber::Angular:return "angular";
    case ProceduralReplayNumber::Dimensionless:return "dimensionless";
    default:return "exact";
    }
}

std::string procedural_replay_location(const ProceduralReplayPath& path) {
    std::string result="$";
    for(const auto& part:path) {
        result+='/';
        if(part.array)result+=std::to_string(part.index);
        else for(const auto character:part.field) {
            if(character=='~')result+="~0";
            else if(character=='/')result+="~1";
            else result+=character;
        }
    }
    return result;
}

std::string procedural_replay_excerpt(const StateJson& value) {
    if(value.is_object())return "<object size="+std::to_string(value.size())+">";
    if(value.is_array())return "<array size="+std::to_string(value.size())+">";
    if(value.is_string()&&value.get_ref<const std::string&>().size()>96)return "<string exceeding 96 bytes>";
    return value.dump().substr(0,128);
}

std::string procedural_replay_mismatch(const StateJson& stored,const StateJson& replay,
    const ProceduralReplayPath& path,const char* reason,ProceduralReplayNumber category,
    double bound=0) {
    std::string result=procedural_replay_location(path)+" ("+reason+", category="+
        procedural_replay_category(category)+", bound="+StateJson(bound).dump()+
        "): stored="+procedural_replay_excerpt(stored)+", replay="+procedural_replay_excerpt(replay);
    if(stored.is_number_float()&&replay.is_number_float()) {
        const double a=stored.get<double>(),b=replay.get<double>();
        if(std::isfinite(a)&&std::isfinite(b)) {
            const double error=std::abs(a-b);
            result+=std::isfinite(error)?", absolute_error="+StateJson(error).dump():", absolute_error=overflow";
        }
    }
    return result;
}

std::string procedural_replay_compare(const StateJson& stored,const StateJson& replay,
    ProceduralReplayScope scope,ProceduralReplayPath& path) {
    const auto exact=ProceduralReplayNumber::Exact;
    if(path.size()>64)return procedural_replay_mismatch(stored,replay,path,"excessive nesting",exact);
    if(stored.type()!=replay.type())return procedural_replay_mismatch(stored,replay,path,"scalar/container type",exact);
    if(stored.is_object()) {
        for(auto entry=stored.begin();entry!=stored.end();++entry)if(!replay.contains(entry.key())) {
            path.push_back({false,0,entry.key()});
            const auto difference=procedural_replay_mismatch(entry.value(),StateJson(nullptr),path,"missing replay field",exact);
            path.pop_back();return difference;
        }
        for(auto entry=replay.begin();entry!=replay.end();++entry)if(!stored.contains(entry.key())) {
            path.push_back({false,0,entry.key()});
            const auto difference=procedural_replay_mismatch(StateJson(nullptr),entry.value(),path,"extra replay field",exact);
            path.pop_back();return difference;
        }
        for(auto entry=stored.begin();entry!=stored.end();++entry) {
            path.push_back({false,0,entry.key()});
            const auto difference=procedural_replay_compare(entry.value(),replay.at(entry.key()),scope,path);
            path.pop_back();if(!difference.empty())return difference;
        }
        return {};
    }
    if(stored.is_array()) {
        if(stored.size()!=replay.size())return procedural_replay_mismatch(stored,replay,path,"array membership",exact);
        for(std::size_t i=0;i<stored.size();++i) {
            path.push_back({true,i,{}});
            const auto difference=procedural_replay_compare(stored[i],replay[i],scope,path);
            path.pop_back();if(!difference.empty())return difference;
        }
        return {};
    }
    if(stored.is_number_float()) {
        const double a=stored.get<double>(),b=replay.get<double>();
        if(!std::isfinite(a)||!std::isfinite(b))return procedural_replay_mismatch(stored,replay,path,"nonfinite value",exact);
        if(stored.dump()==replay.dump())return {};
        const auto category=procedural_replay_number(scope,path);
        if(category!=exact) {
            const double ceiling=category==ProceduralReplayNumber::Linear?procedural_replay_linear_ceiling:
                category==ProceduralReplayNumber::Angular?procedural_replay_angular_ceiling:procedural_replay_dimensionless_ceiling;
            const double magnitude=std::max({1.,std::abs(a),std::abs(b)});
            const double bound=std::min(ceiling,procedural_replay_roundoff*magnitude);
            const double error=std::abs(a-b);
            if(std::isfinite(error)&&error<=bound)return {};
            return procedural_replay_mismatch(stored,replay,path,"reader numeric resolution exceeded",category,bound);
        }
    } else if(stored.dump()==replay.dump())return {};
    return procedural_replay_mismatch(stored,replay,path,"exact value",exact);
}

std::string procedural_replay_difference(const StateJson& stored,const StateJson& replay,
    ProceduralReplayScope scope) {
    ProceduralReplayPath path;
    return procedural_replay_compare(stored,replay,scope,path);
}

void require_procedural_replay_equal(const StateJson& stored,const StateJson& replay,
    ProceduralReplayScope scope,const char* message) {
    const auto difference=procedural_replay_difference(stored,replay,scope);
    if(!difference.empty())throw std::runtime_error(std::string(message)+" First difference at "+difference+".");
}
