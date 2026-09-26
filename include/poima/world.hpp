// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

namespace poima {
// Persistent authored-world service over newline-delimited JSON-RPC 2.0.
// Simulation/renderer state is deliberately not stored in this document.
int run_world_session(const std::string& utf8_path);
}
