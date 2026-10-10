// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/development_jobs.hpp"
#include "poima/development_profiles.hpp"
#include <nlohmann/json.hpp>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
namespace poima::development {
struct ServiceError : std::runtime_error {
    int code;
    ServiceError(int value,const std::string& message):std::runtime_error(message),code(value) {}
};
// Owner-thread authoring adapter. Receipts are session-local; compiled artifacts
// do not replace gameplay until an explicit guarded load succeeds.
class Service {
    struct Receipt { nlohmann::json params;JobId job=0; };
    std::unique_ptr<Jobs> jobs_;
    std::map<std::string,Receipt> receipts_;
    std::deque<std::string> receipt_order_;
    std::set<std::string> used_requests_;
    bool configured_=false;
    std::map<std::string,Profile> profiles_;
    std::vector<std::filesystem::path> output_reservations_;
public:
    void configure(std::vector<Profile> profiles); // Once, before any accepted job.
    nlohmann::json dispatch(const std::string& method,const nlohmann::json& params);
    static nlohmann::json schemas();
};
}
