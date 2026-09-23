// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace trackknife::ui {

// 2: album views no longer show artist, album and date columns by default;
// the album header says them. Version 1 layouts are migrated on reading.
inline constexpr int track_view_layout_schema_version = 2;

enum class TrackViewPresentation {
    albums_side_artwork,
    albums_header_artwork,
    plain_columns,
    compact_queue,
};

struct TrackViewColumnLayout {
    QString id;
    int width{100};
    bool visible{true};

    friend bool operator==(const TrackViewColumnLayout&, const TrackViewColumnLayout&) = default;
};

// Versioned presentation state for one queue/list binding. Column IDs are
// stable semantic names; their vector order is the visual order.
struct TrackViewLayout {
    int schema_version{track_view_layout_schema_version};
    TrackViewPresentation presentation{TrackViewPresentation::albums_side_artwork};
    std::vector<TrackViewColumnLayout> columns;

    friend bool operator==(const TrackViewLayout&, const TrackViewLayout&) = default;
};

[[nodiscard]] QByteArray serializeTrackViewLayout(const TrackViewLayout& layout);

// Requires unique registered columns, at least one visible column, bounded
// widths, and a known presentation. Newly registered columns append hidden.
// A version 1 album view is migrated: its artist, album and date columns are
// hidden, since every row under an album header repeated them.
// Unknown/newer state is rejected
// so callers can display a fallback without overwriting the original bytes.
[[nodiscard]] std::optional<TrackViewLayout>
deserializeTrackViewLayout(const QByteArray& bytes, const QStringList& registered_column_ids,
                           QString* error = nullptr);

} // namespace trackknife::ui
