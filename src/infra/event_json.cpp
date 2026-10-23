#include "mpm/infra/event_json.hpp"

#include "mpm/infra/text.hpp"

#include <stdexcept>

namespace mpm {
namespace {

std::string SeverityToString(Severity severity) {
    return ToString(severity);
}

Severity SeverityFromString(const std::string& value) {
    if (value == "critical") {
        return Severity::Critical;
    }
    if (value == "warning") {
        return Severity::Warning;
    }
    return Severity::Info;
}

SignatureState SignatureFromString(const std::string& value) {
    if (value == "microsoft") {
        return SignatureState::Microsoft;
    }
    if (value == "trusted") {
        return SignatureState::Trusted;
    }
    if (value == "untrusted") {
        return SignatureState::Untrusted;
    }
    if (value == "unsigned") {
        return SignatureState::Unsigned;
    }
    if (value == "access_denied") {
        return SignatureState::AccessDenied;
    }
    return SignatureState::Unknown;
}

} // namespace

nlohmann::json ToJson(const AttributionGroup& group) {
    nlohmann::json members = nlohmann::json::array();
    for (const auto& member : group.members) {
        members.push_back({{"pid", member.pid}, {"createTime100ns", member.createTime100ns}});
    }

    return nlohmann::json{
        {"key", Utf8(group.key)},
        {"displayName", Utf8(group.displayName)},
        {"imagePath", Utf8(group.imagePath)},
        {"publisher", Utf8(group.publisher)},
        {"signature", Utf8(ToString(group.signature))},
        {"services", Utf8(group.services)},
        {"cpuPercent", group.cpuPercent},
        {"shareOfSystem", group.shareOfSystem},
        {"knownMaintenance", group.knownMaintenance},
        {"members", std::move(members)},
    };
}

nlohmann::json ToJson(const DetectionEvent& event) {
    nlohmann::json contributors = nlohmann::json::array();
    for (const auto& group : event.contributors) {
        contributors.push_back(ToJson(group));
    }

    return nlohmann::json{
        {"id", event.id},
        {"episodeKey", event.episodeKey},
        {"startedUnixMs", event.startedUnixMs},
        {"startedLocal", LocalTimeString(event.startedUnixMs)},
        {"endedUnixMs", event.endedUnixMs},
        {"endedLocal", LocalTimeString(event.endedUnixMs)},
        {"severity", SeverityToString(event.severity)},
        {"score", event.score},
        {"trigger", event.trigger},
        {"systemCpuAvg", event.systemCpuAvg},
        {"systemCpuPeak", event.systemCpuPeak},
        {"baselineP95", event.baselineP95},
        {"idleSeconds", event.idleSeconds},
        {"durationSeconds", event.durationSeconds},
        {"culprit", ToJson(event.culprit)},
        {"contributors", std::move(contributors)},
        {"reasons", event.reasons},
        {"knownMaintenance", event.knownMaintenance},
        {"mediaPlaying", event.mediaPlaying},
        {"fullscreenApp", event.fullscreenApp},
        {"foregroundPid", event.foregroundPid},
        {"finished", event.finished},
    };
}

DetectionEvent EventFromJson(const nlohmann::json& value) {
    DetectionEvent event;
    try {
        event.id = value.at("id").get<std::string>();
        event.episodeKey = value.value("episodeKey", event.id);
        event.startedUnixMs = value.value("startedUnixMs", 0ULL);
        event.endedUnixMs = value.value("endedUnixMs", 0ULL);
        event.severity = SeverityFromString(value.value("severity", std::string("info")));
        event.score = value.value("score", 0);
        event.trigger = value.value("trigger", std::string{});
        event.systemCpuAvg = value.value("systemCpuAvg", 0.0);
        event.systemCpuPeak = value.value("systemCpuPeak", 0.0);
        event.baselineP95 = value.value("baselineP95", 0.0);
        event.idleSeconds = value.value("idleSeconds", 0ULL);
        event.durationSeconds = value.value("durationSeconds", 0ULL);
        event.knownMaintenance = value.value("knownMaintenance", false);
        event.mediaPlaying = value.value("mediaPlaying", false);
        event.fullscreenApp = value.value("fullscreenApp", false);
        event.foregroundPid = value.value("foregroundPid", 0U);
        event.finished = value.value("finished", false);
        event.reasons = value.value("reasons", std::vector<std::string>{});

        if (value.contains("culprit") && value["culprit"].is_object()) {
            const auto& culprit = value["culprit"];
            event.culprit.displayName = Utf16(culprit.value("displayName", std::string{}));
            event.culprit.imagePath = Utf16(culprit.value("imagePath", std::string{}));
            event.culprit.publisher = Utf16(culprit.value("publisher", std::string{}));
            event.culprit.signature = SignatureFromString(
                culprit.value("signature", std::string("unknown")));
            event.culprit.services = Utf16(culprit.value("services", std::string{}));
            event.culprit.cpuPercent = culprit.value("cpuPercent", 0.0);
            event.culprit.shareOfSystem = culprit.value("shareOfSystem", 0.0);
            event.culprit.knownMaintenance = culprit.value("knownMaintenance", false);
        }
        if (value.contains("contributors") && value["contributors"].is_array()) {
            for (const auto& item : value["contributors"]) {
                AttributionGroup group;
                group.displayName = Utf16(item.value("displayName", std::string{}));
                group.imagePath = Utf16(item.value("imagePath", std::string{}));
                group.publisher = Utf16(item.value("publisher", std::string{}));
                group.signature =
                    SignatureFromString(item.value("signature", std::string("unknown")));
                group.services = Utf16(item.value("services", std::string{}));
                group.cpuPercent = item.value("cpuPercent", 0.0);
                group.shareOfSystem = item.value("shareOfSystem", 0.0);
                group.knownMaintenance = item.value("knownMaintenance", false);
                event.contributors.push_back(std::move(group));
            }
        }
    } catch (const std::exception&) {
        // The caller gets a partially filled event. Bad rows are ignored by
        // the recent-events query when the id is empty.
    }
    return event;
}

} // namespace mpm