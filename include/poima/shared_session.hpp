// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
namespace poima {
class WorldSession;
class LocalSessionServer;
// These CLI adapters keep protocol data on stdout and diagnostics on stderr.
// Pump an already prepared session and acquired endpoint on their owner thread.
int run_shared_session(WorldSession& session,LocalSessionServer& host,const std::string& endpoint);
int run_shared_world(const std::string& world,const std::string& endpoint,const std::string& development_profiles={});
int run_connected_session(const std::string& endpoint,std::uint32_t timeout_ms=30000);
}
