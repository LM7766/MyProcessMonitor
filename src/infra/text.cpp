#include "mpm/infra/text.hpp"

#include <Windows.h>

#include <array>
#include <ctime>
#include <stdexcept>

namespace mpm {

std::string Utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

std::wstring Utf16(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        size);
    return result;
}

std::string LocalTimeString(std::uint64_t unixTimeMs) {
    const std::time_t seconds = static_cast<std::time_t>(unixTimeMs / 1000);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) {
        return {};
    }
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S", &local) == 0) {
        return {};
    }
    return buffer.data();
}

} // namespace mpm