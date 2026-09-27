// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
namespace poima {
// These CLI adapters keep protocol data on stdout and diagnostics on stderr.
int run_shared_world(const std::string& world,const std::string& endpoint);
int run_connected_session(const std::string& endpoint,std::uint32_t timeout_ms=30000);
}
