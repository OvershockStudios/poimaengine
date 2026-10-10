// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/frame_performance.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
namespace poima::frame_performance {
struct ServiceError : std::runtime_error {
    int code;
    ServiceError(int value,const std::string& message):std::runtime_error(message),code(value) {}
};
class Service {
    Recorder recorder_;
    std::string capture_,expected_,player_;
    std::set<std::string> used_;
    std::uint32_t capacity_=0;
    std::uint64_t drain_ns_=0;
    bool drain_failed_=false;
    nlohmann::json status() const;
public:
    Recorder& recorder() noexcept { return recorder_; }
    nlohmann::json dispatch(const std::string&,const nlohmann::json&,
        const std::string& active_player={},bool ready=false,const std::function<void()>& drain={});
    static nlohmann::json schemas();
};
}
