// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window_helpers.hpp"

#include "trackknife/persistence/rating_identity.hpp"
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <charconv>
#include <ranges>
#include <unordered_set>

namespace trackknife::bench {

QIcon albumShuffleIcon(const QPalette& palette) {
    QIcon icon;
    for (const auto mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active}) {
        for (const int size : {24, 48}) {
            QPixmap pixmap(size, size);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(static_cast<double>(size) / 24.0, static_cast<double>(size) / 24.0);
            painter.setPen(
                QPen(palette.color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active,
                                   QPalette::ButtonText),
                     1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            // Stacked album sleeves above two crossing shuffle arrows.
            painter.drawRoundedRect(QRectF(4, 2, 10, 8), 1, 1);
            painter.drawPolyline(QPolygonF{QPointF(16, 4), QPointF(18, 4), QPointF(18, 12)});
            painter.drawEllipse(QPointF(9, 6), 2, 2);
            QPainterPath arrows;
            arrows.moveTo(3, 14);
            arrows.lineTo(7, 14);
            arrows.lineTo(15, 21);
            arrows.lineTo(21, 21);
            arrows.moveTo(3, 21);
            arrows.lineTo(7, 21);
            arrows.lineTo(15, 14);
            arrows.lineTo(21, 14);
            arrows.moveTo(18, 12);
            arrows.lineTo(21, 14);
            arrows.lineTo(18, 16);
            arrows.moveTo(18, 19);
            arrows.lineTo(21, 21);
            arrows.lineTo(18, 23);
            painter.drawPath(arrows);
            painter.end();
            icon.addPixmap(pixmap, mode);
        }
    }
    return icon;
}

QString trackColumnId(const int logical) {
    const auto found = std::ranges::find(track_column_specs, logical, &TrackColumnSpec::logical);
    return found == track_column_specs.end() ? QString{} : QString::fromLatin1(found->id);
}

int trackColumnLogical(const QString& id) {
    const auto found = std::ranges::find_if(
        track_column_specs, [&id](const auto& spec) { return id == QString::fromLatin1(spec.id); });
    return found == track_column_specs.end() ? -1 : found->logical;
}

QStringList trackColumnIds() {
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(track_column_specs.size()));
    for (const auto& spec : track_column_specs) {
        ids.push_back(QString::fromLatin1(spec.id));
    }
    return ids;
}

