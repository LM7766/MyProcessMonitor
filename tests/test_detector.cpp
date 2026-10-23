#include "mpm/core/detector.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

class FakeCatalog final : public mpm::IProcessCatalog {
public:
    FakeCatalog(std::wstring path, mpm::SignatureState signature,
                std::wstring publisher = L"")
        : path_(std::move(path)), signature_(signature), publisher_(std::move(publisher)) {}

    mpm::ProcessInfo Describe(const mpm::ProcessKey& key) override {
        mpm::ProcessInfo info;
        info.key = key;
        info.imagePath = path_;
        const std::size_t slash = path_.find_last_of(L"\\/");
        info.imageName = slash == std::wstring::npos ? path_ : path_.substr(slash + 1);
        info.publisher = publisher_;
        info.signature = signature_;
        info.accessible = true;
        return info;
    }

private:
    std::wstring path_;
    mpm::SignatureState signature_;
    std::wstring publisher_;
};

mpm::ProcessCpu MakeProcess(std::uint32_t pid, std::wstring name, double cpu,
                            std::uint64_t createTime = 100) {
    mpm::ProcessCpu process;
    process.key.pid = pid;
    process.key.createTime100ns = createTime;
    process.imageName = std::move(name);
    process.systemPercent = cpu;
    process.oneCorePercent = cpu * 16.0;
    process.parentPid = 1000;
    process.sessionId = 2;
    return process;
}

mpm::SystemSample MakeSample(std::uint64_t unixMs, double systemCpu,
                             std::vector<mpm::ProcessCpu> processes) {
    mpm::SystemSample sample;
    sample.valid = true;
    sample.unixTimeMs = unixMs;
    sample.qpc = unixMs * 1000;
    sample.elapsedSeconds = 1.0;
    sample.systemCpuPercent = systemCpu;
    sample.idleCpuPercent = 100.0 - systemCpu;
    sample.logicalCpuCount = 16;
    sample.processes = std::move(processes);
    return sample;
}

mpm::ActivityState Idle() {
    mpm::ActivityState activity;
    activity.idleSeconds = 600;
    activity.sessionActive = true;
    return activity;
}

mpm::AppConfig TestConfig() {
    mpm::AppConfig config;
    config.idleThresholdSeconds = 1;
    config.startupGraceSeconds = 0;
    config.sustainSeconds = 1;
    config.exitSustainSeconds = 2;
    config.cooldownSeconds = 0;
    config.systemCpuTriggerPercent = 10.0;
    config.minBusyCores = 0.5;
    config.warningScore = 60;
    config.criticalScore = 80;
    config.knownMaintenanceCap = 65;
    config.suppressWhenMediaPlaying = true;
    config.suppressWhenFullscreen = true;
    return config;
}

} // namespace

TEST(Detector, StartsAndFinishesSustainedIdleEpisode) {
    FakeCatalog catalog(L"C:\\Users\\me\\AppData\\Local\\Temp\\hog.exe",
                        mpm::SignatureState::Unsigned);
    mpm::Detector detector(TestConfig(), &catalog, 9999);

    const auto first = detector.Evaluate(
        MakeSample(1000, 50.0, {MakeProcess(1234, L"hog.exe", 50.0)}), Idle());
    EXPECT_EQ(first.kind, mpm::DetectionUpdateKind::None);

    const auto started = detector.Evaluate(
        MakeSample(2000, 50.0, {MakeProcess(1234, L"hog.exe", 50.0)}), Idle());
    ASSERT_EQ(started.kind, mpm::DetectionUpdateKind::Started);
    EXPECT_EQ(started.event.culprit.displayName, L"hog.exe");
    EXPECT_GE(started.event.score, 60);
    EXPECT_TRUE(started.notify);
    EXPECT_FALSE(started.event.finished);

    const auto low = detector.Evaluate(
        MakeSample(4000, 1.0, {MakeProcess(1234, L"hog.exe", 0.5)}), Idle());
    EXPECT_EQ(low.kind, mpm::DetectionUpdateKind::None);

    const auto finished = detector.Evaluate(
        MakeSample(7000, 1.0, {MakeProcess(1234, L"hog.exe", 0.5)}), Idle());
    ASSERT_EQ(finished.kind, mpm::DetectionUpdateKind::Finished);
    EXPECT_TRUE(finished.event.finished);
    EXPECT_GT(finished.event.endedUnixMs, finished.event.startedUnixMs);
}

