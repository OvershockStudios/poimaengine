// SPDX-License-Identifier: Apache-2.0
#include "runtime_physics_state.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Geometry/Sphere.h>
#include <Jolt/Physics/Body/BodyPair.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace poima {
namespace {
using Json=nlohmann::json;
constexpr auto state_mask=JPH::EStateRecorderState::Global|JPH::EStateRecorderState::Bodies|
    JPH::EStateRecorderState::Contacts;
constexpr const char* profile="jolt-036ea7b1d717b3e713ac9d8cbd47118fb9cd5d60-r64-f32-le-v1";
static_assert(std::endian::native==std::endian::little);
static_assert(sizeof(float)==4&&sizeof(double)==8&&sizeof(bool)==1);
static_assert(sizeof(JPH::Float3)==3*sizeof(float)&&sizeof(JPH::Double3)==3*sizeof(double));
static_assert(sizeof(JPH::Sphere)==4*sizeof(float)&&sizeof(JPH::Quat)==4*sizeof(float));
static_assert(sizeof(JPH::BodyPair)==2*sizeof(std::uint32_t)&&sizeof(JPH::SubShapeIDPair)==4*sizeof(std::uint32_t));
#ifndef JPH_DOUBLE_PRECISION
#error Runtime physical-state profile requires Jolt double-precision positions
#endif
[[noreturn]] void invalid(const char* text) {throw std::invalid_argument(text);}
void keys(const Json& v,std::initializer_list<const char*> names) {
    if(!v.is_object()||v.size()!=names.size())invalid("Physics state object keys differ from the pinned schema.");
    for(auto name:names)if(!v.contains(name))invalid("Physics state required field is missing.");
}
void array(const Json& v,std::size_t max) {
    if(!v.is_array()||v.size()>max)invalid("Physics state array exceeds its bound.");
}
std::uint32_t integer(const Json& v,std::uint32_t max=std::numeric_limits<std::uint32_t>::max()) {
    if(!v.is_number_integer()||v.is_boolean())invalid("Physics state integer has an invalid type.");
    if(v.is_number_unsigned()) {
        auto x=v.get<std::uint64_t>();if(x>max)invalid("Physics state integer exceeds its bound.");
        return static_cast<std::uint32_t>(x);
    }
    auto x=v.get<std::int64_t>();if(x<0||std::uint64_t(x)>max)invalid("Physics state integer exceeds its bound.");
    return static_cast<std::uint32_t>(x);
}
bool boolean(const Json& v) {
    if(!v.is_boolean())invalid("Physics state boolean has an invalid type.");return v.get<bool>();
}
double number(const Json& v,double bound,bool f32=true) {
    if(!v.is_number()||v.is_boolean())invalid("Physics state scalar has an invalid type.");
    const double x=v.get<double>();
    if(!std::isfinite(x)||std::abs(x)>bound||(f32&&double(float(x))!=x))
        invalid("Physics state scalar is nonfinite, outside bounds, or not exactly native-representable.");
    return x;
}
double vector(const Json& v,std::size_t n,double bound,bool f32=true) {
    if(!v.is_array()||v.size()!=n)invalid("Physics state vector has an invalid dimension.");
    double norm2=0;for(const auto& x:v){double d=number(x,bound,f32);norm2+=d*d;}return norm2;
}
std::uint32_t body_id(const Json& v) {
    auto id=integer(v);
    if((id&JPH::BodyID::cBroadPhaseBit)!=0||(id&JPH::BodyID::cMaxBodyIndex)>=runtime_physics_state_max_bodies)
        invalid("Physics state body ID is outside the owned rigid-body profile.");
    return id;
}
struct BodyInfo {std::uint32_t id;int motion;bool properties;};
std::vector<BodyInfo> body_info(const JPH::PhysicsSystem& system) {
    JPH::BodyIDVector ids;system.GetBodies(ids);
    if(ids.size()>runtime_physics_state_max_bodies)invalid("Physics world exceeds the portable body limit.");
    std::vector<BodyInfo> out;
    for(auto id:ids) {
        JPH::BodyLockRead lock(system.GetBodyLockInterface(),id);
        if(!lock.Succeeded())invalid("Physics state body disappeared during owner-thread capture.");
        const auto& b=lock.GetBody();
        if(!b.IsRigidBody())invalid("Soft-body state is outside the pinned portable profile.");
        if(b.IsCollisionCacheInvalid())
            invalid("Portable physics state requires a committed boundary without pending live contact-cache invalidation.");
        if(b.IsInBroadPhase())out.push_back({id.GetIndexAndSequenceNumber(),int(b.GetMotionType()),b.GetMotionPropertiesUnchecked()!=nullptr});
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return (a.id&JPH::BodyID::cMaxBodyIndex)<(b.id&JPH::BodyID::cMaxBodyIndex);});
    return out;
}
// This reader only receives bytes produced in this process by pinned Jolt.
// User input is JSON; it never reaches this reader or a native recorder.
struct TrustedReader {
    const std::string& data;std::size_t offset=0;
    template<class T>T read() {
        static_assert(std::is_trivially_copyable_v<T>);
        if(sizeof(T)>data.size()-offset)throw std::runtime_error("Pinned Jolt state was truncated.");
        T out;std::memcpy(&out,data.data()+offset,sizeof(T));offset+=sizeof(T);return out;
    }
    bool flag(){auto x=read<std::uint8_t>();if(x>1)throw std::runtime_error("Pinned Jolt bool encoding changed.");return x!=0;}
    template<class T>Json values(std::size_t n){auto a=Json::array();for(std::size_t i=0;i<n;++i)a.push_back(read<T>());return a;}
    std::uint32_t count(std::size_t max){auto n=read<std::uint32_t>();if(n>max)throw std::runtime_error("Pinned Jolt state exceeded its owned count.");return n;}
    Json key(){return {{"body1",read<std::uint32_t>()},{"subshape1",read<std::uint32_t>()},{"body2",read<std::uint32_t>()},{"subshape2",read<std::uint32_t>()}};}
};
Json decode(const std::string& data,const std::vector<BodyInfo>& infos) {
    TrustedReader r{data};
    if(r.read<std::uint8_t>()!=7)throw std::runtime_error("Pinned Jolt state mask changed.");
    Json out={{"profile",profile},{"previous_step_dt",r.read<float>()},{"gravity",r.values<float>(3)},
        {"bodies",Json::array()},{"pairs",Json::array()},{"ccd",Json::array()}};
    auto count=r.count(runtime_physics_state_max_bodies);
    if(count!=infos.size())throw std::runtime_error("Pinned Jolt body count differed from locked metadata.");
    for(std::size_t i=0;i<count;++i) {
        auto id=r.read<std::uint32_t>();if(id!=infos[i].id)throw std::runtime_error("Pinned Jolt body slot order changed.");
        Json b={{"id",id},{"active",r.flag()},{"motion_type",infos[i].motion},
            {"position",r.values<double>(3)},{"rotation",r.values<float>(4)},{"motion",nullptr}};
        if(infos[i].properties) {
            Json m={{"linear_velocity",r.values<float>(3)},{"angular_velocity",r.values<float>(3)},
                {"force",r.values<float>(3)},{"torque",r.values<float>(3)},{"sleep_offset",r.values<double>(3)},
                {"sleep_spheres",Json::array()}};
            // Sphere is exactly Float3 center + float radius in the pinned source.
            for(int k=0;k<3;++k)m["sleep_spheres"].push_back({{"center",r.values<float>(3)},{"radius",r.read<float>()}});
            m["sleep_timer"]=r.read<float>();m["allow_sleeping"]=r.flag();b["motion"]=std::move(m);
        }
        out["bodies"].push_back(std::move(b));
    }
    count=r.count(runtime_physics_state_max_pairs);std::size_t manifolds=0;
    for(std::uint32_t i=0;i<count;++i) {
        Json p={{"body_a",r.read<std::uint32_t>()},{"body_b",r.read<std::uint32_t>()},
            {"delta_position",r.values<float>(3)},{"delta_rotation",r.values<float>(3)},{"manifolds",Json::array()}};
        auto n=r.count(runtime_physics_state_max_manifolds);
        if(manifolds+n>runtime_physics_state_max_manifolds)throw std::runtime_error("Pinned Jolt manifold count exceeded its bound.");
        manifolds+=n;
        for(std::uint32_t j=0;j<n;++j) {
            auto key=r.key();auto points=r.read<std::uint16_t>();
            if(points>4)throw std::runtime_error("Pinned Jolt contact-point limit changed.");
            Json m={{"key",std::move(key)},{"normal",r.values<float>(3)},{"points",Json::array()}};
            for(std::uint16_t k=0;k<points;++k)m["points"].push_back({{"position1",r.values<float>(3)},
                {"position2",r.values<float>(3)},{"normal_lambda",r.read<float>()},{"friction_lambda",r.values<float>(2)}});
            p["manifolds"].push_back(std::move(m));
        }
        out["pairs"].push_back(std::move(p));
    }
    count=r.count(runtime_physics_state_max_manifolds);
    if(manifolds+count>runtime_physics_state_max_manifolds)throw std::runtime_error("Pinned Jolt combined manifold count exceeded its bound.");
    for(std::uint32_t i=0;i<count;++i)out["ccd"].push_back(r.key());
    if(r.offset!=data.size())throw std::runtime_error("Pinned Jolt state has unaccounted trailing fields.");
    return out;
}
using Key=std::array<std::uint32_t,4>;
Key contact_key(const Json& v) {
    keys(v,{"body1","subshape1","body2","subshape2"});
    Key out{body_id(v.at("body1")),integer(v.at("subshape1")),body_id(v.at("body2")),integer(v.at("subshape2"))};
    if(out[0]==out[2])invalid("Physics contact cache key refers to the same body twice.");
    return out;
}
void validate(const Json& s,const std::vector<BodyInfo>& infos) {
    keys(s,{"profile","previous_step_dt","gravity","bodies","pairs","ccd"});
    if(!s.at("profile").is_string()||s.at("profile")!=profile)invalid("Physics state profile is unsupported.");
    if(number(s.at("previous_step_dt"),1)<0)invalid("Physics previous dt must be nonnegative.");
    vector(s.at("gravity"),3,1e4);
    const auto& bodies=s.at("bodies");array(bodies,runtime_physics_state_max_bodies);
    if(bodies.size()!=infos.size())invalid("Physics state broadphase membership differs from the target world.");
    for(std::size_t i=0;i<bodies.size();++i) {
        const auto& b=bodies[i];keys(b,{"id","active","motion_type","position","rotation","motion"});
        if(body_id(b.at("id"))!=infos[i].id||integer(b.at("motion_type"),2)!=std::uint32_t(infos[i].motion))
            invalid("Physics state body slot, generation, or motion type differs from the target world.");
        if(boolean(b.at("active"))&&infos[i].motion==int(JPH::EMotionType::Static))invalid("Static physics body cannot be active.");
        vector(b.at("position"),3,1e6,false);
        if(std::abs(vector(b.at("rotation"),4,1.001)-1)>1e-4)invalid("Physics body quaternion is not unit length.");
        const auto& m=b.at("motion");
        if(!infos[i].properties) {if(!m.is_null())invalid("Physics body unexpectedly has motion properties.");continue;}
        keys(m,{"linear_velocity","angular_velocity","force","torque","sleep_offset","sleep_spheres","sleep_timer","allow_sleeping"});
        vector(m.at("linear_velocity"),3,1e4);vector(m.at("angular_velocity"),3,1e4);
        vector(m.at("force"),3,1e10);vector(m.at("torque"),3,1e10);
        vector(m.at("sleep_offset"),3,1e7,false);
        const auto& spheres=m.at("sleep_spheres");if(!spheres.is_array()||spheres.size()!=3)invalid("Physics sleep sphere count differs from the pinned profile.");
        for(const auto& sphere:spheres){keys(sphere,{"center","radius"});vector(sphere.at("center"),3,1e7);if(number(sphere.at("radius"),1e7)<0)invalid("Physics sleep radius must be nonnegative.");}
        if(number(m.at("sleep_timer"),1e9)<0)invalid("Physics sleep timer must be nonnegative.");boolean(m.at("allow_sleeping"));
    }
    const auto& pairs=s.at("pairs");array(pairs,runtime_physics_state_max_pairs);
    std::uint64_t last_pair=0;bool first=true;std::size_t total=0;std::set<Key> all_keys;
    for(const auto& p:pairs) {
        keys(p,{"body_a","body_b","delta_position","delta_rotation","manifolds"});
        auto a=body_id(p.at("body_a")),b=body_id(p.at("body_b"));
        const std::uint64_t sort_key=(std::uint64_t(b)<<32)|a;
        if(a>=b||(!first&&sort_key<=last_pair))invalid("Physics body pairs are not in native canonical sorted order.");
        first=false;last_pair=sort_key;vector(p.at("delta_position"),3,1e7);
        if(vector(p.at("delta_rotation"),3,1.001)>1.0001)invalid("Physics cached delta quaternion is invalid.");
        const auto& ms=p.at("manifolds");array(ms,runtime_physics_state_max_manifolds);
        if(ms.size()>runtime_physics_state_max_manifolds-total)invalid("Physics manifold count exceeds capacity.");total+=ms.size();
        Key previous{};bool first_m=true;
        for(const auto& m:ms) {
            keys(m,{"key","normal","points"});auto key=contact_key(m.at("key"));
            if(!((key[0]==a&&key[2]==b)||(key[0]==b&&key[2]==a)))invalid("Physics manifold does not belong to its body pair.");
            if((!first_m&&!(previous<key))||!all_keys.insert(key).second)invalid("Physics manifold keys are duplicated or unsorted.");
            first_m=false;previous=key;
            if(std::abs(vector(m.at("normal"),3,1.001)-1)>1e-4)invalid("Physics contact normal is not unit length.");
            array(m.at("points"),4);
            for(const auto& point:m.at("points")) {
                keys(point,{"position1","position2","normal_lambda","friction_lambda"});
                vector(point.at("position1"),3,1e7);vector(point.at("position2"),3,1e7);
                if(number(point.at("normal_lambda"),1e8)<0)
                    invalid("Physics nonpenetration warm-start impulse must be nonnegative.");
                vector(point.at("friction_lambda"),2,1e8);
            }
        }
    }
    const auto& ccd=s.at("ccd");array(ccd,runtime_physics_state_max_manifolds);
    if(ccd.size()>runtime_physics_state_max_manifolds-total)invalid("Physics combined manifold count exceeds capacity.");
    Key previous{};first=true;
    for(const auto& item:ccd){auto key=contact_key(item);if((!first&&!(previous<key))||!all_keys.insert(key).second)invalid("Physics CCD keys are duplicated or unsorted.");first=false;previous=key;}
    if(s.dump().size()>runtime_physics_state_max_json_bytes)invalid("Physics state JSON exceeds its owned byte budget.");
}
template<class T>void write_values(JPH::StateRecorderImpl& r,const Json& values) {for(const auto& v:values)r.Write(v.get<T>());}
void write_key(JPH::StateRecorderImpl& r,const Json& key) {
    // Exact field order of SubShapeIDPair, all four fields are uint32, no padding.
    for(auto name:{"body1","subshape1","body2","subshape2"})r.Write(key.at(name).get<std::uint32_t>());
}
JPH::StateRecorderImpl encode(const Json& s) {
    JPH::StateRecorderImpl r;r.Write(state_mask);r.Write(s.at("previous_step_dt").get<float>());write_values<float>(r,s.at("gravity"));
    r.Write(static_cast<std::uint32_t>(s.at("bodies").size()));
    for(const auto& b:s.at("bodies")) {
        r.Write(b.at("id").get<std::uint32_t>());r.Write(b.at("active").get<bool>());
        write_values<double>(r,b.at("position"));write_values<float>(r,b.at("rotation"));
        const auto& m=b.at("motion");if(m.is_null())continue;
        for(auto name:{"linear_velocity","angular_velocity","force","torque"})write_values<float>(r,m.at(name));
        write_values<double>(r,m.at("sleep_offset"));
        for(const auto& sphere:m.at("sleep_spheres")){write_values<float>(r,sphere.at("center"));r.Write(sphere.at("radius").get<float>());}
        r.Write(m.at("sleep_timer").get<float>());r.Write(m.at("allow_sleeping").get<bool>());
    }
    r.Write(static_cast<std::uint32_t>(s.at("pairs").size()));
    for(const auto& p:s.at("pairs")) {
        r.Write(p.at("body_a").get<std::uint32_t>());r.Write(p.at("body_b").get<std::uint32_t>());
        write_values<float>(r,p.at("delta_position"));write_values<float>(r,p.at("delta_rotation"));
        r.Write(static_cast<std::uint32_t>(p.at("manifolds").size()));
        for(const auto& m:p.at("manifolds")) {
            write_key(r,m.at("key"));r.Write(static_cast<std::uint16_t>(m.at("points").size()));write_values<float>(r,m.at("normal"));
            for(const auto& point:m.at("points")){write_values<float>(r,point.at("position1"));write_values<float>(r,point.at("position2"));r.Write(point.at("normal_lambda").get<float>());write_values<float>(r,point.at("friction_lambda"));}
        }
    }
    r.Write(static_cast<std::uint32_t>(s.at("ccd").size()));for(const auto& key:s.at("ccd"))write_key(r,key);
    if(r.GetDataSize()>runtime_physics_state_max_native_bytes)invalid("Physics state native encoding exceeds its owned byte budget.");
    return r;
}
JPH::StateRecorderImpl native_capture(const JPH::PhysicsSystem& system) {
    JPH::StateRecorderImpl r;system.SaveState(r,state_mask);
    if(r.IsFailed()||r.GetDataSize()>runtime_physics_state_max_native_bytes)throw std::runtime_error("Native physics capture exceeded its owned byte budget.");
    return r;
}
}
Json capture_runtime_physics_state(const JPH::PhysicsSystem& system) {
    const auto infos=body_info(system);auto native=native_capture(system);const auto bytes=native.GetData();
    auto result=decode(bytes,infos);validate(result,infos);auto encoded=encode(result);
    // No padding, hidden fields, canonicalization or tolerance is exempted.
    if(encoded.GetData()!=bytes)throw std::runtime_error("Named physics state did not exactly re-encode pinned native SaveState bytes.");
    return result;
}
void validate_runtime_physics_state(const JPH::PhysicsSystem& system,const Json& state) {
    validate(state,body_info(system));(void)encode(state);
}
void restore_runtime_physics_state(JPH::PhysicsSystem& system,const Json& state) {
    validate(state,body_info(system));auto fresh=encode(state);auto old=native_capture(system);
    fresh.Rewind();bool restored=false;
    try {restored=system.RestoreState(fresh)&&!fresh.IsFailed();}
    catch(...) {
        old.Rewind();if(!system.RestoreState(old)||old.IsFailed())throw std::runtime_error("Native physics restore threw and trusted rollback failed; discard the candidate world.");
        throw;
    }
    if(!restored) {
        old.Rewind();if(!system.RestoreState(old)||old.IsFailed())throw std::runtime_error("Native physics restore failed and trusted rollback failed; discard the candidate world.");
        throw std::runtime_error("Native physics restore failed; trusted previous state restored.");
    }
}
}
