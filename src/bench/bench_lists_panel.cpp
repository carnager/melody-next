// SPDX-License-Identifier: GPL-3.0-only

// ADR-0233: the lists as a pane beside the tracks -- every engine's, open
// here or not -- instead of as a tab bar.

#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/lists_panel.hpp"
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
    lists_panel_->setDropHandler([this](QDropEvent* drop, const bool remote, const QString& id) {
        return dropOnPanelList(drop, remote, id);
    });
    connect(lists_panel_, &ListsPanel::listChosen, this,
            [this](const bool remote, const QString& id) { openEngineList(remote, id); });
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

    lists_fetch_timer_ = new QTimer(this);
    lists_fetch_timer_->setSingleShot(true);
    lists_fetch_timer_->setInterval(150);
    connect(lists_fetch_timer_, &QTimer::timeout, this, [this] {
        const auto ask = [this](EnginePlayback* engine, const int slot) {
            if (engine == nullptr || !engine->active()) {
                engine_lists_[slot].reset();
                engine_lists_error_[slot] = QStringLiteral("Not connected");
                return;
            }
            engine->request(QStringLiteral("list.all"), protocol::Json::object(),
                            [this, slot](const core::Result<protocol::Json>& answer) {
                                if (answer) {
                                    engine_lists_[slot] =
                                        answer->value("lists", std::vector<protocol::Json>{});
                                    engine_lists_error_[slot].clear();
                                } else {
                                    engine_lists_[slot].reset();
                                    engine_lists_error_[slot] =
                                        QString::fromStdString(answer.error().message);
                                }
                                refreshListsPanel();
                            });
        };
        ask(local_playback_, 0);
        ask(remote_playback_, 1);
        refreshListsPanel();
    });
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
    if (lists_fetch_timer_ != nullptr && listsInPanel()) {
        lists_fetch_timer_->start();
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
    const auto group_for = [this](const bool remote) {
        ListsPanel::Group group;
        group.remote = remote;
        group.name = !remote                               ? QStringLiteral("This computer")
                     : remote_catalogue_source_ != nullptr ? remote_catalogue_source_->name()
                                                           : QStringLiteral("Remote");
        const auto slot = remote ? 1 : 0;
        std::unordered_map<std::string, std::size_t> at;
        if (engine_lists_[slot]) {
            for (const auto& list : *engine_lists_[slot]) {
                const auto id = list.value("id", std::string{});
                at.emplace(id, group.lists.size());
                group.lists.push_back(ListsPanel::List{
                    .id = QString::fromStdString(id),
                    .name = QString::fromStdString(list.value("name", std::string{})),
                    .saved = list.value("kind", std::string{}) == "saved",
                    .tracks = list.value("tracks", -1),
                });
            }
        }
        // What is open here is as this window has it: newer than the engine's
        // until it is sent, and there even with the engine away.
        for (int index = 0; index < tabs_->count(); ++index) {
            auto* view = tabs_->widget(index);
            const auto id = view->property("bench-document-id").toString();
            auto* tab = id.isEmpty() ? nullptr : tabForDocument(id);
            if (tab == nullptr || tab->view != view || tab->document.remote != remote) {
                continue;
            }
            const auto known = at.find(id.toStdString());
            if (known == at.end()) {
                at.emplace(id.toStdString(), group.lists.size());
                group.lists.emplace_back();
                group.lists.back().id = id;
            }
            auto& list = group.lists[at.at(id.toStdString())];
            list.name = displayText(tab->document.name);
            list.saved = tab->document.kind == persistence::ListKind::saved;
            list.open = true;
            list.dirty = tab->document.dirty;
            list.playing = view->property("bench-playback-active").toBool();
            list.tracks = tab->model->rowCount();
        }
        // Saved lists first, by name: they are the ones kept for a reason.
        std::ranges::stable_sort(group.lists, [](const auto& left, const auto& right) {
            if (left.saved != right.saved) {
                return left.saved;
            }
            return left.saved && QString::localeAwareCompare(left.name, right.name) < 0;
        });
        if (group.lists.empty()) {
            group.note = engine_lists_error_[slot].isEmpty()
                             ? QStringLiteral("No lists")
                             : QStringLiteral("No lists: %1").arg(engine_lists_error_[slot]);
        }
        return group;
    };
    std::vector<ListsPanel::Group> groups;
    groups.push_back(group_for(false));
    if (remote_catalogue_source_ != nullptr) {
        groups.push_back(group_for(true));
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
    const bool remote = ListsPanel::remoteOf(item);
    QMenu menu(lists_panel_);
    if (!id.isEmpty()) {
        auto* tab = tabForDocument(id);
        const auto name = item->text(0).remove(QRegularExpression(QStringLiteral(" \\*$")));
        const auto show = [this, remote, id] { openEngineList(remote, id); };
        if (tab == nullptr) {
            menu.addAction(QStringLiteral("Open"), this, show);
        } else {
            auto* close = menu.addAction(QStringLiteral("Close"), this, [this, id] {
                if (auto* open = tabForDocument(id); open != nullptr) {
                    closeTabAt(tabs_->indexOf(open->view));
                }
            });
            close->setEnabled(!tab->document.pinned);
            if (tab->document.kind != persistence::ListKind::saved || tab->document.dirty) {
                menu.addAction(QStringLiteral("Save"), this, [this, remote, id] {
                    openEngineList(remote, id, [this] { saveCurrentList(); });
                });
            }
        }
        menu.addAction(QStringLiteral("Rename…"), this, [this, remote, id, name] {
            if (tabForDocument(id) != nullptr) {
                openEngineList(remote, id, [this] { renameCurrentList(); });
                return;
            }
            auto* engine = remote ? remote_playback_ : local_playback_;
            if (engine == nullptr) {
                return;
            }
            bool accepted = false;
            const auto chosen =
                QInputDialog::getText(this, QStringLiteral("Rename list"), QStringLiteral("Name:"),
                                      QLineEdit::Normal, name, &accepted)
                    .trimmed();
            if (!accepted || chosen.isEmpty() || chosen == name) {
                return;
            }
            engine->request(
                QStringLiteral("list.rename"),
                protocol::Json{{"id", id.toStdString()}, {"name", chosen.toStdString()}},
                [this](const core::Result<protocol::Json>& answer) {
                    if (!answer) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not rename the list: %1")
                                .arg(QString::fromStdString(answer.error().message)),
                            5'000);
                    }
                    fetchEngineLists();
                });
        });
        menu.addAction(QStringLiteral("Delete…"), this, [this, remote, id, name] {
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
                    [this, remote, id, remove](QAbstractButton* clicked) {
                        if (clicked != remove) {
                            return;
                        }
                        if (auto* open = tabForDocument(id); open != nullptr) {
                            // Deleted, so nothing unsaved to ask about.
                            open->document.dirty = false;
                            open->document.pinned = false;
                            closeTabAt(tabs_->indexOf(open->view));
                        }
                        auto* engine = remote ? remote_playback_ : local_playback_;
                        if (engine == nullptr) {
                            return;
                        }
                        engine->request(
                            QStringLiteral("list.delete"), protocol::Json{{"id", id.toStdString()}},
                            [this](const core::Result<protocol::Json>&) { fetchEngineLists(); });
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
