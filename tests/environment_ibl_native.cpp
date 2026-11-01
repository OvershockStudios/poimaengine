// SPDX-License-Identifier: Apache-2.0
#include "poima/environment_ibl.hpp"
#include "poima/jobs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
using V=std::array<double,3>;
constexpr double pi=std::numbers::pi_v<double>;
void check(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
void close(double actual,double expected,double tolerance,const char* message) {
    check(std::isfinite(actual) && std::isfinite(expected) && std::abs(actual-expected)<=tolerance,message);
}
template<class Function> void rejects(Function function,const char* message) {
    try { function(); }catch(const std::exception&) { return; }
    throw std::runtime_error(message);
}
V normalized(V value) {
    const double length=std::hypot(value[0],value[1],value[2]);
    for(auto& x:value)x/=length;
    return value;
}
V texel_direction(unsigned face,unsigned x,unsigned y,unsigned size) {
    // Independent orientation oracle: face centre and increasing U/V axes.
    constexpr V centres[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    constexpr V u_axes[]={{0,0,-1},{0,0,1},{1,0,0},{1,0,0},{1,0,0},{-1,0,0}};
    constexpr V v_axes[]={{0,-1,0},{0,-1,0},{0,0,1},{0,0,-1},{0,-1,0},{0,-1,0}};
    V result{};
    for(std::size_t axis=0;axis<3;++axis)
        result[axis]=centres[face][axis]+u_axes[face][axis]*(2*(x+.5)/size-1)+v_axes[face][axis]*(2*(y+.5)/size-1);
    return normalized(result);
}
const std::array<float,4>& texel(const poima::EnvironmentIblLevel& level,unsigned face,unsigned x,unsigned y) {
    return level.texels[(std::size_t(face)*level.size+y)*level.size+x];
}
poima::SkySettings directional_sky() {
    poima::SkySettings sky;sky.enabled=true;sky.zenith={1,0,0};sky.horizon={0,0,0};sky.ground={0,0,1};sky.horizon_falloff=1;
    return sky;
}
void check_dimensions(const poima::EnvironmentIbl& value) {
    check(value.diffuse.size==16 && value.diffuse.texels.size()==6*16*16,"Wrong diffuse layout.");
    std::size_t bytes=value.diffuse.texels.size()*sizeof(value.diffuse.texels.front());
    for(unsigned mip=0;mip<7;++mip) {
        const auto& level=value.specular[mip];const auto size=64u>>mip;
        check(level.size==size && level.texels.size()==std::size_t(6)*size*size,"Wrong specular mip/face layout.");
        bytes+=level.texels.size()*sizeof(level.texels.front());
    }
    check(value.brdf.size()==64*64,"Wrong BRDF LUT layout.");bytes+=value.brdf.size()*sizeof(value.brdf.front());
    check(bytes==581600,"Unbounded or changed environment texel storage.");
}
void equal(const poima::EnvironmentIbl& left,const poima::EnvironmentIbl& right) {
    check(left.diffuse.size==right.diffuse.size && left.diffuse.texels==right.diffuse.texels,"Parallel diffuse output changed complete values.");
    for(std::size_t mip=0;mip<left.specular.size();++mip)
        check(left.specular[mip].size==right.specular[mip].size && left.specular[mip].texels==right.specular[mip].texels,
              "Parallel specular output changed complete values.");
    check(left.brdf==right.brdf,"Parallel BRDF LUT changed complete values.");
}
// Integrate D*correlated-Smith*F*NoL directly over uniform solid-angle cells.
// This does not use the bake's importance sampler, half-vector PDF or sequence.
std::array<double,2> reference_brdf(double nv,double roughness) {
    constexpr unsigned vertical=256,azimuths=1024;
    const V view{std::sqrt(1-nv*nv),0,nv};
    const double alpha=roughness*roughness,alpha_squared=alpha*alpha;
    std::array<double,2> sum{};
    for(unsigned y=0;y<vertical;++y) {
        const double nl=(y+.5)/vertical,radial=std::sqrt(1-nl*nl);
        for(unsigned x=0;x<azimuths;++x) {
            const double angle=2*pi*(x+.5)/azimuths;
            const V light{radial*std::cos(angle),radial*std::sin(angle),nl};
            const auto half=normalized({view[0]+light[0],view[1]+light[1],view[2]+light[2]});
            const double vh=std::clamp(view[0]*half[0]+view[1]*half[1]+view[2]*half[2],0.,1.);
            const double shape=half[2]*half[2]*(alpha_squared-1)+1;
            const double distribution=alpha_squared/(pi*shape*shape);
            const double visibility=.5/std::max(nl*std::sqrt(nv*nv*(1-alpha_squared)+alpha_squared)+
                                                  nv*std::sqrt(nl*nl*(1-alpha_squared)+alpha_squared),1e-6);
            const double fresnel=std::pow(1-vh,5),weight=distribution*visibility*nl*(2*pi/(vertical*azimuths));
            sum[0]+=(1-fresnel)*weight;sum[1]+=fresnel*weight;
        }
    }
    return sum;
}
void check_brdf(const poima::EnvironmentIbl& baked) {
    for(const auto& sample:baked.brdf) {
        check(std::isfinite(sample[0]) && std::isfinite(sample[1]) && sample[0]>=0 && sample[1]>=0,"Nonfinite/negative BRDF energy.");
        check(double(sample[0])+sample[1]<=1.000001,"White furnace gained specular energy.");
    }
    for(unsigned y:{31u,63u})for(unsigned x:{3u,15u,31u,63u}) {
        const auto expected=reference_brdf((x+.5)/64,(y+.5)/64);const auto& actual=baked.brdf[y*64+x];
        // Fixed512 visible-normal samples versus independent dense quadrature.
        close(actual[0],expected[0],.003,"GGX scale differs from direct correlated-Smith integral.");
        close(actual[1],expected[1],.0015,"GGX Fresnel bias differs from direct integral.");
    }
    const auto& smooth=baked.brdf[63];
    check(smooth[0]>.99 && smooth[1]<1e-4,"Smooth near-normal BRDF lost mirror energy.");
    check(baked.brdf[0][0]<.07 && baked.brdf[0][1]>.93,"Smooth grazing BRDF lost Schlick's angular response.");
    const auto& rough=baked.brdf[63*64+63];
    check(rough[0]+rough[1]>.25 && rough[0]+rough[1]<.4,"Rough normal-incidence energy is not correlated GGX.");
}
void constant_sky() {
    poima::SkySettings sky;sky.zenith=sky.horizon=sky.ground={.25f,.5f,.75f};
    // Pure convolution ignores presentation flags and sun/disk metadata.
    sky.enabled=false;sky.sun="unused";sky.sun_size_degrees=std::numeric_limits<float>::quiet_NaN();sky.sun_intensity=-1;
    const auto baked=poima::bake_environment_ibl(sky);check_dimensions(baked);
    auto check_level=[](const poima::EnvironmentIblLevel& level) {
        for(const auto& sample:level.texels)check(sample==std::array<float,4>{.25f,.5f,.75f,1},"Constant environment changed radiance or E/pi normalization.");
    };
    check_level(baked.diffuse);for(const auto& level:baked.specular)check_level(level);
    check_brdf(baked);
}
double hemisphere_ramp(double normal_y) {
    // Closed-form Lambert phase integral of max(L.y,0), divided by pi.
    return 2/(3*pi)*(std::sqrt(std::max(0.,1-normal_y*normal_y))+(pi-std::acos(std::clamp(normal_y,-1.,1.)))*normal_y);
}
void directional_and_parallel() {
    const auto sky=directional_sky();const auto serial=poima::bake_environment_ibl(sky);check_dimensions(serial);
    const auto& sharp=serial.specular[0];
    for(unsigned face=0;face<6;++face)for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        const auto direction=texel_direction(face,x,y,64);const auto& sample=texel(sharp,face,x,y);
        close(sample[0],std::max(direction[1],0.),2e-7,"Cube face/U/V orientation differs from Vulkan oracle.");
        close(sample[1],0,0,"Directional sky gained green.");
        close(sample[2],std::max(-direction[1],0.),2e-7,"Cube vertical orientation is inverted.");check(sample[3]==1,"Cube alpha is not one.");
    }
    for(unsigned y=0;y<64;++y)
        check(texel(sharp,0,0,y)==texel(sharp,4,63,y),"Equal-Y cube seam directions changed gradient values.");
    for(unsigned face=0;face<6;++face)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const auto direction=texel_direction(face,x,y,16);const auto& sample=texel(serial.diffuse,face,x,y);
        close(sample[0],hemisphere_ramp(direction[1]),.02,"Diffuse differs from independent Lambert ramp integral.");
        close(sample[2],hemisphere_ramp(-direction[1]),.02,"Lower hemisphere diffuse differs from analytic integral.");
        close(sample[1],0,0,"Diffuse invented green radiance.");
    }
    const auto& broad=serial.specular.back();double sharp_max=0,broad_max=0;
    for(const auto& value:sharp.texels)sharp_max=std::max(sharp_max,double(value[0]));
    for(const auto& value:broad.texels)broad_max=std::max(broad_max,double(value[0]));
    check(broad_max<sharp_max*.85,"Maximum roughness did not broaden environment response.");
    close(texel(broad,0,0,0)[0],2/(3*pi),.02,"Rough horizon prefilter did not gather off-axis sky radiance.");
    close(texel(broad,0,0,0)[0],texel(broad,4,0,0)[0],.015,"Axially symmetric sky changed under azimuth rotation.");
    for(const auto& level:serial.specular)for(const auto& value:level.texels)
        for(float channel:value)check(std::isfinite(channel) && channel>=0 && channel<=1,"Specular convolution escaped finite source bounds.");
    poima::jobs::Config config;config.workers=2;config.trace_always=true;
    poima::jobs::Executor executor(config);const auto parallel=poima::bake_environment_ibl(sky,&executor);equal(serial,parallel);
    const auto status=executor.status();check(status.workers==2 && status.frame.high_water_tasks==56 && status.frame.tasks==0 && status.frame.groups==0 && status.executing==0,"Bake did not retire its bounded56-task frame group.");
    const auto traces=executor.drain_traces();check(traces.dropped==0 && traces.records.size()==56,"Bake task traces are incomplete.");
    for(const auto& trace:traces.records)check(trace.outcome==poima::jobs::Outcome::succeeded,"Bake task failed or was cancelled.");
    executor.shutdown();
    rejects([&]{(void)poima::bake_environment_ibl(sky,&executor);},"Stopped executor admitted a bake.");
}
void invalid_and_atomic_admission() {
    auto sky=directional_sky();
    for(float invalid:{-1.f,1.00001f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        sky=directional_sky();sky.zenith[1]=invalid;
        rejects([&]{(void)poima::bake_environment_ibl(sky);},"Invalid gradient admitted a bake.");
        rejects([&]{(void)poima::environment_ibl_radiance(sky,{0,1,0});},"Invalid gradient admitted a radiance query.");
    }
    for(float invalid:{0.f,.099f,16.001f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        sky=directional_sky();sky.horizon_falloff=invalid;
        rejects([&]{(void)poima::bake_environment_ibl(sky);},"Invalid falloff admitted a bake.");
    }
    sky=directional_sky();
    for(V direction:{V{0,0,0},V{1,std::numeric_limits<double>::infinity(),0},V{1,0,std::numeric_limits<double>::quiet_NaN()}})
        rejects([&]{(void)poima::environment_ibl_radiance(sky,direction);},"Degenerate direction admitted.");
    for(double magnitude:{std::numeric_limits<double>::denorm_min(),1.,std::numeric_limits<double>::max()})
        check(poima::environment_ibl_radiance(sky,{0,magnitude,0})==V{1,0,0},"Finite direction normalization overflowed or underflowed.");
    const auto tilted=normalized({.6,.7,.2});
    for(double angle:{0.,.6,2.,4.}) {
        const V rotated{tilted[0]*std::cos(angle)-tilted[2]*std::sin(angle),tilted[1],tilted[0]*std::sin(angle)+tilted[2]*std::cos(angle)};
        const auto a=poima::environment_ibl_radiance(sky,tilted),b=poima::environment_ibl_radiance(sky,rotated);
        for(std::size_t channel=0;channel<3;++channel)close(a[channel],b[channel],1e-14,"Sky gradient lost azimuth invariance.");
    }
    poima::jobs::Config config;config.workers=2;config.frame.tasks=55;
    poima::jobs::Executor executor(config);
    rejects([&]{(void)poima::bake_environment_ibl(sky,&executor);},"Partial bake admitted below complete task budget.");
    auto status=executor.status();check(status.frame.tasks==0 && status.frame.groups==0 && status.executing==0,"Rejected bake leaked task admission.");
    bool ran=false;const auto group=executor.submit({{[&](poima::jobs::Context&){ran=true;},{},"ibl.admission.recovery"}});
    const auto result=executor.wait(group);result.rethrow();check(ran && result.snapshot.state==poima::jobs::State::succeeded,"Executor did not recover after rejected bake.");
}
void environment_settings_validation() {
    poima::LightingEnvironment environment;poima::validate_environment(environment);
    for(bool specular:{false,true})for(float invalid:{-1.f,64.001f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        environment={};environment.ibl=poima::IblSettings{};
        (specular ? environment.ibl->specular_intensity : environment.ibl->diffuse_intensity)=invalid;
        rejects([&]{poima::validate_environment(environment);},"Disabled IBL skipped gain validation.");
    }
    environment={};environment.ibl=poima::IblSettings{true,1,1};
    rejects([&]{poima::validate_environment(environment);},"IBL admitted without enabled procedural sky.");
    environment.sky.enabled=true;
    for(float gain:{0.f,64.f}) {environment.ibl->diffuse_intensity=gain;environment.ibl->specular_intensity=gain;poima::validate_environment(environment);}
}
}

int main() {
    try {
        constant_sky();directional_and_parallel();invalid_and_atomic_admission();environment_settings_validation();
        std::cout<<"Environment IBL: constant radiance, cube orientation, independent diffuse/BRDF integrals, roughness, complete parallel equality and bounded recovery passed.\n";
        return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
