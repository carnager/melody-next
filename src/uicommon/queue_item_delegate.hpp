// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QStyledItemDelegate>

#include <utility>

namespace trackknife::ui {

class QueueItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT

  public:
    static constexpr int album_header_height = 30;

    explicit QueueItemDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
    [[nodiscard]] bool isAlbumHeaderHit(const QModelIndex& index, int relative_y) const;
    [[nodiscard]] std::pair<int, int> albumRowRange(const QModelIndex& index) const;

  private:
    [[nodiscard]] bool beginsAlbum(const QModelIndex& index) const;
    [[nodiscard]] QString albumTitle(const QModelIndex& index) const;
    [[nodiscard]] qint64 albumDurationMs(const QModelIndex& index) const;
};

} // namespace trackknife::ui
