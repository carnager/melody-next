// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/acoustid.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <string>
#include <utility>

namespace trackknife::musicbrainz {
namespace {

[[nodiscard]] core::Error service_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

[[nodiscard]] bool is_musicbrainz_id(const std::string_view id) {
    if (id.size() != 36U) {
        return false;
    }
    for (std::size_t index = 0U; index < id.size(); ++index) {
        const auto character = id[index];
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (character != '-') {
                return false;
            }
            continue;
        }
        const auto hexadecimal = (character >= '0' && character <= '9') ||
                                 (character >= 'a' && character <= 'f') ||
                                 (character >= 'A' && character <= 'F');
        if (!hexadecimal) {
            return false;
        }
    }
    return true;
}

} // namespace

core::Result<AcoustIdLookup> parse_acoustid_lookup(const std::string_view body,
                                                   const WebServiceLimits& limits) {
    if (body.size() > limits.body_bytes) {
        return std::unexpected(service_error(core::ErrorCode::limit_exceeded,
                                             "the AcoustID response exceeds the size limit"));
    }
    QJsonParseError parse_error{};
    const auto document = QJsonDocument::fromJson(
        QByteArray{body.data(), static_cast<qsizetype>(body.size())}, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(
            service_error(core::ErrorCode::backend, "the AcoustID response is not a JSON object"));
    }
    const auto root = document.object();
    const auto status = root.value(QStringLiteral("status")).toString();
    if (status != QStringLiteral("ok")) {
        const auto message = root.value(QStringLiteral("error"))
                                 .toObject()
                                 .value(QStringLiteral("message"))
                                 .toString();
        return std::unexpected(
            service_error(core::ErrorCode::backend,
                          "AcoustID rejected the lookup" +
                              (message.isEmpty() ? std::string{} : ": " + message.toStdString())));
    }
    const auto results = root.value(QStringLiteral("results"));
    if (!results.isArray()) {
        return std::unexpected(
            service_error(core::ErrorCode::backend, "the AcoustID response lacks a result list"));
    }
    AcoustIdLookup lookup;
    const auto entries = results.toArray();
    if (static_cast<std::size_t>(entries.size()) > limits.releases) {
        return std::unexpected(service_error(core::ErrorCode::limit_exceeded,
                                             "the AcoustID response exceeds the result limit"));
    }
    lookup.results.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto& entry : entries) {
        if (!entry.isObject()) {
            continue;
        }
        const auto result = entry.toObject();
        AcoustIdResult parsed{
            .score = result.value(QStringLiteral("score")).toDouble(0.0),
            .recordings = {},
        };
        const auto recordings = result.value(QStringLiteral("recordings"));
        if (recordings.isArray()) {
            for (const auto& recording_entry : recordings.toArray()) {
                if (parsed.recordings.size() == limits.identifiers) {
                    break;
                }
                if (!recording_entry.isObject()) {
                    continue;
                }
                const auto recording = recording_entry.toObject();
                AcoustIdRecording parsed_recording{
                    .id = recording.value(QStringLiteral("id")).toString().toStdString(),
                    .release_ids = {},
                };
                if (!is_musicbrainz_id(parsed_recording.id)) {
                    continue;
                }
                const auto releases = recording.value(QStringLiteral("releases"));
                if (releases.isArray()) {
                    for (const auto& release_entry : releases.toArray()) {
                        if (parsed_recording.release_ids.size() == limits.releases) {
                            break;
                        }
                        auto release_id = release_entry.toObject()
                                              .value(QStringLiteral("id"))
                                              .toString()
                                              .toStdString();
                        if (is_musicbrainz_id(release_id)) {
                            parsed_recording.release_ids.push_back(std::move(release_id));
                        }
                    }
                }
                parsed.recordings.push_back(std::move(parsed_recording));
            }
        }
        lookup.results.push_back(std::move(parsed));
    }
    std::ranges::stable_sort(lookup.results, [](const auto& left, const auto& right) {
        return left.score > right.score;
    });
    return lookup;
}

} // namespace trackknife::musicbrainz
