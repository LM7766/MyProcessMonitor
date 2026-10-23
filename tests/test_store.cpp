#include "mpm/core/types.hpp"
#include "mpm/infra/sqlite_event_store.hpp"

#include <Windows.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

TEST(EventStore, UpsertRecentAndPrune) {
    const auto directory =
        std::filesystem::temp_directory_path() /
        (L"mpm_store_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    const auto database = directory / L"monitor.db";
    {
        mpm::SqliteEventStore store;
        std::wstring error;
        ASSERT_TRUE(store.Open(database, &error)) << error;

        mpm::DetectionEvent event;
        event.id = "20250101T000000Z-1234";
        event.episodeKey = "hog.exe";
        event.startedUnixMs = 1000;
        event.severity = mpm::Severity::Warning;
        event.score = 72;
        event.culprit.displayName = L"hog.exe";
        event.culprit.key = L"hog.exe";
        event.culprit.cpuPercent = 35.5;
        event.culprit.members.push_back(mpm::ProcessKey{1234, 42});
        event.systemCpuPeak = 62.0;
        event.reasons = {"idle_sustained", "unknown_binary"};
        ASSERT_TRUE(store.Upsert(event)) << store.LastError();

        auto recent = store.Recent(10);
        ASSERT_EQ(recent.size(), 1u);
        EXPECT_EQ(recent[0].id, event.id);
        EXPECT_EQ(recent[0].culprit.displayName, L"hog.exe");
        EXPECT_EQ(recent[0].severity, mpm::Severity::Warning);
        EXPECT_FALSE(recent[0].finished);

        event.finished = true;
        event.endedUnixMs = 2000;
        ASSERT_TRUE(store.Upsert(event)) << store.LastError();
        recent = store.Recent(10);
        ASSERT_EQ(recent.size(), 1u);
        EXPECT_TRUE(recent[0].finished);
        EXPECT_EQ(recent[0].endedUnixMs, 2000u);

        store.PruneBefore(3000);
        EXPECT_TRUE(store.Recent(10).empty());
        store.Close();
    }
    std::filesystem::remove_all(directory);
}