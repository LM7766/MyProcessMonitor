#pragma once

#include <cstdint>
#include <string>

namespace mpm {

struct AppConfig {
    int schemaVersion = 1;

    int samplingIntervalMs = 1000;
    int idleThresholdSeconds = 120;
    bool treatLockedAsIdle = true;
    bool treatDisplayOffAsIdle = true;
    bool suppressWhenMediaPlaying = true;
    bool suppressWhenFullscreen = true;
    int foregroundGraceSeconds = 600;

    double systemCpuTriggerPercent = 12.0;
    double minBusyCores = 0.75;
    int sustainSeconds = 30;
    double exitBelowPercent = 6.0;
    int exitSustainSeconds = 60;
    int cooldownSeconds = 900;
    int startupGraceSeconds = 180;

    int warningScore = 60;
    int criticalScore = 80;
    int knownMaintenanceCap = 65;

    int retentionDays = 90;
    bool storeCommandLine = false;
    bool autoStart = false;

    std::string logLevel = "info";
};

} // namespace mpm