// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay_save_writer.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace poima {
namespace {
std::string digest(const std::string& text) {
    return sha256(std::as_bytes(std::span(text.data(),text.size())));
}
std::string epoch_text(GameplaySaveEpoch epoch) {
    constexpr char hex[]="0123456789abcdef";
    std::string text(32,'0');
    for(unsigned index=0;index<16;++index) {
        const auto shift=(15-index)*4;
        text[index]=hex[(epoch.high>>shift)&15];
        text[index+16]=hex[(epoch.low>>shift)&15];
    }
    return text;
}
std::int32_t error_code(const std::exception& error) noexcept {
    const auto* storage=dynamic_cast<const saves::Error*>(&error);
    return storage && storage->kind==saves::ErrorKind::conflict ? -32009 : -32070;
}
void validate(const GameplaySaveWriteInput& input) {
    GameplaySaveLedger validation;
    GameplaySaveCompletion failure;
    failure.error_code=-32070;
    if(input.request.kind!=GameplaySaveKind::save ||
        validation.complete(input.request,failure)!=GameplaySavePublication::recorded)
        throw std::invalid_argument("Background save requires a valid committed save request.");
    if(input.bytes.size()>saves::maximum_checkpoint_bytes)
        throw std::invalid_argument("Background save checkpoint exceeds 64 MiB.");
    if(input.content_hash.size()!=64 || input.content_hash.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw std::invalid_argument("Background save content identity must be a SHA256 hash.");
    const auto path=input.path.u8string();
    if(!input.path.is_absolute() || !input.path.has_filename() || input.path==input.path.root_path() ||
        std::find(path.begin(),path.end(),char8_t{})!=path.end())
        throw std::invalid_argument("Background save requires an absolute slot directory without NUL.");
}
}

struct GameplaySaveWriter::Pending {
    const GameplaySaveWriteInput input;
    GameplaySaveWriteResult result;
    saves::FaultHook fault;
    std::int32_t write_error=-32070;
    std::array<char,256> write_diagnostic{};
    explicit Pending(GameplaySaveWriteInput value,saves::FaultHook hook)
        :input(std::move(value)),fault(std::move(hook)) {
        result.request=input.request;
        result.path=input.path;
        gameplay_save_diagnostic(write_diagnostic,"Save write failed before publication.");
    }
    void fail(std::int32_t code,std::string_view diagnostic) noexcept {
        result.completion={};result.completion.error_code=code;
        gameplay_save_diagnostic(result.completion.diagnostic,diagnostic);
        result.state=GameplaySaveState::failed;
    }
    void resolving(std::string_view diagnostic) noexcept {
        result.completion={};result.completion.error_code=-32070;
        gameplay_save_diagnostic(result.completion.diagnostic,diagnostic);
        result.state=GameplaySaveState::resolving;
    }
    void succeed(std::uint64_t generation) noexcept {
        result.completion={};result.completion.succeeded=true;
        result.completion.generation=generation;result.state=GameplaySaveState::succeeded;
    }
    void resolve() noexcept {
        try {
            const auto observed=saves::Store(input.path).observe_receipt(result.operation,result.hash);
            if(observed.receipt) succeed(observed.receipt->committed.generation);
            else if(observed.current_generation<=result.expected_generation ||
                (observed.oldest_retained_generation>0 &&
                 result.expected_generation+1>=observed.oldest_retained_generation)) {
                fail(write_error,std::string_view(write_diagnostic.data()));
            } else resolving("Save receipt is outside retained history; publication cannot be determined.");
        } catch(const std::exception& error) { resolving(error.what()); }
        catch(...) { resolving("Save receipt verification failed; operation retained."); }
    }
    void write() noexcept {
        result.started=true;
        bool entered_write=false;
        try {
            saves::Store store(input.path,fault);
            const auto observed=store.inspect();
            result.expected_generation=input.request.has_expected_generation ?
                input.request.expected_generation : observed.generation;
            result.generation_chosen=true;
            const auto identity=epoch_text(input.request.ticket.epoch)+":"+std::to_string(input.request.ticket.sequence);
            result.operation=digest(identity).substr(0,32);
            result.hash=digest(nlohmann::json{{"ticket",identity},{"slot",std::string(input.request.slot.data())},
                {"generation",result.expected_generation},{"tick",input.request.committed_tick},
                {"content",input.content_hash}}.dump());
            // Retain the exact identity before entering irreversible storage.
            // Jobs cancellation is not a durable-publication observation.
            entered_write=true;
            const auto written=store.write(result.expected_generation,result.operation,input.bytes,false,result.hash);
            succeed(written.committed.generation);
        } catch(const std::exception& error) {
            if(!entered_write) { fail(error_code(error),error.what());return; }
            write_error=error_code(error);
            gameplay_save_diagnostic(write_diagnostic,error.what());
            resolve();
        } catch(...) {
            if(!entered_write) { fail(-32070,"Gameplay save operation failed before write entry.");return; }
            write_error=-32070;
            gameplay_save_diagnostic(write_diagnostic,"Gameplay save operation failed.");
            resolve();
        }
    }
};

GameplaySaveWriter::GameplaySaveWriter(std::shared_ptr<jobs::Executor> executor,saves::FaultHook fault)
    :executor_(std::move(executor)),fault_(std::move(fault)),owner_(std::this_thread::get_id()) {
    if(!executor_)throw std::invalid_argument("Background save writer requires an owner executor.");
}
GameplaySaveWriter::~GameplaySaveWriter() noexcept {
    try {
        require_owner();
        if(group_) (void)executor_->wait(group_);
    } catch(...) { std::terminate(); }
}
void GameplaySaveWriter::require_owner() const {
    if(std::this_thread::get_id()!=owner_)
        throw std::logic_error("Background save writer requires its creating owner thread.");
}
void GameplaySaveWriter::submit(GameplaySaveWriteInput input) {
    require_owner();
    if(pending_)throw std::logic_error("A background save operation is already retained.");
    input.path=input.path.lexically_normal();
    validate(input);
    auto candidate=std::make_shared<Pending>(std::move(input),fault_);
    std::vector<jobs::Task> tasks;
    tasks.push_back({[candidate](jobs::Context&) { candidate->write(); },{},"save.write.durable"});
    auto admitted=executor_->submit(std::move(tasks),jobs::Lane::background,candidate->input.attribution);
    pending_=std::move(candidate);group_=std::move(admitted);ready_=false;
}
void GameplaySaveWriter::submit_resolution() {
    require_owner();
    if(!poll() || pending_->result.state!=GameplaySaveState::resolving)
        throw std::logic_error("Background save receipt is not awaiting resolution.");
    auto candidate=pending_;
    std::vector<jobs::Task> tasks;
    tasks.push_back({[candidate](jobs::Context&) { candidate->resolve(); },{},"save.resolve.receipt"});
    auto admitted=executor_->submit(std::move(tasks),jobs::Lane::background,candidate->input.attribution);
    group_=std::move(admitted);ready_=false;
}
bool GameplaySaveWriter::busy() const { require_owner();return bool(pending_); }
const GameplaySaveWriteResult* GameplaySaveWriter::poll() {
    require_owner();
    if(!pending_ || (!ready_ && !group_.poll().terminal()))return nullptr;
    if(!ready_) {
        if(!pending_->result.started)
            pending_->fail(-32070,"Background save task was cancelled before it started; no write occurred.");
        ready_=true;
    }
    return &pending_->result;
}
void GameplaySaveWriter::clear() {
    require_owner();
    const auto* result=poll();
    if(!result || (result->state!=GameplaySaveState::succeeded && result->state!=GameplaySaveState::failed))
        throw std::logic_error("Background save requires a definitive terminal result before release.");
    group_={};pending_.reset();ready_=false;
}
}
