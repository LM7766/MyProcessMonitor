#include "mpm/win32/system_sampler.hpp"

#include "mpm/core/cpu_math.hpp"

#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mpm {
namespace {

constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023L);
constexpr std::size_t kInitialBufferSize = 1U << 20U;
constexpr std::size_t kMaximumBufferSize = 64U << 20U;

// The public winternl.h declaration hides CreateTime/UserTime/KernelTime in
// Reserved1. This prefix mirrors the documented SYSTEM_PROCESS_INFORMATION
// layout used by NtQuerySystemInformation(SystemProcessInformation).
struct NtProcessRecord {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
};

using NtQuerySystemInformationFn =
    NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

struct RawProcess {
    ProcessKey key;
    std::wstring imageName;
    std::uint32_t parentPid = 0;
    std::uint32_t sessionId = 0;
    std::uint64_t kernel100ns = 0;
    std::uint64_t user100ns = 0;
    std::uint64_t workingSetBytes = 0;
};

std::uint64_t FileTimeToU64(const FILETIME& value) {
    ULARGE_INTEGER integer;
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

std::uint64_t QpcTo100ns(LARGE_INTEGER counter, LARGE_INTEGER frequency) {
    if (frequency.QuadPart <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        static_cast<double>(counter.QuadPart) * 10000000.0 /
        static_cast<double>(frequency.QuadPart));
}

std::wstring FormatStatus(NTSTATUS status) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, std::size(buffer), L"0x%08lX",
               static_cast<unsigned long>(status));
    return buffer;
}

std::wstring FormatWin32Error(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring result;
    if (length > 0 && text != nullptr) {
        result.assign(text, length);
        while (!result.empty() &&
               (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
            result.pop_back();
        }
    } else {
        result = L"Win32 error " + std::to_wstring(code);
    }
    if (text != nullptr) {
        LocalFree(text);
    }
    return result;
}

std::uint32_t ActiveProcessorCount() {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count > 0) {
        return count;
    }
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return std::max<DWORD>(1, info.dwNumberOfProcessors);
}

std::uint64_t WallClockMilliseconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

struct Win32SystemSampler::Impl {
    Impl() {
        ntQuery = reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
        if (ntQuery == nullptr) {
            useNt = false;
            error = L"ntdll!NtQuerySystemInformation 不可用，使用 Toolhelp 回退";
        }
        QueryPerformanceFrequency(&qpcFrequency);
    }

    bool QueryNt(std::vector<RawProcess>& processes) {
        if (ntQuery == nullptr) {
            return false;
        }

        std::size_t bufferSize = kInitialBufferSize;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::vector<std::byte> buffer(bufferSize);
            ULONG needed = 0;
            const NTSTATUS status = ntQuery(
                SystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()),
                &needed);
            if (status == 0) {
                std::size_t offset = 0;
                while (offset + sizeof(NtProcessRecord) <= buffer.size()) {
                    const auto* record =
                        reinterpret_cast<const NtProcessRecord*>(buffer.data() + offset);
                    RawProcess process;
                    process.key.pid = static_cast<std::uint32_t>(
                        reinterpret_cast<std::uintptr_t>(record->UniqueProcessId));
                    process.key.createTime100ns =
                        static_cast<std::uint64_t>(record->CreateTime.QuadPart);
                    process.parentPid = static_cast<std::uint32_t>(
                        reinterpret_cast<std::uintptr_t>(record->InheritedFromUniqueProcessId));
                    process.sessionId = record->SessionId;
                    process.kernel100ns =
                        static_cast<std::uint64_t>(record->KernelTime.QuadPart);
                    process.user100ns = static_cast<std::uint64_t>(record->UserTime.QuadPart);
                    process.workingSetBytes =
                        static_cast<std::uint64_t>(record->WorkingSetSize);

                    if (record->ImageName.Buffer != nullptr && record->ImageName.Length > 0) {
                        const std::size_t characters =
                            record->ImageName.Length / sizeof(wchar_t);
                        process.imageName.assign(record->ImageName.Buffer, characters);
                    }
                    processes.push_back(std::move(process));

                    if (record->NextEntryOffset == 0) {
                        break;
                    }
                    const std::size_t next =
                        offset + static_cast<std::size_t>(record->NextEntryOffset);
                    if (next <= offset || next + sizeof(NtProcessRecord) > buffer.size()) {
                        error = L"NtQuerySystemInformation 返回的链表越界";
                        return false;
                    }
                    offset = next;
                }
                return !processes.empty();
            }

