#include "mpm/win32/process_catalog.hpp"

#include <Windows.h>
#include <Wincrypt.h>
#include <MSCat.h>
#include <Psapi.h>
#include <SoftPub.h>
#include <WinTrust.h>
#include <winsvc.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mpm {
namespace {

const GUID kGenericVerifyV2 = {
    0x00aac56b,
    0xcd44,
    0x11d0,
    {0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee},
};

std::wstring ToLowerAscii(std::wstring_view text) {
    std::wstring result(text);
    for (wchar_t& ch : result) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return result;
}

std::wstring FileNameOf(const std::wstring& path) {
    const std::size_t position = path.find_last_of(L"\\/");
    if (position == std::wstring::npos) {
        return path;
    }
    return path.substr(position + 1);
}

std::wstring Join(const std::vector<std::wstring>& values) {
    std::wstring result;
    for (const auto& value : values) {
        if (!result.empty()) {
            result += L", ";
        }
        result += value;
    }
    return result;
}

std::wstring CertName(PCCERT_CONTEXT certificate) {
    if (certificate == nullptr) {
        return {};
    }
    const DWORD length =
        CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
    if (length <= 1) {
        return {};
    }
    std::wstring result(length, L'\0');
    if (CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, result.data(),
                           length) == 0) {
        return {};
    }
    while (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

std::wstring ExtractPublisher(HANDLE state) {
    if (state == nullptr) {
        return {};
    }
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(state);
    if (provider == nullptr) {
        return {};
    }
    CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
    if (signer == nullptr) {
        return {};
    }
    CRYPT_PROVIDER_CERT* certificate = WTHelperGetProvCertFromChain(signer, 0);
    if (certificate == nullptr) {
        return {};
    }
    return CertName(certificate->pCert);
}

std::wstring HashToTag(const std::vector<BYTE>& hash) {
    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring result;
    result.reserve(hash.size() * 2);
    for (BYTE byte : hash) {
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 0x0F]);
    }
    return result;
}

void CloseTrustState(WINTRUST_DATA& data) {
    if (data.hWVTStateData == nullptr) {
        return;
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, const_cast<GUID*>(&kGenericVerifyV2), &data);
    data.hWVTStateData = nullptr;
}

struct SignatureResult {
    SignatureState state = SignatureState::Unknown;
    std::wstring publisher;
};

bool VerifyCatalogSignature(const std::wstring& path, std::wstring* publisher) {
    HCATADMIN admin = nullptr;
    if (!CryptCATAdminAcquireContext(&admin, nullptr, 0)) {
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        CryptCATAdminReleaseContext(admin, 0);
        return false;
    }

    DWORD hashSize = 0;
    CryptCATAdminCalcHashFromFileHandle(file, &hashSize, nullptr, 0);
    if (hashSize == 0) {
        CloseHandle(file);
        CryptCATAdminReleaseContext(admin, 0);
        return false;
    }

    std::vector<BYTE> hash(hashSize);
    if (!CryptCATAdminCalcHashFromFileHandle(file, &hashSize, hash.data(), 0)) {
        CloseHandle(file);
        CryptCATAdminReleaseContext(admin, 0);
        return false;
    }
    hash.resize(hashSize);

    HCATINFO catalog = CryptCATAdminEnumCatalogFromHash(admin, hash.data(), hashSize, 0, nullptr);
    if (catalog == nullptr) {
        CloseHandle(file);
        CryptCATAdminReleaseContext(admin, 0);
        return false;
    }

    CATALOG_INFO catalogInfo{};
    catalogInfo.cbStruct = sizeof(catalogInfo);
    if (!CryptCATCatalogInfoFromContext(catalog, &catalogInfo, 0)) {
        CryptCATAdminReleaseCatalogContext(admin, catalog, 0);
        CloseHandle(file);
        CryptCATAdminReleaseContext(admin, 0);
        return false;
    }

    const std::wstring memberTag = HashToTag(hash);
    WINTRUST_CATALOG_INFO catalogTrust{};
    catalogTrust.cbStruct = sizeof(catalogTrust);
    catalogTrust.dwCatalogVersion = 0;
    catalogTrust.pcwszCatalogFilePath = catalogInfo.wszCatalogFile;
    catalogTrust.pcwszMemberTag = memberTag.c_str();
    catalogTrust.pcwszMemberFilePath = path.c_str();
    catalogTrust.hMemberFile = file;
    catalogTrust.pbCalculatedFileHash = hash.data();
    catalogTrust.cbCalculatedFileHash = hashSize;
    catalogTrust.hCatAdmin = admin;

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_CATALOG;
    data.pCatalog = &catalogTrust;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    const LONG status =
        WinVerifyTrust(nullptr, const_cast<GUID*>(&kGenericVerifyV2), &data);
    if (status == ERROR_SUCCESS && publisher != nullptr) {
        *publisher = ExtractPublisher(data.hWVTStateData);
    }
    CloseTrustState(data);

    CryptCATAdminReleaseCatalogContext(admin, catalog, 0);
    CloseHandle(file);
    CryptCATAdminReleaseContext(admin, 0);
    return status == ERROR_SUCCESS;
}

