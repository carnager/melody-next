// SPDX-License-Identifier: GPL-3.0-only

#include "bench/search_dialog.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/persistence/tkq_row.hpp"

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
namespace {

// Results are listed in full up to this bound, which exists only so a
// query matching most of the library cannot make the dialog crawl. The
// list is uniform-height and filled in one batch, so it stays responsive
// well past the few thousand a real search returns.
constexpr int result_display_limit = 20'000;
// Words in a library show the first of each kind; the rest is a narrower
// search away, and a list of hundreds of artists is not a search result.
constexpr std::size_t grouped_limit = 20U;
constexpr std::size_t grouped_track_limit = 200U;
// What a result row is, for opening it: an index into the artists, albums
// or tracks found. Headings carry neither.
constexpr int result_kind_role = Qt::UserRole;
constexpr int result_index_role = Qt::UserRole + 1;

// The dialog's technical-need test mirrors the planner's pseudo-field
// vocabulary without depending on persistence internals.
[[nodiscard]] bool references_technicals(const query::CompiledTkq& compiled) {
    static const std::set<std::string> technicals{"codec", "samplerate", "bitspersample",
                                                  "channels", "lengthms"};
    for (const auto& name : compiled.field_dependencies()) {
        if (technicals.contains(metadata::canonicalize_field_name(name))) {
            return true;
        }
    }
    const auto program_needs = [](const titleformat::Program& program) {
        return !program.technicalDependencies().empty();
    };
    if (std::ranges::any_of(compiled.programs, program_needs)) {
        return true;
    }
    return compiled.sort && program_needs(compiled.sort->program);
}

[[nodiscard]] std::string result_label(const LocalTrackRow& row) {
    auto label = row.artist.empty() ? std::string{"Unknown artist"} : row.artist;
    label += " — ";
    if (!row.track_number.empty()) {
        label += row.track_number + ". ";
    }
    label += row.title;
    return label;
}

} // namespace

SearchDialog::SearchDialog(const CatalogueSource& catalogues, TabAccess tab_access,
                           TechnicalsSink technicals_sink, QWidget* parent,
                           const CatalogueSource* remote, QString remote_label)
    : QDialog(parent), catalogues_(&catalogues), remote_(remote),
      tab_access_(std::move(tab_access)), technicals_sink_(std::move(technicals_sink)) {
    setWindowTitle(QStringLiteral("Search"));
    setObjectName(QStringLiteral("bench-search-dialog"));
    setModal(false);
    resize(720, 480);

    auto* layout = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    scope_ = new QComboBox(this);
    scope_->setObjectName(QStringLiteral("bench-search-scope"));
    scope_->addItem(QStringLiteral("This computer"), QStringLiteral("local"));
    scope_->addItem(QStringLiteral("Current tab"), QStringLiteral("tab"));
    if (remote_ != nullptr) {
        // ADR-0227: the remote's library is searched there, and what is found
        // opens in a remote tab.
        scope_->addItem(remote_label.isEmpty() ? QStringLiteral("Remote library") : remote_label,
                        QStringLiteral("remote"));
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
    query_mode_->setChecked(QSettings{}.value(QStringLiteral("search/query-mode"), false).toBool());
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
    saved_status_ = new QLabel(QStringLiteral("Loading saved searches…"), this);
    saved_status_->setObjectName(QStringLiteral("bench-search-saved-status"));
    saved_status_->setTextFormat(Qt::PlainText);
    saved_status_->setWordWrap(true);
    layout->addWidget(saved_status_);
    connect(saved_searches_, &QComboBox::activated, this, &SearchDialog::useSavedSearch);
    connect(save_search_, &QPushButton::clicked, this, [this] { saveSearch(false); });
    connect(update_search_, &QPushButton::clicked, this, [this] { saveSearch(true); });
    connect(rename_search_, &QPushButton::clicked, this, &SearchDialog::renameSearch);
    connect(delete_search_, &QPushButton::clicked, this, &SearchDialog::deleteSearch);
    connect(&catalog_watcher_, &QFutureWatcherBase::finished, this,
            &SearchDialog::finishSavedSearches);

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
            [this] { openResults(LocalLibraryAction::new_list, false); });
    buttons->addWidget(open_button_);
    buttons->addStretch();
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    close->setObjectName(QStringLiteral("bench-search-close"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(200);
    connect(debounce_, &QTimer::timeout, this, &SearchDialog::startSearch);
    connect(input_, &QLineEdit::textChanged, this, &SearchDialog::scheduleSearch);
    connect(scope_, &QComboBox::currentIndexChanged, this, &SearchDialog::scheduleSearch);
    connect(query_mode_, &QCheckBox::toggled, this, [this](const bool enabled) {
        QSettings{}.setValue(QStringLiteral("search/query-mode"), enabled);
        error_->hide();
        scheduleSearch();
    });
    connect(&watcher_, &QFutureWatcherBase::finished, this, &SearchDialog::finishSearch);

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
                    [this, action] { openResults(action, true); });
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
            [this](QListWidgetItem*) { openResults(LocalLibraryAction::append, true); });
    loadSavedSearches();
}