TEST(Detector, DoesNotStartWhenMediaIsPlaying) {
    FakeCatalog catalog(L"C:\\Program Files\\App\\video.exe",
                        mpm::SignatureState::Trusted);
    mpm::Detector detector(TestConfig(), &catalog, 9999);

    mpm::ActivityState activity = Idle();
    activity.mediaPlaying = true;

    detector.Evaluate(MakeSample(1000, 50.0, {MakeProcess(1234, L"video.exe", 50.0)}),
                      activity);
    const auto update = detector.Evaluate(
        MakeSample(2000, 50.0, {MakeProcess(1234, L"video.exe", 50.0)}), activity);
    EXPECT_EQ(update.kind, mpm::DetectionUpdateKind::None);
}

TEST(Detector, InputReturnFinishesEpisode) {
    FakeCatalog catalog(L"C:\\Users\\me\\AppData\\Local\\Temp\\hog.exe",
                        mpm::SignatureState::Unsigned);
    mpm::Detector detector(TestConfig(), &catalog, 9999);

    detector.Evaluate(MakeSample(1000, 50.0, {MakeProcess(1234, L"hog.exe", 50.0)}), Idle());
    const auto started = detector.Evaluate(
        MakeSample(2000, 50.0, {MakeProcess(1234, L"hog.exe", 50.0)}), Idle());
    ASSERT_EQ(started.kind, mpm::DetectionUpdateKind::Started);

    mpm::ActivityState active = Idle();
    active.idleSeconds = 0;
    const auto finished = detector.Evaluate(
        MakeSample(3000, 50.0, {MakeProcess(1234, L"hog.exe", 50.0)}), active);
    ASSERT_EQ(finished.kind, mpm::DetectionUpdateKind::Finished);
    EXPECT_TRUE(finished.event.finished);
}

TEST(Detector, AggregatesSameImageAndCapsKnownMaintenance) {
    FakeCatalog catalog(L"C:\\Windows\\System32\\SearchIndexer.exe",
                        mpm::SignatureState::Microsoft, L"Microsoft Corporation");
    mpm::AppConfig config = TestConfig();
    config.warningScore = 10;
    mpm::Detector detector(config, &catalog, 9999);

    const std::vector<mpm::ProcessCpu> processes = {
        MakeProcess(100, L"SearchIndexer.exe", 20.0, 1),
        MakeProcess(101, L"SearchIndexer.exe", 10.0, 2),
    };
    detector.Evaluate(MakeSample(1000, 40.0, processes), Idle());
    const auto started = detector.Evaluate(MakeSample(2000, 40.0, processes), Idle());
    ASSERT_EQ(started.kind, mpm::DetectionUpdateKind::Started);
    EXPECT_DOUBLE_EQ(started.event.culprit.cpuPercent, 30.0);
    EXPECT_TRUE(started.event.knownMaintenance);
    EXPECT_LE(started.event.score, 65);
}

TEST(Detector, StartupGraceSuppressesEvents) {
    FakeCatalog catalog(L"C:\\Users\\me\\AppData\\Local\\Temp\\hog.exe",
                        mpm::SignatureState::Unsigned);
    mpm::AppConfig config = TestConfig();
    config.startupGraceSeconds = 10;
    mpm::Detector detector(config, &catalog, 9999);

    detector.Evaluate(MakeSample(1000, 50.0, {MakeProcess(1234, L"hog.exe", 50.0)}), Idle());
    const auto duringGrace = detector.Evaluate(
        MakeSample(3000, 50.0, {MakeProcess(1234, L"hog.exe", 50.0)}), Idle());
    EXPECT_EQ(duringGrace.kind, mpm::DetectionUpdateKind::None);

    const auto firstEligible = detector.Evaluate(
        MakeSample(12000, 50.0, {MakeProcess(1234, L"hog.exe", 50.0)}), Idle());
    EXPECT_EQ(firstEligible.kind, mpm::DetectionUpdateKind::None);

    const auto afterGrace = detector.Evaluate(
        MakeSample(13000, 50.0, {MakeProcess(1234, L"hog.exe", 50.0)}), Idle());
    EXPECT_EQ(afterGrace.kind, mpm::DetectionUpdateKind::Started);
}