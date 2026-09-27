// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace poima {
inline constexpr std::size_t local_session_request_limit=1024*1024;
inline constexpr std::size_t local_session_response_limit=32*1024*1024;
inline constexpr std::size_t local_session_client_limit=8;
struct LocalSessionRequest { std::uint64_t token=0;std::string payload; };

// Compiled, same-user local transport. Endpoint is 1..64 ASCII letters/digits/
// '_'/'-'; it is not a path or network address. All calls on each object must
// be serialized by its owner. Frames are uint32 little-endian byte length then
// strict UTF-8 payload. Requests are nonempty; an empty reply acknowledges a
// notification. One request per client can await a reply at a time.
class LocalSessionServer {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit LocalSessionServer(std::string_view endpoint);
    ~LocalSessionServer();
    LocalSessionServer(const LocalSessionServer&)=delete;
    LocalSessionServer& operator=(const LocalSessionServer&)=delete;
    // Bounded, nonblocking I/O pump. Also flushes previously queued replies.
    std::vector<LocalSessionRequest> poll();
    // Queues a reply; poll() completes delivery. False for a disconnected or
    // already-replied token. Invalid/oversized UTF-8 throws without mutation.
    bool reply(std::uint64_t token,std::string_view payload);
    std::size_t clients() const;
    std::string address() const;
};

class LocalSessionClient {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit LocalSessionClient(std::string_view endpoint,std::uint32_t connect_timeout_ms=5000);
    ~LocalSessionClient();
    LocalSessionClient(const LocalSessionClient&)=delete;
    LocalSessionClient& operator=(const LocalSessionClient&)=delete;
    // Timeout/disconnect/protocol failure closes this connection and throws.
    // Never automatically replays a request. Successful exchanges reuse it.
    std::string exchange(std::string_view payload,std::uint32_t timeout_ms=5000);
    std::string address() const;
};
}
