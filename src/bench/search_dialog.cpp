// SPDX-License-Identifier: GPL-3.0-only

#include "bench/search_dialog.hpp"

#include "bench/bench_main_window_helpers.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <utility>

namespace trackknife::bench {

SearchDialog::SearchDialog(const CatalogueSource& catalogues, TabAccess tab_access,
                           TechnicalsSink technicals_sink, QWidget* parent,
                           std::vector<OtherLibrary> others)
    : QDialog(parent),
      session_(new SearchSession(catalogues, std::move(tab_access), std::move(technicals_sink),
                                 std::move(others), this)) {
    setWindowTitle(QStringLiteral("Search"));
    setObjectName(QStringLiteral("bench-search-dialog"));
    setModal(false);
    resize(720, 480);

    auto* layout = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    scope_ = new QComboBox(this);
    scope_->setObjectName(QStringLiteral("bench-search-scope"));
    for (const auto& scope : session_->scopes()) {
        scope_->addItem(scope.label, scope.value);
    }
    top->addWidget(scope_);
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("bench-search-input"));
    input_->setClearButtonEnabled(true);
    input_->setPlaceholderText(QStringLiteral("Search…"));
    top->addWidget(input_, 1);
    query_mode_ = new QCheckBox(QStringLiteral("Query"), this);
    query_mode_->setObjectName(QStringLiteral("bench-search-query-mode"));
    query_mode_->setToolTip(
        QStringLiteral("Interpret the input as a tkq query, e.g. genre HAS jazz AND date "
                       "GREATER 1990. Library history: HISTORY(albumplaycount) EQUAL 0, or "
                       "HISTORY(albumdayssinceplayed) GREATER 180."));
    query_mode_->setChecked(session_->queryMode());
    top->addWidget(query_mode_);
    layout->addLayout(top);

    auto* presets = new QPushButton(QStringLiteral("Browse presets"), this);
    presets->setAutoDefault(false);
    presets->setObjectName(QStringLiteral("bench-search-presets"));
    presets->setToolTip(
        QStringLiteral("Choose a starting point; adjust the query and Save as… to keep it"));
    auto* preset_menu = new QMenu(presets);
    preset_menu->setObjectName(QStringLiteral("bench-search-presets-menu"));
    presets->setMenu(preset_menu);
    connect(preset_menu, &QMenu::aboutToShow, this,
            [this, preset_menu] { populatePresets(preset_menu); });

