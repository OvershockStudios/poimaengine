// SPDX-License-Identifier: Apache-2.0
// Actual Windows failures use only fresh owned files and real WinAPI calls.
#include "../src/world_storage.hpp"
#include <fstream>
#include <chrono>
#include <random>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace {
namespace fs = std::filesystem;
using namespace poima::world_detail;
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string read(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);check(bool(input),"Cannot read owned output.");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
fs::path owned_output(int argc,char** argv) {
    if(argc==3&&std::string(argv[1])=="--output") {
        const auto path=fs::absolute(fs::u8path(argv[2]));
        check(!fs::exists(path)&&!fs::is_symlink(path),"Fresh owned output required.");
        check(fs::create_directories(path),"Cannot create fresh owned output.");
        return path;
    }
    check(argc==1,"Expected no arguments or --output FRESH_DIRECTORY.");
    const auto parent=fs::temp_directory_path();
    const auto time=std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device nonce;
    for(unsigned attempt=0;attempt!=16;++attempt) {
        const auto path=parent/("poima-world-storage-"+std::to_string(time)+"-"+
            std::to_string(nonce())+"-"+std::to_string(attempt));
        // Claim only a directory created by this exact call; a collision is
        // never used or deleted. All evidence remains after the test exits.
        if(fs::create_directory(path))return fs::absolute(path);
    }
    throw std::runtime_error("Cannot claim a fresh temporary output.");
}
void output_identity(const fs::path& path,bool automatic) {
    const auto utf8=path.generic_u8string();
    const char hex[]="0123456789abcdef";
    std::cout<<"{\"owned_output\":\"";
    for(const auto character:utf8) {
        const auto byte=static_cast<unsigned char>(character);
        if(byte=='"'||byte=='\\')std::cout<<'\\'<<static_cast<char>(byte);
        else if(byte<32)std::cout<<"\\u00"<<hex[byte>>4]<<hex[byte&15];
        else std::cout<<static_cast<char>(byte);
    }
    std::cout<<"\",\"automatic\":"<<(automatic ? "true" : "false")<<"}\n"<<std::flush;
}

