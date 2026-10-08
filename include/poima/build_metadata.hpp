// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace poima {
// Immutable metadata from this binary's compiled CMake configuration.
// Declarations do not depend on generated headers or the patch version.
struct BuildMetadata {
    const char* version;
    const char* compiler;
    const char* compiler_version;
    const char* target_os;
    const char* target_arch;
};
const BuildMetadata& build_metadata() noexcept;
}
