// SPDX-License-Identifier: Apache-2.0
#include "poima/save_store.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <map>
#include <random>
#include <set>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace poima::saves {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
constexpr std::uint64_t max_generation=9007199254740991ULL;
constexpr std::size_t manifest_limit=64*1024;
constexpr const char* current_name="current.json";
constexpr const char* backup_name="previous.json";
void require(bool valid,ErrorKind kind,const std::string& message) { if(!valid)throw Error(kind,message); }
bool hex(const std::string& text,std::size_t size) { return text.size()==size && text.find_first_not_of("0123456789abcdef")==std::string::npos; }
std::string digest(const std::string& bytes) { return sha256(std::as_bytes(std::span(bytes.data(),bytes.size()))); }
std::string nonce() {
    std::random_device random;std::string result;result.reserve(32);
    for(unsigned i=0;i<4;++i) { const auto n=random();for(unsigned shift=0;shift<32;shift+=4)result.push_back("0123456789abcdef"[(n>>shift)&15]); }
    return result;
}
bool payload_name(const std::string& name) { return name.size()==38 && name.starts_with("p-") && name.ends_with(".bin") && hex(name.substr(2,32),32); }
bool temporary_name(const std::string& name) { return name.size()==39 && name.starts_with("m-") && name.ends_with(".json") && hex(name.substr(2,32),32); }
void path_guard(const fs::path& root,bool missing_leaf) {
    fs::path walk;
    for(const auto& part:root) {
        walk/=part;std::error_code error;const auto status=fs::symlink_status(walk,error);
        if(error==std::errc::no_such_file_or_directory && missing_leaf && walk==root)return;
        require(!error,ErrorKind::io,"Cannot inspect save directory ancestry.");
        require(fs::is_directory(status) && !fs::is_symlink(status),ErrorKind::invalid,"Save directory ancestry must contain only real directories.");
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(walk.c_str());
        require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),ErrorKind::invalid,"Save directory ancestry cannot contain reparse points.");
