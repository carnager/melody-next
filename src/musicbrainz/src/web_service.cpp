// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/web_service.hpp"
#include "trackknife/musicbrainz/bandcamp.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace trackknife::musicbrainz {
namespace {

constexpr std::string_view web_service_root = "https://musicbrainz.org/ws/2";

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

// Lucene phrase escaping: the value is embedded inside double quotes, so
// only the quote and backslash need escaping.
[[nodiscard]] std::string lucene_phrase(const std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2U);
    escaped.push_back('"');
    for (const auto character : value) {
        if (character == '"' || character == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

[[nodiscard]] std::string url_encoded(const std::string& value) {
    const auto encoded = QUrl::toPercentEncoding(QString::fromStdString(value));
    return std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

[[nodiscard]] std::string bounded_text(const QJsonValue& value, const WebServiceLimits& limits) {
    if (!value.isString()) {
        return {};
    }
    auto text = value.toString().toStdString();
    if (text.size() > limits.text_bytes) {
        text.resize(limits.text_bytes);
    }
    return text;
}

[[nodiscard]] std::optional<std::int64_t> optional_milliseconds(const QJsonValue& value) {
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const auto length = value.toInteger();
    return length > 0 ? std::optional{length} : std::nullopt;
}

[[nodiscard]] std::vector<ArtistCredit> parse_artist_credits(const QJsonValue& value,
                                                             const WebServiceLimits& limits) {
    std::vector<ArtistCredit> credits;
    if (!value.isArray()) {
        return credits;
    }
    const auto entries = value.toArray();
    credits.reserve(std::min(static_cast<std::size_t>(entries.size()), limits.artist_credits));
    for (const auto& entry : entries) {
        if (credits.size() == limits.artist_credits) {
            break;
        }
        if (!entry.isObject()) {
            continue;
        }
        const auto credit = entry.toObject();
        const auto artist = credit.value(QStringLiteral("artist")).toObject();
        ArtistCredit parsed{
            .name = bounded_text(credit.value(QStringLiteral("name")), limits),
            .join_phrase = bounded_text(credit.value(QStringLiteral("joinphrase")), limits),
            .artist_id = bounded_text(artist.value(QStringLiteral("id")), limits),
            .sort_name = bounded_text(artist.value(QStringLiteral("sort-name")), limits),
        };
        if (parsed.name.empty()) {
            parsed.name = bounded_text(artist.value(QStringLiteral("name")), limits);
        }
        if (!parsed.name.empty()) {
            credits.push_back(std::move(parsed));
        }
    }
    return credits;
}

[[nodiscard]] std::vector<std::string> bounded_string_list(const QJsonValue& value,
                                                           const std::size_t bound,
                                                           const WebServiceLimits& limits) {
    std::vector<std::string> entries;
    if (!value.isArray()) {
        return entries;
    }
    for (const auto& entry : value.toArray()) {
        if (entries.size() == bound) {
            break;
        }
        if (entry.isString()) {
            auto text = bounded_text(entry, limits);
            if (!text.empty()) {
                entries.push_back(std::move(text));
            }
        }
    }
    return entries;
}

// Recording-level relations carry the performed works; only valid work ids
// survive.
[[nodiscard]] std::vector<std::string> parse_work_ids(const QJsonValue& value,
                                                      const WebServiceLimits& limits) {
    std::vector<std::string> ids;
    if (!value.isArray()) {
        return ids;
    }
    for (const auto& entry : value.toArray()) {
        if (ids.size() == limits.identifiers) {
            break;
        }
        if (!entry.isObject()) {
            continue;
        }
        auto id = bounded_text(
            entry.toObject().value(QStringLiteral("work")).toObject().value(QStringLiteral("id")),
            limits);
        if (is_musicbrainz_id(id)) {
            ids.push_back(std::move(id));
        }
    }
    return ids;
}

[[nodiscard]] core::Result<ReleaseMedium> parse_medium(const QJsonObject& medium,
                                                       const WebServiceLimits& limits) {
    ReleaseMedium parsed{
        .position = static_cast<std::size_t>(
            std::max(medium.value(QStringLiteral("position")).toInt(0), 0)),
        .format = bounded_text(medium.value(QStringLiteral("format")), limits),
        .title = bounded_text(medium.value(QStringLiteral("title")), limits),
        .track_count = static_cast<std::size_t>(
            std::max(medium.value(QStringLiteral("track-count")).toInt(0), 0)),
        .tracks = {},
    };
    const auto tracks = medium.value(QStringLiteral("tracks"));
    if (!tracks.isArray()) {
        return parsed;
    }
    const auto entries = tracks.toArray();
    if (static_cast<std::size_t>(entries.size()) > limits.tracks_per_medium) {
        return std::unexpected(service_error(core::ErrorCode::limit_exceeded,
                                             "a MusicBrainz medium exceeds the track limit"));
    }
    parsed.tracks.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto& entry : entries) {
        if (!entry.isObject()) {
            return std::unexpected(service_error(core::ErrorCode::backend,
                                                 "a MusicBrainz track entry is not an object"));
        }
        const auto track = entry.toObject();
        const auto recording = track.value(QStringLiteral("recording")).toObject();
        ReleaseTrack parsed_track{
            .track_id = bounded_text(track.value(QStringLiteral("id")), limits),
            .recording_id = bounded_text(recording.value(QStringLiteral("id")), limits),
            .position = static_cast<std::size_t>(
                std::max(track.value(QStringLiteral("position")).toInt(0), 0)),
            .number = bounded_text(track.value(QStringLiteral("number")), limits),
            .title = bounded_text(track.value(QStringLiteral("title")), limits),
            .length_ms = optional_milliseconds(track.value(QStringLiteral("length"))),
            .artist_credits =
                parse_artist_credits(track.value(QStringLiteral("artist-credit")), limits),
            .isrcs = bounded_string_list(recording.value(QStringLiteral("isrcs")),
                                         limits.identifiers, limits),
            .work_ids = parse_work_ids(recording.value(QStringLiteral("relations")), limits),
        };
        if (parsed_track.title.empty()) {
            parsed_track.title = bounded_text(recording.value(QStringLiteral("title")), limits);
        }
        if (!parsed_track.length_ms) {
            parsed_track.length_ms =
                optional_milliseconds(recording.value(QStringLiteral("length")));
        }
        if (parsed_track.artist_credits.empty()) {
            parsed_track.artist_credits =
                parse_artist_credits(recording.value(QStringLiteral("artist-credit")), limits);
        }
        parsed.tracks.push_back(std::move(parsed_track));
    }
    return parsed;
}

[[nodiscard]] core::Result<Release> parse_release_object(const QJsonObject& release,
                                                         const WebServiceLimits& limits) {
    Release parsed{
        .id = bounded_text(release.value(QStringLiteral("id")), limits),
        .title = bounded_text(release.value(QStringLiteral("title")), limits),
        .status = bounded_text(release.value(QStringLiteral("status")), limits),
        .disambiguation = bounded_text(release.value(QStringLiteral("disambiguation")), limits),
        .date = bounded_text(release.value(QStringLiteral("date")), limits),
        .country = bounded_text(release.value(QStringLiteral("country")), limits),
        .barcode = bounded_text(release.value(QStringLiteral("barcode")), limits),
        .release_group_id = bounded_text(
            release.value(QStringLiteral("release-group")).toObject().value(QStringLiteral("id")),
            limits),
        .release_group_primary_type = bounded_text(release.value(QStringLiteral("release-group"))
                                                       .toObject()
                                                       .value(QStringLiteral("primary-type")),
                                                   limits),
        .release_group_secondary_types =
            bounded_string_list(release.value(QStringLiteral("release-group"))
                                    .toObject()
                                    .value(QStringLiteral("secondary-types")),
                                limits.identifiers, limits),
        .release_group_first_release_date =
            bounded_text(release.value(QStringLiteral("release-group"))
                             .toObject()
                             .value(QStringLiteral("first-release-date")),
                         limits),
        .script = bounded_text(release.value(QStringLiteral("text-representation"))
                                   .toObject()
                                   .value(QStringLiteral("script")),
                               limits),
        .language = bounded_text(release.value(QStringLiteral("text-representation"))
                                     .toObject()
                                     .value(QStringLiteral("language")),
                                 limits),
        .label = {},
        .catalog_number = {},
        .search_score = release.value(QStringLiteral("score")).toInt(0),
        .track_count = static_cast<std::size_t>(
            std::max(release.value(QStringLiteral("track-count")).toInt(0), 0)),
        .artist_credits =
            parse_artist_credits(release.value(QStringLiteral("artist-credit")), limits),
        .media = {},
    };
    if (!is_musicbrainz_id(parsed.id) || parsed.title.empty()) {
        return std::unexpected(service_error(core::ErrorCode::backend,
                                             "a MusicBrainz release lacks its identity or title"));
    }
    const auto label_info = release.value(QStringLiteral("label-info"));
    if (label_info.isArray() && !label_info.toArray().isEmpty()) {
        const auto first = label_info.toArray().first().toObject();
        parsed.label = bounded_text(
            first.value(QStringLiteral("label")).toObject().value(QStringLiteral("name")), limits);
        parsed.catalog_number = bounded_text(first.value(QStringLiteral("catalog-number")), limits);
    }
    const auto media = release.value(QStringLiteral("media"));
    if (media.isArray()) {
        const auto entries = media.toArray();
        if (static_cast<std::size_t>(entries.size()) > limits.media) {
            return std::unexpected(service_error(core::ErrorCode::limit_exceeded,
                                                 "a MusicBrainz release exceeds the medium limit"));
        }
        parsed.media.reserve(static_cast<std::size_t>(entries.size()));
        for (const auto& entry : entries) {
            if (!entry.isObject()) {
                continue;
            }
            auto medium = parse_medium(entry.toObject(), limits);
            if (!medium) {
                return std::unexpected(std::move(medium.error()));
            }
            parsed.media.push_back(std::move(*medium));
        }
    }
    if (parsed.track_count == 0U) {
        for (const auto& medium : parsed.media) {
            parsed.track_count += medium.tracks.empty() ? medium.track_count : medium.tracks.size();
        }
    }
    return parsed;
}

[[nodiscard]] core::Result<QJsonObject> parse_body(const std::string_view body,
                                                   const WebServiceLimits& limits) {
    if (body.size() > limits.body_bytes) {
        return std::unexpected(service_error(core::ErrorCode::limit_exceeded,
                                             "the MusicBrainz response exceeds the size limit"));
    }
    QJsonParseError parse_error{};
    const auto document = QJsonDocument::fromJson(
        QByteArray{body.data(), static_cast<qsizetype>(body.size())}, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(service_error(core::ErrorCode::backend,
                                             "the MusicBrainz response is not a JSON object"));
    }
    return document.object();
}

} // namespace

core::Result<std::string> build_release_search_url(const ReleaseSearchQuery& query) {
    if (query.artist.empty() && query.release.empty()) {
        return std::unexpected(
            service_error(core::ErrorCode::invalid_argument,
                          "a MusicBrainz release search needs an artist or a release title"));
    }
    if (query.limit == 0U || query.limit > WebServiceLimits{}.releases) {
        return std::unexpected(service_error(core::ErrorCode::invalid_argument,
                                             "the MusicBrainz search limit is out of range"));
    }
    std::string lucene;
    if (!query.artist.empty()) {
        lucene += "artist:" + lucene_phrase(query.artist);
    }
    if (!query.release.empty()) {
        if (!lucene.empty()) {
            lucene += " AND ";
        }
        lucene += "release:" + lucene_phrase(query.release);
    }
    if (query.track_count) {
        lucene += " AND tracks:" + std::to_string(*query.track_count);
    }
    return std::string{web_service_root} + "/release/?query=" + url_encoded(lucene) +
           "&fmt=json&limit=" + std::to_string(query.limit);
}

core::Result<std::string> build_release_lookup_url(const std::string_view release_id) {
    if (!is_musicbrainz_id(release_id)) {
        return std::unexpected(service_error(core::ErrorCode::invalid_argument,
                                             "a MusicBrainz release lookup needs a release id"));
    }
    return std::string{web_service_root} + "/release/" + std::string{release_id} +
           "?inc=artist-credits+recordings+release-groups+labels"
           "+isrcs+recording-level-rels+work-rels&fmt=json";
}

namespace {

[[nodiscard]] std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1U);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1U);
    }
    return text;
}

