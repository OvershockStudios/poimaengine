// SPDX-License-Identifier: Apache-2.0
#include "poima/save_store.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace poima::saves;
namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
std::string id(unsigned n) { std::string result(32,'0');for(unsigned i=0;n;++i,n>>=4)result[31-i]="0123456789abcdef"[n&15];return result; }
std::string hash(const std::string& bytes) { return poima::sha256(std::as_bytes(std::span(bytes.data(),bytes.size()))); }
std::string read_file(const fs::path& path) { std::ifstream in(path,std::ios::binary);check(bool(in),"Cannot read fixture file.");return {std::istreambuf_iterator<char>(in),{}}; }
void overwrite(const fs::path& path,const std::string& value) { std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(value.data(),static_cast<std::streamsize>(value.size()));out.close();check(out.good(),"Cannot corrupt test-owned fixture."); }
template<class F> void rejects(F action,ErrorKind kind) {
    bool failed=false;try { action(); }catch(const Error& error) { failed=error.kind==kind;if(!failed)std::cerr<<"Unexpected error kind: "<<error.what()<<'\n'; }
    check(failed,"Expected typed save store rejection was absent.");
}
struct Workspace {
    fs::path root;
    explicit Workspace(const fs::path& base) {
        std::random_device random;
        for(unsigned i=0;i<32;++i) {auto path=base/("poima-save-test-"+std::to_string(random())+"-"+std::to_string(random()));if(fs::create_directory(path)) {root=path;break;} }
        check(!root.empty(),"Cannot create unique save test directory.");
    }
    ~Workspace() { std::error_code ignored;fs::remove_all(root,ignored); }
};
std::size_t payloads(const fs::path& path) { std::size_t count=0;for(const auto& file:fs::directory_iterator(path))if(file.path().extension()==".bin")++count;return count; }
fs::path named_payload(const fs::path& root,const char* which) { return root/Json::parse(read_file(root/"current.json")).at("payload").at(which).at("file").get<std::string>(); }
void initial(const fs::path& path) { Store store(path);store.write(0,id(1),"A");store.write(1,id(2),"B"); }
constexpr std::array boundaries{Boundary::payload_flushed,Boundary::payload_directory_flushed,Boundary::backup_flushed,
    Boundary::backup_replaced,Boundary::backup_directory_flushed,Boundary::manifest_flushed,Boundary::manifest_replaced,
    Boundary::manifest_directory_flushed,Boundary::pruned};
void crash_child(const fs::path& path,Boundary boundary) {
    Store store(path,[=](Boundary point){if(point==boundary)std::_Exit(73);});store.write(2,id(3),"C");std::_Exit(74);
}
void run_crash(const fs::path& path,Boundary boundary) {
#ifdef _WIN32
    std::wstring executable(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    check(length>0 && length<executable.size(),"Cannot discover save test executable.");executable.resize(length);
    auto command=L"\""+executable+L"\" --crash \""+path.wstring()+L"\" "+std::to_wstring(static_cast<int>(boundary));
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)!=0,"Cannot spawn test-owned crash child.");
    CloseHandle(process.hThread);const auto wait=WaitForSingleObject(process.hProcess,30000);
    if(wait!=WAIT_OBJECT_0) { TerminateProcess(process.hProcess,75);WaitForSingleObject(process.hProcess,5000); }
    DWORD code=0;const auto got=GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);
    check(wait==WAIT_OBJECT_0 && got && code==73,"Save crash child did not exit at the requested boundary.");
#else
    const auto child=::fork();check(child>=0,"Cannot fork save crash child.");
    if(child==0)crash_child(path,boundary);
    int status=0;check(::waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==73,"Save crash child did not exit at the requested boundary.");
