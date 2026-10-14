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
// 3 (ADR-0282): a tab names the list view it is drawn by; its columns'
// order and visibility are that view's, their widths the tab's. Versions 1
// and 2 are read for the workspace to migrate.
inline constexpr int track_view_layout_schema_version = 3;

enum class TrackViewPresentation {
    albums_side_artwork,
    albums_header_artwork,
    plain_columns,
    compact_queue,
};

// A presentation as it is saved: "albums-side-artwork" and so on.
[[nodiscard]] QString trackViewPresentationId(TrackViewPresentation presentation);
[[nodiscard]] std::optional<TrackViewPresentation> trackViewPresentationFromId(const QString& id);

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
    // ADR-0282: the list view's id; empty in a layout read from version 1
    // or 2, which the workspace migrates to one.
    QString view;
    // ADR-0283: the groups folded to their headers, by their keys.
    QStringList collapsed;

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
