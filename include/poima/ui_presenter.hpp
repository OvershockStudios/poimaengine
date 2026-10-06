// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/ui.hpp"
#include "poima/ui_model.hpp"
#include <memory>
namespace poima {
// Default nested vertical layout. All authoritative values arrive in an owned
// projection; generated RML and the embedded font are presentation resources.
class UiPresenter {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    UiPresenter();
    ~UiPresenter();
    UiPresenter(const UiPresenter&)=delete;
    UiPresenter& operator=(const UiPresenter&)=delete;
    std::shared_ptr<const UiFrame> frame(std::shared_ptr<const ui::Presentation>,std::uint32_t width,std::uint32_t height,float scale=1);
};
}
