#pragma once

#include "mpm/core/interfaces.hpp"

#include <filesystem>
#include <mutex>
#include <string>

struct sqlite3;

namespace mpm {

class SqliteEventStore final : public IEventStore {
public:
    SqliteEventStore() = default;
    ~SqliteEventStore() override;

    SqliteEventStore(const SqliteEventStore&) = delete;
    SqliteEventStore& operator=(const SqliteEventStore&) = delete;

    bool Open(const std::filesystem::path& path, std::wstring* error);
    void Close();

    bool Upsert(const DetectionEvent& event) override;
    std::vector<DetectionEvent> Recent(int limit) override;
    bool PruneBefore(std::uint64_t unixTimeMs) override;
    const std::wstring& LastError() const noexcept override;

private:
    bool Exec(const char* sql);
    void SetSqliteError(const char* context);
    void CloseUnlocked();

    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
    std::wstring lastError_;
};

} // namespace mpm