void SearchDialog::focusInput() { input_->setFocus(Qt::ShortcutFocusReason); }

void SearchDialog::followLibrary(const bool remote) {
    if (!databaseScope()) {
        return;
    }
    const auto wanted = scope_->findData(remote ? QStringLiteral("remote") : QStringLiteral("local"));
    if (wanted >= 0 && wanted != scope_->currentIndex()) {
        scope_->setCurrentIndex(wanted);
    }
}

void SearchDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    focusInput();
    // QDialog may restore the previous focus widget after showEvent returns.
    QTimer::singleShot(0, this, &SearchDialog::focusInput);
}

void SearchDialog::populatePresets(QMenu* menu) {
    // clear() removes menu actions but can retain their owned submenus.
    const auto groups = menu->findChildren<QMenu*>(QString{}, Qt::FindDirectChildrenOnly);
    for (auto* group : groups)
        delete group;
    menu->clear();
    QString topic;
    QMenu* group = nullptr;
    int available = 0;
    for (const auto& preset : query::search_presets()) {
        const auto source = query::preset_query(preset, preset.example);
        if (!source)
            continue;
        const auto compiled = query::compile_tkq(*source);
        if (!compiled)
            continue;
        const auto label = displayText(std::string{preset.topic});
        if (topic != label) {
            group = menu->addMenu(label);
            topic = label;
        }
        auto* action = group->addAction(displayText(std::string{preset.title}));
        action->setObjectName(
            QStringLiteral("search-preset-%1").arg(displayText(std::string{preset.id})));
        connect(action, &QAction::triggered, this, [this, preset] { usePreset(preset); });
        ++available;
    }
    if (available == 0) {
        menu->addAction(QStringLiteral("No presets supported in this scope"))->setEnabled(false);
    }
}

void SearchDialog::usePreset(const query::SearchPreset& preset) {
    QString value = displayText(std::string{preset.example});
    if (preset.input != query::PresetInput::none) {
        bool accepted = false;
        if (preset.input == query::PresetInput::integer) {
            const int number = QInputDialog::getInt(this, displayText(std::string{preset.title}),
                                                    displayText(std::string{preset.prompt}),
                                                    value.toInt(), preset.minimum, preset.maximum,
                                                    preset.id == "decade" ? 10 : 1, &accepted);
            value = QString::number(number);
        } else {
            value = QInputDialog::getText(this, displayText(std::string{preset.title}),
                                          displayText(std::string{preset.prompt}),
                                          QLineEdit::Normal, value, &accepted);
        }
        if (!accepted)
            return;
    }
    const auto source = query::preset_query(preset, utf8Bytes(value));
    if (!source) {
        error_->setText(displayText(source.error().message));
        error_->show();
        return;
    }
    // A built-in is a starting point, never an implicit edit to a saved search.
    saved_searches_->setCurrentIndex(0);
    updateSavedSearchButtons();
    query_mode_->setChecked(true);
    input_->setText(displayText(*source));
    focusInput();
}

SearchDialog::~SearchDialog() {
    cancellation_.request_cancellation();
    if (searching_) {
        watcher_.waitForFinished();
    }
}

std::optional<persistence::SavedSearch> SearchDialog::selectedSearch() const {
    const auto index = saved_searches_->currentIndex() - 1;
    return index < 0 || static_cast<std::size_t>(index) >= catalog_.size()
               ? std::nullopt
               : std::optional{catalog_[static_cast<std::size_t>(index)]};
}

