// SPDX-License-Identifier: GPL-3.0-only

// ADR-0233: the lists as a pane beside the tracks -- every engine's, open
// here or not -- instead of as a tab bar.

#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/lists_panel.hpp"
#include "bench/playback_tab_widget.hpp"
#include "workspace/lists_catalog.hpp"

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
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <unordered_map>

namespace trackknife::bench {

void BenchMainWindow::buildListsPanel() {
    // ADR-0233: every engine's lists, open here or not -- what the library's
    // Playlists show (ADR-0296), and what saves and closes them.
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
}

bool BenchMainWindow::playlistsShown() const {
    return playlists_panel_ != nullptr && source_stack_ != nullptr &&
           source_stack_->currentWidget() == playlists_panel_;
}

void BenchMainWindow::followLibraryPlaylists() {
    if (playlistsShown() && !(playlists_engine_ == workspace_.libraryKey())) {
        playlists_engine_ = workspace_.libraryKey();
        fetchEngineLists();
        refreshListsPanel();
    }
}

void BenchMainWindow::fetchEngineLists() {
    if (lists_catalog_ != nullptr && playlistsShown()) {
        lists_catalog_->fetch();
    }
}

void BenchMainWindow::refreshListsPanel() {
    if (lists_present_timer_ != nullptr && playlistsShown()) {
        lists_present_timer_->start();
    }
}

void BenchMainWindow::presentListsPanel() {
    if (playlists_view_ == nullptr) {
        return;
    }
    // What is open here as this window has it, gathered again.
    {
        const QSignalBlocker blocker{lists_catalog_};
        lists_catalog_->present();
    }
    // ADR-0296: the library's playlists -- its engine's saved lists only, a
    // tab not saved is not a playlist -- headed Playlists.
    std::vector<ListsPanel::Group> shown;
    for (const auto& group : lists_catalog_->groups()) {
        if (!(group.engine == playlists_engine_)) {
            continue;
        }
        ListsPanel::Group playlists{.name = QStringLiteral("Playlists"),
                                    .engine = group.engine,
                                    .lists = {},
                                    .note = group.note};
        for (const auto& list : group.lists) {
            if (list.saved) {
                playlists.lists.push_back({.id = list.id,
                                           .name = list.name,
                                           .saved = true,
                                           .open = list.open,
                                           .dirty = list.dirty,
                                           .playing = list.playing,
                                           .tracks = list.tracks});
            }
        }
        if (playlists.lists.empty() && playlists.note.isEmpty()) {
            playlists.note = QStringLiteral("None yet: save a tab, or put playlist files in the "
                                            "playlist folder (Settings › Engines)");
        }
        shown.push_back(std::move(playlists));
    }
    const auto* current = currentListTab();
    const auto current_id =
        current != nullptr ? QString::fromStdString(current->document.id.to_string()) : QString{};
    playlists_view_->present(shown, {}, current_id, nullptr);
    // How many, and on which engine.
    const auto count = shown.empty() ? 0U : shown.front().lists.size();
    playlists_status_->setText(
        count == 0U ? QStringLiteral("No playlists on %1").arg(engineName(playlists_engine_))
        : count == 1U
            ? QStringLiteral("1 playlist on %1").arg(engineName(playlists_engine_))
            : QStringLiteral("%1 playlists on %2").arg(count).arg(engineName(playlists_engine_)));
}

void BenchMainWindow::showListsMenu(ListsPanel* panel, const QPoint& position) {
    auto* item = panel->itemAt(position);
    const auto id = ListsPanel::idOf(item);
    const auto engine = ListsPanel::engineOf(item);
    QMenu menu(panel);
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
                                QStringLiteral("Delete “%1”? Its tracks are not touched; its "
                                               "playlist file is set aside in .deleted.")
                                    .arg(name),
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
    menu.exec(panel->viewport()->mapToGlobal(position));
}

} // namespace trackknife::bench