[[nodiscard]] std::string lowered(const std::string_view id) {
    std::string found{id};
    std::ranges::transform(found, found.begin(), [](const char character) {
        return character >= 'A' && character <= 'F' ? static_cast<char>(character - 'A' + 'a')
                                                     : character;
    });
    return found;
}

// The id a MusicBrainz link to a `kind` ("release", "release-group") names.
[[nodiscard]] std::optional<std::string> linked_id(const std::string_view text,
                                                   const std::string_view kind) {
    const auto host = "musicbrainz.org/" + std::string{kind} + "/";
    const auto at = text.find(host);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const auto before = text.substr(0U, at);
    // Only MusicBrainz's own site: musicbrainz.org, or one of its
    // subdomains (beta.), after a scheme or nothing.
    if (!before.empty() && !before.ends_with("://") && !before.ends_with(".")) {
        return std::nullopt;
    }
    const auto id = text.substr(at + host.size(), 36U);
    const auto rest = text.substr(std::min(text.size(), at + host.size() + 36U));
    if (!is_musicbrainz_id(id) ||
        (!rest.empty() && rest.front() != '/' && rest.front() != '?' && rest.front() != '#')) {
        return std::nullopt;
    }
    return lowered(id);
}

} // namespace

std::optional<std::string> release_id_in(std::string_view text) {
    text = trimmed(text);
    if (is_musicbrainz_id(text)) {
        return lowered(text);
    }
    return linked_id(text, "release");
}

