// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/profiler.hpp"
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
namespace poima::profiling {
struct ServiceError : std::runtime_error {
    int code;
    ServiceError(int value,const std::string& message):std::runtime_error(message),code(value) {}
};
class Service {
    Recorder recorder_;
    std::string capture_,expected_;
    std::set<std::string> used_;
    std::uint32_t capacity_=0;
    nlohmann::json status() const;
public:
    Recorder& recorder() noexcept { return recorder_; }
    nlohmann::json dispatch(const std::string&,const nlohmann::json&);
    static nlohmann::json schemas();
};
}
