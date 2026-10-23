#pragma once

#include "mpm/core/interfaces.hpp"

namespace mpm {

class Win32IdleProvider final : public IIdleStateProvider {
public:
    ActivityState Query() override;
};

} // namespace mpm