// SPDX-License-Identifier: Apache-2.0
#include "poima/local_session.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif
using namespace poima;
namespace {
using Clock=std::chrono::steady_clock;
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
std::string endpoint(const char* suffix) {
#ifdef _WIN32
    const auto process=GetCurrentProcessId();
#else
    const auto process=getpid();
#endif
    return "native_"+std::to_string(process)+"_"+std::to_string(Clock::now().time_since_epoch().count())+"_"+suffix;
}
template<class F> void rejects(F&& operation,const char* message) {
    bool failed=false;try { operation(); }catch(const std::exception&) { failed=true; }check(failed,message);
}
template<class Ready,class Receive> void pump(LocalSessionServer& server,Ready ready,Receive receive) {
    const auto end=Clock::now()+std::chrono::seconds(8);
    while(!ready()) {
        for(auto& request:server.poll())receive(request);
        check(Clock::now()<end,"Native local-session test pump timed out.");std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
template<class T> bool ready(std::future<T>& result) { return result.wait_for(std::chrono::milliseconds(0))==std::future_status::ready; }

struct RawClient {
    std::string incoming;
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit RawClient(const std::string& path) {
        const std::wstring wide(path.begin(),path.end());handle=CreateFileW(wide.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        check(handle!=INVALID_HANDLE_VALUE,"Raw pipe connect failed.");DWORD mode=PIPE_READMODE_BYTE|PIPE_NOWAIT;check(SetNamedPipeHandleState(handle,&mode,nullptr,nullptr)!=0,"Raw pipe mode failed.");
    }
    ~RawClient() { if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle); }
    void send(std::string_view data) { DWORD n=0;check(WriteFile(handle,data.data(),static_cast<DWORD>(data.size()),&n,nullptr)!=0 && n==data.size(),"Raw pipe short test write."); }
    bool gone() { DWORD count=0;return !PeekNamedPipe(handle,nullptr,0,nullptr,&count,nullptr); }
    std::size_t read_available(char* data,std::size_t count) {
        DWORD used=0;if(ReadFile(handle,data,static_cast<DWORD>(count),&used,nullptr))return used;
        const auto error=GetLastError();
        if(error==ERROR_NO_DATA || error==ERROR_PIPE_BUSY || error==ERROR_PIPE_LISTENING)return 0;
        throw std::runtime_error("Raw pipe disconnected while awaiting reply.");
    }
#else
    int fd=-1;
    explicit RawClient(const std::string& path) {
        fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);check(fd>=0,"Raw socket creation failed.");sockaddr_un address{};address.sun_family=AF_UNIX;
        check(path.size()<sizeof(address.sun_path),"Raw address too long.");std::memcpy(address.sun_path,path.c_str(),path.size()+1);
        check(connect(fd,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==0,"Raw socket connect failed.");
    }
    ~RawClient() { if(fd>=0)::close(fd); }
    void send(std::string_view data) { check(::send(fd,data.data(),data.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(data.size()),"Raw socket short test write."); }
    bool gone() { char c=0;const auto n=recv(fd,&c,1,MSG_PEEK|MSG_DONTWAIT);return n==0 || (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR); }
    std::size_t read_available(char* data,std::size_t count) {
        const auto used=recv(fd,data,count,MSG_DONTWAIT);
        if(used>0)return static_cast<std::size_t>(used);
        if(used<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
        throw std::runtime_error("Raw socket disconnected while awaiting reply.");
    }
#endif
    bool receive_frame(std::string& payload) {
        // A deliberately small reader allows a large reply to remain pending
        // while another connection is serviced. Retain subsequent frame bytes.
        std::array<char,32*1024> chunk{};
        const auto used=read_available(chunk.data(),chunk.size());incoming.append(chunk.data(),used);
        if(incoming.size()<4)return false;
        std::uint32_t size=0;
        for(unsigned i=0;i<4;++i)size|=static_cast<std::uint32_t>(static_cast<unsigned char>(incoming[i]))<<(8*i);
        check(size<=local_session_response_limit,"Raw reply declared an oversized response frame.");
        if(incoming.size()<4+static_cast<std::size_t>(size))return false;
        payload.assign(incoming.data()+4,size);incoming.erase(0,4+static_cast<std::size_t>(size));return true;
    }
};
std::string header(std::uint32_t size) { std::string bytes(4,'\0');for(unsigned i=0;i<4;++i)bytes[i]=static_cast<char>((size>>(8*i))&255);return bytes; }
#ifdef _WIN32
// Independent raw peer: tiny buffers force pending client writes, and explicit
// same-user ownership exercises the native client's real ownership check.
struct RawPipeServer {
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit RawPipeServer(const std::string& name) {
        std::string path;{ LocalSessionServer derive(name);path=derive.address(); }
        HANDLE token=nullptr;check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)!=0,"Raw server token query failed.");
        DWORD count=0;GetTokenInformation(token,TokenUser,nullptr,0,&count);std::vector<unsigned char> info(count);
        const bool found=count>0 && GetTokenInformation(token,TokenUser,info.data(),count,&count)!=0;CloseHandle(token);
        check(found,"Raw server user SID query failed.");
        LPWSTR text=nullptr;check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid,&text)!=0,"Raw server SID conversion failed.");
        const std::unique_ptr<void,decltype(&LocalFree)> sid_owner(text,&LocalFree);
        const std::wstring sddl=L"O:"+std::wstring(text)+L"D:P(A;;GA;;;"+std::wstring(text)+L")";
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=0,"Raw server ACL conversion failed.");
        const std::unique_ptr<void,decltype(&LocalFree)> descriptor_owner(descriptor,&LocalFree);
        SECURITY_ATTRIBUTES attributes{};attributes.nLength=sizeof(attributes);attributes.lpSecurityDescriptor=descriptor;
        const std::wstring wide(path.begin(),path.end());
        handle=CreateNamedPipeW(wide.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&attributes);
        check(handle!=INVALID_HANDLE_VALUE,"Raw server creation failed.");
    }
    RawPipeServer(const RawPipeServer&)=delete;
    ~RawPipeServer() { close(); }
    void close() { if(handle!=INVALID_HANDLE_VALUE) { CloseHandle(handle);handle=INVALID_HANDLE_VALUE; } }
    void connect() {
        const auto end=Clock::now()+std::chrono::seconds(4);
        for(;;) {
            const bool listening=ConnectNamedPipe(handle,nullptr)!=0;
            const auto error=listening ? ERROR_SUCCESS : GetLastError();
            if(error==ERROR_PIPE_CONNECTED)return;
            // NOWAIT success enters listening state; only PIPE_CONNECTED
            // establishes a client. Do not race the async client's startup.
            check(error==ERROR_SUCCESS || error==ERROR_PIPE_LISTENING,"Raw server accept failed.");
            check(Clock::now()<end,"Raw server accept timed out.");std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    bool send(std::string_view bytes) {
        DWORD used=0;return WriteFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&used,nullptr)!=0 && used==bytes.size();
    }
    std::string request() {
        std::string bytes;const auto end=Clock::now()+std::chrono::seconds(4);
        for(;;) {
            std::array<char,4096> buffer{};DWORD used=0;
            const bool read=ReadFile(handle,buffer.data(),static_cast<DWORD>(buffer.size()),&used,nullptr)!=0;
            if(!read)check(GetLastError()==ERROR_NO_DATA,"Raw server request disconnected.");
            bytes.append(buffer.data(),used);
            if(bytes.size()>=4) {
                std::uint32_t size=0;for(unsigned i=0;i<4;++i)size|=static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i]))<<(8*i);
                check(size<=local_session_request_limit,"Raw server received oversized request.");
                if(bytes.size()>=4+static_cast<std::size_t>(size)) {
                    check(bytes.size()==4+static_cast<std::size_t>(size),"Raw server received an automatic replay or extra request.");
                    return bytes.substr(4);
                }
            }
            check(Clock::now()<end,"Raw server request timed out.");std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void await_buffered_request() {
        const auto end=Clock::now()+std::chrono::seconds(4);
        for(;;) {
            DWORD count=0;check(PeekNamedPipe(handle,nullptr,0,nullptr,&count,nullptr)!=0,"Raw server lost unread request.");
            if(count>0)return;
            check(Clock::now()<end,"Raw server never received write bytes.");std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};
std::string failure(LocalSessionClient& client,std::string_view request,std::uint32_t timeout) {
    try { client.exchange(request,timeout); }catch(const std::exception& error) { return error.what(); }
    throw std::runtime_error("Raw-peer exchange unexpectedly succeeded.");
}
template<class T> T complete(std::future<T>& client,RawPipeServer& server) {
    const bool finished=client.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    if(!finished) {
        // Release a faulty pending implementation before std::future's joining
        // destructor; the failed deadline still fails this regression.
        server.close();client.wait_for(std::chrono::seconds(5));
    }
    check(finished,"Raw-peer client did not finish within its bounded deadline.");return client.get();
}
void windows_client_completion_contract() {
    const auto reusable=endpoint("completion_reuse");
    // A response already queued before exchange covers immediate completion;
    // the ordinary roundtrip above also exercises response reads that must wait.
    for(unsigned i=0;i<3;++i) {
        RawPipeServer server(reusable);LocalSessionClient client(reusable);server.connect();
        check(server.send(header(5)+"ready"),"Prequeued raw response failed.");
        check(client.exchange("first",2000)=="ready","Prequeued response changed.");
        check(server.request()=="first","Synchronous request changed.");
        check(server.send(header(0)),"Prequeued empty response failed.");
        check(client.exchange("empty",2000).empty(),"Prequeued empty response changed.");
        check(server.request()=="empty","Connection failed after immediate completion.");
    }
    // Each cancellation is followed by a fresh connection on exactly the same
    // endpoint, detecting leaked handles/listeners rather than just new names.
    for(unsigned i=0;i<3;++i) {
        {
            RawPipeServer server(reusable);
            auto client=std::async(std::launch::async,[&] {
                LocalSessionClient connection(reusable);
                const auto message=failure(connection,"withheld",250);
                check(message.find("pending read timed out")!=std::string::npos,"Withheld response did not time out a pending read.");
                rejects([&]{connection.exchange("retry",50);},"Timed-out response connection was reusable.");
            });
            server.connect();check(server.request()=="withheld","Pending read request changed.");complete(client,server);
            check(!server.send(header(4)+"late"),"Late reply survived cancellation and closed connection.");
        }
        {
            RawPipeServer server(reusable);
            auto client=std::async(std::launch::async,[&] {
                LocalSessionClient connection(reusable);
                const auto message=failure(connection,std::string(local_session_request_limit,'w'),250);
                check(message.find("pending write timed out")!=std::string::npos,"Unread oversized-quota request did not time out a pending write.");
                rejects([&]{connection.exchange("retry",50);},"Timed-out write connection was reusable.");
            });
            server.connect();server.await_buffered_request();
            // Sizes are advisory: assert the pending-write timeout diagnostic
            // rather than assuming the operating system cannot grow buffers.
            complete(client,server);check(!server.send(header(4)+"late"),"Late reply survived pending-write cancellation.");
        }
    }
    {
        RawPipeServer server(reusable);
        auto client=std::async(std::launch::async,[&] {
            LocalSessionClient connection(reusable);
            const auto message=failure(connection,"disconnect",2000);
            check(message.find("disconnected")!=std::string::npos,"Peer close was reported as a timeout instead of disconnect.");
            rejects([&]{connection.exchange("retry",50);},"Disconnected response connection was reusable.");
        });
        server.connect();check(server.request()=="disconnect","Pending disconnect request changed.");server.close();complete(client,server);
    }
    {
        RawPipeServer server(reusable);
        auto client=std::async(std::launch::async,[&] {
            LocalSessionClient connection(reusable);
            const auto message=failure(connection,"fragment",800);
            check(message.find("timed out")!=std::string::npos,"Fragmented response did not honor the original exchange deadline.");
            rejects([&]{connection.exchange("retry",50);},"Fragment timeout connection was reusable.");
        });
        server.connect();check(server.request()=="fragment","Partial response request changed.");
        const auto started=Clock::now();const auto prefix=header(2);
        check(server.send(std::string_view(prefix).substr(0,2)),"First response fragment failed.");
        std::this_thread::sleep_until(started+std::chrono::milliseconds(450));
        check(server.send(prefix.substr(2)+"a"),"Second response fragment failed before its deadline.");
        // Resetting the deadline per read/header/payload would allow this last
        // byte and falsely complete the request; one exchange deadline rejects.
        std::this_thread::sleep_until(started+std::chrono::milliseconds(1100));
        const bool late=server.send("b");complete(client,server);check(!late,"Late partial response reached a connection that should be closed.");
    }
    {
        RawPipeServer server(reusable);LocalSessionClient client(reusable);server.connect();
        check(server.send(header(8)+"reopened"),"Fresh reply after cancellation failed.");
        check(client.exchange("final",2000)=="reopened","Cancellation poisoned a subsequent fresh client.");
        check(server.request()=="final","Fresh request after cancellation changed.");
    }
}
#endif
std::string receive_without_poll(RawClient& client) {
    const auto end=Clock::now()+std::chrono::seconds(8);std::string reply;
    while(!client.receive_frame(reply)) {
        check(Clock::now()<end,"Reply required another server poll instead of reaching the waiting client.");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return reply;
}

// Owner-side dispatch sleeps on transport readiness, never on a poll cadence.
// Client futures retain their own exchange deadlines if this fixture fails.
template<class Ready,class Receive> void wait_pump(LocalSessionServer& server,Ready done,Receive receive) {
    const auto end=Clock::now()+std::chrono::seconds(8);
    while(!done()) {
        for(auto& request:server.poll())receive(request);
        if(done())return;
        check(Clock::now()<end,"Readiness-driven owner timed out.");server.wait(100);
    }
}
void quiescent(LocalSessionServer& server) {
    for(unsigned i=0;i<16;++i) {
        check(server.poll().empty(),"Quiescent transport fabricated or repeated a request.");
        if(!server.wait(0)) {
            check(!server.wait(40),"Quiescent/deferred transport woke without actionable progress.");
            check(!server.wait(0),"Quiescent/deferred transport retained a permanently ready event.");return;
        }
    }
    throw std::runtime_error("Readiness kept waking after all actionable progress was drained.");
}
void readiness_contract() {
    {
        LocalSessionServer server(endpoint("wait_idle"));
        check(!server.wait(0),"Idle zero wait reported work.");
        check(!server.wait(30),"Idle positive wait reported work.");
        rejects([&]{server.wait(600001);},"Readiness wait accepted an excessive timeout.");
        rejects([&]{server.wait(std::numeric_limits<std::uint32_t>::max());},"Readiness wait accepted an overflowing timeout.");
        quiescent(server);
    }
    {
        const auto name=endpoint("wait_arrival");LocalSessionServer server(name);
        std::promise<void> begin;auto gate=begin.get_future();
        auto client=std::async(std::launch::async,[&] {
            gate.wait();LocalSessionClient connection(name,3000);return connection.exchange("awaken",3000);
        });
        check(server.poll().empty(),"Empty server emitted a request before connection.");begin.set_value();
        check(server.wait(3000),"Incoming connection/request did not wake the owner.");
        std::size_t count=0;
        wait_pump(server,[&]{return ready(client);},[&](const auto& request) {
            check(request.payload=="awaken" && ++count==1,"Readiness lost or repeated an arriving request.");
            check(server.reply(request.token,"awake"),"Readiness-driven reply failed.");
        });check(client.get()=="awake" && count==1,"Readiness-driven roundtrip differed.");
        wait_pump(server,[&]{return server.clients()==0;},[&](const auto&){throw std::runtime_error("Disconnected client request replayed.");});
        quiescent(server);
    }
    {
        const auto name=endpoint("wait_budget");LocalSessionServer server(name);
        const std::string maximum(local_session_request_limit,'b');
        auto client=std::async(std::launch::async,[&] {
            LocalSessionClient connection(name,4000);return connection.exchange(maximum,4000);
        });
        std::size_t count=0;
        wait_pump(server,[&]{return ready(client);},[&](const auto& request) {
            check(request.payload==maximum && ++count==1,"Readiness budget continuation lost or repeated request bytes.");
            check(server.reply(request.token,"budget-complete"),"Readiness budget continuation reply failed.");
        });check(client.get()=="budget-complete" && count==1,"Readiness failed after exhausting a request poll budget.");
    }
    {
        LocalSessionServer server(endpoint("wait_partial"));RawClient raw(server.address());
        const auto prefix=header(4);raw.send(std::string_view(prefix).substr(0,1));
        wait_pump(server,[&]{return server.clients()==1;},[&](const auto&){throw std::runtime_error("Partial header fabricated a request.");});
        quiescent(server);
        // Deliberately arrive between the last poll and the next wait.
        check(server.poll().empty(),"Incomplete header emitted a request.");raw.send(std::string_view(prefix).substr(1));
        check(server.wait(3000),"Bytes arriving between poll and wait were lost.");quiescent(server);
        raw.send("pi");check(server.wait(3000),"Partial payload did not wake transport progress.");quiescent(server);
        raw.send("ng");check(server.wait(3000),"Completed fragmented payload did not wake owner.");
        std::uint64_t token=0;wait_pump(server,[&]{return token!=0;},[&](const auto& request) {
            check(token==0 && request.payload=="ping","Fragmented readiness request differed or repeated.");token=request.token;
        });
        raw.send(header(4)+"next");quiescent(server);
        // Readable pipelined bytes belong to an outstanding token and must not
        // become a hot readiness loop or dispatch before its reply completes.
        check(!server.wait(60),"Deferred token made pipelined bytes continuously ready.");
        check(server.reply(token,"first"),"Deferred readiness reply failed.");
        check(receive_without_poll(raw)=="first","Deferred readiness response changed.");
        std::uint64_t next=0;wait_pump(server,[&]{return next!=0;},[&](const auto& request) {
            check(next==0 && request.payload=="next","Deferred pipelined request was lost or repeated.");next=request.token;
        });
        check(next!=token && !server.reply(token,"stale"),"Readiness revived a stale token.");
        check(server.reply(next,"second"),"Pipelined readiness reply failed.");
        check(receive_without_poll(raw)=="second","Pipelined readiness frame order changed.");quiescent(server);
    }
    {
        const auto name=endpoint("wait_capacity");LocalSessionServer server(name);
        std::vector<std::future<std::string>> clients;
        for(std::size_t i=0;i<local_session_client_limit;++i)clients.push_back(std::async(std::launch::async,[&,i] {
            LocalSessionClient connection(name,4000);return connection.exchange("held"+std::to_string(i),4000);
        }));
        std::vector<LocalSessionRequest> held;
        wait_pump(server,[&]{return held.size()==local_session_client_limit;},[&](auto request){held.push_back(std::move(request));});
        check(server.clients()==local_session_client_limit,"Readiness did not fill all eight slots.");quiescent(server);
#ifndef _WIN32
        // Unix permits the ninth connection into the listener backlog even
        // though no peer slot is available. Its readable listener is ineligible.
        auto ninth=std::make_unique<RawClient>(server.address());ninth->send(header(5)+"ninth");
#else
        const auto path=server.address();const std::wstring wide(path.begin(),path.end());
        HANDLE unavailable=CreateFileW(wide.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        const auto capacity_error=GetLastError();if(unavailable!=INVALID_HANDLE_VALUE)CloseHandle(unavailable);
        check(unavailable==INVALID_HANDLE_VALUE && capacity_error==ERROR_PIPE_BUSY,"Ninth Windows connection unexpectedly bypassed occupied slots.");
        std::promise<void> attempting;auto attempted=attempting.get_future();
        auto ninth=std::async(std::launch::async,[&] {
            attempting.set_value();LocalSessionClient connection(name,4000);return connection.exchange("ninth",4000);
        });attempted.wait();
#endif
        quiescent(server);check(!server.wait(100),"Full slots plus queued ninth connection spun readiness.");
        for(const auto& request:held)check(server.reply(request.token,request.payload),"Held capacity reply failed.");
        std::uint64_t ninth_token=0;
        wait_pump(server,[&]{return ninth_token!=0 && std::all_of(clients.begin(),clients.end(),[](auto& c){return ready(c);});},[&](const auto& request) {
            check(request.payload=="ninth" && ninth_token==0,"Capacity queue lost or repeated the ninth request.");ninth_token=request.token;
        });
        for(std::size_t i=0;i<clients.size();++i)check(clients[i].get()=="held"+std::to_string(i),"Held capacity responses crossed clients.");
        check(server.reply(ninth_token,"last"),"Queued ninth reply failed.");
#ifdef _WIN32
        wait_pump(server,[&]{return ready(ninth);},[&](const auto&){throw std::runtime_error("Ninth client replayed a request.");});
        check(ninth.get()=="last","Queued ninth response differed.");
#else
        check(receive_without_poll(*ninth)=="last","Queued ninth response differed.");ninth.reset();
#endif
        wait_pump(server,[&]{return server.clients()==0;},[&](const auto&){throw std::runtime_error("Capacity cleanup replayed a request.");});quiescent(server);
    }
    const auto reusable=endpoint("wait_cleanup");
    for(unsigned i=0;i<3;++i) {
        { LocalSessionServer server(reusable);server.wait(0); }
        // Destroy with a read armed against an incomplete request.
        {
            auto server=std::make_unique<LocalSessionServer>(reusable);auto raw=std::make_unique<RawClient>(server->address());
            raw->send(header(8)+"a");wait_pump(*server,[&]{return server->clients()==1;},[&](const auto&){throw std::runtime_error("Cleanup partial read fabricated request.");});
            quiescent(*server);server->wait(0);server.reset();raw.reset();
        }
        // Destroy with a large response and a reader deliberately not draining.
        {
            auto server=std::make_unique<LocalSessionServer>(reusable);auto raw=std::make_unique<RawClient>(server->address());
            raw->send(header(4)+"hold");std::uint64_t token=0;
            wait_pump(*server,[&]{return token!=0;},[&](const auto& request){check(request.payload=="hold","Cleanup request changed.");token=request.token;});
            check(server->reply(token,std::string(2*1024*1024,'q')),"Cleanup large reply failed.");server->wait(0);server.reset();raw.reset();
        }
        { LocalSessionServer reopened(reusable);check(!reopened.wait(0),"Reopened listener inherited stale readiness."); }
    }
}
}

int main() {
    try {
        for(const auto& bad:{std::string{},std::string("../escape"),std::string("has space"),std::string(65,'x'),std::string("nul\0value",9)}) {
            rejects([&]{LocalSessionServer server(bad);},"Invalid endpoint accepted by server.");
            rejects([&]{LocalSessionClient client(bad,1);},"Invalid endpoint accepted by client.");
        }
        const auto name=endpoint("roundtrip");std::string address;
        {
            LocalSessionServer server(name);address=server.address();
            rejects([&]{LocalSessionServer duplicate(name);},"Duplicate endpoint host was accepted.");
#ifndef _WIN32
            struct stat info{};check(lstat(address.c_str(),&info)==0 && S_ISSOCK(info.st_mode) && (info.st_mode&07777)==0600 && info.st_uid==geteuid(),"Socket permissions/owner are incorrect.");
#endif
            auto client=std::async(std::launch::async,[&] {
                LocalSessionClient connection(name);check(connection.address()==address,"Client/server address derivation differs.");
                check(connection.exchange("first")=="reply:first","First response differed.");
                check(connection.exchange("notification").empty(),"Notification response was not empty.");
                const std::string maximum(local_session_request_limit,'x');
                check(connection.exchange(maximum)==std::string(2*1024*1024,'y'),"Partial large-frame transfer differed.");
                check(connection.exchange("last")=="reply:last","Connection was not reusable after large/empty frames.");
            });
            std::size_t count=0;
            pump(server,[&]{return ready(client);},[&](const auto& request) {
                ++count;
                if(request.payload=="notification")check(server.reply(request.token,""),"Empty reply could not be queued.");
                else if(request.payload.size()==local_session_request_limit) {
                    check(request.payload==std::string(local_session_request_limit,'x'),"Large request changed in transit.");
                    rejects([&]{server.reply(request.token,std::string(local_session_response_limit+1,'z'));},"Oversized response accepted.");
                    rejects([&]{server.reply(request.token,std::string("\xc0\x80",2));},"Malformed response UTF-8 accepted.");
                    check(server.reply(request.token,std::string(2*1024*1024,'y')), "Valid reply after rejected response failed.");
                }else check(server.reply(request.token,"reply:"+request.payload),"Reply token unexpectedly stale.");
                check(!server.reply(request.token,"duplicate"),"Duplicate reply accepted.");
            });client.get();check(count==4,"Roundtrip request count differed.");
            pump(server,[&]{return server.clients()==0;},[&](const auto&){throw std::runtime_error("Unexpected request during disconnect cleanup.");});
        }
#ifndef _WIN32
        check(!std::filesystem::exists(address),"Owned socket endpoint survived server destruction.");
#endif
        { LocalSessionServer reopened(name);check(reopened.address()==address,"Reopened endpoint address changed."); }

        {
            LocalSessionServer server(endpoint("immediate"));RawClient raw(server.address());
            raw.send(header(5)+"small");std::uint64_t small=0;
            pump(server,[&]{return small!=0;},[&](const auto& request){
                check(small==0 && request.payload=="small","Immediate small request was duplicated or corrupted.");small=request.token;
            });
            check(server.reply(small,"delivered-now"),"Immediate small reply was rejected.");
            check(receive_without_poll(raw)=="delivered-now","Small reply did not arrive intact without polling.");
            check(!server.reply(small,"duplicate"),"Immediate completed reply token was reused.");

            raw.send(header(5)+"empty");std::uint64_t empty=0;
            pump(server,[&]{return empty!=0;},[&](const auto& request){
                check(empty==0 && request.payload=="empty","Empty-response request was duplicated or discarded.");empty=request.token;
            });
            check(empty!=small && !server.reply(small,"stale"),"Stale small token could answer a later request.");
            check(server.reply(empty,""),"Immediate empty reply was rejected.");
            check(receive_without_poll(raw).empty(),"Empty response frame did not arrive without polling.");
            check(!server.reply(empty,"duplicate"),"Immediate empty token was reused.");

            raw.send(header(5)+"reuse");std::uint64_t reused=0;
            pump(server,[&]{return reused!=0;},[&](const auto& request){
                check(reused==0 && request.payload=="reuse","Request following empty reply was duplicated or discarded.");reused=request.token;
            });
            check(server.reply(reused,"after-empty"),"Reply following empty response was rejected.");
            check(receive_without_poll(raw)=="after-empty","Immediate frame order changed after empty response.");
        }

        {
            LocalSessionServer server(endpoint("slow_reader"));RawClient slow(server.address());
            slow.send(header(4)+"slow");std::uint64_t slow_token=0,next_token=0,fast_token=0;
            std::size_t slow_count=0,next_count=0,fast_count=0;
            wait_pump(server,[&]{return slow_token!=0;},[&](const auto& request){
                check(request.payload=="slow" && ++slow_count==1,"Slow request was corrupted or re-emitted.");slow_token=request.token;
            });
            const std::string large="begin:"+std::string(2*1024*1024,'z')+":end";
            check(server.reply(slow_token,large),"Large slow-reader reply was rejected.");
            check(!server.reply(slow_token,"duplicate"),"Pending large-reply token accepted a duplicate.");
            std::string received;const auto prefix_deadline=Clock::now()+std::chrono::seconds(8);
            while(slow.incoming.size()<4) {
                check(!slow.receive_frame(received),"Large reply unexpectedly completed during its prefix read.");
                check(Clock::now()<prefix_deadline,"Large reply prefix needed another server poll.");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            slow.send(header(10)+"after-slow");
            RawClient fast(server.address());fast.send(header(4)+"fast");
            const auto dispatch=[&](const auto& request) {
                if(request.payload=="fast") {
                    check(++fast_count==1,"Other client request was re-emitted.");fast_token=request.token;
                }else if(request.payload=="after-slow") {
                    check(++next_count==1,"Pipelined slow-reader request was re-emitted.");next_token=request.token;
                }else throw std::runtime_error("Completed slow request was re-emitted or a request was corrupted.");
            };
            wait_pump(server,[&]{return fast_token!=0;},dispatch);
            check(server.reply(fast_token,"fast-now"),"Other client reply was rejected behind slow output.");
            check(receive_without_poll(fast)=="fast-now","Slow reader blocked another client's immediate reply.");
            check(!server.reply(fast_token,"duplicate"),"Other completed client token was reused.");
            wait_pump(server,[&]{return slow.receive_frame(received);},dispatch);
            check(received==large,"Partial large response lost, duplicated or reordered bytes.");
            wait_pump(server,[&]{return next_token!=0;},dispatch);
            check(next_token!=slow_token && !server.reply(slow_token,"stale"),"Stale slow token could answer its follow-up request.");
            check(server.reply(next_token,"after-large"),"Pipelined request was discarded after slow output.");
            check(receive_without_poll(slow)=="after-large","Follow-up frame crossed or corrupted the large response.");
            check(!server.reply(next_token,"duplicate"),"Completed follow-up token was reused.");
            check(server.poll().empty(),"Completed requests were re-emitted after all replies.");
            check(slow_count==1 && next_count==1 && fast_count==1,"Slow-reader multi-client request counts differ.");
        }

        {
            const auto token=endpoint("delayed");
            auto client=std::async(std::launch::async,[&] { LocalSessionClient connection(token,2000);return connection.exchange("started-later"); });
            std::this_thread::sleep_for(std::chrono::milliseconds(40));LocalSessionServer server(token);
            pump(server,[&]{return ready(client);},[&](const auto& request){check(server.reply(request.token,request.payload),"Delayed endpoint reply failed.");});
            check(client.get()=="started-later","Client did not wait for endpoint startup.");
            const auto before=Clock::now();rejects([&]{LocalSessionClient missing(endpoint("absent"),30);},"Absent endpoint did not time out.");
            check(Clock::now()-before>=std::chrono::milliseconds(20),"Connection timeout failed immediately before its deadline.");
        }

        {
            const auto token=endpoint("clients");LocalSessionServer server(token);std::vector<std::future<std::string>> clients;
            for(std::size_t i=0;i<local_session_client_limit;++i)clients.push_back(std::async(std::launch::async,[&,i]{LocalSessionClient client(token);return client.exchange("client"+std::to_string(i));}));
            std::vector<LocalSessionRequest> pending;
            pump(server,[&]{return pending.size()==local_session_client_limit;},[&](auto request){pending.push_back(std::move(request));});
            check(server.clients()==local_session_client_limit,"Eight independent connections were not accepted.");
            check(server.poll().empty(),"An in-flight request was emitted twice.");
            for(const auto& request:pending)check(server.reply(request.token,request.payload),"Deferred client reply failed.");
            pump(server,[&]{return std::all_of(clients.begin(),clients.end(),[](auto& item){return ready(item);});},[&](const auto&){throw std::runtime_error("Unexpected extra multi-client request.");});
            for(std::size_t i=0;i<clients.size();++i)check(clients[i].get()=="client"+std::to_string(i),"Multi-client responses crossed connections.");
        }
        {
            const auto token=endpoint("timeout");LocalSessionServer server(token);std::uint64_t pending=0;
            auto client=std::async(std::launch::async,[&] {
                LocalSessionClient connection(token);rejects([&]{connection.exchange("deferred",80);},"Missing response did not time out.");
                rejects([&]{connection.exchange("retry",10);},"Timed-out connection was reused or request automatically replayed.");
            });
            pump(server,[&]{return ready(client);},[&](const auto& request){check(pending==0,"Timed-out request replayed.");pending=request.token;});client.get();
            check(pending!=0,"Timeout test never delivered its request.");check(!server.reply(pending,"late"),"Reply to disconnected client did not return false.");
            pump(server,[&]{return server.clients()==0;},[&](const auto&){throw std::runtime_error("Unexpected timeout replay.");});
        }
        {
            const auto token=endpoint("shutdown");auto server=std::make_unique<LocalSessionServer>(token);bool received=false;
            auto client=std::async(std::launch::async,[&]{LocalSessionClient connection(token);rejects([&]{connection.exchange("pending",2000);},"Server shutdown did not disconnect client.");});
            pump(*server,[&]{return received;},[&](const auto&){received=true;});const auto before=Clock::now();server.reset();
            check(Clock::now()-before<std::chrono::milliseconds(500),"Server destructor blocked on pending client.");client.get();
        }
        {
            LocalSessionServer server(endpoint("framing"));RawClient raw(server.address());auto bytes=header(4);
            raw.send(std::string_view(bytes).substr(0,1));check(server.poll().empty(),"Partial length prefix emitted a request.");
            raw.send(std::string_view(bytes).substr(1));raw.send("pi");check(server.poll().empty(),"Partial payload emitted a request.");
            raw.send("ng");std::uint64_t token=0;
            pump(server,[&]{return token!=0;},[&](const auto& request){check(request.payload=="ping","Fragmented request was corrupted.");token=request.token;});
            check(server.reply(token,""),"Fragmented request could not be answered.");
        }
        for(const auto invalid:{std::uint32_t(0),std::uint32_t(local_session_request_limit+1),std::numeric_limits<std::uint32_t>::max()}) {
            LocalSessionServer server(endpoint("badsize"));RawClient raw(server.address());raw.send(header(invalid));
            pump(server,[&]{return raw.gone();},[&](const auto&){throw std::runtime_error("Malformed raw frame reached service.");});
        }
        {
            LocalSessionServer server(endpoint("badutf8"));RawClient raw(server.address());raw.send(header(3)+std::string("\xed\xa0\x80",3));
            pump(server,[&]{return raw.gone();},[&](const auto&){throw std::runtime_error("UTF-8 surrogate reached service.");});
        }
#ifndef _WIN32
        {
            const auto token=endpoint("reserved");std::string path;
            { LocalSessionServer first(token);path=first.address(); }
            { std::ofstream file(path);file<<"preserve"; }
            rejects([&]{LocalSessionServer invalid(token);},"Pre-existing regular file was replaced by endpoint.");
            std::ifstream file(path);std::string contents;file>>contents;check(contents=="preserve","Pre-existing endpoint file changed.");file.close();std::filesystem::remove(path);
        }
        {
            const auto token=endpoint("owned");auto server=std::make_unique<LocalSessionServer>(token);const auto path=server->address(),moved=path+".moved";
            std::filesystem::rename(path,moved);{ std::ofstream replacement(path);replacement<<"replacement"; }server.reset();
            std::ifstream file(path);std::string contents;file>>contents;check(contents=="replacement","Server cleanup removed a replacement endpoint.");file.close();std::filesystem::remove(path);std::filesystem::remove(moved);
        }
#endif
#ifdef _WIN32
        windows_client_completion_contract();
#endif
        readiness_contract();
        std::cout<<"Local session transport tests passed.\n";return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
