// SPDX-License-Identifier: GPL-3.0-only

// ADR-0233: the lists as a pane beside the tracks -- every engine's, open
// here or not -- instead of as a tab bar.

#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/lists_panel.hpp"
#include "workspace/lists_catalog.hpp"
#include "bench/playback_tab_widget.hpp"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <unordered_map>

namespace trackknife::bench {

void BenchMainWindow::buildListsPanel() {
    lists_pane_ = new QWidget(track_area_);
    lists_pane_->setObjectName(QStringLiteral("bench-lists-pane"));
    lists_pane_->setMinimumWidth(160);
    auto* layout = new QVBoxLayout(lists_pane_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(8, 4, 4, 2);
    auto* title = new QLabel(QStringLiteral("Lists"), lists_pane_);
    auto font = title->font();
    font.setBold(true);
    title->setFont(font);
    heading->addWidget(title, 1);
    auto* create = new QToolButton(lists_pane_);
    create->setObjectName(QStringLiteral("bench-lists-new"));
    create->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
    create->setToolTip(QStringLiteral("New list"));
    create->setAutoRaise(true);
    connect(create, &QToolButton::clicked, this, &BenchMainWindow::createList);
    heading->addWidget(create);
    layout->addLayout(heading);
    lists_panel_ = new ListsPanel(lists_pane_);
    layout->addWidget(lists_panel_, 1);
    lists_panel_->setDropHandler(
        [this](QDropEvent* drop, const EngineKey& engine, const QString& id) {
            return dropOnPanelList(drop, engine, id);
        });
    connect(lists_panel_, &ListsPanel::listChosen, this,
            [this](const EngineKey& engine, const QString& id) { openEngineList(engine, id); });
    connect(lists_panel_, &ListsPanel::otherChosen, this,
            [this](QWidget* widget) { tabs_->setCurrentWidget(widget); });
    connect(lists_panel_, &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showListsPanelMenu);
    track_area_->addWidget(lists_pane_);
    track_area_->setStretchFactor(0, 1);
    track_area_->setStretchFactor(1, 0);
    const auto width = QSettings{}.value(QStringLiteral("lists-panel/width"), 220).toInt();
    track_area_->setSizes({std::max(1, track_area_->width() - width), width});
    connect(track_area_, &QSplitter::splitterMoved, this, [this](const int, const int) {
        if (lists_pane_->isVisible()) {
            QSettings{}.setValue(QStringLiteral("lists-panel/width"), lists_pane_->width());
        }
    });

    lists_catalog_ = new ListsCatalog(workspace_, this);
    connect(lists_catalog_, &ListsCatalog::changed, this, &BenchMainWindow::refreshListsPanel);
    connect(lists_catalog_, &ListsCatalog::closeWanted, this, [this](const QString& id) {
        if (auto* open = tabForDocument(id); open != nullptr) {
            closeTabAt(tabs_->indexOf(open->view));
        }
    });
    connect(lists_catalog_, &ListsCatalog::saveWanted, this, [this] { saveCurrentList(); });
    lists_present_timer_ = new QTimer(this);
    lists_present_timer_->setSingleShot(true);
    lists_present_timer_->setInterval(0);
    connect(lists_present_timer_, &QTimer::timeout, this, &BenchMainWindow::presentListsPanel);
    applyListsDisplay();
}

bool BenchMainWindow::listsInPanel() const {
    return QSettings{}.value(QLatin1String(lists_display_key)).toString() ==
           QStringLiteral("panel");
}

void BenchMainWindow::applyListsDisplay() {
    if (lists_pane_ == nullptr) {
        return;
    }
    const bool panel = listsInPanel();
    tabs_->tabBar()->setVisible(!panel);
    lists_pane_->setVisible(panel);
    if (lists_panel_action_ != nullptr) {
        const QSignalBlocker blocker{lists_panel_action_};
        lists_panel_action_->setChecked(panel);
    }
    if (panel) {
        fetchEngineLists();
    }
}

void BenchMainWindow::fetchEngineLists() {
    if (lists_catalog_ != nullptr && listsInPanel()) {
        lists_catalog_->fetch();
    }
}

void BenchMainWindow::refreshListsPanel() {
    if (lists_present_timer_ != nullptr && lists_pane_ != nullptr && !lists_pane_->isHidden()) {
        lists_present_timer_->start();
    }
}

void BenchMainWindow::presentListsPanel() {
    if (lists_panel_ == nullptr) {
        return;
    }
    // What is open here as this window has it, gathered again.
    {
        const QSignalBlocker blocker{lists_catalog_};
        lists_catalog_->present();
    }
    std::vector<ListsPanel::Group> groups;
    for (const auto& group : lists_catalog_->groups()) {
        ListsPanel::Group shown{.name = group.name, .engine = group.engine, .lists = {},
                                .note = group.note};
        for (const auto& list : group.lists) {
            shown.lists.push_back({.id = list.id,
                                   .name = list.name,
                                   .saved = list.saved,
                                   .open = list.open,
                                   .dirty = list.dirty,
                                   .playing = list.playing,
                                   .tracks = list.tracks});
        }
        groups.push_back(std::move(shown));
    }
    std::vector<ListsPanel::Other> others;
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* widget = tabs_->widget(index);
        const auto id = widget->property("bench-document-id").toString();
        if (id.isEmpty() || tabForDocument(id) == nullptr) {
            others.push_back(ListsPanel::Other{.widget = widget, .name = tabs_->tabText(index)});
        }
    }
    const auto* current = currentListTab();
    const auto current_id =
        current != nullptr ? QString::fromStdString(current->document.id.to_string()) : QString{};
    auto* current_widget = tabs_->currentWidget();
    const bool other_current = current == nullptr && current_widget != nullptr &&
                               std::ranges::any_of(others, [&](const auto& other) {
                                   return other.widget == current_widget;
                               });
    lists_panel_->present(groups, others, current_id, other_current ? current_widget : nullptr);
}

void BenchMainWindow::showListsPanelMenu(const QPoint& position) {
    auto* item = lists_panel_->itemAt(position);
    const auto id = ListsPanel::idOf(item);
    const auto engine = ListsPanel::engineOf(item);
    QMenu menu(lists_panel_);
    if (!id.isEmpty()) {
        auto* tab = tabForDocument(id);
        const auto name = item->text(0).remove(QRegularExpression(QStringLiteral(" \\*$")));
        if (tab == nullptr) {
            menu.addAction(QStringLiteral("Open"), this,
                           [this, engine, id] { lists_catalog_->open(engine, id); });
        } else {
            auto* close = menu.addAction(QStringLiteral("Close"), this,
                                         [this, engine, id] { lists_catalog_->close(engine, id); });
            close->setEnabled(!tab->document.pinned);
            if (tab->document.kind != persistence::ListKind::saved || tab->document.dirty) {
                menu.addAction(QStringLiteral("Save"), this,
                               [this, engine, id] { lists_catalog_->save(engine, id); });
            }
        }
        menu.addAction(QStringLiteral("Rename…"), this, [this, engine, id, name] {
            bool accepted = false;
            const auto chosen =
                QInputDialog::getText(this, QStringLiteral("Rename list"), QStringLiteral("Name:"),
                                      QLineEdit::Normal, name, &accepted)
                    .trimmed();
            if (accepted && !chosen.isEmpty() && chosen != name) {
                lists_catalog_->rename(engine, id, chosen);
            }
        });
        menu.addAction(QStringLiteral("Delete…"), this, [this, engine, id, name] {
            auto* question =
                new QMessageBox(QMessageBox::Question, QStringLiteral("Delete list"),
                                QStringLiteral("Delete “%1”? Its files are not touched.").arg(name),
                                QMessageBox::Cancel, this);
            question->setObjectName(QStringLiteral("bench-lists-delete"));
            question->setAttribute(Qt::WA_DeleteOnClose);
            question->setOption(QMessageBox::Option::DontUseNativeDialog);
            auto* remove =
                question->addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
            remove->setObjectName(QStringLiteral("bench-lists-delete-confirm"));
            connect(question, &QMessageBox::buttonClicked, this,
                    [this, engine, id, remove](QAbstractButton* clicked) {
                        if (clicked == remove) {
                            lists_catalog_->remove(engine, id);
                        }
                    });
            question->open();
        });
        menu.addSeparator();
    }
    menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("New list"), this,
                   &BenchMainWindow::createList);
    menu.exec(lists_panel_->viewport()->mapToGlobal(position));
}

} // namespace trackknife::bench
