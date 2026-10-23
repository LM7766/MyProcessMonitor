#include "mpm/infra/config_loader.hpp"

#include "mpm/infra/text.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace mpm {
namespace {

template <typename T>
T ClampValue(T value, T low, T high, const char* name, std::wstring* warning) {
    if (value < low || value > high) {
        if (warning != nullptr) {
            if (!warning->empty()) {
                *warning += L"；";
            }
            *warning += Utf16(std::string(name) + " 超出范围，已修正为 " +
                              std::to_string(value < low ? low : high));
        }
        return std::clamp(value, low, high);
    }
    return value;
}

nlohmann::json DefaultJson() {
    return nlohmann::json{
        {"schemaVersion", 1},
        {"sampling", {{"intervalMs", 1000}}},
        {"idle",
         {{"thresholdSeconds", 120},
          {"treatLockedAsIdle", true},
          {"treatDisplayOffAsIdle", true},
          {"suppressWhenMediaPlaying", true},
          {"suppressWhenFullscreen", true},
          {"foregroundGraceSeconds", 600}}},
        {"detection",
         {{"systemCpuTriggerPercent", 12.0},
          {"minBusyCores", 0.75},
          {"sustainSeconds", 30},
          {"exitBelowPercent", 6.0},
          {"exitSustainSeconds", 60},
          {"cooldownSeconds", 900},
          {"startupGraceSeconds", 180}}},
        {"scoring", {{"warningScore", 60}, {"criticalScore", 80}, {"knownMaintenanceCap", 65}}},
        {"storage", {{"retentionDays", 90}}},
        {"privacy", {{"storeCommandLine", false}}},
        {"logging", {{"level", "info"}}},
        {"startup", {{"autoStart", false}}},
    };
}

} // namespace