            if (status == kStatusInfoLengthMismatch || status == kStatusBufferTooSmall) {
                const std::size_t requested =
                    needed > 0 ? static_cast<std::size_t>(needed) + 65536 : bufferSize * 2;
                bufferSize = std::min(kMaximumBufferSize, std::max(bufferSize * 2, requested));
                if (bufferSize >= kMaximumBufferSize && attempt >= 4) {
                    break;
                }
                continue;
            }

            error = L"NtQuerySystemInformation 失败，NTSTATUS=" + FormatStatus(status);
            return false;
        }

        error = L"NtQuerySystemInformation 无法分配足够的缓冲区";
        return false;
    }

    bool QueryToolhelp(std::vector<RawProcess>& processes) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            error = L"CreateToolhelp32Snapshot 失败: " + FormatWin32Error(GetLastError());
            return false;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot, &entry)) {
            error = L"Process32FirstW 失败: " + FormatWin32Error(GetLastError());
            CloseHandle(snapshot);
            return false;
        }

        do {
            if (entry.th32ProcessID == 0) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }

            FILETIME creation{}, exit{}, kernel{}, user{};
            if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
                RawProcess raw;
                raw.key.pid = entry.th32ProcessID;
                raw.key.createTime100ns = FileTimeToU64(creation);
                raw.imageName = entry.szExeFile;
                raw.parentPid = entry.th32ParentProcessID;
                raw.kernel100ns = FileTimeToU64(kernel);
                raw.user100ns = FileTimeToU64(user);
                DWORD sessionId = 0;
                ProcessIdToSessionId(entry.th32ProcessID, &sessionId);
                raw.sessionId = sessionId;
                processes.push_back(std::move(raw));
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));

        CloseHandle(snapshot);
        return !processes.empty();
    }

    void ResetDeltaBaseline() {
        havePrevious_ = false;
        previousProcessTimes_.clear();
    }

    bool CaptureRaw(std::vector<RawProcess>& raw) {
        bool snapshotOk = false;
        if (useNt) {
            snapshotOk = QueryNt(raw);
            if (!snapshotOk) {
                useNt = false;
                error += L"；自动切换到 Toolhelp32 回退";
            }
        }
        if (!snapshotOk) {
            raw.clear();
            snapshotOk = QueryToolhelp(raw);
        }
        return snapshotOk;
    }

    std::optional<SystemSample> Capture() {
        FILETIME idleTime{}, kernelTime{}, userTime{};
        if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
            error = L"GetSystemTimes 失败: " + FormatWin32Error(GetLastError());
            return std::nullopt;
        }

        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        const std::uint64_t idle = FileTimeToU64(idleTime);
        const std::uint64_t kernel = FileTimeToU64(kernelTime);
        const std::uint64_t user = FileTimeToU64(userTime);

        if (!havePrevious_) {
            std::vector<RawProcess> raw;
            if (CaptureRaw(raw)) {
                for (const auto& process : raw) {
                    const auto key =
                        std::make_pair(process.key.pid, process.key.createTime100ns);
                    previousProcessTimes_[key] = {process.kernel100ns, process.user100ns};
                }
            }
            previousIdle_ = idle;
            previousKernel_ = kernel;
            previousUser_ = user;
            previousQpc_ = qpc;
            havePrevious_ = true;
            return std::nullopt;
        }

        const std::uint64_t elapsed100ns = QpcTo100ns(qpc, qpcFrequency) -
                                           QpcTo100ns(previousQpc_, qpcFrequency);
        const double elapsedSeconds = static_cast<double>(elapsed100ns) / 10000000.0;
        if (elapsedSeconds <= 0.0 || elapsedSeconds > 10.0) {
            previousIdle_ = idle;
            previousKernel_ = kernel;
            previousUser_ = user;
            previousQpc_ = qpc;
            previousProcessTimes_.clear();
            return std::nullopt;
        }

        std::vector<RawProcess> raw;
        const bool snapshotOk = CaptureRaw(raw);
        if (!snapshotOk) {
            previousIdle_ = idle;
            previousKernel_ = kernel;
            previousUser_ = user;
            previousQpc_ = qpc;
            return std::nullopt;
        }

        const std::uint64_t idleDelta =
            idle >= previousIdle_ ? idle - previousIdle_ : 0;
        const std::uint64_t kernelDelta =
            kernel >= previousKernel_ ? kernel - previousKernel_ : 0;
        const std::uint64_t userDelta =
            user >= previousUser_ ? user - previousUser_ : 0;
        const std::uint64_t totalDelta = kernelDelta + userDelta;
        const std::uint64_t busyDelta = totalDelta > idleDelta ? totalDelta - idleDelta : 0;

        const std::uint32_t cpuCount = ActiveProcessorCount();
        SystemSample sample;
        sample.qpc = static_cast<std::uint64_t>(qpc.QuadPart);
        sample.unixTimeMs = WallClockMilliseconds();
        sample.elapsedSeconds = elapsedSeconds;
        sample.systemCpuPercent =
            ComputeSystemCpuPercent(busyDelta, elapsedSeconds, cpuCount);
        sample.idleCpuPercent =
            ComputeSystemCpuPercent(idleDelta, elapsedSeconds, cpuCount);
        sample.logicalCpuCount = cpuCount;

        std::map<std::pair<std::uint32_t, std::uint64_t>, std::pair<std::uint64_t, std::uint64_t>>
            currentTimes;
        for (const auto& process : raw) {
            const auto key = std::make_pair(process.key.pid, process.key.createTime100ns);
            currentTimes[key] = {process.kernel100ns, process.user100ns};

            // PID 0 is the idle pseudo-process. Its "CPU time" is the inverse
            // of useful CPU and must never be treated as a culprit.
            if (process.key.pid == 0) {
                continue;
            }

            auto previous = previousProcessTimes_.find(key);
            if (previous == previousProcessTimes_.end()) {
                continue;
            }
            const std::uint64_t previousTotal = previous->second.first + previous->second.second;
            const std::uint64_t currentTotal = process.kernel100ns + process.user100ns;
            const std::uint64_t delta =
                currentTotal >= previousTotal ? currentTotal - previousTotal : 0;

            ProcessCpu cpu;
            cpu.key = process.key;
            cpu.imageName = process.imageName.empty() ? L"<unknown>" : process.imageName;
            cpu.parentPid = process.parentPid;
            cpu.sessionId = process.sessionId;
            cpu.systemPercent =
                ComputeSystemCpuPercent(delta, elapsedSeconds, cpuCount);
            cpu.oneCorePercent = ComputeOneCoreCpuPercent(delta, elapsedSeconds);
            cpu.workingSetBytes = process.workingSetBytes;
            sample.processes.push_back(std::move(cpu));
        }

        previousProcessTimes_ = std::move(currentTimes);
        previousIdle_ = idle;
        previousKernel_ = kernel;
        previousUser_ = user;
        previousQpc_ = qpc;
        sample.valid = true;
        return sample;
    }

    bool useNt = true;
    NtQuerySystemInformationFn ntQuery = nullptr;
    LARGE_INTEGER qpcFrequency{};
    LARGE_INTEGER previousQpc_{};
    std::uint64_t previousIdle_ = 0;
    std::uint64_t previousKernel_ = 0;
    std::uint64_t previousUser_ = 0;
    bool havePrevious_ = false;
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::pair<std::uint64_t, std::uint64_t>>
        previousProcessTimes_;
    std::wstring error;
};

Win32SystemSampler::Win32SystemSampler() : impl_(std::make_unique<Impl>()) {}
Win32SystemSampler::~Win32SystemSampler() = default;

std::optional<SystemSample> Win32SystemSampler::Capture() {
    return impl_->Capture();
}

const wchar_t* Win32SystemSampler::Name() const noexcept {
    return impl_->useNt ? L"NtQuerySystemInformation" : L"Toolhelp32+GetProcessTimes";
}

const std::wstring& Win32SystemSampler::LastError() const noexcept {
    return impl_->error;
}

} // namespace mpm