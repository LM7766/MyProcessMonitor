#pragma once

#include "mpm/core/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpm {

class ISystemSampler {
public:
    virtual ~ISystemSampler() = default;
    virtual std::optional<SystemSample> Capture() = 0;
    virtual const wchar_t* Name() const noexcept = 0;
    virtual const std::wstring& LastError() const noexcept = 0;
};

class IIdleStateProvider {
public:
    virtual ~IIdleStateProvider() = default;
    virtual ActivityState Query() = 0;
};

class IProcessCatalog {
public:
    virtual ~IProcessCatalog() = default;
    virtual ProcessInfo Describe(const ProcessKey& key) = 0;
};

class IEventStore {
public:
    virtual ~IEventStore() = default;
    virtual bool Upsert(const DetectionEvent& event) = 0;
    virtual std::vector<DetectionEvent> Recent(int limit) = 0;
    virtual bool PruneBefore(std::uint64_t unixTimeMs) = 0;
    virtual const std::wstring& LastError() const noexcept = 0;
};

} // namespace mpm