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
// These are policy/formatting tests, not fabricated WinAPI failure evidence.
void retry_policy_and_formatting() {
    for (const auto stage : {StorageStage::unspecified, StorageStage::primary_staging,
             StorageStage::previous_staging, StorageStage::previous_publish, StorageStage::primary_publish}) {
        for (const auto code : {0u, 2u, 3u, 5u, 6u, 32u, 33u, 112u, std::numeric_limits<std::uint32_t>::max()}) {
            const bool expected = (stage == StorageStage::previous_publish || stage == StorageStage::primary_publish) &&
                (code == 5 || code == 32 || code == 33);
            check(world_publication_retryable(stage, code) == expected, "Retry policy widened beyond explicit publication refusals.");
        }
    }
    const std::array<std::uint32_t, 5> waits{2, 4, 8, 16, 32};
    std::uint32_t total = 0;
    for (std::uint32_t attempt = 1; attempt <= waits.size(); ++attempt) {
        check(world_publication_wait_ms(attempt) == waits[attempt - 1], "Scheduled backoff differs."); total += waits[attempt - 1];
    }
    check(total == 62 && world_publication_wait_ms(0) == 0 && world_publication_wait_ms(6) == 0 &&
        world_publication_wait_ms(std::numeric_limits<std::uint32_t>::max()) == 0, "Attempt/wait bound or shift safety differs.");
    check(world_publication_wait_ms(1, 97999) == 2 && world_publication_wait_ms(1, 98000) == 0 &&
        world_publication_wait_ms(1, 100000) == 0 &&
        world_publication_wait_ms(1, std::numeric_limits<std::uint64_t>::max()) == 0,
        "Deadline eligibility overflowed or allowed a wait at/beyond100ms.");
    StorageRetryFacts facts; facts.attempts = 2; facts.first_error = 5; facts.last_error = 32;
    facts.scheduled_wait_ms = 2; facts.elapsed_us = 2500;
    const StorageFailure exhausted("Cannot publish world snapshot.", StorageOperation::replace,
        StorageStage::previous_publish, 32, {}, {}, {}, facts);
    std::ostringstream text; text << std::hex << std::showbase; append_storage_diagnostic(text, exhausted);
    check(text.str() == " [storage operation=MoveFileExW stage=previous_publish win32_code=32 retry_attempts=2 first_win32_code=5 last_win32_code=32 scheduled_wait_ms=2 retry_elapsed_us=2500]",
        "Bounded retry facts changed codes/units or decimal formatting.");
    std::ostringstream recovered; report_world_publication_recovery(recovered, StorageStage::primary_publish, facts);
    check(recovered.str() == "World publication recovered: [storage operation=MoveFileExW stage=primary_publish retry_attempts=2 first_win32_code=5 last_win32_code=32 scheduled_wait_ms=2 retry_elapsed_us=2500]\n",
        "Recovery must emit exactly one line without an invented successful-call error code.");
    StorageRetryFacts pending; pending.first_error = 5; pending.last_error = 5;
    std::ostringstream pending_text; report_world_publication_retry_pending(pending_text, StorageStage::previous_publish, pending);
    check(pending_text.str() == "World publication retry pending: [storage operation=MoveFileExW stage=previous_publish win32_code=5 retry_attempts=1]\n",
        "First real refusal event grammar differs.");
    std::ostringstream no_event;
    report_world_publication_retry_pending(no_event, StorageStage::previous_publish, facts);
    pending.last_error = 2; report_world_publication_retry_pending(no_event, StorageStage::primary_publish, pending);
    report_world_publication_retry_pending(no_event, StorageStage::primary_staging, pending);
    check(no_event.str().empty(), "Subsequent/noneligible/nonpublication failure emitted a pending event.");
    std::ostringstream ordinary; report_world_publication_recovery(ordinary, StorageStage::primary_publish, {});
    check(ordinary.str().empty(), "An ordinary one-attempt success emitted diagnostics.");
    struct ThrowingBuffer : std::streambuf {
        int_type overflow(int_type) override { throw std::runtime_error("Owned output failure."); }
    } buffer;
    std::ostream failed(&buffer); failed.exceptions(std::ios::badbit | std::ios::failbit);
    report_world_publication_recovery(failed, StorageStage::primary_publish, facts);
    pending.last_error = 5; report_world_publication_retry_pending(failed, StorageStage::primary_publish, pending);
    // Reaching here proves diagnostic I/O cannot invalidate an already moved file.
}
void bounded_retry_observations(const fs::path& root) {
    const auto directory = root / "retry-observations"; fs::create_directory(directory);
    const auto primary = directory / "world.json", source = directory / "world.pending";
    const std::string original("A\0B", 3), prepared("C\0D", 3);
    write_flushed(primary, original); write_flushed(source, prepared);
    check(world_publication_unchanged(primary, true, original, source, prepared), "Exact bounded binary guard rejected original inputs.");
    check(!world_publication_unchanged(primary, false, original, source, prepared) &&
        !world_publication_unchanged(primary, true, "different", source, prepared), "Primary existence/bytes guard was lost.");
    write_flushed(source, "C"); check(!world_publication_unchanged(primary, true, original, source, prepared), "Truncated prepared bytes accepted.");
    write_flushed(source, prepared + "x"); check(!world_publication_unchanged(primary, true, original, source, prepared), "Grown prepared bytes accepted.");
    write_flushed(source, ""); check(world_publication_bytes_equal(source, {}), "Empty bounded comparison failed.");
    fs::remove(source); check(!world_publication_unchanged(primary, true, original, source, prepared), "Missing prepared source accepted.");
    write_flushed(source, prepared); unsigned guards = 0;
    const auto facts = replace_world_publication(source, primary, StorageStage::primary_publish, [&] {
        ++guards; throw std::runtime_error("One-attempt success must not read retry guards.");
    });
    check(facts.attempts == 1 && !facts.first_error && !facts.last_error && facts.scheduled_wait_ms == 0 &&
        facts.elapsed_us == 0 && guards == 0 && !fs::exists(source) && read(primary) == prepared,
        "Ordinary replacement acquired retry work, altered bytes or consulted an extra guard.");
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
struct RetryWitness {
    HeldFile* held = nullptr;
    bool release = false, failed = false;
    fs::path edit;
    std::string edit_bytes;
    std::array<std::uint32_t, world_publication_max_attempts> codes{};
    std::uint32_t count = 0;
    StorageStage stage = StorageStage::unspecified;
    static void observe(void* context, StorageStage stage, const StorageRetryFacts& facts) noexcept {
        auto& witness = *static_cast<RetryWitness*>(context);
        try {
            check(stage == witness.stage && facts.last_error.has_value() &&
                facts.attempts == witness.count + 1 && witness.count < witness.codes.size(), "Actual failure observer facts differ.");
            witness.codes[witness.count++] = *facts.last_error;
            if (witness.count == 1 && witness.release) {
                if (!witness.edit.empty()) write_flushed(witness.edit, witness.edit_bytes);
                witness.held->close();
            }
        } catch (...) { witness.failed = true; }
    }
};
struct GuardConflict : std::runtime_error {
    int code = -32009;
    GuardConflict() : std::runtime_error("World file changed outside this session; reopen before editing.") {}
};
void retry_publication(const fs::path& directory, StorageStage stage, bool release) {
    fs::create_directory(directory);
    const auto primary = directory / fs::path(u8"世界 scene.json");
    const auto source = directory / "prepared.pending";
    const auto destination = stage == StorageStage::primary_publish ? primary : directory / "world.previous";
    const std::string original = "complete original primary\n", previous = "complete older backup\n";
    const std::string prepared = stage == StorageStage::primary_publish ? "complete next primary\n" : original;
    write_flushed(primary, original); write_flushed(source, prepared);
    if (destination != primary) write_flushed(destination, previous);
    HeldFile blocker(destination);
    const auto direct = MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    const auto oracle = direct ? DWORD{0} : GetLastError();
    check(direct == 0 && world_publication_retryable(stage, oracle), "Owned direct oracle did not produce an eligible actual refusal.");
    RetryWitness witness; witness.held = &blocker; witness.release = release; witness.stage = stage;
    const StorageRetryObserver observer{&witness, RetryWitness::observe};
    unsigned guards = 0; bool exhausted = false;
    try {
        const auto facts = replace_world_publication(source, destination, stage, [&] {
            ++guards;
            if (!world_publication_unchanged(primary, true, original, source, prepared)) throw GuardConflict();
        }, &observer);
        check(release && !witness.failed && witness.count >= 1 && facts.attempts == witness.count + 1 &&
            facts.attempts <= world_publication_max_attempts && facts.first_error == oracle &&
            facts.last_error == witness.codes[witness.count - 1] && facts.scheduled_wait_ms <= 62 && guards >= 1,
            "Real retry recovery lost actual failure, attempt/wait facts or guard.");
        check(!fs::exists(source) && read(destination) == prepared &&
            (destination == primary || read(primary) == original), "Recovered rename changed unrelated primary or prepared bytes.");
        std::cout << "{\"case\":\"released_after_observed_failure\",\"stage\":\"" << storage_stage_name(stage)
            << "\",\"failed_attempts\":" << witness.count << ",\"attempts\":" << facts.attempts
            << ",\"first_actual_win32_code\":" << *facts.first_error << ",\"scheduled_wait_ms\":" << facts.scheduled_wait_ms << "}\n";
    } catch (const StorageFailure& failure) {
        exhausted = true;
        check(!release && !witness.failed && failure.retry.has_value() && failure.operation == StorageOperation::replace &&
            failure.stage == stage && std::string(failure.what()) == "Cannot publish world snapshot.", "Persistent refusal did not preserve original failure scope.");
        const auto& facts = *failure.retry;
        check(witness.count == facts.attempts && facts.attempts >= 1 && facts.attempts <= world_publication_max_attempts &&
            facts.scheduled_wait_ms <= 62 && facts.first_error == oracle && facts.last_error == witness.codes[witness.count - 1] &&
            failure.os_error == facts.last_error, "Exhaustion lost bounded actual attempts/error codes.");
        check(read(source) == prepared && read(primary) == original &&
            (destination == primary || read(destination) == previous), "Persistent failure published or altered complete bytes.");
        std::cout << "{\"case\":\"persistent_refusal\",\"stage\":\"" << storage_stage_name(stage)
            << "\",\"attempts\":" << facts.attempts << ",\"first_actual_win32_code\":" << *facts.first_error
            << ",\"last_actual_win32_code\":" << *facts.last_error << ",\"scheduled_wait_ms\":" << facts.scheduled_wait_ms << "}\n";
    }
    check(exhausted != release && !witness.failed, "Real release/exhaustion outcome differs.");
}
void retry_conflict(const fs::path& directory, bool edit_primary) {
    fs::create_directory(directory);
    const auto primary = directory / "world.json", source = directory / "world.pending";
    const auto destination = edit_primary ? directory / "world.previous" : primary;
    const auto stage = edit_primary ? StorageStage::previous_publish : StorageStage::primary_publish;
    const std::string original = "original primary\n", prepared = "prepared exact bytes\n", previous = "original backup\n", external = "external changed bytes\n";
    write_flushed(primary, original); write_flushed(source, prepared);
    if (destination != primary) write_flushed(destination, previous);
    HeldFile blocker(destination);
    RetryWitness witness; witness.held = &blocker; witness.release = true; witness.stage = stage;
    witness.edit = edit_primary ? primary : source; witness.edit_bytes = external;
    const StorageRetryObserver observer{&witness, RetryWitness::observe};
    bool rejected = false;
    try {
        (void)replace_world_publication(source, destination, stage, [&] {
            if (!world_publication_unchanged(primary, true, original, source, prepared)) throw GuardConflict();
        }, &observer);
    } catch (const GuardConflict& conflict) { rejected = true; check(conflict.code == -32009, "Conflict code changed."); }
    check(rejected && !witness.failed && witness.count == 1 &&
        read(primary) == (edit_primary ? external : original) &&
        read(source) == (edit_primary ? prepared : external) &&
        (destination == primary || read(destination) == previous), "Retry overwrote external bytes or renamed after conflict.");
    std::cout << "{\"case\":\"retry_guard_conflict\",\"changed\":\"" << (edit_primary ? "primary" : "prepared") << "\",\"code\":-32009}\n";
}

#endif
}
int main(int argc,char** argv) {
    try {
        const auto root=owned_output(argc,argv);output_identity(root,argc==1);formatter();retry_policy_and_formatting();bounded_retry_observations(root);
#ifdef _WIN32
        blocked_creation(root);blocked_publication(root/"primary",StorageStage::primary_publish);
        blocked_publication(root/"previous",StorageStage::previous_publish);
        retry_publication(root/"retry-primary-released",StorageStage::primary_publish,true);
        retry_publication(root/"retry-previous-released",StorageStage::previous_publish,true);
        retry_publication(root/"retry-primary-persistent",StorageStage::primary_publish,false);
        retry_publication(root/"retry-previous-persistent",StorageStage::previous_publish,false);
        retry_conflict(root/"retry-primary-conflict",true);retry_conflict(root/"retry-prepared-conflict",false);
        std::cout<<"{\"passed\":true,\"legacy_real_windows_failure_cases\":3,\"real_retry_cases\":6,\"synthetic_policy_and_formatter_separate\":true,\"flush_short_write_close_failure_forced\":false}\n";
#else
        write_flushed(root/"source","complete bytes\n");replace_file(root/"source",root/"destination");
        check(read(root/"destination")=="complete bytes\n","Unchanged ordinary Unix storage path failed.");
        std::cout<<"{\"passed\":true,\"real_windows_failures\":0,\"scope\":\"formatter and ordinary Unix path only\"}\n";
#endif
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