#endif
    }
}
struct Slot {
    fs::path root;
#ifdef _WIN32
    HANDLE directory=INVALID_HANDLE_VALUE,lock=INVALID_HANDLE_VALUE;
#else
    int directory=-1,lock=-1;
#endif
    explicit Slot(const fs::path& value,bool create):root(value) {
        path_guard(root,true);
        std::error_code error;const bool exists=fs::exists(root,error);require(!error,ErrorKind::io,"Cannot inspect save slot.");
        if(!exists) {
            require(create,ErrorKind::io,"Save slot does not exist.");
#ifdef _WIN32
            require(CreateDirectoryW(root.c_str(),nullptr)!=0 || GetLastError()==ERROR_ALREADY_EXISTS,ErrorKind::io,"Cannot create save slot directory.");
#else
            require(::mkdir(root.c_str(),0700)==0 || errno==EEXIST,ErrorKind::io,"Cannot create save slot directory.");
#endif
            path_guard(root,false);
#ifndef _WIN32
            const int parent=::open(root.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            require(parent>=0,ErrorKind::io,"Cannot open save slot parent for flush.");const int flushed=::fsync(parent);::close(parent);
            require(flushed==0,ErrorKind::io,"Cannot flush save slot parent directory.");
#endif
        }
        try {
#ifdef _WIN32
            directory=CreateFileW(root.c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            require(directory!=INVALID_HANDLE_VALUE,ErrorKind::io,"Cannot hold save slot directory.");
            lock=CreateFileW((root/".lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(lock==INVALID_HANDLE_VALUE)throw Error(GetLastError()==ERROR_SHARING_VIOLATION?ErrorKind::busy:ErrorKind::io,"Save slot is busy or its lock cannot be opened.");
            verify(lock,manifest_limit);
#else
            directory=::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);require(directory>=0,ErrorKind::io,"Cannot open save slot directory.");
            lock=::openat(directory,".lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);require(lock>=0,ErrorKind::io,"Cannot open save slot lock.");
            verify(lock,manifest_limit);
            if(::flock(lock,LOCK_EX|LOCK_NB)!=0)throw Error((errno==EWOULDBLOCK || errno==EAGAIN)?ErrorKind::busy:ErrorKind::io,"Save slot is busy or cannot be locked.");
#endif
        }catch(...) { close();throw; }
    }
    Slot(const Slot&)=delete;
    ~Slot() { close(); }
    void close() noexcept {
#ifdef _WIN32
        if(lock!=INVALID_HANDLE_VALUE)CloseHandle(std::exchange(lock,INVALID_HANDLE_VALUE));
        if(directory!=INVALID_HANDLE_VALUE)CloseHandle(std::exchange(directory,INVALID_HANDLE_VALUE));
#else
        if(lock>=0)::close(std::exchange(lock,-1));
        if(directory>=0)::close(std::exchange(directory,-1));
#endif
    }
#ifdef _WIN32
    static std::uint64_t verify(HANDLE handle,std::uint64_t maximum) {
        BY_HANDLE_FILE_INFORMATION info{};
        require(GetFileInformationByHandle(handle,&info)!=0,ErrorKind::io,"Cannot inspect save file handle.");
        require(!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) && info.nNumberOfLinks==1,
                ErrorKind::invalid,"Save files cannot be directories, reparse points or hard links.");
        const auto size=(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
        require(size<=maximum,ErrorKind::corrupt,"Save file exceeds its size limit.");return size;
    }
#else
    static std::uint64_t verify(int handle,std::uint64_t maximum) {
        struct stat status{};require(::fstat(handle,&status)==0,ErrorKind::io,"Cannot inspect save file handle.");
        require(S_ISREG(status.st_mode) && status.st_nlink==1,ErrorKind::invalid,"Save files must be regular, without hard-link aliases.");
        require(status.st_size>=0 && std::uint64_t(status.st_size)<=maximum,ErrorKind::corrupt,"Save file exceeds its size limit.");return static_cast<std::uint64_t>(status.st_size);
    }
#endif
    std::string read(const std::string& name,std::uint64_t maximum) const {
#ifdef _WIN32
        const HANDLE file=CreateFileW((root/name).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        require(file!=INVALID_HANDLE_VALUE,ErrorKind::io,"Cannot open save file: "+name);
        try {
            const auto size=verify(file,maximum);std::string bytes(static_cast<std::size_t>(size),'\0');std::size_t offset=0;
            while(offset<bytes.size()) { DWORD count=0;require(ReadFile(file,bytes.data()+offset,static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1U<<20)),&count,nullptr)!=0 && count>0,ErrorKind::io,"Cannot read save file.");offset+=count; }
            require(verify(file,maximum)==size,ErrorKind::io,"Save file changed while reading.");CloseHandle(file);return bytes;
        }catch(...) { CloseHandle(file);throw; }
#else
        const int file=::openat(directory,name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);require(file>=0,ErrorKind::io,"Cannot open save file: "+name);
        try {
            const auto size=verify(file,maximum);std::string bytes(static_cast<std::size_t>(size),'\0');std::size_t offset=0;
            while(offset<bytes.size()) { const auto count=::read(file,bytes.data()+offset,bytes.size()-offset);if(count<0 && errno==EINTR)continue;require(count>0,ErrorKind::io,"Cannot read save file.");offset+=static_cast<std::size_t>(count); }
            require(verify(file,maximum)==size,ErrorKind::io,"Save file changed while reading.");::close(file);return bytes;
        }catch(...) { ::close(file);throw; }
#endif
    }
    void create(const std::string& name,const std::string& bytes) const {
#ifdef _WIN32
        const HANDLE file=CreateFileW((root/name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(file!=INVALID_HANDLE_VALUE,ErrorKind::io,"Cannot exclusively create save file.");
        try {
            std::size_t offset=0;while(offset<bytes.size()) { DWORD count=0;require(WriteFile(file,bytes.data()+offset,static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1U<<20)),&count,nullptr)!=0 && count>0,ErrorKind::io,"Cannot write save file.");offset+=count; }
            require(FlushFileBuffers(file)!=0,ErrorKind::io,"Cannot flush save file.");CloseHandle(file);
        }catch(...) { CloseHandle(file);throw; }
#else
        const int file=::openat(directory,name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);require(file>=0,ErrorKind::io,"Cannot exclusively create save file.");
        try {
            std::size_t offset=0;while(offset<bytes.size()) { const auto count=::write(file,bytes.data()+offset,bytes.size()-offset);if(count<0 && errno==EINTR)continue;require(count>0,ErrorKind::io,"Cannot write save file.");offset+=static_cast<std::size_t>(count); }
            require(::fsync(file)==0,ErrorKind::io,"Cannot flush save file.");::close(file);
        }catch(...) { ::close(file);throw; }
#endif
    }
    void replace(const std::string& temporary,const std::string& target) const {
#ifdef _WIN32
        const auto destination=root/target;const auto source=root/temporary;
        if(GetFileAttributesW(destination.c_str())==INVALID_FILE_ATTRIBUTES)
            require(MoveFileExW(source.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)!=0,ErrorKind::io,"Cannot publish initial save manifest.");
        else require(ReplaceFileW(destination.c_str(),source.c_str(),nullptr,0,nullptr,nullptr)!=0,ErrorKind::io,"Cannot atomically replace save manifest.");
        // ReplaceFile's WRITE_THROUGH flag is unsupported. Flush the published
        // file explicitly; Windows directory/device power-loss remains unproven.
        const HANDLE file=CreateFileW(destination.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        require(file!=INVALID_HANDLE_VALUE,ErrorKind::io,"Cannot reopen published manifest for flush.");const auto flushed=FlushFileBuffers(file);CloseHandle(file);
        require(flushed!=0,ErrorKind::io,"Cannot flush published manifest.");
#else
        require(::renameat(directory,temporary.c_str(),directory,target.c_str())==0,ErrorKind::io,"Cannot atomically replace save manifest.");
#endif
    }
    void flush_directory() const {
#ifndef _WIN32
        require(::fsync(directory)==0,ErrorKind::io,"Cannot flush save directory.");
#endif
    }
    void remove(const std::string& name) const {
#ifdef _WIN32
        require(DeleteFileW((root/name).c_str())!=0,ErrorKind::io,"Cannot prune save file.");
#else
        require(::unlinkat(directory,name.c_str(),0)==0,ErrorKind::io,"Cannot prune save file.");
#endif
    }
    std::set<std::string> inventory() const {
        std::set<std::string> files;std::uint64_t total=0;
        for(const auto& entry:fs::directory_iterator(root)) {
            const auto raw=entry.path().filename().u8string();const std::string name(raw.begin(),raw.end());
            require(files.size()<32,ErrorKind::corrupt,"Save slot contains too many files.");
            require(name==".lock" || name==current_name || name==backup_name || payload_name(name) || temporary_name(name),ErrorKind::invalid,"Save slot contains an unrecognized filename.");
            const auto status=entry.symlink_status();require(fs::is_regular_file(status) && !fs::is_symlink(status),ErrorKind::invalid,"Save slot entries must be regular files, without links.");
            if(name!=".lock") {
#ifdef _WIN32
                const HANDLE handle=CreateFileW(entry.path().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                require(handle!=INVALID_HANDLE_VALUE,ErrorKind::io,"Cannot inspect save inventory.");
                try { total+=verify(handle,payload_name(name)?maximum_checkpoint_bytes:manifest_limit);CloseHandle(handle); }catch(...) { CloseHandle(handle);throw; }
#else
                const int handle=::openat(directory,name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);require(handle>=0,ErrorKind::invalid,"Cannot inspect save inventory without following links.");
                try { total+=verify(handle,payload_name(name)?maximum_checkpoint_bytes:manifest_limit);::close(handle); }catch(...) { ::close(handle);throw; }
#endif
            }
            require(total<=4*maximum_checkpoint_bytes+1024*1024,ErrorKind::corrupt,"Save slot exceeds its aggregate storage budget.");files.insert(name);
        }
        return files;
    }
};
void fields(const Json& value,std::initializer_list<const char*> names) {
    require(value.is_object() && value.size()==names.size(),ErrorKind::corrupt,"Invalid save manifest fields.");
    for(const auto* name:names)require(value.contains(name),ErrorKind::corrupt,"Missing save manifest field.");
}
std::uint64_t integer(const Json& value,std::uint64_t limit=max_generation) {
    require(value.is_number_integer() && value>=0 && value<=limit,ErrorKind::corrupt,"Invalid save manifest integer.");return value.get<std::uint64_t>();
}
std::string hash_field(const Json& value,std::size_t size=64) {
    require(value.is_string() && hex(value.get<std::string>(),size),ErrorKind::corrupt,"Invalid save manifest identifier/hash.");return value.get<std::string>();
}
Generation generation(const Json& value) {
    fields(value,{"generation","file","bytes","sha256"});Generation result;
    result.generation=integer(value.at("generation"));require(result.generation>0,ErrorKind::corrupt,"Invalid save payload generation.");
    result.bytes=integer(value.at("bytes"),maximum_checkpoint_bytes);result.sha256=hash_field(value.at("sha256"));
    require(value.at("file").is_string() && payload_name(value.at("file").get<std::string>()),ErrorKind::corrupt,"Invalid save payload filename.");return result;
}
Json parse_manifest(const std::string& bytes) {
    try {
        std::vector<std::set<std::string>> keys;
        auto document=Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& value) {
            require(depth<=12,ErrorKind::corrupt,"Save manifest nesting limit exceeded.");
            if(event==Json::parse_event_t::object_start)keys.emplace_back();
            if(event==Json::parse_event_t::key)require(keys.back().insert(value.get<std::string>()).second,ErrorKind::corrupt,"Duplicate save manifest field.");
            if(event==Json::parse_event_t::object_end)keys.pop_back();
            return true;
        });
        fields(document,{"format","version","payload","sha256"});require(document.at("format")=="poima.save-slot" && integer(document.at("version"))==1,ErrorKind::corrupt,"Unsupported save slot format/version.");
        const auto& data=document.at("payload");fields(data,{"generation","current","previous","quarantine","receipts"});
        require(hash_field(document.at("sha256"))==digest(data.dump()),ErrorKind::corrupt,"Save manifest checksum mismatch.");
        const auto current=generation(data.at("current"));require(integer(data.at("generation"))==current.generation,ErrorKind::corrupt,"Save manifest generation differs.");
        if(!data.at("previous").is_null()) {
            const auto previous=generation(data.at("previous"));require(previous.generation<current.generation && data.at("previous").at("file")!=data.at("current").at("file"),ErrorKind::corrupt,"Invalid prior save generation.");
        }
        if(!data.at("quarantine").is_null()) {
            const auto failed=generation(data.at("quarantine"));
            require(failed.generation<current.generation && data.at("quarantine").at("file")!=data.at("current").at("file") &&
                    (data.at("previous").is_null() || data.at("quarantine").at("file")!=data.at("previous").at("file")),
                    ErrorKind::corrupt,"Invalid quarantined save generation.");
        }
        const auto& receipts=data.at("receipts");require(receipts.is_array() && receipts.size()==std::min<std::uint64_t>(32,current.generation),ErrorKind::corrupt,"Invalid save receipt history.");
        std::set<std::string> ids;auto next=current.generation-receipts.size()+1;
        for(const auto& receipt:receipts) {
            fields(receipt,{"operation_id","request_sha256","expected_generation","acknowledge_recovery","generation","bytes","sha256"});
            require(ids.insert(hash_field(receipt.at("operation_id"),32)).second,ErrorKind::corrupt,"Duplicate save receipt operation.");
            (void)hash_field(receipt.at("request_sha256"));(void)hash_field(receipt.at("sha256"));(void)integer(receipt.at("bytes"),maximum_checkpoint_bytes);
            require(receipt.at("acknowledge_recovery").is_boolean() && integer(receipt.at("generation"))==next && integer(receipt.at("expected_generation"))==next-1,ErrorKind::corrupt,"Invalid save receipt chronology.");++next;
        }
        const auto& last=receipts.back();require(last.at("bytes")==current.bytes && last.at("sha256")==current.sha256,ErrorKind::corrupt,"Current save differs from its persisted receipt.");
        return document;
    }catch(const Error&) { throw; }catch(const std::exception&) { throw Error(ErrorKind::corrupt,"Malformed save manifest JSON."); }
}
std::string encode_manifest(const Json& data) {
    const auto bytes=Json{{"format","poima.save-slot"},{"version",1},{"payload",data},{"sha256",digest(data.dump())}}.dump();
    require(bytes.size()<=manifest_limit,ErrorKind::invalid,"Save manifest exceeds its size budget.");return bytes;
}
struct Loaded { Status status;Json document;std::string manifest,bytes;std::set<std::string> files; };
Loaded load(const Slot& slot) {
    Loaded result;result.status.exists=true;result.files=slot.inventory();
    auto attempt=[&](const char* name) { const auto bytes=slot.read(name,manifest_limit);auto document=parse_manifest(bytes);result.manifest=bytes;result.document=std::move(document); };
    bool primary=false;
    if(result.files.contains(current_name)) {
        try { attempt(current_name);primary=true; }catch(const Error& error) { result.status.recovery_reason=std::string("Current manifest is unreadable: ")+error.what(); }
    }else if(result.files.contains(backup_name))result.status.recovery_reason="Current manifest is missing.";
    if(!primary && result.files.contains(backup_name)) {
        attempt(backup_name);result.status.recovered=true;result.status.manifest_recovered=true;
        result.status.recovery_reason+=" Using previous committed manifest for read recovery only.";
    }
    if(result.document.is_null()) {
        require(!result.files.contains(current_name),ErrorKind::corrupt,"No verified committed save manifest is available.");
        for(const auto& name:result.files)if(payload_name(name) || temporary_name(name))++result.status.orphan_files;
        return result;
    }
    const auto& data=result.document.at("payload");result.status.generation=integer(data.at("generation"));
    auto verify=[&](const Json& description,Generation& record,std::string* out) {
        try { auto bytes=slot.read(description.at("file").get<std::string>(),maximum_checkpoint_bytes);
            require(bytes.size()==record.bytes && digest(bytes)==record.sha256,ErrorKind::corrupt,"Save payload checksum/size mismatch.");record.verified=true;if(out)*out=std::move(bytes);
        }catch(const Error&) { record.verified=false; }
    };
    result.status.current=generation(data.at("current"));verify(data.at("current"),*result.status.current,&result.bytes);
    if(!data.at("previous").is_null()) {
        result.status.previous=generation(data.at("previous"));verify(data.at("previous"),*result.status.previous,result.status.current->verified?nullptr:&result.bytes);
    }
    if(!data.at("quarantine").is_null()) {
        result.status.quarantined=generation(data.at("quarantine"));verify(data.at("quarantine"),*result.status.quarantined,nullptr);
    }
    if(result.status.current->verified)result.status.selected=result.status.current;
    else if(result.status.previous && result.status.previous->verified) {
        result.status.selected=result.status.previous;result.status.recovered=true;
        if(!result.status.recovery_reason.empty())result.status.recovery_reason+=' ';
        result.status.recovery_reason+="Newest payload is unreadable; using verified prior generation.";
    }else throw Error(ErrorKind::corrupt,"Neither retained save generation has a verified payload.");
    const auto current_file=data.at("current").at("file").get<std::string>();
    const auto previous_file=data.at("previous").is_null()?std::string{}:data.at("previous").at("file").get<std::string>();
    const auto quarantined_file=data.at("quarantine").is_null()?std::string{}:data.at("quarantine").at("file").get<std::string>();
    for(const auto& name:result.files)if(temporary_name(name) || (payload_name(name) && name!=current_file && name!=previous_file && name!=quarantined_file))++result.status.orphan_files;
    return result;
}
bool slot_exists(const fs::path& root) {
    path_guard(root,true);std::error_code error;const bool value=fs::exists(root,error);require(!error,ErrorKind::io,"Cannot inspect save slot.");return value;
}
const Json* receipt(const Loaded& state,const std::string& operation) {
    if(!state.document.is_null())for(const auto& item:state.document.at("payload").at("receipts"))if(item.at("operation_id")==operation)return &item;
    return nullptr;
}
WriteResult replay(const Loaded& state,const Json& item) {
    Generation committed{integer(item.at("generation")),integer(item.at("bytes"),maximum_checkpoint_bytes),hash_field(item.at("sha256")),false};
    for(const auto* record:{&state.status.current,&state.status.previous})if(*record && (*record)->generation==committed.generation)committed.verified=(*record)->verified;
    return {state.status,committed,true,state.status.orphan_files>0};
}
void prune(const Slot& slot,const std::set<std::string>& files,const std::set<std::string>& retained) {
    for(const auto& name:files)if((payload_name(name) || temporary_name(name)) && !retained.contains(name))slot.remove(name);
    slot.flush_directory();
}
}
Store::Store(fs::path root,FaultHook fault):root_(fs::absolute(std::move(root)).lexically_normal()),fault_(std::move(fault)) {
    require(root_.has_filename() && root_!=root_.root_path(),ErrorKind::invalid,"Save slot requires a final directory name.");
    const auto bytes=root_.u8string();require(std::find(bytes.begin(),bytes.end(),char8_t{})==bytes.end(),ErrorKind::invalid,"Save slot path contains NUL.");
}
Status Store::inspect() const { if(!slot_exists(root_))return {};Slot slot(root_,false);return load(slot).status; }
ReadResult Store::read() const {
    require(slot_exists(root_),ErrorKind::io,"Save slot does not exist.");Slot slot(root_,false);auto state=load(slot);
    require(state.status.selected.has_value(),ErrorKind::io,"Save slot has no committed generation.");return {std::move(state.status),std::move(state.bytes)};
}
std::optional<WriteResult> Store::lookup(const std::string& operation_id,const std::string& request_sha256) const {
    return observe_receipt(operation_id,request_sha256).receipt;
}
ReceiptObservation Store::observe_receipt(const std::string& operation_id,const std::string& request_sha256) const {
    require(hex(operation_id,32) && hex(request_sha256,64),ErrorKind::invalid,"Save operation/request identity is malformed.");
    if(!slot_exists(root_))return {};
    Slot slot(root_,false);auto state=load(slot);
    ReceiptObservation result;
    result.current_generation=state.status.generation;
    if(!state.document.is_null())result.oldest_retained_generation=integer(state.document.at("payload").at("receipts").front().at("generation"));
    if(const auto* item=receipt(state,operation_id)) {
        require(item->at("request_sha256")==request_sha256,ErrorKind::reused_id,"Save operation ID was reused with a different request.");
        result.receipt=replay(state,*item);return result;
    }
    require(!state.status.manifest_recovered,ErrorKind::recovery_required,"Manifest recovery cannot establish missing operation history; use a new slot/manual repair.");return result;
}
WriteResult Store::write(std::uint64_t expected_generation,const std::string& operation_id,const std::string& bytes,bool acknowledge_recovery,const std::string& request_sha256) const {
    require(expected_generation<max_generation && hex(operation_id,32) && bytes.size()<=maximum_checkpoint_bytes,ErrorKind::invalid,"Invalid save generation, operation ID or checkpoint size.");
    const auto hash=digest(bytes),request=request_sha256.empty()?hash:request_sha256;
    require(hex(request,64),ErrorKind::invalid,"Save request SHA-256 is malformed.");Slot slot(root_,true);auto state=load(slot);
    if(const auto* item=receipt(state,operation_id)) {
        require(item->at("request_sha256")==request && item->at("expected_generation")==expected_generation && item->at("acknowledge_recovery")==acknowledge_recovery && item->at("bytes")==bytes.size() && item->at("sha256")==hash,
                ErrorKind::reused_id,"Save operation ID was reused with different request/payload data.");return replay(state,*item);
    }
    require(!state.status.manifest_recovered,ErrorKind::recovery_required,"Recovered manifest is read-only; use a new slot/manual repair to preserve unknown receipt history.");
    require(expected_generation==state.status.generation,ErrorKind::conflict,"Save generation changed; inspect before writing.");
    require(!state.status.recovered || acknowledge_recovery,ErrorKind::recovery_required,"Newest payload is corrupt; explicitly acknowledge using the verified prior generation.");
    require(!state.status.recovered || !state.status.quarantined,ErrorKind::recovery_required,"A failed generation is already quarantined; use a new slot/manual archive before another recovery write.");
    std::set<std::string> retained;Json previous=nullptr,quarantine=nullptr;
    if(state.status.selected) {
        const auto& old=state.document.at("payload");previous=state.status.current->verified?old.at("current"):old.at("previous");
        retained.insert(previous.at("file").get<std::string>());
        // Preserve both currently referenced files until the new manifest commits.
        retained.insert(old.at("current").at("file").get<std::string>());
        if(!old.at("previous").is_null())retained.insert(old.at("previous").at("file").get<std::string>());
        quarantine=state.status.recovered?old.at("current"):old.at("quarantine");
        if(!quarantine.is_null())retained.insert(quarantine.at("file").get<std::string>());
    }
    prune(slot,state.files,retained);
    auto boundary=[&](Boundary point) { if(fault_)fault_(point); };
    const auto payload="p-"+nonce()+".bin";slot.create(payload,bytes);boundary(Boundary::payload_flushed);
    require(digest(slot.read(payload,maximum_checkpoint_bytes))==hash,ErrorKind::io,"New save payload failed read-back verification.");
    slot.flush_directory();boundary(Boundary::payload_directory_flushed);
    if(!state.document.is_null()) {
        const auto pending="m-"+nonce()+".json";slot.create(pending,state.manifest);boundary(Boundary::backup_flushed);
        require(slot.read(pending,manifest_limit)==state.manifest,ErrorKind::io,"Staged backup manifest failed read-back verification.");
        slot.replace(pending,backup_name);boundary(Boundary::backup_replaced);slot.flush_directory();boundary(Boundary::backup_directory_flushed);
    }
    Json receipts=state.document.is_null()?Json::array():state.document.at("payload").at("receipts");if(receipts.size()==32)receipts.erase(receipts.begin());
    const auto number=expected_generation+1;
    receipts.push_back({{"operation_id",operation_id},{"request_sha256",request},{"expected_generation",expected_generation},{"acknowledge_recovery",acknowledge_recovery},{"generation",number},{"bytes",bytes.size()},{"sha256",hash}});
    Json data={{"generation",number},{"current",{{"generation",number},{"file",payload},{"bytes",bytes.size()},{"sha256",hash}}},{"previous",previous},{"quarantine",quarantine},{"receipts",std::move(receipts)}};
    const auto encoded=encode_manifest(data);(void)parse_manifest(encoded);
    const auto pending="m-"+nonce()+".json";slot.create(pending,encoded);boundary(Boundary::manifest_flushed);
    require(slot.read(pending,manifest_limit)==encoded,ErrorKind::io,"Staged save manifest failed read-back verification.");
    slot.replace(pending,current_name);boundary(Boundary::manifest_replaced);slot.flush_directory();boundary(Boundary::manifest_directory_flushed);
    bool cleanup_pending=false;retained={payload};if(!previous.is_null())retained.insert(previous.at("file").get<std::string>());
    if(!quarantine.is_null())retained.insert(quarantine.at("file").get<std::string>());
    try { prune(slot,slot.inventory(),retained); }catch(const Error&) { cleanup_pending=true; }
    boundary(Boundary::pruned);
    auto committed=load(slot);const bool verified=committed.status.current && committed.status.current->generation==number && committed.status.current->verified;
    return {std::move(committed.status),Generation{number,static_cast<std::uint64_t>(bytes.size()),hash,verified},false,cleanup_pending};
}
}
