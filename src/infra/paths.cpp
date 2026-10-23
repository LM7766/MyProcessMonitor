#include "mpm/infra/paths.hpp"

#include "mpm/infra/text.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <objbase.h>

#include <system_error>

namespace mpm {
namespace {

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw != nullptr) {
        std::filesystem::path result(raw);
        CoTaskMemFree(raw);
        return result;
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }

    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(buffer);
    }
    return std::filesystem::temp_directory_path();
}

} // namespace

std::filesystem::path LocalAppDataDirectory() {
    return KnownFolder(FOLDERID_LocalAppData);
}

std::filesystem::path AppDataDirectory() {
    return LocalAppDataDirectory() / L"MyProcessMonitor";
}

std::filesystem::path ConfigFile() {
    return AppDataDirectory() / L"config.json";
}

std::filesystem::path DatabaseFile() {
    return AppDataDirectory() / L"data" / L"monitor.db";
}

std::filesystem::path LogDirectory() {
    return AppDataDirectory() / L"logs";
}

std::filesystem::path LogFile() {
    return LogDirectory() / L"mpm.log";
}

bool EnsureAppDirectories(std::wstring* error) {
    std::error_code ec;
    std::filesystem::create_directories(AppDataDirectory(), ec);
    if (ec) {
        if (error != nullptr) {
            *error = L"无法创建应用数据目录: " + Utf16(ec.message());
        }
        return false;
    }
    std::filesystem::create_directories(DatabaseFile().parent_path(), ec);
    if (ec) {
        if (error != nullptr) {
            *error = L"无法创建数据库目录: " + Utf16(ec.message());
        }
        return false;
    }
    std::filesystem::create_directories(LogDirectory(), ec);
    if (ec) {
        if (error != nullptr) {
            *error = L"无法创建日志目录: " + Utf16(ec.message());
        }
        return false;
    }
    return true;
}

} // namespace mpm