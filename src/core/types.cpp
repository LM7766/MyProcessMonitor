#include "mpm/core/types.hpp"

namespace mpm {

const wchar_t* ToString(SignatureState state) noexcept {
    switch (state) {
        case SignatureState::Microsoft:
            return L"microsoft";
        case SignatureState::Trusted:
            return L"trusted";
        case SignatureState::Untrusted:
            return L"untrusted";
        case SignatureState::Unsigned:
            return L"unsigned";
        case SignatureState::AccessDenied:
            return L"access_denied";
        case SignatureState::Unknown:
        default:
            return L"unknown";
    }
}

const char* ToString(Severity severity) noexcept {
    switch (severity) {
        case Severity::Critical:
            return "critical";
        case Severity::Warning:
            return "warning";
        case Severity::Info:
        default:
            return "info";
    }
}

} // namespace mpm