void SearchDialog::updateSavedSearchButtons() {
    const auto selected = selectedSearch();
    const bool available = catalog_ready_ && !catalog_busy_;
    const bool has_input = !input_->text().trimmed().isEmpty();
    saved_searches_->setEnabled(available);
    save_search_->setEnabled(available && has_input);
    const bool changed =
        selected &&
        (selected->expression != utf8Bytes(input_->text().trimmed()) ||
         selected->dialect != (query_mode_->isChecked() ? "tkq-1" : "words-1") ||
         selected->scope != (databaseScope() ? persistence::SavedSearchScope::library
                                             : persistence::SavedSearchScope::current_tab));
    update_search_->setEnabled(available && has_input && changed);
    rename_search_->setEnabled(available && selected.has_value());
    delete_search_->setEnabled(available && selected.has_value());
}

void SearchDialog::loadSavedSearches(std::optional<persistence::SavedSearch> write, bool remove) {
    if (catalog_busy_) {
        return;
    }
    catalog_busy_ = true;
    catalog_selection_ = write && !remove ? std::optional{write->id} : std::nullopt;
    saved_status_->setText(write ? QStringLiteral("Saving search definitions…")
                                 : QStringLiteral("Loading saved searches…"));
    updateSavedSearchButtons();
    catalog_watcher_.setFuture(
        QtConcurrent::run([database = catalogues_->database(), write = std::move(write),
                           remove]() -> core::Result<std::vector<persistence::SavedSearch>> {
            // ADR-0220: ask the core, do not open its database.
            auto workspace = engine::Workspace::open(database);
            if (!workspace) {
                return std::unexpected(workspace.error());
            }
            if (write) {
                auto result =
                    remove ? workspace->remove_search(*write) : workspace->save_search(*write);
                if (!result) {
                    return std::unexpected(result.error());
                }
            }
            return workspace->load_saved_searches();
        }));
}

void SearchDialog::finishSavedSearches() {
    catalog_busy_ = false;
    auto result = catalog_watcher_.result();
    if (!result) {
        saved_status_->setText(displayText(result.error().message));
        updateSavedSearchButtons();
        return;
    }
    catalog_ready_ = true;
    catalog_ = std::move(*result);
    const QSignalBlocker blocker{saved_searches_};
    saved_searches_->clear();
    saved_searches_->addItem(QStringLiteral("Saved searches…"));
    int selected = 0;
    for (const auto& search : catalog_) {
        saved_searches_->addItem(displayText(search.name));
        saved_searches_->setItemData(
            saved_searches_->count() - 1,
            QStringLiteral("%1 · %2\n%3")
                .arg(search.scope == persistence::SavedSearchScope::current_tab
                         ? QStringLiteral("Current tab")
                         : QStringLiteral("Library database"))
                .arg(search.dialect == "tkq-1" ? QStringLiteral("Query") : QStringLiteral("Words"))
                .arg(displayText(search.expression)),
            Qt::ToolTipRole);
        if (catalog_selection_ == search.id) {
            selected = saved_searches_->count() - 1;
        }
    }
    saved_searches_->setCurrentIndex(selected);
    saved_status_->setText(QStringLiteral("%1 saved search%2 · select one to run it again")
                               .arg(catalog_.size())
                               .arg(catalog_.size() == 1U ? QString{} : QStringLiteral("es")));
    updateSavedSearchButtons();
}

void SearchDialog::useSavedSearch(int index) {
    if (index <= 0 || catalog_busy_ || static_cast<std::size_t>(index) > catalog_.size()) {
        updateSavedSearchButtons();
        return;
    }
    const auto& search = catalog_[static_cast<std::size_t>(index - 1)];
    {
        const QSignalBlocker input_blocker{input_};
        const QSignalBlocker scope_blocker{scope_};
        const QSignalBlocker mode_blocker{query_mode_};
        input_->setText(displayText(search.expression));
        // A search saved against the retired server library runs against the
        // library database, which is the same question asked of the library
        // that remains.
        // A saved library search runs in whichever library is chosen.
        if (search.scope == persistence::SavedSearchScope::current_tab) {
            scope_->setCurrentIndex(scope_->findData(QStringLiteral("tab")));
        } else if (!databaseScope()) {
            scope_->setCurrentIndex(scope_->findData(QStringLiteral("local")));
        }
        query_mode_->setChecked(search.dialect == "tkq-1");
    }
    QSettings{}.setValue(QStringLiteral("search/query-mode"), query_mode_->isChecked());
    error_->hide();
    scheduleSearch();
}

