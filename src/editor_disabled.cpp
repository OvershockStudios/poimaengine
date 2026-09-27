// SPDX-License-Identifier: Apache-2.0
#include "poima/editor.hpp"
namespace poima {
Reply run_editor(const EditorOptions&) {
    return {3,R"({"protocol_version":1,"request_id":null,"command":"editor","status":"error","result":null,"diagnostics":[{"code":"unavailable","message":"Native editor is not built. Configure POIMA_BUILD_EDITOR=ON with the Vulkan renderer."}]})"};
}
}
