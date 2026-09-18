// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/track_view_layout.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

namespace trackknife::ui {
namespace {

constexpr int minimum_column_width = 24;
constexpr int maximum_column_width = 4'096;
constexpr qsizetype maximum_layout_bytes = 64 * 1'024;

[[nodiscard]] QString presentationName(const TrackViewPresentation presentation) {
    switch (presentation) {
    case TrackViewPresentation::albums_side_artwork:
        return QStringLiteral("albums-side-artwork");
    case TrackViewPresentation::albums_header_artwork:
        return QStringLiteral("albums-header-artwork");
    case TrackViewPresentation::plain_columns:
        return QStringLiteral("plain-columns");
    case TrackViewPresentation::compact_queue:
        return QStringLiteral("compact-queue");
    }
    return {};
}

[[nodiscard]] std::optional<TrackViewPresentation> parsePresentation(const QString& name) {
    if (name == QStringLiteral("albums-side-artwork")) {
        return TrackViewPresentation::albums_side_artwork;
    }
    if (name == QStringLiteral("albums-header-artwork")) {
        return TrackViewPresentation::albums_header_artwork;
    }
    if (name == QStringLiteral("plain-columns")) {
        return TrackViewPresentation::plain_columns;
    }
    if (name == QStringLiteral("compact-queue")) {
        return TrackViewPresentation::compact_queue;
    }
    return std::nullopt;
}

} // namespace

QByteArray serializeTrackViewLayout(const TrackViewLayout& layout) {
    QJsonArray columns;
    for (const auto& column : layout.columns) {
        columns.push_back(QJsonObject{{QStringLiteral("id"), column.id},
                                      {QStringLiteral("width"), column.width},
                                      {QStringLiteral("visible"), column.visible}});
    }
    return QJsonDocument{
        QJsonObject{
            {QStringLiteral("schema"), layout.schema_version},
            {QStringLiteral("presentation"), presentationName(layout.presentation)},
            {QStringLiteral("columns"), columns},
        }}
        .toJson(QJsonDocument::Compact);
}

std::optional<TrackViewLayout> deserializeTrackViewLayout(const QByteArray& bytes,
                                                          const QStringList& registered_column_ids,
                                                          QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<TrackViewLayout> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    if (bytes.isEmpty() || bytes.size() > maximum_layout_bytes) {
        return fail(QStringLiteral("Track-view layout size is invalid"));
    }
    const QSet<QString> registered{registered_column_ids.begin(), registered_column_ids.end()};
    if (registered.isEmpty() || registered.size() != registered_column_ids.size()) {
        return fail(QStringLiteral("Track-view column registry is invalid"));
    }

    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("Invalid track-view layout JSON"));
    }
    const auto object = document.object();
    const auto schema = object.value(QStringLiteral("schema")).toInt();
    if (schema != track_view_layout_schema_version) {
        return fail(QStringLiteral("Unsupported track-view layout version"));
    }
    const auto presentation =
        parsePresentation(object.value(QStringLiteral("presentation")).toString());
    if (!presentation) {
        return fail(QStringLiteral("Track-view presentation is unsupported"));
    }
    const auto column_values = object.value(QStringLiteral("columns"));
    if (!column_values.isArray()) {
        return fail(QStringLiteral("Track-view columns are malformed"));
    }
    const auto array = column_values.toArray();
    if (array.size() > registered.size()) {
        return fail(QStringLiteral("Track-view layout does not place every registered column"));
    }

    QSet<QString> used;
    std::vector<TrackViewColumnLayout> columns;
    columns.reserve(static_cast<std::size_t>(array.size()));
    bool any_visible = false;
    for (const auto& value : array) {
        if (!value.isObject()) {
            return fail(QStringLiteral("Track-view column entry is malformed"));
        }
        const auto column = value.toObject();
        const auto id = column.value(QStringLiteral("id")).toString();
        if (!registered.contains(id) || used.contains(id)) {
            return fail(
                QStringLiteral("Track-view layout references an unavailable or duplicate column"));
        }
        const auto width_value = column.value(QStringLiteral("width"));
        const auto visible_value = column.value(QStringLiteral("visible"));
        if (!width_value.isDouble() || !visible_value.isBool()) {
            return fail(QStringLiteral("Track-view column attributes are malformed"));
        }
        const auto width = width_value.toInt(-1);
        if (width < minimum_column_width || width > maximum_column_width ||
            width_value.toDouble() != static_cast<double>(width)) {
            return fail(QStringLiteral("Track-view column width is outside the supported range"));
        }
        const auto visible = visible_value.toBool();
        any_visible = any_visible || visible;
        used.insert(id);
        columns.push_back(TrackViewColumnLayout{.id = id, .width = width, .visible = visible});
    }
    if (!any_visible) {
        return fail(
            QStringLiteral("Track-view layout must place every column and show at least one"));
    }
    // Columns registered after the layout was saved migrate by appending
    // hidden at the end instead of rejecting the whole saved layout.
    for (const auto& id : registered_column_ids) {
        if (!used.contains(id)) {
            columns.push_back(TrackViewColumnLayout{.id = id, .width = 100, .visible = false});
        }
    }
    return TrackViewLayout{
        .schema_version = schema, .presentation = *presentation, .columns = std::move(columns)};
}

} // namespace trackknife::ui
