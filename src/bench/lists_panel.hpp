// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QPointer>
#include <QTreeWidget>

#include <functional>
#include <vector>

class QDropEvent;

namespace trackknife::bench {

// ADR-0233: the lists as a pane instead of a tab bar -- every engine's, under
// its name, whether open in this window or not. A list is shown by choosing
// it, and tracks dropped on one are added to it; that the library, the lists
// and the tracks are all in sight at once is the point of having it.
class ListsPanel final : public QTreeWidget {
    Q_OBJECT

  public:
    struct List {
        QString id;
        QString name;
        bool saved{false};
        bool open{false};
        bool dirty{false};
        bool playing{false};
        // Unknown: -1.
        int tracks{-1};
    };
    struct Group {
        QString name;
        bool remote{false};
        std::vector<List> lists;
        // Said in place of lists when there are none: an engine away, say.
        QString note;
    };
    // A tab that is not a list -- a tag editor -- shown so it can be reached
    // with the tab bar hidden.
    struct Other {
        QPointer<QWidget> widget;
        QString name;
    };

    explicit ListsPanel(QWidget* parent = nullptr);

    // Everything anew; what was chosen stays chosen when it is still there.
    void present(const std::vector<Group>& groups, const std::vector<Other>& others,
                 const QString& current_id, const QWidget* current_other);

    // Whether a drop on a list is taken, and on Drop, taking it.
    using DropHandler = std::function<bool(QDropEvent*, bool remote, const QString& id)>;
    void setDropHandler(DropHandler handler) { drop_handler_ = std::move(handler); }

    // For the window and tests: the list under a row.
    [[nodiscard]] static QString idOf(const QTreeWidgetItem* item);
    [[nodiscard]] static bool remoteOf(const QTreeWidgetItem* item);
    [[nodiscard]] QTreeWidgetItem* itemFor(const QString& id) const;

  signals:
    void listChosen(bool remote, const QString& id);
    void otherChosen(QWidget* widget);

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    bool offer(QDropEvent* event);

    DropHandler drop_handler_;
    bool presenting_{false};
};

} // namespace trackknife::bench
