// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "uicommon/track_grouping.hpp"

#include <QStyledItemDelegate>

#include <utility>

class QAbstractItemModel;

namespace trackknife::ui {

// Draws a group header into `rect`: the album bold, the details muted, and a
// hairline above it that separates it from the group before (`separated`).
void paintAlbumHeader(QPainter* painter, const QRect& rect, const QPalette& palette,
                      const QFont& font, const AlbumHeaderText& text, bool separated);

// The line between groups: the text colour, faint, so it reads as a line on
// a dark theme and a light one alike.
[[nodiscard]] QColor groupHairline(const QPalette& palette);
// What stands for a cover that is not there, as the Qt Quick window's
// InitialsTile draws it: a name's first letters on a tile coloured by that
// name -- the same name always the same colour, in either window -- or quiet
// grey when there is no name.
void paintInitialsTile(QPainter* painter, const QRect& rect, const QString& name,
                       const QPalette& palette);

class QueueItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT

  public:
    // The shared measures (track_grouping.hpp), by their older names.
    static constexpr int album_header_height = ui::album_header_height;
    static constexpr int loose_run_gap = ui::loose_run_gap;
    static constexpr int hairline_offset = ui::hairline_offset;
    static constexpr int disc_header_height = ui::disc_header_height;

    explicit QueueItemDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
    [[nodiscard]] bool isAlbumHeaderHit(const QModelIndex& index, int relative_y) const;
    [[nodiscard]] std::pair<int, int> albumRowRange(const QModelIndex& index) const;

  private:
    [[nodiscard]] bool beginsAlbum(const QModelIndex& index) const;
    [[nodiscard]] bool beginsLooseRun(const QModelIndex& index) const;
};

// "Disc 2" where a disc begins in an album of several, else empty.
[[nodiscard]] QString discStart(const QModelIndex& index);

} // namespace trackknife::ui
