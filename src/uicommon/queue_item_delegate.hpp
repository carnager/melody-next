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

class QueueItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT

  public:
    static constexpr int album_header_height = 34;

    explicit QueueItemDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
    [[nodiscard]] bool isAlbumHeaderHit(const QModelIndex& index, int relative_y) const;
    [[nodiscard]] std::pair<int, int> albumRowRange(const QModelIndex& index) const;

  private:
    [[nodiscard]] bool beginsAlbum(const QModelIndex& index) const;
};

} // namespace trackknife::ui
