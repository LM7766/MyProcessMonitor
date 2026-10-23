#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mpm {

std::string Utf8(std::wstring_view text);
std::wstring Utf16(std::string_view text);
std::string LocalTimeString(std::uint64_t unixTimeMs);

} // namespace mpm