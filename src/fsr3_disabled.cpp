// SPDX-License-Identifier: Apache-2.0
#include "poima/fsr3.hpp"
#include <stdexcept>
namespace poima::fsr3 {
struct Context::Impl {};
Context::Context(VkDevice,VkPhysicalDevice,PFN_vkGetDeviceProcAddr,Extent,Extent) { throw std::runtime_error("FSR3 upscaler is not included in this build"); }
Context::~Context()=default;
void Context::dispatch(const Dispatch&){throw std::runtime_error("FSR3 upscaler is not included in this build");}
DiagnosticResources Context::diagnostic_resources()const{throw std::runtime_error("FSR3 upscaler is not included in this build");}
std::uint64_t Context::gpu_bytes()const noexcept{return 0;}
std::uint64_t Context::scratch_bytes()const noexcept{return 0;}
std::array<float,2> jitter(std::uint64_t,Extent,Extent){throw std::runtime_error("FSR3 upscaler is not included in this build");}
}
