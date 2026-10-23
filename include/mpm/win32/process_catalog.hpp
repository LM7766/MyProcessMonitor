#pragma once

#include "mpm/core/interfaces.hpp"

#include <memory>

namespace mpm {

class Win32ProcessCatalog final : public IProcessCatalog {
public:
    Win32ProcessCatalog();
    ~Win32ProcessCatalog() override;

    Win32ProcessCatalog(const Win32ProcessCatalog&) = delete;
    Win32ProcessCatalog& operator=(const Win32ProcessCatalog&) = delete;

    ProcessInfo Describe(const ProcessKey& key) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mpm