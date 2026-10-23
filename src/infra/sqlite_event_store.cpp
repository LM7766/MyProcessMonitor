#include "mpm/infra/sqlite_event_store.hpp"

#include "mpm/infra/event_json.hpp"
#include "mpm/infra/text.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <utility>

namespace mpm {
namespace {

constexpr const char* kSchema = R"sql(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS events (
    id TEXT PRIMARY KEY,
    started_utc INTEGER NOT NULL,
    ended_utc INTEGER NOT NULL DEFAULT 0,
    severity TEXT NOT NULL,
    score INTEGER NOT NULL,
    culprit_name TEXT NOT NULL DEFAULT '',
    finished INTEGER NOT NULL DEFAULT 0,
    event_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_events_started ON events(started_utc DESC);
CREATE INDEX IF NOT EXISTS idx_events_severity ON events(severity, started_utc DESC);
)sql";

} // namespace

SqliteEventStore::~SqliteEventStore() {
    Close();
}

void SqliteEventStore::SetSqliteError(const char* context) {
    std::wstring message = Utf16(context == nullptr ? "sqlite error" : context);
    if (db_ != nullptr) {
        message += L": ";
        message += Utf16(sqlite3_errmsg(db_));
    }
    lastError_ = std::move(message);
}

bool SqliteEventStore::Exec(const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        if (error != nullptr) {
            lastError_ = Utf16(error);
            sqlite3_free(error);
        } else {
            SetSqliteError("sqlite3_exec");
        }
        return false;
    }
    return true;
}

bool SqliteEventStore::Open(const std::filesystem::path& path, std::wstring* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseUnlocked();

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        lastError_ = L"无法创建数据库目录: " + Utf16(ec.message());
        if (error != nullptr) {
            *error = lastError_;
        }
        return false;
    }

    const int rc = sqlite3_open16(path.wstring().c_str(), &db_);
    if (rc != SQLITE_OK) {
        SetSqliteError("sqlite3_open16");
        if (error != nullptr) {
            *error = lastError_;
        }
        if (db_ != nullptr) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
        return false;
    }

    sqlite3_busy_timeout(db_, 2000);
    if (!Exec(kSchema)) {
        if (error != nullptr) {
            *error = lastError_;
        }
        sqlite3_close_v2(db_);
        db_ = nullptr;
        return false;
    }

    lastError_.clear();
    return true;
}

void SqliteEventStore::CloseUnlocked() {
    if (db_ != nullptr) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

void SqliteEventStore::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseUnlocked();
}

bool SqliteEventStore::Upsert(const DetectionEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        lastError_ = L"数据库未打开";
        return false;
    }

    static constexpr const char* kSql =
        "INSERT INTO events(id, started_utc, ended_utc, severity, score, culprit_name, "
        "finished, event_json) VALUES(?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "started_utc=excluded.started_utc, ended_utc=excluded.ended_utc, "
        "severity=excluded.severity, score=excluded.score, "
        "culprit_name=excluded.culprit_name, finished=excluded.finished, "
        "event_json=excluded.event_json";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &statement, nullptr) != SQLITE_OK) {
        SetSqliteError("sqlite3_prepare_v2(upsert)");
        return false;
    }

    const std::string json = ToJson(event).dump();
    const std::string severity = ToString(event.severity);
    const std::string culprit = Utf8(event.culprit.displayName);

    auto bindText = [&](int index, const std::string& value) {
        sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT);
    };

    bindText(1, event.id);
    sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(event.startedUnixMs));
    sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(event.endedUnixMs));
    bindText(4, severity);
    sqlite3_bind_int(statement, 5, event.score);
    bindText(6, culprit);
    sqlite3_bind_int(statement, 7, event.finished ? 1 : 0);
    bindText(8, json);

    const int rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (rc != SQLITE_DONE) {
        SetSqliteError("sqlite3_step(upsert)");
        return false;
    }
    return true;
}

std::vector<DetectionEvent> SqliteEventStore::Recent(int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DetectionEvent> result;
    if (db_ == nullptr) {
        lastError_ = L"数据库未打开";
        return result;
    }

    limit = std::clamp(limit, 1, 500);
    static constexpr const char* kSql =
        "SELECT event_json FROM events ORDER BY started_utc DESC LIMIT ?";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &statement, nullptr) != SQLITE_OK) {
        SetSqliteError("sqlite3_prepare_v2(recent)");
        return result;
    }
    sqlite3_bind_int(statement, 1, limit);

    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        if (text == nullptr) {
            continue;
        }
        try {
            DetectionEvent event = EventFromJson(nlohmann::json::parse(text));
            if (!event.id.empty()) {
                result.push_back(std::move(event));
            }
        } catch (const std::exception&) {
            // Ignore a single corrupt row; the rest of the history is still useful.
        }
    }
    sqlite3_finalize(statement);
    return result;
}

bool SqliteEventStore::PruneBefore(std::uint64_t unixTimeMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        lastError_ = L"数据库未打开";
        return false;
    }
    static constexpr const char* kSql =
        "DELETE FROM events WHERE started_utc < ? AND finished = 1";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &statement, nullptr) != SQLITE_OK) {
        SetSqliteError("sqlite3_prepare_v2(prune)");
        return false;
    }
    sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(unixTimeMs));
    const int rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (rc != SQLITE_DONE) {
        SetSqliteError("sqlite3_step(prune)");
        return false;
    }
    return true;
}

const std::wstring& SqliteEventStore::LastError() const noexcept {
    return lastError_;
}

} // namespace mpm