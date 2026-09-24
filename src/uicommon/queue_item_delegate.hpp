// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QStyledItemDelegate>

#include <utility>

class QAbstractItemModel;

namespace trackknife::ui {

// What an album group's header says: the album, and a quieter line of
// "artist · year · N tracks · length" beside it.
struct AlbumHeaderText {
    QString album;
    QString details;
};

// The header text of the group starting at `first_row`, read from its rows.
[[nodiscard]] AlbumHeaderText albumHeaderText(const QAbstractItemModel& model, int first_row,
                                              int album_column, int date_column);

// Draws a group header into `rect`: the album bold, the details muted, and a
// hairline above it that separates it from the group before (`separated`).
void paintAlbumHeader(QPainter* painter, const QRect& rect, const QPalette& palette,
                      const QFont& font, const AlbumHeaderText& text, bool separated);

// The line between groups: the text colour, faint, so it reads as a line on
// a dark theme and a light one alike.
[[nodiscard]] QColor groupHairline(const QPalette& palette);

class QueueItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT

  public:
    static constexpr int album_header_height = 34;
    // Space above a run of lone tracks that follows an album, where its
    // hairline goes.
    static constexpr int loose_run_gap = 10;
    // Where a group's hairline is drawn, below the top of its header or gap,
    // so it does not touch the row above.
    static constexpr int hairline_offset = 5;
    // Above the first track of each disc, in an album of several.
    static constexpr int disc_header_height = 26;

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
