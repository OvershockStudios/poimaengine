// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace poima {
struct NativeGameplayPayload {
    std::string path,sha256,role;
    std::uint64_t size=0;
};
struct NativeGameplayArtifact {
    std::string descriptor,descriptor_sha256,root,library,library_sha256,schema,type,identity,target_os,target_arch;
    std::vector<NativeGameplayPayload> files;
};
// Reads a bounded, self-contained artifact directory without executing code.
// Payload paths are relative to the descriptor's directory; the fields above
// named descriptor/root/library are resolved absolute paths for native callers.
NativeGameplayArtifact load_native_gameplay_artifact(const std::string& descriptor);
}