std::optional<std::string> release_group_id_in(const std::string_view text) {
    return linked_id(trimmed(text), "release-group");
}

core::Result<std::string> build_release_group_releases_url(const std::string_view group_id) {
    if (!is_musicbrainz_id(group_id)) {
        return std::unexpected(service_error(core::ErrorCode::invalid_argument,
                                             "a MusicBrainz release group's releases need its id"));
    }
    return std::string{web_service_root} + "/release?release-group=" + std::string{group_id} +
           "&inc=artist-credits+labels+media+release-groups&fmt=json&limit=" +
           std::to_string(release_group_releases_limit);
}

core::Result<ReleaseSearchResult> parse_release_search(const std::string_view body,
                                                       const WebServiceLimits& limits) {
    auto root = parse_body(body, limits);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    ReleaseSearchResult result{
        // A search says "count", a release group's listing "release-count".
        .total_count = static_cast<std::size_t>(
            std::max(root->value(QStringLiteral("count"))
                         .toInt(root->value(QStringLiteral("release-count")).toInt(0)),
                     0)),
        .releases = {},
    };
    const auto releases = root->value(QStringLiteral("releases"));
    if (!releases.isArray()) {
        return std::unexpected(
            service_error(core::ErrorCode::backend, "the MusicBrainz search lacks a release list"));
    }
    const auto entries = releases.toArray();
    if (static_cast<std::size_t>(entries.size()) > limits.releases) {
        return std::unexpected(service_error(core::ErrorCode::limit_exceeded,
                                             "the MusicBrainz search exceeds the release limit"));
    }
    result.releases.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto& entry : entries) {
        if (!entry.isObject()) {
            return std::unexpected(service_error(core::ErrorCode::backend,
                                                 "a MusicBrainz release entry is not an object"));
        }
        auto release = parse_release_object(entry.toObject(), limits);
        if (!release) {
            return std::unexpected(std::move(release.error()));
        }
        result.releases.push_back(std::move(*release));
    }
    return result;
}

