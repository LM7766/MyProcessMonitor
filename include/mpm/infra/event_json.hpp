#pragma once

#include "mpm/core/types.hpp"

#include <nlohmann/json.hpp>

namespace mpm {

nlohmann::json ToJson(const AttributionGroup& group);
nlohmann::json ToJson(const DetectionEvent& event);
DetectionEvent EventFromJson(const nlohmann::json& value);

} // namespace mpm