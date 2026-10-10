// SPDX-License-Identifier: Apache-2.0
#include "poima/mcp.hpp"
#include "poima/local_session.hpp"
#include "poima/world.hpp"
#include <iostream>
#include <cstdio>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace poima {
namespace {
int serve_stdio(McpSession& session) {
#ifdef _WIN32
    // MCP bytes stay UTF-8 with LF framing even when inherited from a console.
    if(_setmode(_fileno(stdin),_O_BINARY)==-1 || _setmode(_fileno(stdout),_O_BINARY)==-1)
        throw std::runtime_error("Cannot configure MCP standard streams.");
#endif
    for(;;) {
        std::string line;
        bool oversized=false;
        char byte=0;
        while(std::cin.get(byte) && byte!='\n') {
            if(line.size()<local_session_request_limit)line+=byte;
            else oversized=true;
        }
        if(line.empty() && !oversized && !std::cin)break;
        // Drain an oversized frame before replying, retaining bounded memory.
        if(oversized)line.push_back(' ');
        const auto reply=session.request(line);
        if(!reply.empty()) {
            std::cout<<reply<<'\n'<<std::flush;
            if(!std::cout)throw std::runtime_error("MCP output stream closed.");
        }
    }
    if(std::cin.bad())throw std::runtime_error("MCP input stream failed.");
    return 0;
}
}
int run_mcp_world(const std::string& path,const std::string& development_profiles) {
    WorldSession world(path);
    if(!development_profiles.empty())world.configure_development_profiles(development_profiles);
    McpSession session([&world](std::string_view message) {return world.request(message);});
    return serve_stdio(session);
}
int run_mcp_connected(const std::string& endpoint,std::uint32_t timeout_ms) {
    LocalSessionClient client(endpoint,timeout_ms);
    McpSession session([&client,timeout_ms](std::string_view message) {return client.exchange(message,timeout_ms);});
    return serve_stdio(session);
}
}