#endif
}
void failure_boundary(const fs::path& path,Boundary boundary,bool crash) {
    initial(path);
    if(crash)run_crash(path,boundary);
    else {
        bool injected=false;Store faulted(path,[&](Boundary point){if(point==boundary) {injected=true;throw std::runtime_error("qualification fault");}});
        try {faulted.write(2,id(3),"C");}catch(const std::runtime_error&) {}
        check(injected,"Requested save exception boundary was not reached.");
    }
    Store reopened(path);const auto read=reopened.read();
    const bool committed=boundary>=Boundary::manifest_replaced;
    check(read.status.generation==(committed?3:2) && read.bytes==(committed?"C":"B"),"Interrupted write exposed partial/uncommitted payload.");
    const auto receipt=reopened.lookup(id(3),hash("C"));check(receipt.has_value()==committed,"Interrupted write receipt differs from commit boundary.");
    const auto result=reopened.write(2,id(3),"C");
    check(result.committed.generation==3 && result.replayed==committed,"Interrupted retry lost or duplicated its commit.");
    reopened.write(3,id(4),"D");check(payloads(path)==2 && reopened.read().bytes=="D","Post-crash repair did not retain exactly two generations.");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==4 && std::string(argv[1])=="--crash")crash_child(fs::path(std::u8string(reinterpret_cast<const char8_t*>(argv[2]))),static_cast<Boundary>(std::stoi(argv[3])));
        const auto base=argc==3 && std::string(argv[1])=="--directory"?fs::path(std::u8string(reinterpret_cast<const char8_t*>(argv[2]))):fs::temp_directory_path();
        Workspace workspace(base);const auto path=workspace.root/"basic";Store store(path);
        check(!store.inspect().exists && !fs::exists(path),"Inspect created a missing save slot.");
        check(!store.lookup(id(1),hash("A")) && !fs::exists(path),"Lookup created a missing save slot.");
        rejects([&]{store.read();},ErrorKind::io);
        rejects([&]{store.write(0,"bad","A");},ErrorKind::invalid);
        check(!fs::exists(path),"Invalid request created slot storage.");
        const std::string binary("A\0B\xff",4);const auto request=hash("normalized-service-request");
        const auto first=store.write(0,id(1),binary,false,request);
        check(first.committed.generation==1 && first.committed.verified && !first.replayed && payloads(path)==1,"Initial save metadata differs.");
        check(store.read().bytes==binary,"Opaque binary checkpoint bytes changed.");
        check(store.lookup(id(1),request)->replayed,"Persisted request lookup failed.");
        rejects([&]{store.lookup(id(1),hash("different"));},ErrorKind::reused_id);
        check(store.write(0,id(1),binary,false,request).replayed,"Exact write retry was not recognized.");
        rejects([&]{store.write(0,id(1),"changed",false,request);},ErrorKind::reused_id);
        rejects([&]{store.write(0,id(1),binary,true,request);},ErrorKind::reused_id);
        rejects([&]{store.write(0,id(2),"B");},ErrorKind::conflict);
        store.write(1,id(2),"B");store.write(2,id(3),"C");
        check(payloads(path)==2 && store.inspect().previous->generation==2,"Successful write retained the wrong generations.");
        auto replayed=store.write(0,id(1),binary,false,request);
        check(replayed.replayed && replayed.committed.generation==1 && !replayed.committed.verified && replayed.status.generation==3,
              "Old receipt should survive pruned payload without claiming its bytes are retained.");
        const auto failed_payload=named_payload(path,"current");overwrite(failed_payload,"corrupt");
        auto recovered=store.read();check(recovered.bytes=="B" && recovered.status.recovered && !recovered.status.manifest_recovered && recovered.status.generation==3 && recovered.status.selected->generation==2,
            "Corrupt newest payload did not explicitly recover its verified predecessor.");
        rejects([&]{store.write(3,id(4),"D");},ErrorKind::recovery_required);
        check(store.write(3,id(4),"D",true).committed.generation==4 && payloads(path)==3,"Acknowledged payload recovery did not preserve prior verified state and quarantine.");
        check(store.inspect().previous->generation==2 && !store.inspect().recovered,"Recovered write retained corrupt data instead of verified predecessor.");
        check(store.inspect().quarantined && store.inspect().quarantined->generation==3 && !store.inspect().quarantined->verified && read_file(failed_payload)=="corrupt",
              "Acknowledged recovery discarded corrupt bytes or their metadata.");
        store.write(4,id(5),"E");check(payloads(path)==3 && read_file(failed_payload)=="corrupt" && store.inspect().quarantined->generation==3,
              "Subsequent normal save silently pruned quarantine.");
        overwrite(named_payload(path,"current"),"second corruption");
        rejects([&]{store.write(5,id(6),"F",true);},ErrorKind::recovery_required);
        check(store.read().bytes=="D" && read_file(failed_payload)=="corrupt" && payloads(path)==3,"Second corruption changed existing recovery evidence.");

        const auto manifest_path=workspace.root/"manifest";initial(manifest_path);Store manifest_store(manifest_path);
        const auto corrupted="{broken manifest";overwrite(manifest_path/"current.json",corrupted);
        const auto fallback=manifest_store.read();check(fallback.bytes=="A" && fallback.status.manifest_recovered && fallback.status.recovered,"Previous committed manifest recovery failed.");
        rejects([&]{manifest_store.write(1,id(3),"C",true);},ErrorKind::recovery_required);
        rejects([&]{manifest_store.lookup(id(3),hash("C"));},ErrorKind::recovery_required);
        check(manifest_store.lookup(id(1),hash("A"))->replayed,"Known prior receipt was lost during read-only manifest recovery.");
        check(read_file(manifest_path/"current.json")==corrupted,"Manifest recovery overwrote damaged evidence.");
        overwrite(manifest_path/"previous.json","also broken");rejects([&]{manifest_store.read();},ErrorKind::corrupt);

        const auto receipt_path=workspace.root/"receipts";Store receipt_store(receipt_path);
        for(unsigned i=0;i<35;++i)receipt_store.write(i,id(i+1),std::to_string(i));
        check(!receipt_store.lookup(id(1),hash("0")),"Receipt ring exceeded its bounded history.");
        rejects([&]{receipt_store.write(0,id(1),"0");},ErrorKind::conflict);
        check(receipt_store.lookup(id(4),hash("3"))->committed.generation==4 && payloads(receipt_path)==2,"Bounded receipt tail or payload retention differs.");
        rejects([&]{receipt_store.write(35,id(36),std::string(maximum_checkpoint_bytes+1,'x'));},ErrorKind::invalid);

        const auto busy_path=workspace.root/"busy";Store busy(busy_path,[&](Boundary boundary) {
            if(boundary==Boundary::payload_flushed)rejects([&]{Store other(busy_path);other.inspect();},ErrorKind::busy);
        });busy.write(0,id(1),"A");
        const auto foreign_path=workspace.root/"foreign";fs::create_directory(foreign_path);overwrite(foreign_path/"unrelated.txt","preserve");
        rejects([&]{Store(foreign_path).write(0,id(1),"A");},ErrorKind::invalid);
        check(read_file(foreign_path/"unrelated.txt")=="preserve","Unknown file was modified.");
        const auto linked_path=workspace.root/"linked";initial(linked_path);const auto payload=named_payload(linked_path,"current");
        const auto target=workspace.root/"outside";overwrite(target,"B");fs::remove(payload);std::error_code link_error;fs::create_symlink(target,payload,link_error);
        if(!link_error)rejects([&]{Store(linked_path).read();},ErrorKind::invalid);
        else {overwrite(payload,"B");std::cout<<"Symlink fixture unavailable on this host: "<<link_error.message()<<'\n';}
        const auto hard_path=workspace.root/"hardlink";initial(hard_path);const auto hard_payload=named_payload(hard_path,"current");fs::remove(hard_payload);
        fs::create_hard_link(target,hard_payload,link_error);
        if(!link_error)rejects([&]{Store(hard_path).read();},ErrorKind::invalid);
        else std::cout<<"Hard-link fixture unavailable on this filesystem: "<<link_error.message()<<'\n';

        for(std::size_t i=0;i<boundaries.size();++i) {
            failure_boundary(workspace.root/("exception-"+std::to_string(i)),boundaries[i],false);
            failure_boundary(workspace.root/("crash-"+std::to_string(i)),boundaries[i],true);
        }
        std::cout<<"Save store passed: binary round-trip, generation/retry guards, 32 receipts, two payloads, corruption recovery, links/locking, 9 exception and 9 process-crash boundaries. No power-loss qualification claimed.\n";
        return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
