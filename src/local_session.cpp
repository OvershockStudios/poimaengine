// SPDX-License-Identifier: Apache-2.0
#include "poima/local_session.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace poima {
namespace {
using Clock=std::chrono::steady_clock;
constexpr std::size_t pump_budget=256*1024;
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void validate_endpoint(std::string_view endpoint) {
    if(endpoint.empty() || endpoint.size()>64 || !std::all_of(endpoint.begin(),endpoint.end(),[](unsigned char c){
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-';
    }))throw std::invalid_argument("Local endpoint must contain 1..64 ASCII letters, digits, '_' or '-'.");
}
void validate_timeout(std::uint32_t timeout) {
    if(timeout==0 || timeout>600000)throw std::invalid_argument("Local session timeout must be 1..600000 milliseconds.");
}
bool utf8(std::string_view text) {
    std::size_t i=0;
    while(i<text.size()) {
        const auto first=static_cast<unsigned char>(text[i++]);if(first<0x80)continue;
        std::uint32_t value=0,minimum=0;unsigned count=0;
        if(first>=0xc2 && first<=0xdf) { value=first&31u;minimum=0x80;count=1; }
        else if(first>=0xe0 && first<=0xef) { value=first&15u;minimum=0x800;count=2; }
        else if(first>=0xf0 && first<=0xf4) { value=first&7u;minimum=0x10000;count=3; }
        else return false;
        if(count>text.size()-i)return false;
        while(count--) { const auto next=static_cast<unsigned char>(text[i++]);if((next&0xc0)!=0x80)return false;value=(value<<6)|(next&63u); }
        if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return false;
    }
    return true;
}
void validate_payload(std::string_view payload,std::size_t maximum,bool empty) {
    if((!empty && payload.empty()) || payload.size()>maximum || !utf8(payload))
        throw std::invalid_argument("Local session frame has invalid UTF-8, size or empty request.");
}
std::uint32_t decode_size(const std::array<unsigned char,4>& header) {
    return std::uint32_t(header[0]) | (std::uint32_t(header[1])<<8) | (std::uint32_t(header[2])<<16) | (std::uint32_t(header[3])<<24);
}
std::string frame(std::string_view payload) {
    const auto size=static_cast<std::uint32_t>(payload.size());std::string result(4,'\0');
    for(unsigned i=0;i<4;++i)result[i]=static_cast<char>((size>>(8*i))&255);
    result.append(payload);return result;
}
struct Io { std::size_t bytes=0;bool closed=false; };

#ifdef _WIN32
std::wstring sid_text(PSID sid) {
    LPWSTR text=nullptr;check(ConvertSidToStringSidW(sid,&text)!=0,"Cannot describe local user SID.");
    std::wstring result(text);LocalFree(text);return result;
}
std::wstring current_sid() {
    HANDLE token=nullptr;check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)!=0,"Cannot inspect local user token.");
    DWORD count=0;GetTokenInformation(token,TokenUser,nullptr,0,&count);
    std::vector<unsigned char> info(count);
    const bool ok=count>0 && GetTokenInformation(token,TokenUser,info.data(),count,&count)!=0;CloseHandle(token);
    check(ok,"Cannot inspect local user SID.");return sid_text(reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid);
}
struct Security {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    SECURITY_ATTRIBUTES attributes{};
    explicit Security(const std::wstring& sid) {
        const auto sddl=L"O:"+sid+L"D:P(A;;GA;;;"+sid+L")";
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=0,"Cannot create local session ACL.");
        attributes.nLength=sizeof(attributes);attributes.lpSecurityDescriptor=descriptor;attributes.bInheritHandle=FALSE;
    }
    ~Security() { if(descriptor)LocalFree(descriptor); }
};
std::string local_address(std::string_view endpoint) {
    const auto sid=current_sid();const std::string bytes(sid.begin(),sid.end());
    return "\\\\.\\pipe\\poima-"+sha256(std::as_bytes(std::span(bytes.data(),bytes.size())))+"-"+std::string(endpoint);
}
struct Channel {
    HANDLE handle=INVALID_HANDLE_VALUE;
    ~Channel() { close(); }
    void close() { if(handle!=INVALID_HANDLE_VALUE) { CloseHandle(handle);handle=INVALID_HANDLE_VALUE; } }
    bool valid() const { return handle!=INVALID_HANDLE_VALUE; }
    bool alive() const { DWORD available=0;return valid() && PeekNamedPipe(handle,nullptr,0,nullptr,&available,nullptr)!=0; }
    Io read(char* data,std::size_t count) {
        DWORD used=0;if(ReadFile(handle,data,static_cast<DWORD>(std::min(count,std::size_t(65536))),&used,nullptr))return {used,false};
        const auto error=GetLastError();if(error==ERROR_NO_DATA && alive())return {};return {0,true};
    }
    Io write(const char* data,std::size_t count) {
        DWORD used=0;if(WriteFile(handle,data,static_cast<DWORD>(std::min(count,std::size_t(65536))),&used,nullptr))return {used,false};
        const auto error=GetLastError();if((error==ERROR_NO_DATA || error==ERROR_PIPE_BUSY) && alive())return {};return {0,true};
    }
};
struct ClientOperation {
    HANDLE pipe;
    OVERLAPPED state{};
    bool pending=false;
    explicit ClientOperation(HANDLE handle):pipe(handle) {
        state.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        check(state.hEvent!=nullptr,"Cannot create local session I/O completion event.");
    }
    ClientOperation(const ClientOperation&)=delete;
    ClientOperation& operator=(const ClientOperation&)=delete;
    ~ClientOperation() {
        if(pending) {
            // Cancellation only requests completion. A raced normal completion
            // (including ERROR_NOT_FOUND from CancelIoEx) must also be reaped
            // before the caller's buffer, this OVERLAPPED or its event is freed.
            // Kernel cancellation/reaping is not bounded by the exchange timeout.
            // https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex
            CancelIoEx(pipe,&state);DWORD ignored=0;
            GetOverlappedResult(pipe,&state,&ignored,TRUE);
        }
        CloseHandle(state.hEvent);
    }
};
Io client_io(Channel& channel,bool writing,char* data,std::size_t count,Clock::time_point deadline) {
    ClientOperation operation(channel.handle);
    check(Clock::now()<deadline,"Local session exchange timed out; connection closed.");
    const auto size=static_cast<DWORD>(std::min(count,std::size_t(65536)));
    const auto completed=writing ? WriteFile(channel.handle,data,size,nullptr,&operation.state)
                                 : ReadFile(channel.handle,data,size,nullptr,&operation.state);
    const auto error=completed ? ERROR_SUCCESS : GetLastError();
    check(completed || error==ERROR_IO_PENDING,"Local session disconnected or overlapped I/O failed.");
    operation.pending=true;
    if(!completed) {
        const auto remaining=std::chrono::ceil<std::chrono::milliseconds>(deadline-Clock::now()).count();
        check(remaining>0,"Local session exchange timed out; connection closed.");
        const auto waited=WaitForSingleObject(operation.state.hEvent,static_cast<DWORD>(std::min<std::int64_t>(remaining,600000)));
        const auto timeout_message=writing ? "Local session pending write timed out; connection closed."
                                           : "Local session pending read timed out; connection closed.";
        check(waited==WAIT_OBJECT_0,waited==WAIT_TIMEOUT ? timeout_message
                                                      : "Local session I/O completion wait failed; connection closed.");
    }
    DWORD used=0;
    const auto success=GetOverlappedResult(channel.handle,&operation.state,&used,FALSE);
    const auto result_error=success ? ERROR_SUCCESS : GetLastError();
    if(result_error!=ERROR_IO_INCOMPLETE)operation.pending=false;
    check(success,"Local session disconnected or overlapped I/O completion failed.");
    check(Clock::now()<deadline,"Local session exchange timed out; connection closed.");
    check(used>0,"Local session I/O completed without transferring frame bytes; connection closed.");
    return {used,false};
}
struct ServerOperation {
    enum class Kind { none,connect,header,body,write };
    HANDLE pipe=INVALID_HANDLE_VALUE;
    OVERLAPPED state{};
    Kind kind=Kind::none;
    bool pending=false,ready=false;
    DWORD bytes=0,error=ERROR_SUCCESS;
    std::size_t requested=0;
    ServerOperation() {
        state.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        check(state.hEvent!=nullptr,"Cannot create local session server completion event.");
    }
    ServerOperation(const ServerOperation&)=delete;
    ServerOperation& operator=(const ServerOperation&)=delete;
    ~ServerOperation() { cancel();CloseHandle(state.hEvent); }
    void cancel() noexcept {
        if(pending) {
            CancelIoEx(pipe,&state);DWORD ignored=0;
            // Even ERROR_NOT_FOUND can mean raced completion, not permission
            // to release the operation/buffer. Reaping can outlast a deadline.
            GetOverlappedResult(pipe,&state,&ignored,TRUE);pending=false;
        }
    }
    void clear() {
        check(!pending,"Cannot reuse pending local server I/O.");
        const auto event=state.hEvent;state={};state.hEvent=event;
        check(ResetEvent(event)!=0,"Cannot reset local server completion event.");
        pipe=INVALID_HANDLE_VALUE;kind=Kind::none;ready=false;bytes=error=0;requested=0;
    }
    bool collect() {
        if(ready)return true;
        if(!pending)return false;
        DWORD used=0;const auto complete=GetOverlappedResult(pipe,&state,&used,FALSE);
        const auto result=complete ? ERROR_SUCCESS : GetLastError();
        if(result==ERROR_IO_INCOMPLETE)return false;
        pending=false;ready=true;bytes=used;error=result;return true;
    }
};
bool owner_is_current(HANDLE pipe) {
    PSID owner=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(GetSecurityInfo(pipe,SE_KERNEL_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
    bool same=false;try { same=owner && sid_text(owner)==current_sid(); }catch(...) { LocalFree(descriptor);throw; }
    LocalFree(descriptor);return same;
}
#else
std::string private_directory(bool create) {
    const auto path="/tmp/poima-"+std::to_string(static_cast<unsigned long long>(geteuid()));
    if(create && ::mkdir(path.c_str(),0700)!=0 && errno!=EEXIST)throw std::runtime_error("Cannot create private local session directory.");
    struct stat info{};check(::lstat(path.c_str(),&info)==0 && S_ISDIR(info.st_mode) && info.st_uid==geteuid() && (info.st_mode&07777)==0700,
        "Local session directory must be owned by this user, mode 0700, and not a symlink.");return path;
}
std::string local_address(std::string_view endpoint) { return "/tmp/poima-"+std::to_string(static_cast<unsigned long long>(geteuid()))+"/"+std::string(endpoint)+".sock"; }
sockaddr_un socket_address(const std::string& path) {
    sockaddr_un address{};address.sun_family=AF_UNIX;
    check(path.size()<sizeof(address.sun_path),"Local endpoint address is too long.");std::memcpy(address.sun_path,path.c_str(),path.size()+1);return address;
}
bool same_user(int fd) { ucred peer{};socklen_t size=sizeof(peer);return getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)==0 && peer.uid==geteuid(); }
struct Channel {
    int fd=-1;
    ~Channel() { close(); }
    void close() { if(fd>=0) { ::close(fd);fd=-1; } }
    bool valid() const { return fd>=0; }
    bool alive() const { char byte=0;const auto result=recv(fd,&byte,1,MSG_PEEK|MSG_DONTWAIT);return result>0 || (result<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)); }
    Io read(char* data,std::size_t count) {
        const auto used=recv(fd,data,count,MSG_DONTWAIT);if(used>0)return {static_cast<std::size_t>(used),false};
        if(used<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return {};
        return {0,true};
    }
    Io write(const char* data,std::size_t count) {
        const auto used=send(fd,data,count,MSG_DONTWAIT|MSG_NOSIGNAL);if(used>=0)return {static_cast<std::size_t>(used),false};
        if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)return {};
        return {0,true};
    }
};
void wait_io(Channel& channel,bool writing,Clock::time_point deadline) {
    const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
    check(remaining>0,"Local session exchange timed out; connection closed.");
    pollfd item{channel.fd,static_cast<short>(writing ? POLLOUT : POLLIN),0};
    const auto result=::poll(&item,1,static_cast<int>(std::min<std::int64_t>(remaining,600000)));
    check(result>0 || (result<0 && errno==EINTR),"Local session exchange timed out or polling failed; connection closed.");
}
#endif

