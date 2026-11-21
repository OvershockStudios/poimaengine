// SPDX-License-Identifier: Apache-2.0
#include "poima/post_effect.hpp"

#include <spirv-tools/libspirv.h>
#include <memory>
#include <stdexcept>

namespace poima {

void validate_post_effect_vulkan(const PostEffect& effect) {
    if (effect.words.size() < 5 || effect.words.size() > max_post_effect_bytes / sizeof(std::uint32_t))
        throw std::runtime_error("Project post-effect module length is invalid.");

    const std::unique_ptr<spv_context_t, decltype(&spvContextDestroy)> context(
        spvContextCreate(SPV_ENV_VULKAN_1_3), spvContextDestroy);
    if (!context) throw std::runtime_error("Could not create the Vulkan 1.3 SPIR-V validator.");

    spv_const_binary_t binary{effect.words.data(), effect.words.size()};
    spv_diagnostic raw_diagnostic = nullptr;
    const auto status = spvValidate(context.get(), &binary, &raw_diagnostic);
    const std::unique_ptr<spv_diagnostic_t, decltype(&spvDiagnosticDestroy)> diagnostic(
        raw_diagnostic, spvDiagnosticDestroy);
    if (status == SPV_SUCCESS) return;

    std::string message = "Project post effect failed Vulkan 1.3 SPIR-V validation.";
    if (diagnostic && diagnostic->error) {
        message += ' ';
        message += std::string_view(diagnostic->error).substr(0, 1024);
    }
    throw std::runtime_error(message);
}

}