SignatureResult VerifySignature(const std::wstring& path) {
    SignatureResult result;
    if (path.empty()) {
        return result;
    }

    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    LONG status =
        WinVerifyTrust(nullptr, const_cast<GUID*>(&kGenericVerifyV2), &data);
    if (status == ERROR_SUCCESS) {
        result.publisher = ExtractPublisher(data.hWVTStateData);
    }
    CloseTrustState(data);

    if (status == ERROR_SUCCESS) {
        const std::wstring publisherLower = ToLowerAscii(result.publisher);
        result.state = publisherLower.find(L"microsoft") != std::wstring::npos
                           ? SignatureState::Microsoft
                           : SignatureState::Trusted;
        return result;
    }

    if (status == TRUST_E_NOSIGNATURE || status == TRUST_E_SUBJECT_FORM_UNKNOWN ||
        status == TRUST_E_PROVIDER_UNKNOWN) {
        std::wstring catalogPublisher;
        if (VerifyCatalogSignature(path, &catalogPublisher)) {
            result.publisher = std::move(catalogPublisher);
            const std::wstring publisherLower = ToLowerAscii(result.publisher);
            result.state = publisherLower.find(L"microsoft") != std::wstring::npos
                               ? SignatureState::Microsoft
                               : SignatureState::Trusted;
            return result;
        }
        result.state = SignatureState::Unsigned;
        return result;
    }

    if (status == TRUST_E_EXPLICIT_DISTRUST || status == TRUST_E_BAD_DIGEST ||
        status == CERT_E_UNTRUSTEDROOT) {
        result.state = SignatureState::Untrusted;
        return result;
    }

    result.state = SignatureState::Unknown;
    return result;
}

} // namespace

struct Win32ProcessCatalog::Impl {
    ProcessInfo Describe(const ProcessKey& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        ProcessInfo info;
        info.key = key;

        HANDLE process =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, key.pid);
        if (process == nullptr) {
            info.signature = SignatureState::AccessDenied;
            return info;
        }

        FILETIME creation{}, exit{}, kernel{}, user{};
        if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
            ULARGE_INTEGER creationValue;
            creationValue.LowPart = creation.dwLowDateTime;
            creationValue.HighPart = creation.dwHighDateTime;
            if (key.createTime100ns != 0 &&
                creationValue.QuadPart != key.createTime100ns) {
                CloseHandle(process);
                info.signature = SignatureState::AccessDenied;
                return info;
            }
        }

        std::vector<wchar_t> buffer(32768);
        DWORD size = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
            info.imagePath.assign(buffer.data(), size);
            info.imageName = FileNameOf(info.imagePath);
            info.accessible = true;
        }
        CloseHandle(process);

        if (info.imagePath.empty()) {
            info.signature = SignatureState::AccessDenied;
            return info;
        }

        const std::wstring lowerPath = ToLowerAscii(info.imagePath);
        auto found = signatureCache_.find(lowerPath);
        if (found == signatureCache_.end()) {
            found = signatureCache_.emplace(lowerPath, VerifySignature(info.imagePath)).first;
        }
        info.publisher = found->second.publisher;
        info.signature = found->second.state;

        if (ToLowerAscii(info.imageName) == L"svchost.exe") {
            RefreshServicesIfNeeded();
            const auto service = services_.find(key.pid);
            if (service != services_.end()) {
                info.services = service->second;
            }
        }
        return info;
    }

    void RefreshServicesIfNeeded() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto nowMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        if (servicesLoaded_ && nowMs - lastServiceRefreshMs_ < 30000) {
            return;
        }
        servicesLoaded_ = true;
        lastServiceRefreshMs_ = nowMs;
        services_.clear();

        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr,
                                            SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
        if (manager == nullptr) {
            return;
        }

        DWORD bytesNeeded = 0;
        DWORD serviceCount = 0;
        DWORD resume = 0;
        EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                              nullptr, 0, &bytesNeeded, &serviceCount, &resume, nullptr);
        if (bytesNeeded == 0) {
            CloseServiceHandle(manager);
            return;
        }

        std::vector<BYTE> buffer(bytesNeeded + 4096);
        DWORD bytes = static_cast<DWORD>(buffer.size());
        if (!EnumServicesStatusExW(
                manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                buffer.data(), bytes, &bytes, &serviceCount, &resume, nullptr)) {
            CloseServiceHandle(manager);
            return;
        }

        auto* entries =
            reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
        std::unordered_map<DWORD, std::vector<std::wstring>> perProcess;
        for (DWORD index = 0; index < serviceCount; ++index) {
            const DWORD pid = entries[index].ServiceStatusProcess.dwProcessId;
            if (pid == 0) {
                continue;
            }
            perProcess[pid].emplace_back(entries[index].lpServiceName);
        }

        for (auto& [pid, names] : perProcess) {
            if (names.size() > 8) {
                names.resize(8);
            }
            services_.emplace(pid, Join(names));
        }
        CloseServiceHandle(manager);
    }

    std::mutex mutex_;
    std::unordered_map<std::wstring, SignatureResult> signatureCache_;
    std::unordered_map<DWORD, std::wstring> services_;
    bool servicesLoaded_ = false;
    long long lastServiceRefreshMs_ = 0;
};

Win32ProcessCatalog::Win32ProcessCatalog() : impl_(std::make_unique<Impl>()) {}
Win32ProcessCatalog::~Win32ProcessCatalog() = default;

ProcessInfo Win32ProcessCatalog::Describe(const ProcessKey& key) {
    return impl_->Describe(key);
}

} // namespace mpm