// SPDX-License-Identifier: Apache-2.0
#include "poima/environment_ibl.hpp"
#include "poima/jobs.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace poima {
namespace {
using V = std::array<double,3>;
static_assert(sizeof(std::array<float,4>)==16 && sizeof(std::array<float,2>)==8);
constexpr std::uint32_t diffuse_samples=128,specular_samples=128,brdf_samples=512;
constexpr double pi=std::numbers::pi_v<double>;
struct Gradient { V zenith,horizon,ground;double falloff; };
void require(bool condition,const char* message) {
    if(!condition)throw std::invalid_argument(message);
}
double dot(const V& a,const V& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
V cross(const V& a,const V& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
V unit(V value) {
    const double maximum=std::max({std::abs(value[0]),std::abs(value[1]),std::abs(value[2])});
    require(std::isfinite(maximum) && maximum>0,"Environment IBL direction must be finite and nonzero.");
    for(auto& x:value) { require(std::isfinite(x),"Environment IBL direction must be finite and nonzero.");x/=maximum; }
    const double length=std::sqrt(dot(value,value));
    for(auto& x:value)x/=length;
    return value;
}
Gradient gradient(const SkySettings& sky) {
    Gradient result{};
    const std::array<std::array<float,3>,3> colors={sky.zenith,sky.horizon,sky.ground};
    V* targets[]={&result.zenith,&result.horizon,&result.ground};
    for(std::size_t index=0;index<colors.size();++index)for(std::size_t channel=0;channel<3;++channel) {
        const auto value=colors[index][channel];
        require(std::isfinite(value) && value>=0 && value<=1,"Environment IBL sky colors must be finite linear RGB in [0,1].");
        (*targets[index])[channel]=value;
    }
    require(std::isfinite(sky.horizon_falloff) && sky.horizon_falloff>=.1f && sky.horizon_falloff<=16,
            "Environment IBL sky falloff must be in [0.1,16].");
    result.falloff=sky.horizon_falloff;
    // enabled, sun identity/disk/halo and all scene gains are intentionally absent.
    return result;
}
V radiance(const Gradient& sky,const V& direction) {
    const double blend=std::pow(std::clamp(std::abs(direction[1]),0.,1.),sky.falloff);
    const auto& pole=direction[1]>=0 ? sky.zenith : sky.ground;
    V result{};
    for(std::size_t channel=0;channel<3;++channel)
        result[channel]=sky.horizon[channel]+(pole[channel]-sky.horizon[channel])*blend;
    return result;
}
V cube_direction(std::uint32_t face,std::uint32_t x,std::uint32_t y,std::uint32_t size) {
    const double u=2*(double(x)+.5)/size-1,v=2*(double(y)+.5)/size-1;
    // Vulkan cube sc/tc convention; the same top-to-bottom rows are uploaded.
    switch(face) {
    case 0:return unit({1,-v,-u});
    case 1:return unit({-1,-v,u});
    case 2:return unit({u,1,v});
    case 3:return unit({u,-1,-v});
    case 4:return unit({u,-v,1});
    default:return unit({-u,-v,-1});
    }
}
struct Basis { V tangent,bitangent,normal; };
Basis basis(const V& normal) {
    const V reference=std::abs(normal[2])<.999 ? V{0,0,1} : V{1,0,0};
    const auto tangent=unit(cross(reference,normal));
    return {tangent,cross(normal,tangent),normal};
}
V orient(const Basis& axes,const V& local) {
    V result{};
    for(std::size_t i=0;i<3;++i)
        result[i]=axes.tangent[i]*local[0]+axes.bitangent[i]*local[1]+axes.normal[i]*local[2];
    return result;
}
double radical_inverse(std::uint32_t value) {
    value=(value>>16)|(value<<16);
    value=((value&0x55555555u)<<1)|((value&0xaaaaaaaau)>>1);
    value=((value&0x33333333u)<<2)|((value&0xccccccccu)>>2);
    value=((value&0x0f0f0f0fu)<<4)|((value&0xf0f0f0f0u)>>4);
    value=((value&0x00ff00ffu)<<8)|((value&0xff00ff00u)>>8);
    return double(value)*0x1p-32;
}
struct Sample { double stratified,cos_phi,sin_phi; };
template<std::size_t Count> std::array<Sample,Count> samples() {
    std::array<Sample,Count> result{};
    for(std::uint32_t i=0;i<Count;++i) {
        const double first=(double(i)+.5)/Count,second=radical_inverse(i),angle=2*pi*second;
        result[i]={first,std::cos(angle),std::sin(angle)};
    }
    return result;
}
std::array<float,4> rgba(const V& value) {
    return {static_cast<float>(value[0]),static_cast<float>(value[1]),static_cast<float>(value[2]),1};
}
bool cancelled(const jobs::Context* context) { return context && context->cancel_requested(); }
void fill_diffuse(EnvironmentIblLevel& level,std::uint32_t face,const Gradient& sky,
                  const std::array<Sample,diffuse_samples>& points,const jobs::Context* context) {
    for(std::uint32_t y=0;y<level.size;++y) {
        if(cancelled(context))return;
        for(std::uint32_t x=0;x<level.size;++x) {
            const auto axes=basis(cube_direction(face,x,y,level.size));V sum{};
            for(const auto& point:points) {
                const double radius=std::sqrt(point.stratified);
                const auto light=radiance(sky,orient(axes,{radius*point.cos_phi,radius*point.sin_phi,std::sqrt(1-point.stratified)}));
                for(std::size_t channel=0;channel<3;++channel)sum[channel]+=light[channel];
            }
            // Cosine-weighted sampling already includes cosine/pi in its PDF.
            for(auto& channel:sum)channel/=diffuse_samples;
            level.texels[(std::size_t(face)*level.size+y)*level.size+x]=rgba(sum);
        }
    }
}
void fill_specular(EnvironmentIblLevel& level,std::uint32_t face,std::uint32_t mip,const Gradient& sky,
                   const std::array<Sample,specular_samples>& points,const jobs::Context* context) {
    const double roughness=double(mip)/(ibl_specular_levels-1),alpha=roughness*roughness,alpha_squared=alpha*alpha;
    for(std::uint32_t y=0;y<level.size;++y) {
        if(cancelled(context))return;
        for(std::uint32_t x=0;x<level.size;++x) {
            const auto normal=cube_direction(face,x,y,level.size);V sum{};
            if(mip==0)sum=radiance(sky,normal);
            else {
                const auto axes=basis(normal);double normalization=0;
                for(const auto& point:points) {
                    const double h_z=std::sqrt((1-point.stratified)/(1+(alpha_squared-1)*point.stratified));
                    const double l_z=2*h_z*h_z-1;
                    if(l_z<=0)continue;
                    const double transverse=2*h_z*std::sqrt(std::max(0.,1-h_z*h_z));
                    const auto light=radiance(sky,orient(axes,{transverse*point.cos_phi,transverse*point.sin_phi,l_z}));
                    for(std::size_t channel=0;channel<3;++channel)sum[channel]+=light[channel]*l_z;
                    normalization+=l_z;
                }
                require(normalization>0,"Environment IBL prefilter has no positive hemisphere samples.");
                for(auto& channel:sum)channel/=normalization;
            }
            level.texels[(std::size_t(face)*level.size+y)*level.size+x]=rgba(sum);
        }
    }
}
std::array<float,2> integrate_brdf(double nv,double roughness,const std::array<Sample,brdf_samples>& points) {
    const double alpha=roughness*roughness,alpha_squared=alpha*alpha;
    const V view{std::sqrt(std::max(0.,1-nv*nv)),0,nv};
    const auto stretched=unit({alpha*view[0],0,nv});
    const double g1=2*nv/(nv+std::sqrt(nv*nv+(1-nv*nv)*alpha_squared));
    double scale=0,bias=0;
    for(const auto& point:points) {
        // Visible GGX normals via a uniform spherical cap in stretched space.
        // Half-vector construction and unstretching avoid grazing-view weights
        // of ordinary NDF sampling. The reduction remains in sample order.
        const double cap_z=(1-point.stratified)*(1+stretched[2])-stretched[2];
        const double cap_r=std::sqrt(std::max(0.,1-cap_z*cap_z));
        const auto halfway=unit({alpha*(stretched[0]+cap_r*point.cos_phi),
                                 alpha*cap_r*point.sin_phi,stretched[2]+cap_z});
        const double vh=std::clamp(dot(view,halfway),0.,1.);
        const double nl=2*vh*halfway[2]-nv;
        if(nl<=0 || vh<=0)continue;
        const double denominator=nl*std::sqrt(nv*nv*(1-alpha_squared)+alpha_squared)+
                                 nv*std::sqrt(nl*nl*(1-alpha_squared)+alpha_squared);
        // V=.5/denominator matches the production correlated Smith BRDF.
        // Divide by visible-normal PDF: weight = 4*NoV*NoL*V/G1(V).
        const double weight=2*nv*nl/(std::max(denominator,1e-6)*g1);
        const double fresnel=std::pow(1-vh,5);
        scale+=(1-fresnel)*weight;bias+=fresnel*weight;
    }
    return {static_cast<float>(scale/brdf_samples),static_cast<float>(bias/brdf_samples)};
}
void fill_brdf(std::vector<std::array<float,2>>& result,std::uint32_t first,std::uint32_t last,
               const std::array<Sample,brdf_samples>& points,const jobs::Context* context) {
    for(std::uint32_t y=first;y<last;++y) {
        if(cancelled(context))return;
        for(std::uint32_t x=0;x<ibl_brdf_size;++x)
            result[std::size_t(y)*ibl_brdf_size+x]=integrate_brdf((double(x)+.5)/ibl_brdf_size,(double(y)+.5)/ibl_brdf_size,points);
    }
}
}

std::array<double,3> environment_ibl_radiance(const SkySettings& sky,const std::array<double,3>& direction) {
    return radiance(gradient(sky),unit(direction));
}
EnvironmentIbl bake_environment_ibl(const SkySettings& sky,jobs::Executor* executor) {
    const auto environment=gradient(sky);
    const auto diffuse_points=samples<diffuse_samples>(),specular_points=samples<specular_samples>();
    const auto brdf_points=samples<brdf_samples>();
    EnvironmentIbl output;
    output.diffuse.size=ibl_diffuse_size;output.diffuse.texels.resize(6*ibl_diffuse_size*ibl_diffuse_size);
    for(std::uint32_t mip=0;mip<ibl_specular_levels;++mip) {
        auto& level=output.specular[mip];level.size=ibl_specular_size>>mip;
        level.texels.resize(std::size_t(6)*level.size*level.size);
    }
    output.brdf.resize(ibl_brdf_size*ibl_brdf_size);
    using Work=std::function<void(const jobs::Context*)>;
    std::vector<Work> work;work.reserve(56);
    for(std::uint32_t face=0;face<6;++face)
        work.emplace_back([&,face](const jobs::Context* context){fill_diffuse(output.diffuse,face,environment,diffuse_points,context);});
    for(std::uint32_t mip=0;mip<ibl_specular_levels;++mip)for(std::uint32_t face=0;face<6;++face)
        work.emplace_back([&,mip,face](const jobs::Context* context){fill_specular(output.specular[mip],face,mip,environment,specular_points,context);});
    for(std::uint32_t first=0;first<ibl_brdf_size;first+=8)
        work.emplace_back([&,first](const jobs::Context* context){fill_brdf(output.brdf,first,first+8,brdf_points,context);});
    if(executor) {
        std::vector<jobs::Task> tasks;tasks.reserve(work.size());
        for(auto& function:work)tasks.push_back({[function=std::move(function)](jobs::Context& context){function(&context);},{},"environment.ibl.bake"});
        const auto group=executor->submit(std::move(tasks),jobs::Lane::frame);
        // wait is affine to this same submitting owner and fences callback
        // captures before output storage or borrowed inputs may be destroyed.
        const auto result=executor->wait(group);result.rethrow();
        require(result.snapshot.state==jobs::State::succeeded,"Environment IBL bake was cancelled; no resources published.");
    }else for(const auto& function:work)function(nullptr);
    return output;
}
}