void SearchDialog::saveSearch(bool update) {
    if (!catalog_ready_ || catalog_busy_ || !compileInput()) {
        return;
    }
    auto search = update ? selectedSearch()
                         : std::optional{persistence::SavedSearch{
                               .id = core::StableId::random(), .name = {}, .expression = {}}};
    if (!search) {
        return;
    }
    if (!update) {
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("Save search"), QStringLiteral("Name:"),
                                  QLineEdit::Normal, input_->text().trimmed().left(80), &accepted)
                .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        search->name = utf8Bytes(name);
    }
    search->expression = utf8Bytes(input_->text().trimmed());
    search->dialect = query_mode_->isChecked() ? "tkq-1" : "words-1";
    search->scope = databaseScope() ? persistence::SavedSearchScope::library
                                    : persistence::SavedSearchScope::current_tab;
    loadSavedSearches(*search);
}

void SearchDialog::renameSearch() {
    auto search = selectedSearch();
    if (!search || catalog_busy_) {
        return;
    }
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("Rename search"), QStringLiteral("Name:"),
                              QLineEdit::Normal, displayText(search->name), &accepted)
            .trimmed();
    if (!accepted || name.isEmpty() || utf8Bytes(name) == search->name) {
        return;
    }
    search->name = utf8Bytes(name);
    loadSavedSearches(*search);
}