AppConfig LoadConfig(const std::filesystem::path& path, std::wstring* warning) {
    AppConfig config;
    if (warning != nullptr) {
        warning->clear();
    }

    std::ifstream file(path.wstring().c_str(), std::ios::binary);
    if (!file.is_open()) {
        if (warning != nullptr) {
            *warning = L"配置文件不存在，使用默认配置";
        }
        return config;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    nlohmann::json value;
    try {
        value = nlohmann::json::parse(content);
    } catch (const std::exception& ex) {
        if (warning != nullptr) {
            *warning = L"配置文件解析失败，使用默认配置: " + Utf16(ex.what());
        }
        return config;
    }
    if (!value.is_object()) {
        if (warning != nullptr) {
            *warning = L"配置文件根节点不是对象，使用默认配置";
        }
        return config;
    }

    try {
        const auto sampling = value.value("sampling", nlohmann::json::object());
        const auto idle = value.value("idle", nlohmann::json::object());
        const auto detection = value.value("detection", nlohmann::json::object());
        const auto scoring = value.value("scoring", nlohmann::json::object());
        const auto storage = value.value("storage", nlohmann::json::object());
        const auto privacy = value.value("privacy", nlohmann::json::object());
        const auto logging = value.value("logging", nlohmann::json::object());
        const auto startup = value.value("startup", nlohmann::json::object());

        config.schemaVersion = value.value("schemaVersion", config.schemaVersion);
        config.samplingIntervalMs =
            sampling.value("intervalMs", config.samplingIntervalMs);
        config.idleThresholdSeconds =
            idle.value("thresholdSeconds", config.idleThresholdSeconds);
        config.treatLockedAsIdle =
            idle.value("treatLockedAsIdle", config.treatLockedAsIdle);
        config.treatDisplayOffAsIdle =
            idle.value("treatDisplayOffAsIdle", config.treatDisplayOffAsIdle);
        config.suppressWhenMediaPlaying =
            idle.value("suppressWhenMediaPlaying", config.suppressWhenMediaPlaying);
        config.suppressWhenFullscreen =
            idle.value("suppressWhenFullscreen", config.suppressWhenFullscreen);
        config.foregroundGraceSeconds =
            idle.value("foregroundGraceSeconds", config.foregroundGraceSeconds);

        config.systemCpuTriggerPercent =
            detection.value("systemCpuTriggerPercent", config.systemCpuTriggerPercent);
        config.minBusyCores = detection.value("minBusyCores", config.minBusyCores);
        config.sustainSeconds = detection.value("sustainSeconds", config.sustainSeconds);
        config.exitBelowPercent = detection.value("exitBelowPercent", config.exitBelowPercent);
        config.exitSustainSeconds =
            detection.value("exitSustainSeconds", config.exitSustainSeconds);
        config.cooldownSeconds = detection.value("cooldownSeconds", config.cooldownSeconds);
        config.startupGraceSeconds =
            detection.value("startupGraceSeconds", config.startupGraceSeconds);

        config.warningScore = scoring.value("warningScore", config.warningScore);
        config.criticalScore = scoring.value("criticalScore", config.criticalScore);
        config.knownMaintenanceCap =
            scoring.value("knownMaintenanceCap", config.knownMaintenanceCap);

        config.retentionDays = storage.value("retentionDays", config.retentionDays);
        config.storeCommandLine = privacy.value("storeCommandLine", config.storeCommandLine);
        config.logLevel = logging.value("level", config.logLevel);
        config.autoStart = startup.value("autoStart", config.autoStart);
    } catch (const std::exception& ex) {
        if (warning != nullptr) {
            *warning = L"部分配置字段类型错误，使用默认值: " + Utf16(ex.what());
        }
        return AppConfig{};
    }

    config.samplingIntervalMs =
        ClampValue(config.samplingIntervalMs, 250, 10000, "sampling.intervalMs", warning);
    config.idleThresholdSeconds =
        ClampValue(config.idleThresholdSeconds, 10, 86400, "idle.thresholdSeconds", warning);
    config.foregroundGraceSeconds =
        ClampValue(config.foregroundGraceSeconds, 0, 86400, "idle.foregroundGraceSeconds", warning);
    config.systemCpuTriggerPercent =
        ClampValue(config.systemCpuTriggerPercent, 1.0, 100.0,
                   "detection.systemCpuTriggerPercent", warning);
    config.minBusyCores =
        ClampValue(config.minBusyCores, 0.0, 256.0, "detection.minBusyCores", warning);
    config.sustainSeconds =
        ClampValue(config.sustainSeconds, 5, 3600, "detection.sustainSeconds", warning);
    config.exitBelowPercent =
        ClampValue(config.exitBelowPercent, 0.0, 100.0, "detection.exitBelowPercent", warning);
    config.exitSustainSeconds =
        ClampValue(config.exitSustainSeconds, 0, 3600, "detection.exitSustainSeconds", warning);
    config.cooldownSeconds =
        ClampValue(config.cooldownSeconds, 0, 86400, "detection.cooldownSeconds", warning);
    config.startupGraceSeconds =
        ClampValue(config.startupGraceSeconds, 0, 3600, "detection.startupGraceSeconds", warning);
    config.warningScore = ClampValue(config.warningScore, 1, 99, "scoring.warningScore", warning);
    config.criticalScore =
        ClampValue(config.criticalScore, 2, 100, "scoring.criticalScore", warning);
    if (config.warningScore >= config.criticalScore) {
        config.warningScore = config.criticalScore - 1;
        if (warning != nullptr) {
            if (!warning->empty()) {
                *warning += L"；";
            }
            *warning += L"warningScore 必须小于 criticalScore，已修正";
        }
    }
    config.knownMaintenanceCap =
        ClampValue(config.knownMaintenanceCap, 0, 100, "scoring.knownMaintenanceCap", warning);
    config.retentionDays =
        ClampValue(config.retentionDays, 1, 3650, "storage.retentionDays", warning);
    if (config.logLevel != "debug" && config.logLevel != "info" &&
        config.logLevel != "warn" && config.logLevel != "error") {
        config.logLevel = "info";
        if (warning != nullptr) {
            if (!warning->empty()) {
                *warning += L"；";
            }
            *warning += L"logging.level 无效，已修正为 info";
        }
    }
    return config;
}

bool SaveDefaultConfig(const std::filesystem::path& path, std::wstring* error) {
    std::ofstream file(path.wstring().c_str(), std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        if (error != nullptr) {
            *error = L"无法写入配置文件: " + path.wstring();
        }
        return false;
    }
    file << DefaultJson().dump(2) << '\n';
    if (!file.good()) {
        if (error != nullptr) {
            *error = L"写入配置文件失败: " + path.wstring();
        }
        return false;
    }
    return true;
}

} // namespace mpm