    auto* saved = new QHBoxLayout;
    saved->addWidget(presets);
    saved_searches_ = new QComboBox(this);
    saved_searches_->setObjectName(QStringLiteral("bench-search-saved"));
    saved_searches_->setAccessibleName(QStringLiteral("Saved searches"));
    saved_searches_->addItem(QStringLiteral("Saved searches…"));
    saved->addWidget(saved_searches_, 1);
    const auto saved_button = [this, saved](const QString& label, const QString& name) {
        auto* button = new QPushButton(label, this);
        button->setObjectName(name);
        saved->addWidget(button);
        return button;
    };
    save_search_ = saved_button(QStringLiteral("Save as…"), QStringLiteral("bench-search-save"));
    update_search_ = saved_button(QStringLiteral("Update"), QStringLiteral("bench-search-update"));
    update_search_->setToolTip(QStringLiteral(
        "Replace the selected saved search with the current query, mode, and scope"));
    rename_search_ = saved_button(QStringLiteral("Rename…"), QStringLiteral("bench-search-rename"));
    delete_search_ = saved_button(QStringLiteral("Delete…"), QStringLiteral("bench-search-delete"));
    layout->addLayout(saved);
    saved_status_ = new QLabel(session_->savedStatus(), this);
    saved_status_->setObjectName(QStringLiteral("bench-search-saved-status"));
    saved_status_->setTextFormat(Qt::PlainText);
    saved_status_->setWordWrap(true);
    layout->addWidget(saved_status_);
    connect(saved_searches_, &QComboBox::currentIndexChanged, session_,
            &SearchSession::selectSaved);
    connect(saved_searches_, &QComboBox::activated, session_, &SearchSession::useSaved);
    connect(save_search_, &QPushButton::clicked, this, [this] {
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("Save search"), QStringLiteral("Name:"),
                                  QLineEdit::Normal, session_->suggestedName(), &accepted)
                .trimmed();
        if (accepted && !name.isEmpty()) {
            session_->saveAs(name);
        }
    });
    connect(update_search_, &QPushButton::clicked, session_, &SearchSession::update);
    connect(rename_search_, &QPushButton::clicked, this, [this] {
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("Rename search"), QStringLiteral("Name:"),
                                  QLineEdit::Normal, session_->selectedName(), &accepted)
                .trimmed();
        if (accepted) {
            session_->rename(name);
        }
    });
    connect(delete_search_, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("Delete saved search"),
                                  session_->deleteQuestion(), QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) == QMessageBox::Yes) {
            session_->remove();
        }
    });

    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("bench-search-error"));
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);

    results_ = new QListWidget(this);
    results_->setObjectName(QStringLiteral("bench-search-results"));
    results_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    results_->setUniformItemSizes(true);
    results_->setLayoutMode(QListView::Batched);
    results_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(results_, 1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-search-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* buttons = new QHBoxLayout;
    open_button_ = new QPushButton(QStringLiteral("Open results in tab"), this);
    open_button_->setObjectName(QStringLiteral("bench-search-open-tab"));
    open_button_->setEnabled(false);
    connect(open_button_, &QPushButton::clicked, this,
            [this] { session_->openAll(LocalLibraryAction::new_list); });
    buttons->addWidget(open_button_);
    buttons->addStretch();
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    close->setObjectName(QStringLiteral("bench-search-close"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(input_, &QLineEdit::textChanged, session_, &SearchSession::setText);
    connect(scope_, &QComboBox::currentIndexChanged, session_, &SearchSession::setScope);
    connect(query_mode_, &QCheckBox::toggled, session_, &SearchSession::setQueryMode);
    connect(session_, &SearchSession::changed, this, &SearchDialog::sync);
    connect(session_, &SearchSession::savedChanged, this, &SearchDialog::syncSaved);
    connect(session_, &SearchSession::resultsChanged, this, &SearchDialog::syncResults);
    connect(session_, &SearchSession::rowsRequested, this, &SearchDialog::rowsRequested);

    // The standard destinations, selection-scoped (ADR-0153).
    connect(results_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        if (results_->selectedItems().isEmpty()) {
            return;
        }
        QMenu menu{this};
        menu.setObjectName(QStringLiteral("bench-search-context-menu"));
        const auto add = [this, &menu](const QString& text, const char* name,
                                       const LocalLibraryAction action) {
            auto* entry = menu.addAction(text);
            entry->setObjectName(QString::fromLatin1(name));
            connect(entry, &QAction::triggered, this,
                    [this, action] { openSelected(action); });
        };
        add(QStringLiteral("Add to current tab"), "action-search-append",
            LocalLibraryAction::append);
        add(QStringLiteral("Play next"), "action-search-next", LocalLibraryAction::next);
        add(QStringLiteral("Replace current tab"), "action-search-replace",
            LocalLibraryAction::replace);
        add(QStringLiteral("Open in new tab"), "action-search-new-tab",
            LocalLibraryAction::new_list);
        menu.exec(results_->mapToGlobal(point));
    });
    connect(results_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem*) { openSelected(LocalLibraryAction::append); });
    sync();
    syncSaved();
}

void SearchDialog::focusInput() { input_->setFocus(Qt::ShortcutFocusReason); }

void SearchDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    focusInput();
    // QDialog may restore the previous focus widget after showEvent returns.
    QTimer::singleShot(0, this, &SearchDialog::focusInput);
}

SearchDialog::~SearchDialog() = default;

