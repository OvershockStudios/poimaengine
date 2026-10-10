// SPDX-License-Identifier: Apache-2.0
#include "poima/shared_session.hpp"
#include "poima/local_session.hpp"
#include "poima/world.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>

namespace poima {
namespace {
volatile std::sig_atomic_t stopping=0;
void stop_signal(int) { stopping=1; }
struct Signals {
    using Handler=void (*)(int);
    Handler interrupt,terminate;
    Signals():interrupt(std::signal(SIGINT,stop_signal)),terminate(std::signal(SIGTERM,stop_signal)) { stopping=0; }
    ~Signals() { if(interrupt!=SIG_ERR)std::signal(SIGINT,interrupt);if(terminate!=SIG_ERR)std::signal(SIGTERM,terminate); }
};
}
int run_shared_world(const std::string& world,const std::string& endpoint,const std::string& development_profiles) {
    // Acquire the endpoint first; an unavailable endpoint must not create a
    // world's writer sidecar. WorldSession never leaves its owning thread.
    LocalSessionServer host(endpoint);WorldSession session(world);
    if(!development_profiles.empty())session.configure_development_profiles(development_profiles);
    return run_shared_session(session,host,endpoint);
}
int run_shared_session(WorldSession& session,LocalSessionServer& host,const std::string& endpoint) {
    Signals signals;
    std::cerr<<"Poima shared world ready: "<<endpoint<<'\n';
    while(!stopping && !session.closed()) {
        for(const auto& request:host.poll())
            host.reply(request.token,session.request(request.payload,WorldRequestScope::shared_headless));
        // Requests and graphics share one owner thread. Finish each accepted
        // batch before advancing one player frame; never recursively dispatch
        // requests from inside a simulation tick or a graphics operation.
        if(!stopping && !session.closed())session.poll_player();
        if(!stopping && !session.closed())host.wait(session.player_active() ? 1 : 100);
    }
    if(!session.closed()) {
        // A signal closes presentation before releasing its runtime/session.
        // Normal RPC shutdown requires the caller to stop the player first.
        session.stop_player();
        session.request(R"({"jsonrpc":"2.0","method":"session.close"})");
    }
    // Deliver the shutdown receipt and already queued replies. This bounded
    // grace period does not wait indefinitely for clients that keep stdin open.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(host.clients() && std::chrono::steady_clock::now()<deadline) {
        for(const auto& request:host.poll())host.reply(request.token,session.request(request.payload,WorldRequestScope::shared_headless));
        const auto remaining=std::chrono::ceil<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(host.clients() && remaining>0)host.wait(static_cast<std::uint32_t>(std::min<std::int64_t>(remaining,100)));
    }
    return 0;
}
int run_connected_session(const std::string& endpoint,std::uint32_t timeout_ms) {
    LocalSessionClient client(endpoint,timeout_ms);
    for(;;) {
        std::string line;bool oversized=false;char c=0;
        while(std::cin.get(c) && c!='\n') { if(line.size()<local_session_request_limit)line+=c;else oversized=true; }
        if(line.empty() && !oversized && !std::cin)break;
        if(oversized) {
            std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Request exceeds 1 MiB.\"}}\n"<<std::flush;
            continue;
        }
        if(line.empty())line=" "; // A blank NDJSON line remains a parse error.
        const auto response=client.exchange(line,timeout_ms);
        if(!response.empty())std::cout<<response<<'\n'<<std::flush;
    }
    return 0;
}
}
