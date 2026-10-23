#include "mpm/core/config.hpp"
#include "mpm/infra/config_loader.hpp"

#include <Windows.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path MakeTempDirectory(const wchar_t* name) {
    const auto directory =
        std::filesystem::temp_directory_path() / (std::wstring(name) + L"_" +
                                                   std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
}

} // namespace

TEST(Config, MissingFileReturnsDefaultsAndWarning) {
    const auto directory = MakeTempDirectory(L"mpm_config_missing");
    std::wstring warning;
    const mpm::AppConfig config =
        mpm::LoadConfig(directory / L"does-not-exist.json", &warning);
    EXPECT_FALSE(warning.empty());
    EXPECT_EQ(config.samplingIntervalMs, 1000);
    EXPECT_EQ(config.idleThresholdSeconds, 120);
    std::filesystem::remove_all(directory);
}

TEST(Config, ReadsNestedValuesAndClampsInvalidInput) {
    const auto directory = MakeTempDirectory(L"mpm_config_values");
    const auto path = directory / L"config.json";
    WriteText(path, R"({
        "sampling": {"intervalMs": 500},
        "idle": {"thresholdSeconds": 30, "treatLockedAsIdle": false},
        "detection": {
            "systemCpuTriggerPercent": 20.0,
            "minBusyCores": 1.5,
            "sustainSeconds": 10,
            "exitBelowPercent": 5.0,
            "exitSustainSeconds": 15,
            "cooldownSeconds": 60,
            "startupGraceSeconds": 5
        },
        "scoring": {"warningScore": 50, "criticalScore": 70, "knownMaintenanceCap": 60},
        "storage": {"retentionDays": 7},
        "logging": {"level": "debug"}
    })");

    std::wstring warning;
    const mpm::AppConfig config = mpm::LoadConfig(path, &warning);
    EXPECT_TRUE(warning.empty()) << warning;
    EXPECT_EQ(config.samplingIntervalMs, 500);
    EXPECT_EQ(config.idleThresholdSeconds, 30);
    EXPECT_FALSE(config.treatLockedAsIdle);
    EXPECT_DOUBLE_EQ(config.systemCpuTriggerPercent, 20.0);
    EXPECT_DOUBLE_EQ(config.minBusyCores, 1.5);
    EXPECT_EQ(config.sustainSeconds, 10);
    EXPECT_EQ(config.retentionDays, 7);
    EXPECT_EQ(config.logLevel, "debug");

    WriteText(path, R"({
        "sampling": {"intervalMs": 1},
        "idle": {"thresholdSeconds": 1},
        "detection": {"systemCpuTriggerPercent": 200.0, "sustainSeconds": 1},
        "scoring": {"warningScore": 90, "criticalScore": 10}
    })");
    const mpm::AppConfig clamped = mpm::LoadConfig(path, &warning);
    EXPECT_GE(clamped.samplingIntervalMs, 250);
    EXPECT_GE(clamped.idleThresholdSeconds, 10);
    EXPECT_LE(clamped.systemCpuTriggerPercent, 100.0);
    EXPECT_GE(clamped.sustainSeconds, 5);
    EXPECT_GT(clamped.criticalScore, clamped.warningScore);

    std::filesystem::remove_all(directory);
}

TEST(Config, SaveDefaultConfigCreatesLoadableFile) {
    const auto directory = MakeTempDirectory(L"mpm_config_save");
    const auto path = directory / L"config.json";
    std::wstring error;
    ASSERT_TRUE(mpm::SaveDefaultConfig(path, &error)) << error;
    ASSERT_TRUE(std::filesystem::exists(path));

    std::wstring warning;
    const mpm::AppConfig config = mpm::LoadConfig(path, &warning);
    EXPECT_TRUE(warning.empty()) << warning;
    EXPECT_EQ(config.samplingIntervalMs, 1000);
    std::filesystem::remove_all(directory);
}