core::Result<Release> parse_release_lookup(const std::string_view body,
                                           const WebServiceLimits& limits) {
    auto root = parse_body(body, limits);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    return parse_release_object(*root, limits);
}

core::Result<std::string> build_cover_art_listing_url(const std::string_view release_id) {
    if (!is_musicbrainz_id(release_id)) {
        return std::unexpected(service_error(core::ErrorCode::invalid_argument,
                                             "a Cover Art Archive listing needs a release id"));
    }
    return "https://coverartarchive.org/release/" + std::string{release_id};
}

core::Result<CoverArtListing> parse_cover_art_listing(const std::string_view body,
                                                      const WebServiceLimits& limits) {
    auto root = parse_body(body, limits);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    const auto images = root->value(QStringLiteral("images"));
    if (!images.isArray()) {
        return std::unexpected(service_error(core::ErrorCode::backend,
                                             "the Cover Art Archive listing lacks an image list"));
    }
    const auto entries = images.toArray();
    if (static_cast<std::size_t>(entries.size()) > limits.cover_art_images) {
        return std::unexpected(
            service_error(core::ErrorCode::limit_exceeded,
                          "the Cover Art Archive listing exceeds the image limit"));
    }
    CoverArtListing listing;
    listing.images.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto& entry : entries) {
        if (!entry.isObject()) {
            continue;
        }
        const auto image = entry.toObject();
        CoverArtImage parsed{
            .id = {},
            .front = image.value(QStringLiteral("front")).toBool(false),
            .back = image.value(QStringLiteral("back")).toBool(false),
            .approved = image.value(QStringLiteral("approved")).toBool(false),
            .comment = bounded_text(image.value(QStringLiteral("comment")), limits),
            .types = {},
            .image_url = bounded_text(image.value(QStringLiteral("image")), limits),
            .thumbnail_url = {},
            .source = {},
        };
        const auto thumbnails = image.value(QStringLiteral("thumbnails")).toObject();
        parsed.thumbnail_url = bounded_text(thumbnails.value(QStringLiteral("250")), limits);
        if (parsed.thumbnail_url.empty()) {
            parsed.thumbnail_url = bounded_text(thumbnails.value(QStringLiteral("small")), limits);
        }
        // The archive serves image ids as JSON numbers; keep them as text.
        const auto identity = image.value(QStringLiteral("id"));
        parsed.id = identity.isDouble() ? std::to_string(identity.toInteger())
                                        : bounded_text(identity, limits);
        const auto types = image.value(QStringLiteral("types"));
        if (types.isArray()) {
            for (const auto& type : types.toArray()) {
                if (type.isString()) {
                    parsed.types.push_back(bounded_text(type, limits));
                }
            }
        }
        if (parsed.image_url.starts_with("http://")) {
            parsed.image_url.replace(0U, 7U, "https://");
        }
        if (parsed.thumbnail_url.starts_with("http://")) {
            parsed.thumbnail_url.replace(0U, 7U, "https://");
        }
        if (!parsed.thumbnail_url.starts_with("https://")) {
            parsed.thumbnail_url.clear();
        }
        if (parsed.image_url.starts_with("https://")) {
            listing.images.push_back(std::move(parsed));
        }
    }
    return listing;
}

