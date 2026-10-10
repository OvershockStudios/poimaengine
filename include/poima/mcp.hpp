// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
namespace poima {
// Synchronous owner-thread MCP adapter. The backend accepts one native JSON-RPC
// request and returns its response. Calls are never retried or interrupted.
int run_mcp_world(const std::string& path,const std::string& development_profiles={});
int run_mcp_connected(const std::string& endpoint, std::uint32_t timeout_ms=30000);
class McpSession {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit McpSession(std::function<std::string(std::string_view)> backend);
    ~McpSession();
    McpSession(const McpSession&)=delete;
    McpSession& operator=(const McpSession&)=delete;
    // One bounded JSON message; empty output for notifications.
    std::string request(std::string_view message);
};
}
