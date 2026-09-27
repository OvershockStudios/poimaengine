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
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit RawClient(const std::string& path) {
        const std::wstring wide(path.begin(),path.end());handle=CreateFileW(wide.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        check(handle!=INVALID_HANDLE_VALUE,"Raw pipe connect failed.");DWORD mode=PIPE_READMODE_BYTE|PIPE_NOWAIT;check(SetNamedPipeHandleState(handle,&mode,nullptr,nullptr)!=0,"Raw pipe mode failed.");
    }
    ~RawClient() { if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle); }
    void send(std::string_view data) { DWORD n=0;check(WriteFile(handle,data.data(),static_cast<DWORD>(data.size()),&n,nullptr)!=0 && n==data.size(),"Raw pipe short test write."); }
    bool gone() { DWORD count=0;return !PeekNamedPipe(handle,nullptr,0,nullptr,&count,nullptr); }
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
#endif
};
std::string header(std::uint32_t size) { std::string bytes(4,'\0');for(unsigned i=0;i<4;++i)bytes[i]=static_cast<char>((size>>(8*i))&255);return bytes; }
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
        std::cout<<"Local session transport tests passed.\n";return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
