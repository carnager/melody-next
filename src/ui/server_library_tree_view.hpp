// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QIcon>
#include <QPersistentModelIndex>
#include <QStyledItemDelegate>
#include <QTreeView>

#include <array>
#include <functional>
#include <vector>

namespace trackknife::ui {

// Shared library interaction and presentation. MPD defaults can be overridden
// with local labels, availability, and row presentation without sharing authority.
// The three inline actions are append, add next, and replace-and-play.
class ServerLibraryTreeView final : public QTreeView {
  public:
    explicit ServerLibraryTreeView(QWidget* parent = nullptr);

    void setActionCallback(std::function<void(const QModelIndex&, int)> callback);
    void setActionLabels(std::array<QString, 3> labels);
    void setActionsAvailable(std::function<bool(const QModelIndex&)> available);
    [[nodiscard]] bool actionsAvailable(const QModelIndex& index) const;

    [[nodiscard]] QModelIndex hoverIndex() const;
    [[nodiscard]] int hoverAction() const noexcept;
    void completePendingExpansions();
    void cancelPendingExpansions();

    [[nodiscard]] static QRect actionRect(const QRect& row_rect, int action);

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void startDrag(Qt::DropActions supported_actions) override;

  private:
    void toggleBranch(const QModelIndex& index);
    [[nodiscard]] int actionAt(const QModelIndex& index, const QPoint& position) const;

    QPersistentModelIndex hover_index_;
    QPersistentModelIndex pressed_index_;
    QPoint pressed_position_;
    std::vector<QPersistentModelIndex> pending_expansions_;
    int hover_action_{-1};
    int pressed_action_{-1};
    bool pressed_expanded_{false};
    bool drag_started_{false};
    std::function<void(const QModelIndex&, int)> action_callback_;
    std::function<bool(const QModelIndex&)> actions_available_;
    std::array<QString, 3> action_labels_{QStringLiteral("Append to live queue"),
                                          QStringLiteral("Insert next in live queue"),
                                          QStringLiteral("Replace queue and play")};
};

class ServerLibraryTreeDelegate final : public QStyledItemDelegate {
  public:
    // Shared row subtitle role for local and server-backed models.
    static constexpr int secondaryTextRole = Qt::UserRole + 6;

    struct Presentation {
        bool track{false};
        bool album{false};
        bool root{false};
        QString secondary;
        // ADR-0179: painted as stars over the album cover when non-zero.
        unsigned album_rating{0U};
    };
    ServerLibraryTreeDelegate(ServerLibraryTreeView* view, std::array<QIcon, 3> action_icons,
                              std::function<Presentation(const QModelIndex&)> presentation = {});

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

  private:
    ServerLibraryTreeView* view_;
    std::array<QIcon, 3> action_icons_;
    std::function<Presentation(const QModelIndex&)> presentation_;
};

} // namespace trackknife::ui