QString displayText(const std::string& utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

std::string utf8Bytes(const QString& text) {
    const auto encoded = text.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

QString formatTime(const qint64 milliseconds) {
    const auto total_seconds = std::max<qint64>(0, milliseconds) / 1'000;
    const auto minutes = total_seconds / 60;
    const auto seconds = total_seconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar{'0'});
}

std::string lowercased_ascii(std::string name) {
    for (auto& character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return name;
}

// FFmpeg exposes a few format-native fields through generic AVDictionary
// spellings. Those spellings are presentation projections, not additional
// native tag identities. Keep this adapter mapping explicit so ADR-0066's
// freeform-field identity never mistakes one for an independently writable
// property.
std::optional<std::string_view> probed_semantic_alias(const std::string_view native_name) {
    const auto name = lowercased_ascii(std::string{native_name});
    if (name == "track") {
        return "tracknumber";
    }
    if (name == "disc") {
        return "discnumber";
    }
    if (name == "album_artist") {
        return "albumartist";
    }
    // FFmpeg's "encoder" is the tag TagLib reads as ENCODING (ID3 TSSE, MP4
    // ©too); without this it came through a second time, as a field that
    // is not a tag and so could never be edited or removed.
    if (name == "encoder") {
        return "encoding";
    }
    return std::nullopt;
}

void remove_shadowed_probed_metadata(metadata::MetadataDocument& document) {
    std::unordered_set<std::string> embedded_semantic_names;
    embedded_semantic_names.reserve(document.fields.size());
    for (const auto& field : document.fields) {
        if (field.provenance == metadata::FieldProvenance::embedded) {
            embedded_semantic_names.insert(field.canonical_name);
        }
    }
    std::erase_if(document.fields, [&embedded_semantic_names](const auto& field) {
        if (field.provenance != metadata::FieldProvenance::stream) {
            return false;
        }
        const auto semantic = probed_semantic_alias(field.native_name);
        return semantic && embedded_semantic_names.contains(std::string{*semantic});
    });
}

std::string metadata_value(const metadata::MetadataDocument& document,
                           const std::initializer_list<std::string_view> candidate_names) {
    for (const auto name : candidate_names) {
        if (auto value = document.first_effective_value(name)) {
            return std::move(*value);
        }
    }
    return {};
}

void project_display_metadata(LocalTrackRow& row) {
    row.title = metadata_value(row.metadata, {"title"});
    row.artist = metadata_value(row.metadata, {"artist"});
    row.album = metadata_value(row.metadata, {"album"});
    row.album_artist = metadata_value(row.metadata, {"albumartist"});
    row.date = metadata_value(row.metadata, {"date", "year"});
    row.track_number = metadata_value(row.metadata, {"tracknumber", "track"});
    if (!row.raw_path.empty()) {
        auto identity = persistence::rating_identity(row.metadata, row.raw_path);
        row.rating_hash = std::move(identity.track_hash);
        row.album_rating_hash = std::move(identity.album_hash);
    }
}

LocalTrackRow cached_library_row(persistence::LibraryTrackSnapshot snapshot) {
    LocalTrackRow row;
    row.raw_path = std::move(snapshot.raw_path);
    for (auto& [name, values] : snapshot.facts.fields) {
        metadata::MetadataField field{.canonical_name = name,
                                      .native_name = name,
                                      .values = {},
                                      .qualifier = {},
                                      .provenance = metadata::FieldProvenance::cached_snapshot};
        for (auto& value : values) {
            field.values.push_back(std::move(value.first));
        }
        row.metadata.fields.push_back(std::move(field));
    }
    project_display_metadata(row);
    if (row.title.empty())
        row.title = std::move(snapshot.facts.title);
    if (row.artist.empty())
        row.artist = std::move(snapshot.facts.artist);
    if (row.album.empty())
        row.album = std::move(snapshot.facts.album);
    if (row.date.empty())
        row.date = std::move(snapshot.facts.date);
    if (snapshot.facts.duration_ms >= 0)
        row.duration_ms = snapshot.facts.duration_ms;
    if (!snapshot.facts.codec.empty()) {
        row.technicals =
            LocalTrackTechnicals{.codec = std::move(snapshot.facts.codec),
                                 .sample_rate = static_cast<int>(snapshot.facts.sample_rate),
                                 .bits = static_cast<int>(snapshot.facts.bits),
                                 .channels = static_cast<int>(snapshot.facts.channels)};
    }
    if (snapshot.facts.rating >= 0) {
        row.rating = static_cast<unsigned>(snapshot.facts.rating);
    }
    if (snapshot.facts.album_rating >= 0) {
        row.album_rating = static_cast<unsigned>(snapshot.facts.album_rating);
    }
    row.probed = true;
    return row;
}

std::string cue_track_logical_reference(const std::string& raw_cue_path,
                                        const std::size_t file_index,
                                        const std::size_t track_index) {
    std::string reference{"cue-v1"};
    reference.push_back('\0');
    reference += raw_cue_path;
    reference.push_back('\0');
    reference += std::to_string(file_index);
    reference.push_back('\0');
    reference += std::to_string(track_index);
    return reference;
}

std::optional<CueLogicalReferenceParts> parse_cue_logical_reference(const std::string& reference) {
    constexpr std::string_view prefix{"cue-v1"};
    if (!reference.starts_with(prefix) || reference.size() <= prefix.size() + 1U ||
        reference[prefix.size()] != '\0') {
        return std::nullopt;
    }
    // Raw paths cannot contain NUL, so the remaining separators are exact.
    const auto body = std::string_view{reference}.substr(prefix.size() + 1U);
    const auto first = body.find('\0');
    const auto second = first == std::string_view::npos ? first : body.find('\0', first + 1U);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        body.find('\0', second + 1U) != std::string_view::npos) {
        return std::nullopt;
    }
    CueLogicalReferenceParts parts;
    parts.raw_cue_path = std::string{body.substr(0U, first)};
    const auto file_text = body.substr(first + 1U, second - first - 1U);
    const auto track_text = body.substr(second + 1U);
    const auto file_parsed =
        std::from_chars(file_text.data(), file_text.data() + file_text.size(), parts.file_index);
    const auto track_parsed = std::from_chars(
        track_text.data(), track_text.data() + track_text.size(), parts.track_index);
    if (parts.raw_cue_path.empty() || file_parsed.ec != std::errc{} ||
        file_parsed.ptr != file_text.data() + file_text.size() || track_parsed.ec != std::errc{} ||
        track_parsed.ptr != track_text.data() + track_text.size()) {
        return std::nullopt;
    }
    return parts;
}

} // namespace trackknife::bench