void SearchDialog::deleteSearch() {
    const auto search = selectedSearch();
    if (!search || catalog_busy_) {
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("Delete saved search"),
            QStringLiteral(
                "Delete “%1” from saved searches? Existing result tabs remain available.")
                .arg(displayText(search->name)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    loadSavedSearches(*search, true);
}

void SearchDialog::watchCurrentModel(QAbstractItemModel* model) {
    for (const auto& connection : current_model_connections_)
        disconnect(connection);
    current_model_connections_.clear();
    const auto changed = [this] {
        if (!databaseScope())
            scheduleSearch();
    };
    if (model) {
        current_model_connections_.push_back(
            connect(model, &QAbstractItemModel::modelReset, this, changed));
        current_model_connections_.push_back(
            connect(model, &QAbstractItemModel::rowsInserted, this, changed));
        current_model_connections_.push_back(
            connect(model, &QAbstractItemModel::rowsRemoved, this, changed));
        current_model_connections_.push_back(
            connect(model, &QAbstractItemModel::layoutChanged, this, changed));
        current_model_connections_.push_back(connect(
            model, &QAbstractItemModel::dataChanged, this,
            [changed](const QModelIndex& first, const QModelIndex&, const QList<int>& roles) {
                // Playback markers and lazy history-cell repaints are not source edits.
                if (roles.empty() || roles.contains(ui::track_rating_role) ||
                    (roles.contains(Qt::DisplayRole) &&
                     first.column() < ui::track_play_count_column))
                    changed();
            }));
    }
    changed();
}

bool SearchDialog::databaseScope() const {
    return scope_->currentData().toString() != QStringLiteral("tab");
}

bool SearchDialog::remoteScope() const {
    return scope_->currentData().toString() == QStringLiteral("remote");
}

const CatalogueSource* SearchDialog::scopeCatalogues() const {
    return remoteScope() ? remote_ : catalogues_;
}

void SearchDialog::scheduleSearch() {
    updateSavedSearchButtons();
    debounce_->stop();
    results_->clear();
    result_rows_.clear();
    result_artists_.clear();
    result_albums_.clear();
    open_button_->setEnabled(false);
    ++generation_;
    cancellation_.request_cancellation();
    cancellation_ = core::CancellationSource{};
    if (input_->text().trimmed().isEmpty()) {
        status_->clear();
        error_->hide();
        return;
    }
    status_->setText(QStringLiteral("Searching…"));
    debounce_->start();
}

std::optional<query::CompiledTkq> SearchDialog::compileInput() {
    const auto text = utf8Bytes(input_->text().trimmed());
    auto compiled =
        query_mode_->isChecked() ? query::compile_tkq(text) : query::compile_tkq_word_search(text);
    if (!compiled) {
        error_->setText(displayText(compiled.error().message));
        error_->show();
        status_->setText(QStringLiteral("Invalid query"));
        return std::nullopt;
    }
    error_->hide();
    return std::move(*compiled);
}

void SearchDialog::startSearch() {
    if (searching_) {
        debounce_->start();
        return;
    }
    search_job_generation_ = generation_;
    auto compiled = compileInput();
    if (!compiled) {
        return;
    }
    result_query_ = input_->text().trimmed();
    result_remote_ = remoteScope();
    if (databaseScope() && !query_mode_->isChecked()) {
        // Words in a library: what the library panel's filter finds, by kind
        // -- artists, albums, tracks -- each a short indexed query, so the
        // results follow the typing.
        searching_ = true;
        watcher_.setFuture(QtConcurrent::run([catalogues = scopeCatalogues(),
                                              text = utf8Bytes(result_query_),
                                              token = cancellation_.token()]() {
            Outcome outcome;
            outcome.grouped = true;
            auto handle = catalogues->open();
            auto& catalogue = *handle;
            const auto find = [&](const persistence::LibraryEntryKind kind,
                                  const std::size_t limit)
                -> std::optional<std::vector<persistence::LibraryEntry>> {
                persistence::LibraryQuery request;
                request.kind = kind;
                request.text = text;
                request.limit = limit;
                auto page = catalogue.query(request, token);
                if (!page) {
                    outcome.error = displayText(page.error().message);
                    return std::nullopt;
                }
                return std::move(page->entries);
            };
            auto artists = find(persistence::LibraryEntryKind::artist, grouped_limit);
            auto albums = artists ? find(persistence::LibraryEntryKind::album, grouped_limit)
                                  : std::nullopt;
            auto tracks = albums ? find(persistence::LibraryEntryKind::track, grouped_track_limit)
                                 : std::nullopt;
            if (!tracks) {
                return outcome;
            }
            outcome.artists = std::move(*artists);
            outcome.albums = std::move(*albums);
            std::vector<std::string> paths;
            paths.reserve(tracks->size());
            for (auto& track : *tracks) {
                paths.push_back(std::move(track.key));
            }
            auto cached = catalogue.cached_tracks(paths, token);
            if (!cached) {
                outcome.error = displayText(cached.error().message);
                return outcome;
            }
            for (auto& track : *cached) {
                auto row = cached_library_row(std::move(track));
                outcome.labels.push_back(result_label(row));
                outcome.rows.push_back(std::move(row));
            }
            return outcome;
        }));
        return;
    }
    if (databaseScope()) {
        searching_ = true;
        watcher_.setFuture(
            QtConcurrent::run([catalogues = scopeCatalogues(),
                               shared = std::make_shared<query::CompiledTkq>(std::move(*compiled)),
                               token = cancellation_.token()]() {
                Outcome outcome;
                // ADR-0220: ask the core, do not open its database.
                auto handle = catalogues->open();
                auto& catalogue = *handle;
                auto paths = catalogue.filter_paths(*shared, token);
                if (!paths) {
                    outcome.error = displayText(paths.error().message);
                    return outcome;
                }
                auto cached = catalogue.cached_tracks(*paths, token);
                if (!cached) {
                    outcome.error = displayText(cached.error().message);
                    return outcome;
                }
                for (auto& track : *cached) {
                    auto row = cached_library_row(std::move(track));
                    if (outcome.labels.size() < static_cast<std::size_t>(result_display_limit)) {
                        outcome.labels.push_back(result_label(row));
                    }
                    outcome.rows.push_back(std::move(row));
                }
                return outcome;
            }));
        return;
    }
    auto snapshot = tab_access_ ? tab_access_() : std::nullopt;
    if (!snapshot) {
        status_->setText(QStringLiteral("No local tab is active."));
        results_->clear();
        open_button_->setEnabled(false);
        return;
    }
    searching_ = true;
    const auto needs_technicals = references_technicals(*compiled);
    watcher_.setFuture(
        QtConcurrent::run([catalogues = catalogues_, rows = std::move(snapshot->rows),
                           shared = std::make_shared<query::CompiledTkq>(std::move(*compiled)),
                           needs_technicals, token = cancellation_.token()]() mutable {
            Outcome outcome;
            if (rows.size() > 100'000U) {
                outcome.error =
                    QStringLiteral("Current-tab searches support at most 100,000 rows.");
                return outcome;
            }
            const bool needs_history = (shared->sort && !shared->sort->history.empty()) ||
                                       std::ranges::any_of(shared->predicates, [](const auto& p) {
                                           return p.operand == query::TkqOperandKind::history;
                                       });
            std::vector<std::array<std::int64_t, 6>> histories;
            if (needs_history) {
                // ADR-0220: ask the core, do not open its database.
                auto handle = catalogues->open();
                auto& catalogue = *handle;
                std::vector<persistence::LibraryHistorySource> sources;
                for (const auto& row : rows) {
                    if (token.is_cancellation_requested()) {
                        outcome.error = QStringLiteral("Search cancelled");
                        return outcome;
                    }
                    persistence::ListItem source;
                    source.source = persistence::ListSource::local;
                    source.source_reference = row.raw_path;
                    source.source_revision = row.source_revision;
                    source.source_selection = persistence::ListItemSourceSelection{
                        row.selection.stream_index, row.selection.subsong_index};
                    if (row.segment)
                        source.segment = persistence::ListItemSegment{row.segment->start_sample,
                                                                      row.segment->end_sample};
                    sources.push_back({std::move(source),
                                       row.album.empty() ? std::string{} : row.album_rating_hash});
                }
                auto loaded = catalogue.history_facts(sources, token);
                if (!loaded) {
                    outcome.error = displayText(loaded.error().message);
                    return outcome;
                }
                histories = std::move(*loaded);
            }
            // ADR-0153: probe exactly the rows a technical query needs and
            // report the results back so the next search is instant.
            std::map<std::string, std::optional<LocalTrackTechnicals>> probed;
            if (needs_technicals) {
                for (auto& row : rows) {
                    if (row.technicals || token.is_cancellation_requested()) {
                        continue;
                    }
                    auto cached = probed.find(row.raw_path);
                    if (cached == probed.end()) {
                        std::optional<LocalTrackTechnicals> technicals;
                        if (auto probe = formats::probe_local_media(row.raw_path, token);
                            probe && probe->best_audio_stream) {
                            const auto found =
                                std::ranges::find(probe->audio_streams, *probe->best_audio_stream,
                                                  &formats::AudioStreamInfo::stream_index);
                            if (found != probe->audio_streams.end()) {
                                technicals = LocalTrackTechnicals{
                                    .codec = found->codec_name,
                                    .sample_rate = found->sample_rate,
                                    .bits = formats::bits_per_sample_hint(found->sample_format),
                                    .channels = found->channels,
                                    .bit_rate =
                                        found->bit_rate > 0 ? found->bit_rate : probe->bit_rate,
                                };
                            }
                        }
                        cached = probed.emplace(row.raw_path, std::move(technicals)).first;
                        ++outcome.scanned;
                    }
                    if (cached->second) {
                        row.technicals = *cached->second;
                    }
                }
                for (auto& [path, technicals] : probed) {
                    if (technicals) {
                        outcome.probed.emplace_back(path, *technicals);
                    }
                }
            }
            struct Keyed {
                std::string key;
                std::size_t position;
            };
            std::vector<Keyed> keyed;
            for (std::size_t position = 0U; position < rows.size(); ++position) {
                if (token.is_cancellation_requested()) {
                    outcome.error = QStringLiteral("Search cancelled");
                    return outcome;
                }
                const auto& row = rows[position];
                auto facts = persistence::make_tkq_row_facts(
                    row.metadata, row.title, row.artist, row.album, row.duration_ms,
                    row.technicals ? std::optional{persistence::TkqRowTechnicals{
                                         .codec = row.technicals->codec,
                                         .sample_rate = row.technicals->sample_rate,
                                         .bits = row.technicals->bits,
                                         .channels = row.technicals->channels}}
                                   : std::nullopt);
                // ADR-0179: tab rows carry their loaded content-identity ratings;
                // an unrated row keeps the facts' unrated default.
                if (row.rating > 0U) {
                    facts.rating = row.rating;
                }
                if (row.album_rating > 0U) {
                    facts.album_rating = row.album_rating;
                }
                if (needs_history)
                    facts.history = histories[position];
                if (!persistence::tkq_matches(*shared, facts, token)) {
                    continue;
                }
                std::string key;
                if (shared->sort) {
                    auto sort_key = persistence::tkq_sort_key(*shared, facts, token);
                    if (!sort_key) {
                        outcome.error = displayText(sort_key.error().message);
                        return outcome;
                    }
                    key = std::move(*sort_key);
                }
                keyed.push_back({std::move(key), position});
            }
            if (shared->sort) {
                const auto descending =
                    shared->sort->direction == query::TkqSortDirection::descending;
                std::ranges::stable_sort(
                    keyed, [descending](const Keyed& left, const Keyed& right) {
                        return descending ? right.key < left.key : left.key < right.key;
                    });
            }
            for (const auto& entry : keyed) {
                outcome.rows.push_back(std::move(rows[entry.position]));
                outcome.labels.push_back(result_label(outcome.rows.back()));
            }
            return outcome;
        }));
}

void SearchDialog::finishSearch() {
    searching_ = false;
    if (search_job_generation_ != generation_) {
        return;
    }
    auto outcome = watcher_.result();
    if (!outcome.error.isEmpty()) {
        status_->setText(outcome.error);
        return;
    }
    for (auto& [path, technicals] : outcome.probed) {
        if (technicals_sink_) {
            technicals_sink_(path, technicals);
        }
    }
    results_->clear();
    result_rows_ = std::move(outcome.rows);
    result_artists_ = std::move(outcome.artists);
    result_albums_ = std::move(outcome.albums);
    const auto add_item = [this](const QString& label, const int kind, const std::size_t index) {
        auto* item = new QListWidgetItem(label, results_);
        item->setData(result_kind_role, kind);
        item->setData(result_index_role, static_cast<qulonglong>(index));
    };
    const auto add_heading = [this](const QString& label) {
        auto* item = new QListWidgetItem(label, results_);
        item->setFlags(Qt::NoItemFlags);
        auto font = item->font();
        font.setBold(true);
        item->setFont(font);
    };
    const auto total = result_rows_.size();
    if (outcome.grouped) {
        const auto heading = [](const QString& kind, const std::size_t found,
                                const std::size_t limit) {
            return found >= limit ? QStringLiteral("%1 (first %2)").arg(kind).arg(found)
                                  : QStringLiteral("%1 (%2)").arg(kind).arg(found);
        };
        if (!result_artists_.empty()) {
            add_heading(heading(QStringLiteral("Artists"), result_artists_.size(), grouped_limit));
            for (std::size_t index = 0U; index < result_artists_.size(); ++index) {
                const auto& artist = result_artists_[index];
                add_item(QStringLiteral("%1 · %2 album%3")
                             .arg(displayText(artist.label))
                             .arg(artist.albums)
                             .arg(artist.albums == 1U ? QString{} : QStringLiteral("s")),
                         static_cast<int>(persistence::LibraryEntryKind::artist), index);
            }
        }
        if (!result_albums_.empty()) {
            add_heading(heading(QStringLiteral("Albums"), result_albums_.size(), grouped_limit));
            for (std::size_t index = 0U; index < result_albums_.size(); ++index) {
                const auto& album = result_albums_[index];
                add_item(QStringLiteral("%1 — %2")
                             .arg(displayText(album.artist), displayText(album.label)),
                         static_cast<int>(persistence::LibraryEntryKind::album), index);
            }
        }
        if (total > 0U) {
            add_heading(heading(QStringLiteral("Tracks"), total, grouped_track_limit));
        }
    }
    const auto shown = std::min<std::size_t>(outcome.labels.size(),
                                             static_cast<std::size_t>(result_display_limit));
    for (std::size_t index = 0U; index < shown; ++index) {
        add_item(displayText(outcome.labels[index]),
                 static_cast<int>(persistence::LibraryEntryKind::track), index);
    }
    open_button_->setEnabled(total > 0U);
    QString text;
    if (outcome.grouped) {
        text = total == 0U && result_artists_.empty() && result_albums_.empty()
                   ? QStringLiteral("0 matches")
                   : QStringLiteral("%1 artist%2 · %3 album%4 · %5 track%6")
                         .arg(result_artists_.size())
                         .arg(result_artists_.size() == 1U ? QString{} : QStringLiteral("s"))
                         .arg(result_albums_.size())
                         .arg(result_albums_.size() == 1U ? QString{} : QStringLiteral("s"))
                         .arg(total)
                         .arg(total == 1U ? QString{} : QStringLiteral("s"));
    } else {
        text = QStringLiteral("%1 match%2").arg(total).arg(total == 1U ? QString{}
                                                                        : QStringLiteral("es"));
        if (total > shown) {
            text += QStringLiteral(" · showing first %1").arg(shown);
        }
    }
    if (outcome.scanned > 0U) {
        text += QStringLiteral(" · scanned %1 file%2")
                    .arg(outcome.scanned)
                    .arg(outcome.scanned == 1U ? QString{} : QStringLiteral("s"));
    }
    status_->setText(text);
}

void SearchDialog::openResults(const LocalLibraryAction action, const bool selection_only) {
    const auto name = QStringLiteral("Search: %1").arg(result_query_);
    if (!selection_only) {
        if (!result_rows_.empty()) {
            emit rowsRequested(name, result_rows_, action, result_remote_);
        }
        return;
    }
    // In list order, whatever the order they were selected in.
    auto selected = results_->selectedItems();
    std::ranges::sort(selected, [this](const QListWidgetItem* left, const QListWidgetItem* right) {
        return results_->row(left) < results_->row(right);
    });
    std::vector<LocalTrackRow> rows;
    std::vector<persistence::LibraryEntry> entries;
    for (const auto* item : selected) {
        const auto kind = item->data(result_kind_role);
        if (!kind.isValid()) {
            continue;
        }
        const auto index = static_cast<std::size_t>(item->data(result_index_role).toULongLong());
        switch (static_cast<persistence::LibraryEntryKind>(kind.toInt())) {
        case persistence::LibraryEntryKind::artist:
            if (index < result_artists_.size()) {
                entries.push_back(result_artists_[index]);
            }
            break;
        case persistence::LibraryEntryKind::album:
            if (index < result_albums_.size()) {
                entries.push_back(result_albums_[index]);
            }
            break;
        case persistence::LibraryEntryKind::track:
            if (index < result_rows_.size()) {
                rows.push_back(result_rows_[index]);
            }
            break;
        }
    }
    if (!entries.empty()) {
        // Artists and albums are resolved to their tracks first; tracks
        // picked alongside them follow, in one request.
        const QPointer self{this};
        auto* watcher = new QFutureWatcher<std::vector<LocalTrackRow>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this,
                [this, watcher, name, action, picked = std::move(rows),
                 remote = result_remote_]() mutable {
                    watcher->deleteLater();
                    auto resolved = watcher->result();
                    resolved.insert(resolved.end(), std::make_move_iterator(picked.begin()),
                                    std::make_move_iterator(picked.end()));
                    if (!resolved.empty()) {
                        emit rowsRequested(name, std::move(resolved), action, remote);
                    }
                });
        watcher->setFuture(QtConcurrent::run([catalogues = scopeCatalogues(),
                                              entries = std::move(entries)] {
            std::vector<LocalTrackRow> found;
            auto handle = catalogues->open();
            for (const auto& entry : entries) {
                persistence::LibraryQuery request;
                request.kind = persistence::LibraryEntryKind::track;
                if (entry.kind == persistence::LibraryEntryKind::artist) {
                    request.artist = entry.key;
                } else {
                    request.album_key = entry.key;
                }
                auto paths = handle->paths(request);
                if (!paths) {
                    continue;
                }
                auto cached = handle->cached_tracks(*paths);
                if (!cached) {
                    continue;
                }
                for (auto& track : *cached) {
                    found.push_back(cached_library_row(std::move(track)));
                }
            }
            return found;
        }));
        return;
    }
    if (!rows.empty()) {
        emit rowsRequested(name, std::move(rows), action, result_remote_);
    }
}

} // namespace trackknife::bench
