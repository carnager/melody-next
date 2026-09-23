// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QStyledItemDelegate>

namespace trackknife::bench {

// Up Next as a short stack of tracks rather than a table: the album's cover,
// the title with the artist beneath it, and the length at the end. It draws
// the title column; the view shows no other.
class UpNextDelegate final : public QStyledItemDelegate {
  public:
    static constexpr int row_height = 40;

    UpNextDelegate(int artist_column, int length_column, QObject* parent = nullptr);

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

  private:
    int artist_column_;
    int length_column_;
};

} // namespace trackknife::bench
