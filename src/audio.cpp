// SPDX-License-Identifier: Apache-2.0
#include "poima/audio.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>
#if POIMA_AUDIO
#include <phonon.h>
#endif
namespace poima {
bool audio_available() { return POIMA_AUDIO!=0; }
#if POIMA_AUDIO
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
void status(IPLerror code) { check(code==IPL_STATUS_SUCCESS,"Steam Audio object creation failed."); }
template<class T,auto release> struct Handle {
    T value=nullptr;
    Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(std::exchange(other.value,nullptr)) {}
    ~Handle() { if(value)release(&value); }
};
IPLVector3 vector(const std::array<double,3>& p) {
    for(double v:p)check(std::isfinite(v) && std::abs(v)<=10000,"Audio coordinates exceed the initial +/-10 km envelope.");
    return {static_cast<float>(p[0]),static_cast<float>(p[1]),static_cast<float>(p[2])};
}
std::array<double,3> position(const Matrix4& m) { return {m[12],m[13],m[14]}; }
IPLCoordinateSpace3 coordinates(const Matrix4& m) {
    check(rigid_transform(m),"Audio listener/source hierarchy must not scale or shear.");
    return {{float(m[0]),float(m[1]),float(m[2])},{float(m[4]),float(m[5]),float(m[6])},{float(-m[8]),float(-m[9]),float(-m[10])},vector(position(m))};
}
}
#endif
AudioReport observe_audio(const AudioSnapshot& s,std::uint32_t frames) {
#if !POIMA_AUDIO
    (void)s;(void)frames;throw std::runtime_error("Audio observation is not built. Configure POIMA_ENABLE_AUDIO=ON.");
#else
    check(frames<=10*audio_rate && s.sources.size()<=max_audio_sources && s.geometry.size()<=10000,"Audio source/capture/geometry budget exceeded.");
    const auto started=Clock::now();AudioReport report;
    Handle<IPLContext,iplContextRelease> context;IPLContextSettings context_settings{};context_settings.version=STEAMAUDIO_VERSION;context_settings.simdLevel=IPL_SIMDLEVEL_SSE2;
    status(iplContextCreate(&context_settings,&context.value));
    Handle<IPLScene,iplSceneRelease> scene;IPLSceneSettings scene_settings{};scene_settings.type=IPL_SCENETYPE_DEFAULT;status(iplSceneCreate(context.value,&scene_settings,&scene.value));
    std::vector<IPLVector3> vertices;std::vector<IPLTriangle> triangles;std::vector<IPLint32> material_indices;std::vector<IPLMaterial> materials;
    constexpr std::array<std::array<float,3>,8> corners{{{-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,.5f,-.5f},{-.5f,.5f,-.5f},{-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f}}};
    constexpr std::array<std::uint32_t,36> box{{0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5}};
    for(const auto& g:s.geometry) {
        if(!g.material.enabled)continue;
        validate_acoustic_material(g.material);
        const auto count=g.mesh ? g.mesh->indices.size() : box.size();check(count%3==0 && count/3<=max_audio_triangles-triangles.size(),"Audio geometry exceeds 131072 triangles.");
        const auto vertex_count=g.mesh ? g.mesh->vertices.size() : corners.size();
        check(vertices.size()<=3*max_audio_triangles && vertex_count<=3*max_audio_triangles-vertices.size(),"Audio geometry vertex budget exceeded.");
        const auto first=static_cast<IPLint32>(vertices.size()),material=static_cast<IPLint32>(materials.size());
        auto append=[&](const std::array<float,3>& p) { std::array<double,3> q{};for(std::size_t row=0;row<3;++row)q[row]=g.world[12+row]+g.world[row]*p[0]+g.world[4+row]*p[1]+g.world[8+row]*p[2];vertices.push_back(vector(q)); };
        if(g.mesh)for(const auto& v:g.mesh->vertices)append(v.position);else for(const auto& v:corners)append(v);
        const std::span<const std::uint32_t> indices=g.mesh ? std::span<const std::uint32_t>(g.mesh->indices) : std::span<const std::uint32_t>(box);
        for(std::size_t i=0;i<count;i+=3) { IPLTriangle t{};for(std::size_t k=0;k<3;++k) { check(indices[i+k]<vertices.size()-static_cast<std::size_t>(first),"Invalid acoustic triangle index.");t.indices[k]=first+static_cast<IPLint32>(indices[i+k]); }triangles.push_back(t);material_indices.push_back(material); }
        IPLMaterial m{};std::copy(g.material.absorption.begin(),g.material.absorption.end(),m.absorption);std::copy(g.material.transmission.begin(),g.material.transmission.end(),m.transmission);m.scattering=g.material.scattering;materials.push_back(m);
    }
    Handle<IPLStaticMesh,iplStaticMeshRelease> mesh;
    if(!triangles.empty()) {
        IPLStaticMeshSettings settings{};settings.numVertices=static_cast<IPLint32>(vertices.size());settings.numTriangles=static_cast<IPLint32>(triangles.size());settings.numMaterials=static_cast<IPLint32>(materials.size());
        settings.vertices=vertices.data();settings.triangles=triangles.data();settings.materialIndices=material_indices.data();settings.materials=materials.data();status(iplStaticMeshCreate(scene.value,&settings,&mesh.value));iplStaticMeshAdd(mesh.value,scene.value);
    }
    iplSceneCommit(scene.value);report.triangles=triangles.size();
    Handle<IPLSimulator,iplSimulatorRelease> simulator;IPLSimulationSettings settings{};settings.flags=IPL_SIMULATIONFLAGS_DIRECT;settings.sceneType=IPL_SCENETYPE_DEFAULT;settings.maxNumOcclusionSamples=1;settings.maxNumSources=static_cast<IPLint32>(max_audio_sources);settings.numThreads=1;settings.samplingRate=audio_rate;settings.frameSize=audio_block;
    status(iplSimulatorCreate(context.value,&settings,&simulator.value));iplSimulatorSetScene(simulator.value,scene.value);
    std::vector<Handle<IPLSource,iplSourceRelease>> sources;sources.reserve(s.sources.size());
    const auto listener=coordinates(s.listener);
    for(const auto& source:s.sources) {
        check(source.emitter.clip && !source.emitter.clip->samples.empty() && source.emitter.clip->samples.size()<=max_audio_clip_frames,"Emitter requires a bounded clip.");
        check(std::isfinite(source.emitter.gain) && source.emitter.gain>=0 && source.emitter.gain<=4,"Audio gain must be in [0,4].");
        sources.emplace_back();IPLSourceSettings source_settings{};source_settings.flags=IPL_SIMULATIONFLAGS_DIRECT;status(iplSourceCreate(simulator.value,&source_settings,&sources.back().value));iplSourceAdd(sources.back().value,simulator.value);
        IPLSimulationInputs inputs{};inputs.flags=IPL_SIMULATIONFLAGS_DIRECT;inputs.directFlags=static_cast<IPLDirectSimulationFlags>(IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION|IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION|IPL_DIRECTSIMULATIONFLAGS_OCCLUSION|IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
        // Initial sources are omnidirectional points. Object scale/shear does
        // not change their orientation; only the listener needs a rigid pose.
        auto source_pose=identity_matrix();std::copy_n(source.world.begin()+12,3,source_pose.begin()+12);
        inputs.source=coordinates(source_pose);inputs.distanceAttenuationModel.type=IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;inputs.distanceAttenuationModel.minDistance=1;inputs.airAbsorptionModel.type=IPL_AIRABSORPTIONTYPE_DEFAULT;inputs.occlusionType=IPL_OCCLUSIONTYPE_RAYCAST;inputs.numTransmissionRays=8;
        iplSourceSetInputs(sources.back().value,IPL_SIMULATIONFLAGS_DIRECT,&inputs);
    }
    iplSimulatorCommit(simulator.value);IPLSimulationSharedInputs shared{};shared.listener=listener;iplSimulatorSetSharedInputs(simulator.value,IPL_SIMULATIONFLAGS_DIRECT,&shared);
    report.scene_ms=ms(started);auto mark=Clock::now();iplSimulatorRunDirect(simulator.value);report.simulation_ms=ms(mark);
    std::vector<IPLDirectEffectParams> direct;
    for(std::size_t i=0;i<s.sources.size();++i) {
        IPLSimulationOutputs out{};iplSourceGetOutputs(sources[i].value,IPL_SIMULATIONFLAGS_DIRECT,&out);direct.push_back(out.direct);
        AudioPath path;path.entity=s.sources[i].entity;path.asset=s.sources[i].emitter.asset;path.source=position(s.sources[i].world);path.listener=position(s.listener);
        std::array<double,3> delta{};for(std::size_t j=0;j<3;++j) { delta[j]=path.source[j]-path.listener[j];path.distance+=delta[j]*delta[j]; }path.distance=std::sqrt(path.distance);check(path.distance<=10000,"Audio propagation distance exceeds 10 km.");
        if(path.distance>1e-8)for(std::size_t j=0;j<3;++j)path.direction[j]=(s.listener[j*4]*delta[0]+s.listener[j*4+1]*delta[1]+s.listener[j*4+2]*delta[2])/path.distance;
        else path.direction={0,0,-1};
        path.propagation_delay_samples=static_cast<std::uint32_t>(std::llround(path.distance/343.0*audio_rate));path.distance_gain=out.direct.distanceAttenuation;path.occlusion=out.direct.occlusion;
        auto coefficient=[](float v) { check(std::isfinite(v) && v>=0 && v<=1,"Steam Audio returned an invalid direct-path coefficient."); };
        coefficient(path.distance_gain);coefficient(path.occlusion);
        for(float v:out.direct.airAbsorption)coefficient(v);
        for(float v:out.direct.transmission)coefficient(v);
        std::copy_n(out.direct.airAbsorption,3,path.air.begin());std::copy_n(out.direct.transmission,3,path.transmission.begin());report.paths.push_back(path);
    }
    if(frames==0)return report;
    mark=Clock::now();IPLAudioSettings audio_settings{audio_rate,audio_block};Handle<IPLHRTF,iplHRTFRelease> hrtf;
    IPLHRTFSettings hrtf_settings{};hrtf_settings.type=IPL_HRTFTYPE_DEFAULT;hrtf_settings.volume=1;status(iplHRTFCreate(context.value,&audio_settings,&hrtf_settings,&hrtf.value));
    report.samples.resize(std::size_t(frames)*2);
    for(std::size_t i=0;i<s.sources.size();++i) {
        Handle<IPLDirectEffect,iplDirectEffectRelease> filter;IPLDirectEffectSettings filter_settings{1};status(iplDirectEffectCreate(context.value,&audio_settings,&filter_settings,&filter.value));
        Handle<IPLBinauralEffect,iplBinauralEffectRelease> spatial;IPLBinauralEffectSettings spatial_settings{hrtf.value};status(iplBinauralEffectCreate(context.value,&audio_settings,&spatial_settings,&spatial.value));
        std::array<float,audio_block> mono{},filtered{},left{},right{};float* input_channels[]{mono.data()};float* filtered_channels[]{filtered.data()};float* output_channels[]{left.data(),right.data()};
        IPLAudioBuffer input{1,audio_block,input_channels},middle{1,audio_block,filtered_channels},output{2,audio_block,output_channels};
        auto params=direct[i];params.flags=static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION|IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION|IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION|IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);params.transmissionType=IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
        IPLBinauralEffectParams spatial_params{};spatial_params.direction=vector(report.paths[i].direction);spatial_params.interpolation=IPL_HRTFINTERPOLATION_BILINEAR;spatial_params.spatialBlend=1;spatial_params.hrtf=hrtf.value;
        const auto& emitter=s.sources[i].emitter;const auto& clip=emitter.clip->samples;const auto delay=report.paths[i].propagation_delay_samples;
        for(std::uint32_t first=0;first<frames;first+=audio_block) {
            for(std::uint32_t k=0;k<audio_block;++k) { const auto sample=first+k;mono[k]=sample>=delay && (emitter.loop || sample-delay<clip.size()) ? clip[(sample-delay)%clip.size()]*emitter.gain : 0; }
            iplDirectEffectApply(filter.value,&params,&input,&middle);iplBinauralEffectApply(spatial.value,&spatial_params,&middle,&output);
            // FFT/filter roundoff can produce tiny values before an impulse.
            // The engine's explicit propagation delay guarantees exact silence
            // until this voice's scheduled arrival, independently of DSP noise.
            for(std::uint32_t k=0;k<audio_block && first+k<frames;++k)if(first+k>=delay) { report.samples[2*(first+k)]+=left[k];report.samples[2*(first+k)+1]+=right[k]; }
        }
    }
    report.dsp_ms=ms(mark);double squares=0;
    for(float sample:report.samples) { check(std::isfinite(sample),"Steam Audio produced nonfinite output.");report.peak=std::max(report.peak,std::abs(double(sample)));squares+=double(sample)*sample;if(std::abs(sample)>1)++report.over_range_samples; }
    report.rms=std::sqrt(squares/double(report.samples.size()));return report;
#endif
}
}