void SearchDialog::populatePresets(QMenu* menu) {
    // clear() removes menu actions but can retain their owned submenus.
    const auto groups = menu->findChildren<QMenu*>(QString{}, Qt::FindDirectChildrenOnly);
    for (auto* group : groups)
        delete group;
    menu->clear();
    const auto presets = SearchSession::presetGroups();
    for (const auto& group : presets) {
        auto* submenu = menu->addMenu(group.topic);
        for (const auto& preset : group.presets) {
            auto* action = submenu->addAction(preset.title);
            action->setObjectName(QStringLiteral("search-preset-%1").arg(preset.id));
            connect(action, &QAction::triggered, this,
                    [this, index = preset.index] { usePreset(index); });
        }
    }
    if (presets.empty()) {
        menu->addAction(QStringLiteral("No presets supported in this scope"))->setEnabled(false);
    }
}

void SearchDialog::usePreset(const int preset) {
    const auto input = SearchSession::presetInput(preset);
    auto value = input.value;
    if (input.kind != SearchSession::PresetInput::Kind::none) {
        bool accepted = false;
        if (input.kind == SearchSession::PresetInput::Kind::integer) {
            value = QString::number(QInputDialog::getInt(this, input.title, input.prompt,
                                                         value.toInt(), input.minimum,
                                                         input.maximum, input.step, &accepted));
        } else {
            value = QInputDialog::getText(this, input.title, input.prompt, QLineEdit::Normal,
                                          value, &accepted);
        }
        if (!accepted)
            return;
    }
    session_->usePreset(preset, value);
    focusInput();
}

void SearchDialog::sync() {
    {
        const QSignalBlocker input_blocker{input_};
        const QSignalBlocker scope_blocker{scope_};
        const QSignalBlocker mode_blocker{query_mode_};
        if (input_->text() != session_->text()) {
            input_->setText(session_->text());
        }
        scope_->setCurrentIndex(session_->scope());
        query_mode_->setChecked(session_->queryMode());
    }
    error_->setText(session_->error());
    error_->setVisible(!session_->error().isEmpty());
    status_->setText(session_->status());
    open_button_->setEnabled(session_->canOpen());
}

void SearchDialog::syncSaved() {
    const auto names = session_->savedNames();
    const auto tips = session_->savedTooltips();
    {
        const QSignalBlocker blocker{saved_searches_};
        bool same = saved_searches_->count() == names.size();
        for (int index = 0; same && index < names.size(); ++index) {
            same = saved_searches_->itemText(index) == names.at(index) &&
                   saved_searches_->itemData(index, Qt::ToolTipRole).toString() == tips.at(index);
        }
        if (!same) {
            saved_searches_->clear();
            for (int index = 0; index < names.size(); ++index) {
                saved_searches_->addItem(names.at(index));
                if (!tips.at(index).isEmpty()) {
                    saved_searches_->setItemData(index, tips.at(index), Qt::ToolTipRole);
                }
            }
        }
        saved_searches_->setCurrentIndex(session_->savedIndex());
    }
    saved_status_->setText(session_->savedStatus());
    saved_searches_->setEnabled(session_->savedAvailable());
    save_search_->setEnabled(session_->canSave());
    update_search_->setEnabled(session_->canUpdate());
    rename_search_->setEnabled(session_->canRenameOrDelete());
    delete_search_->setEnabled(session_->canRenameOrDelete());
}

void SearchDialog::syncResults() {
    results_->clear();
    for (const auto& result : session_->results()) {
        auto* item = new QListWidgetItem(result.label, results_);
        if (result.heading) {
            item->setFlags(Qt::NoItemFlags);
            auto font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
    }
    open_button_->setEnabled(session_->canOpen());
}

void SearchDialog::openSelected(const LocalLibraryAction action) {
    std::vector<int> rows;
    for (const auto* item : results_->selectedItems()) {
        rows.push_back(results_->row(item));
    }
    session_->openRows(std::move(rows), action);
}

} // namespace trackknife::bench