struct Peer {
    Channel channel;
    bool connected=false;
    std::array<unsigned char,4> header{};
    std::size_t header_used=0,input_used=0,output_used=0;
    std::string input,output;
    std::uint64_t token=0;
#ifdef _WIN32
    bool continuation=false;
    // Declared last: its destructor cancels/reaps before buffers or channel
    // destruction, including partial construction and exceptional unwinding.
    ServerOperation operation;
#endif
    void reset_request() { header_used=input_used=output_used=0;input.clear();output.clear();token=0; }
};
}

struct LocalSessionServer::Impl {
    std::string path;
    std::array<std::unique_ptr<Peer>,local_session_client_limit> peers;
    std::uint64_t next_token=1;
#ifdef _WIN32
    std::unique_ptr<Security> security;
#else
    Channel listener;
    dev_t socket_device=0;ino_t socket_inode=0;bool owns_socket=false;
#endif
    explicit Impl(std::string_view endpoint):path(local_address(endpoint)) {
#ifdef _WIN32
        security=std::make_unique<Security>(current_sid());
        const std::wstring name(path.begin(),path.end());
        // Persistent per-peer operations are armed/harvested only by the owner;
        // PIPE_WAIT plus overlapped events provides genuine completion readiness.
        // https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-type-read-and-wait-modes
        for(std::size_t i=0;i<peers.size();++i) {
            auto peer=std::make_unique<Peer>();
            peer->channel.handle=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|(i==0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,static_cast<DWORD>(peers.size()),65536,65536,0,&security->attributes);
            check(peer->channel.valid(),"Local endpoint already exists or cannot be created.");peers[i]=std::move(peer);
        }
#else
        private_directory(true);listener.fd=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        check(listener.valid(),"Cannot create local session socket.");const auto address=socket_address(path);
        check(bind(listener.fd,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==0,"Local endpoint already exists or cannot be bound; existing paths are never removed.");
        struct stat info{};
        if(::lstat(path.c_str(),&info)==0 && S_ISSOCK(info.st_mode) && info.st_uid==geteuid()) { owns_socket=true;socket_device=info.st_dev;socket_inode=info.st_ino; }
        try { check(owns_socket && chmod(path.c_str(),0600)==0 && listen(listener.fd,8)==0,"Cannot secure/listen on local session socket."); }
        catch(...) { cleanup_socket();throw; }
#endif
    }
#ifndef _WIN32
    void cleanup_socket() noexcept {
        if(!owns_socket)return;
        struct stat info{};
        if(::lstat(path.c_str(),&info)==0 && S_ISSOCK(info.st_mode) && info.st_dev==socket_device && info.st_ino==socket_inode && info.st_uid==geteuid())::unlink(path.c_str());
        owns_socket=false;
    }
#endif
    ~Impl() {
#ifndef _WIN32
        listener.close();cleanup_socket();
#endif
        // Windows Peer destroys/reaps its operation before its buffers/channel.
        for(auto& peer:peers)peer.reset();
    }
    void drop(std::size_t index) {
#ifdef _WIN32
        auto& peer=*peers[index];peer.operation.cancel();peer.operation.clear();
        DisconnectNamedPipe(peer.channel.handle);peer.connected=false;peer.continuation=false;peer.reset_request();
#else
        peers[index].reset();
#endif
    }
#ifndef _WIN32
    void accept_clients() {
        // Bound accept work even if clients keep connecting concurrently.
        for(std::size_t attempt=0;attempt<local_session_client_limit;++attempt) {
            const auto slot=std::find_if(peers.begin(),peers.end(),[](const auto& peer){return !peer;});if(slot==peers.end())return;
            auto peer=std::make_unique<Peer>();
            const auto fd=accept4(listener.fd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if(fd<0) { if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)return;throw std::runtime_error("Local socket accept failed."); }
            peer->channel.fd=fd;if(!same_user(fd))continue;peer->connected=true;*slot=std::move(peer);
        }
    }
    bool terminal(const Peer& peer) {
        pollfd item{peer.channel.fd,0,0};const auto result=::poll(&item,1,0);
        check(result>=0 || errno==EINTR,"Cannot inspect local socket disconnect.");
        return result>0 && (item.revents&(POLLHUP|POLLERR|POLLNVAL));
    }
#else
    void arm(Peer& peer,std::size_t budget) {
        auto& op=peer.operation;if(op.kind!=ServerOperation::Kind::none || !budget || peer.token)return;
        op.clear();op.pipe=peer.channel.handle;
        BOOL complete=FALSE;
        if(!peer.connected) {
            op.kind=ServerOperation::Kind::connect;
            complete=ConnectNamedPipe(op.pipe,&op.state);
        }else {
            char* data=nullptr;
            if(!peer.output.empty()) {
                op.kind=ServerOperation::Kind::write;data=peer.output.data()+peer.output_used;
                op.requested=std::min(budget,peer.output.size()-peer.output_used);
            }else if(peer.header_used<peer.header.size()) {
                op.kind=ServerOperation::Kind::header;data=reinterpret_cast<char*>(peer.header.data())+peer.header_used;
                op.requested=std::min(budget,peer.header.size()-peer.header_used);
            }else {
                op.kind=ServerOperation::Kind::body;data=peer.input.data()+peer.input_used;
                op.requested=std::min(budget,peer.input.size()-peer.input_used);
            }
            op.requested=std::min(op.requested,std::size_t(65536));
            check(op.requested>0,"Local server has no eligible frame bytes.");
            const auto count=static_cast<DWORD>(op.requested);
            complete=op.kind==ServerOperation::Kind::write ? WriteFile(op.pipe,data,count,nullptr,&op.state)
                                                         : ReadFile(op.pipe,data,count,nullptr,&op.state);
        }
        const auto error=complete ? ERROR_SUCCESS : GetLastError();
        if(op.kind==ServerOperation::Kind::connect && (complete || error==ERROR_PIPE_CONNECTED)) {
            // PIPE_CONNECTED never issued pending I/O; do not ask the kernel
            // to reap that synthetic completion or depend on an event signal.
            op.ready=true;return;
        }
        if(complete || error==ERROR_IO_PENDING) {
            op.pending=true;
            if(complete)op.collect();
        }else { op.ready=true;op.error=error; }
    }
#endif
    bool flush_output(std::size_t index,std::size_t& budget) {
        auto& peer=*peers[index];
        if(peer.output.empty())return true;
        while(peer.output_used<peer.output.size() && budget) {
#ifdef _WIN32
            arm(peer,budget);auto& op=peer.operation;
            check(op.kind==ServerOperation::Kind::write,"Local server reply has conflicting pending I/O.");
            if(!op.collect())break;
            if(op.error!=ERROR_SUCCESS || !op.bytes || op.bytes>op.requested || op.bytes>budget) { drop(index);return false; }
            peer.output_used+=op.bytes;budget-=op.bytes;op.clear();
#else
            const auto io=peer.channel.write(peer.output.data()+peer.output_used,std::min(budget,peer.output.size()-peer.output_used));
            if(io.closed) { drop(index);return false; }
            if(!io.bytes)break;
            peer.output_used+=io.bytes;budget-=io.bytes;
#endif
        }
        if(peer.output_used==peer.output.size())peer.reset_request();
#ifdef _WIN32
        else if(!budget)peer.continuation=true;
#endif
        return true;
    }
    std::vector<LocalSessionRequest> poll() {
#ifndef _WIN32
        accept_clients();std::vector<LocalSessionRequest> requests;requests.reserve(peers.size());
#else
        std::vector<LocalSessionRequest> requests;requests.reserve(peers.size());
#endif
        for(std::size_t i=0;i<peers.size();++i) {
#ifdef _WIN32
            auto& peer=*peers[i];peer.continuation=false;std::size_t budget=pump_budget;
            if(!peer.connected) {
                arm(peer,budget);auto& op=peer.operation;
                if(!op.collect())continue;
                if(op.error!=ERROR_SUCCESS) {
                    const auto error=op.error;drop(i);
                    check(error==ERROR_NO_DATA || error==ERROR_BROKEN_PIPE || error==ERROR_OPERATION_ABORTED,"Local pipe listener failed.");
                    continue;
                }
                op.clear();peer.connected=true;
            }
            if(!peer.channel.alive()) { drop(i);continue; }
            if(peer.token)continue;
            if(!peer.output.empty()) {
                if(!flush_output(i,budget))continue;
                if(!peer.output.empty())continue;
            }
            while(budget && !peer.token) {
                arm(peer,budget);auto& op=peer.operation;
                if(!op.collect())break;
                if(op.error!=ERROR_SUCCESS || !op.bytes || op.bytes>op.requested || op.bytes>budget) { drop(i);break; }
                const auto kind=op.kind;
                if(kind==ServerOperation::Kind::header)peer.header_used+=op.bytes;
                else { check(kind==ServerOperation::Kind::body,"Local server has an unexpected read phase.");peer.input_used+=op.bytes; }
                budget-=op.bytes;op.clear();
                if(kind==ServerOperation::Kind::header && peer.header_used==peer.header.size()) {
                    const auto size=decode_size(peer.header);if(size==0 || size>local_session_request_limit) { drop(i);break; }
                    peer.input.resize(size);
                }
                if(kind==ServerOperation::Kind::body && peer.input_used==peer.input.size()) {
                    if(!utf8(peer.input)) { drop(i);break; }
                    check(next_token!=0,"Local session request token space exhausted.");peer.token=next_token++;
                    requests.push_back({peer.token,std::move(peer.input)});
                }
            }
            if(peer.connected && !budget && !peer.token)peer.continuation=true;
#else
            if(!peers[i] || !peers[i]->connected)continue;
            auto& peer=*peers[i];std::size_t budget=pump_budget;
            // While a response is owner-held or pending, full hangup/error is
            // a disconnect even if unread pipelined bytes remain buffered.
            // A write-half shutdown alone must not discard a readable reply.
            if((peer.token || !peer.output.empty()) && terminal(peer)) { drop(i);continue; }
            if(!peer.channel.alive()) { drop(i);continue; }
            if(peer.token)continue;
            if(!peer.output.empty()) {
                if(!flush_output(i,budget))continue;
                if(!peer.output.empty())continue;
            }
            while(budget && !peer.token) {
                if(peer.header_used<4) {
                    const auto io=peer.channel.read(reinterpret_cast<char*>(peer.header.data())+peer.header_used,std::min(budget,4-peer.header_used));
                    if(io.closed) { drop(i);break; }if(!io.bytes)break;peer.header_used+=io.bytes;budget-=io.bytes;if(peer.header_used<4)continue;
                    const auto size=decode_size(peer.header);if(size==0 || size>local_session_request_limit) { drop(i);break; }
                    peer.input.resize(size);
                }
                if(!budget)break;
                const auto io=peer.channel.read(peer.input.data()+peer.input_used,std::min(budget,peer.input.size()-peer.input_used));
                if(io.closed) { drop(i);break; }if(!io.bytes)break;peer.input_used+=io.bytes;budget-=io.bytes;
                if(peer.input_used==peer.input.size()) {
                    if(!utf8(peer.input)) { drop(i);break; }
                    check(next_token!=0,"Local session request token space exhausted.");peer.token=next_token++;
                    requests.push_back({peer.token,std::move(peer.input)});
                }
            }
#endif
        }
        return requests;
    }
    bool wait(std::uint32_t timeout) {
#ifdef _WIN32
        const auto deadline=Clock::now()+std::chrono::milliseconds(timeout);
        for(;;) {
            std::array<HANDLE,local_session_client_limit> events{};DWORD count=0;
            bool ready=false,deferred=false;
            for(auto& pointer:peers) {
                auto& peer=*pointer;
                if(peer.token) { deferred=true;if(!peer.channel.alive())ready=true;continue; }
                arm(peer,pump_budget);auto& op=peer.operation;
                if(peer.continuation || op.ready)ready=true;
                if(op.pending)events[count++]=op.state.hEvent;
            }
            if(ready)return true;
            auto remaining=std::chrono::ceil<std::chrono::milliseconds>(deadline-Clock::now()).count();
            if(remaining<0)remaining=0;
            const auto interval=static_cast<DWORD>(std::min<std::int64_t>(remaining,deferred ? 100 : 600000));
            // When every peer awaits its application reply, no I/O is armed.
            // Its reset private event is an unsignaled idle wait target. Check
            // dormant disconnects every 100ms, without reading pipeline bytes.
            const auto result=count ? WaitForMultipleObjects(count,events.data(),FALSE,interval)
                                    : WaitForSingleObject(peers[0]->operation.state.hEvent,interval);
            if(count && result<WAIT_OBJECT_0+count)return true;
            check(result==WAIT_TIMEOUT,"Local server completion wait failed.");
            if(timeout==0 || Clock::now()>=deadline)return false;
        }
#else
        std::array<pollfd,local_session_client_limit+1> items{};nfds_t count=0;
        if(std::any_of(peers.begin(),peers.end(),[](const auto& peer){return !peer;}))items[count++]={listener.fd,POLLIN,0};
        for(const auto& peer:peers)if(peer && peer->connected) {
            const auto events=static_cast<short>(peer->token ? 0 : peer->output.empty() ? POLLIN : POLLOUT);
            items[count++]={peer->channel.fd,events,0};
        }
        const auto result=::poll(items.data(),count,static_cast<int>(timeout));
        if(result<0 && errno==EINTR)return false;
        check(result>=0,"Local server socket readiness wait failed.");return result>0;
#endif
    }
    bool reply(std::uint64_t token,std::string_view payload) {
        validate_payload(payload,local_session_response_limit,true);
        for(std::size_t i=0;i<peers.size();++i)if(peers[i] && peers[i]->connected && token!=0 && peers[i]->token==token) {
            auto& peer=*peers[i];
#ifndef _WIN32
            if(terminal(peer)) { drop(i);return false; }
#endif
            if(!peer.channel.alive()) { drop(i);return false; }
            auto bytes=frame(payload);peer.output=std::move(bytes);peer.output_used=0;peer.token=0;
            std::size_t budget=pump_budget;return flush_output(i,budget);
        }
        return false;
    }
};

struct LocalSessionClient::Impl {
    std::string path;Channel channel;
    Impl(std::string_view endpoint,std::uint32_t timeout):path(local_address(endpoint)) {
        const auto deadline=Clock::now()+std::chrono::milliseconds(timeout);
#ifdef _WIN32
        const std::wstring name(path.begin(),path.end());
        while(!channel.valid()) {
            channel.handle=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
            if(channel.valid())break;const auto error=GetLastError();
            check(error==ERROR_PIPE_BUSY || error==ERROR_FILE_NOT_FOUND,"Cannot connect to local session pipe.");
            check(Clock::now()<deadline,"Local session connection timed out.");std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(owner_is_current(channel.handle),"Local pipe server is not owned by the current user.");DWORD mode=PIPE_READMODE_BYTE|PIPE_WAIT;
        check(SetNamedPipeHandleState(channel.handle,&mode,nullptr,nullptr)!=0,"Cannot configure overlapped local pipe client.");
#else
        const auto directory=path.substr(0,path.find_last_of('/'));const auto address=socket_address(path);
        auto await_endpoint=[&] {
            channel.close();check(Clock::now()<deadline,"Local session connection timed out.");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        for(;;) {
            check(Clock::now()<deadline,"Local session connection timed out.");struct stat info{};
            if(::lstat(directory.c_str(),&info)!=0) {
                check(errno==ENOENT,"Cannot inspect local session directory.");await_endpoint();continue;
            }
            private_directory(false);
            if(::lstat(path.c_str(),&info)!=0) {
                check(errno==ENOENT,"Cannot inspect local session endpoint.");await_endpoint();continue;
            }
            check(S_ISSOCK(info.st_mode) && info.st_uid==geteuid(),"Local endpoint must be a same-user socket.");
            // bind() publishes the pathname before the server can chmod it.
            // Never connect using interim permissions; wait within the same
            // bounded startup deadline and recheck ownership/type each time.
            if((info.st_mode&07777)!=0600) { await_endpoint();continue; }
            channel.fd=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);check(channel.valid(),"Cannot create local client socket.");
            const auto result=connect(channel.fd,reinterpret_cast<const sockaddr*>(&address),sizeof(address));
            if(result==0)break;
            int error=errno;
            if(error==EINPROGRESS) {
                wait_io(channel,true,deadline);socklen_t size=sizeof(error);
                check(getsockopt(channel.fd,SOL_SOCKET,SO_ERROR,&error,&size)==0,"Cannot inspect local socket connection status.");
                if(error==0)break;
            }
            check(error==ENOENT || error==ECONNREFUSED || error==EAGAIN || error==EWOULDBLOCK,"Cannot connect to local session socket.");
            await_endpoint();
        }
        check(same_user(channel.fd),"Local socket server is not owned by the current user.");
#endif
    }
    std::string exchange(std::string_view payload,std::uint32_t timeout) {
        check(channel.valid(),"Local session connection is closed.");const auto deadline=Clock::now()+std::chrono::milliseconds(timeout);
        try {
            validate_payload(payload,local_session_request_limit,false);auto bytes=frame(payload);std::size_t sent=0;
            while(sent<bytes.size()) {
                check(Clock::now()<deadline,"Local session exchange timed out; connection closed.");
#ifdef _WIN32
                const auto io=client_io(channel,true,bytes.data()+sent,bytes.size()-sent,deadline);sent+=io.bytes;
#else
                const auto io=channel.write(bytes.data()+sent,bytes.size()-sent);check(!io.closed,"Local session disconnected while sending.");sent+=io.bytes;if(!io.bytes)wait_io(channel,true,deadline);
#endif
            }
            auto read_exact=[&](char* data,std::size_t count) {
                std::size_t received=0;while(received<count) {
                    check(Clock::now()<deadline,"Local session exchange timed out; connection closed.");
#ifdef _WIN32
                    const auto io=client_io(channel,false,data+received,count-received,deadline);received+=io.bytes;
#else
                    const auto io=channel.read(data+received,count-received);check(!io.closed,"Local session disconnected while receiving.");received+=io.bytes;if(!io.bytes)wait_io(channel,false,deadline);
#endif
                }
            };
            std::array<unsigned char,4> header{};read_exact(reinterpret_cast<char*>(header.data()),header.size());const auto count=decode_size(header);
            check(count<=local_session_response_limit,"Local session response exceeds 32 MiB.");std::string result(count,'\0');read_exact(result.data(),result.size());
            check(utf8(result),"Local session response is not UTF-8.");return result;
        }catch(...) { channel.close();throw; }
    }
};

LocalSessionServer::LocalSessionServer(std::string_view endpoint) { validate_endpoint(endpoint);impl_=std::make_unique<Impl>(endpoint); }
LocalSessionServer::~LocalSessionServer()=default;
std::vector<LocalSessionRequest> LocalSessionServer::poll() { return impl_->poll(); }
bool LocalSessionServer::wait(std::uint32_t timeout) {
    if(timeout>600000)throw std::invalid_argument("Local server wait must be 0..600000 milliseconds.");
    return impl_->wait(timeout);
}
bool LocalSessionServer::reply(std::uint64_t token,std::string_view payload) { return impl_->reply(token,payload); }
std::size_t LocalSessionServer::clients() const { return static_cast<std::size_t>(std::count_if(impl_->peers.begin(),impl_->peers.end(),[](const auto& peer){return peer && peer->connected;})); }
std::string LocalSessionServer::address() const { return impl_->path; }
LocalSessionClient::LocalSessionClient(std::string_view endpoint,std::uint32_t timeout) { validate_endpoint(endpoint);validate_timeout(timeout);impl_=std::make_unique<Impl>(endpoint,timeout); }
LocalSessionClient::~LocalSessionClient()=default;
std::string LocalSessionClient::exchange(std::string_view payload,std::uint32_t timeout) { validate_timeout(timeout);return impl_->exchange(payload,timeout); }
std::string LocalSessionClient::address() const { return impl_->path; }
}