void formatter() {
    const StorageFailure primary("Cannot flush world staging file.",StorageOperation::write,
        StorageStage::primary_staging,112,100,17,6);
    check(std::string(primary.what())=="Cannot flush world staging file.","Legacy exception message changed.");
    std::ostringstream text;text<<std::hex<<std::showbase;
    append_storage_diagnostic(text,primary);
    check(text.str()==" [storage operation=WriteFile stage=primary_staging win32_code=112 requested=100 written=17 close_win32_code=6]",
        "Formatter must preserve primary/secondary codes and decimal counts independently of stream flags.");
    const StorageFailure short_write("Cannot flush world staging file.",StorageOperation::short_write,
        StorageStage::unspecified,std::nullopt,100,0);
    std::ostringstream short_text;append_storage_diagnostic(short_text,short_write);
    check(short_text.str()==" [storage operation=short_write requested=100 written=0]",
        "Short write invented an OS error or a caller stage, or lost a zero count.");
    const StorageFailure zero("Cannot publish world snapshot.",StorageOperation::replace,
        StorageStage::primary_publish,0);
    std::ostringstream zero_text;append_storage_diagnostic(zero_text,zero);
    check(zero_text.str()==" [storage operation=MoveFileExW stage=primary_publish win32_code=0]",
        "An actually captured zero code must remain distinct from absent code.");
    const StorageFailure maximum("Cannot flush world staging file.",StorageOperation::write,
        StorageStage::previous_staging,std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::size_t>::max(),std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::uint32_t>::max());
    std::ostringstream bounded;append_storage_diagnostic(bounded,maximum);
    check(bounded.str().size()<256,"Scalar diagnostic must stay bounded.");
    for(unsigned char c:bounded.str())check(c>=32&&c<=126,"Diagnostic must contain only printable ASCII.");
}
#ifdef _WIN32
struct HeldFile {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit HeldFile(const fs::path& path) {
        value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        check(value!=INVALID_HANDLE_VALUE,"Cannot open owned deny-delete blocker.");
    }
    void close() {
        const auto owned=value;value=INVALID_HANDLE_VALUE;
        check(CloseHandle(owned)!=0,"Owned blocker close failed; do not retry unknown closure.");
    }
    ~HeldFile() {if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
void blocked_publication(const fs::path& directory,StorageStage stage) {
    fs::create_directory(directory);
    const auto source=directory/"source.pending",destination=directory/fs::path(u8"世界 scene.json");
    const std::string old="original complete bytes\n",next="new complete bytes\n";
    write_flushed(source,next);write_flushed(destination,old);
    HeldFile blocker(destination);
    // An independent direct WinAPI failure records the actual expected code;
    // no guessed code, hooked API, fault switch or external process is used.
    const auto replaced=MoveFileExW(source.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    const auto observed=replaced ? DWORD{0} : GetLastError();
    check(replaced==0,"Real deny-delete blocker unexpectedly allowed replacement.");
    bool rejected=false;
    try {replace_file(source,destination,stage);}
    catch(const StorageFailure& failure) {
        rejected=true;
        check(failure.operation==StorageOperation::replace&&failure.stage==stage,"Wrong real publication boundary.");
        check(failure.os_error&&*failure.os_error==observed&&!failure.close_error,"Captured actual MoveFileExW code differs.");
        check(std::string(failure.what())=="Cannot publish world snapshot.","Shared caller exception message changed.");
        SetLastError(ERROR_INVALID_DATA);
        check(*failure.os_error==observed,"Diagnostic depends on later thread LastError.");
        std::cout<<"{\"boundary\":\""<<storage_stage_name(stage)<<"\",\"actual_win32_code\":"<<observed<<"}\n";
    }
    check(rejected&&read(source)==next&&read(destination)==old,"Failed replacement changed complete source/primary bytes.");
    blocker.close();replace_file(source,destination,stage);
    check(!fs::exists(source)&&read(destination)==next,"Explicit post-release recovery did not publish exact bytes.");
}
void blocked_creation(const fs::path& root) {
    const auto directory=root/"directory-is-not-staging";fs::create_directory(directory);
    bool rejected=false;
    try {write_flushed(directory,"owned data",StorageStage::primary_staging);}
    catch(const StorageFailure& failure) {
        rejected=true;
        check(failure.operation==StorageOperation::create&&failure.stage==StorageStage::primary_staging
              &&failure.os_error&&!failure.requested&&!failure.written&&!failure.close_error,
              "Failed real CreateFileW lost operation/code or invented later-call observations.");
        check(std::string(failure.what())=="Cannot create world staging file.","Legacy create error changed.");
        std::cout<<"{\"boundary\":\"create_staging\",\"actual_win32_code\":"<<*failure.os_error<<"}\n";
    }
    check(rejected&&fs::is_directory(directory)&&fs::is_empty(directory),"Failed staging creation altered owned directory.");
}
#endif
}
int main(int argc,char** argv) {
    try {
        const auto root=owned_output(argc,argv);output_identity(root,argc==1);formatter();
#ifdef _WIN32
        blocked_creation(root);blocked_publication(root/"primary",StorageStage::primary_publish);
        blocked_publication(root/"previous",StorageStage::previous_publish);
        std::cout<<"{\"passed\":true,\"real_windows_failures\":3,\"flush_short_write_close_failure_forced\":false}\n";
#else
        write_flushed(root/"source","complete bytes\n");replace_file(root/"source",root/"destination");
        check(read(root/"destination")=="complete bytes\n","Unchanged ordinary Unix storage path failed.");
        std::cout<<"{\"passed\":true,\"real_windows_failures\":0,\"scope\":\"formatter and ordinary Unix path only\"}\n";
#endif
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
