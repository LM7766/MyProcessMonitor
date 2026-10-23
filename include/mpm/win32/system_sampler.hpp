#pragma once

#include "mpm/core/interfaces.hpp"

#include <memory>

namespace mpm {

class Win32SystemSampler final : public ISystemSampler {
public:
    Win32SystemSampler();
    ~Win32SystemSampler() override;

    Win32SystemSampler(const Win32SystemSampler&) = delete;
    Win32SystemSampler& operator=(const Win32SystemSampler&) = delete;

    std::optional<SystemSample> Capture() override;
    const wchar_t* Name() const noexcept override;
    const std::wstring& LastError() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mpm