core::Result<std::string> build_release_links_url(const std::string_view release_id) {
    if (!is_musicbrainz_id(release_id)) {
        return std::unexpected(service_error(core::ErrorCode::invalid_argument,
                                             "a release's links need its id"));
    }
    return std::string{web_service_root} + "/release/" + std::string{release_id} +
           "?inc=url-rels+release-groups&fmt=json";
}

core::Result<std::string> build_release_group_links_url(const std::string_view group_id) {
    if (!is_musicbrainz_id(group_id)) {
        return std::unexpected(service_error(core::ErrorCode::invalid_argument,
                                             "an album's links need its id"));
    }
    return std::string{web_service_root} + "/release-group/" + std::string{group_id} +
           "?inc=url-rels&fmt=json";
}

core::Result<ReleaseLinks> parse_release_links(const std::string_view body,
                                               const WebServiceLimits& limits) {
    auto root = parse_body(body, limits);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    ReleaseLinks links;
    for (const auto& relation : root->value(QStringLiteral("relations")).toArray()) {
        const auto url = relation.toObject()
                             .value(QStringLiteral("url"))
                             .toObject()
                             .value(QStringLiteral("resource"))
                             .toString();
        if (!url.isEmpty() && links.urls.size() < 200U) {
            links.urls.push_back(url.toStdString());
        }
    }
    const auto group =
        root->value(QStringLiteral("release-group")).toObject().value(QStringLiteral("id"));
    if (group.isString() && is_musicbrainz_id(group.toString().toStdString())) {
        links.release_group_id = group.toString().toStdString();
    }
    return links;
}

std::optional<CoverArtImage> bandcamp_cover(const std::string_view page) {
    const auto urls = bandcamp_cover_urls(page);
    if (!urls) {
        return std::nullopt;
    }
    return CoverArtImage{.id = "bandcamp",
                         .front = true,
                         .back = false,
                         .approved = true,
                         .comment = {},
                         .types = {"Front"},
                         .image_url = urls->image,
                         .thumbnail_url = urls->thumbnail,
                         .source = "Bandcamp"};
}

std::optional<std::size_t> select_front_cover(const CoverArtListing& listing) {
    const auto by = [&listing](const auto& predicate) -> std::optional<std::size_t> {
        for (std::size_t index = 0U; index < listing.images.size(); ++index) {
            if (predicate(listing.images[index])) {
                return index;
            }
        }
        return std::nullopt;
    };
    if (const auto flagged = by([](const CoverArtImage& image) { return image.front; })) {
        return flagged;
    }
    if (const auto typed = by([](const CoverArtImage& image) {
            return std::ranges::find(image.types, "Front") != image.types.end();
        })) {
        return typed;
    }
    if (const auto approved = by([](const CoverArtImage& image) { return image.approved; })) {
        return approved;
    }
    return listing.images.empty() ? std::optional<std::size_t>{} : std::optional{std::size_t{0U}};
}

} // namespace trackknife::musicbrainz
