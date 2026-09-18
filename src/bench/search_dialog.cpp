// SPDX-License-Identifier: GPL-3.0-only

#include "bench/search_dialog.hpp"

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
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
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

constexpr int result_display_limit = 500;

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

SearchDialog::SearchDialog(std::filesystem::path database_path, TabAccess tab_access,
                           TechnicalsSink technicals_sink, ServerScope server_scope,
                           QWidget* parent)
    : QDialog(parent), database_path_(std::move(database_path)), tab_access_(std::move(tab_access)),
      technicals_sink_(std::move(technicals_sink)), server_scope_(std::move(server_scope)) {
    setWindowTitle(QStringLiteral("Search"));
    setObjectName(QStringLiteral("bench-search-dialog"));
    setModal(false);
    resize(720, 480);

    auto* layout = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    scope_ = new QComboBox(this);
    scope_->setObjectName(QStringLiteral("bench-search-scope"));
    scope_->addItem(QStringLiteral("Library database"));
    scope_->addItem(QStringLiteral("Current tab"));
    if (server_scope_.run) {
        scope_->addItem(QStringLiteral("Server library"));
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
                       "GREATER 1990"));
    query_mode_->setChecked(QSettings{}.value(QStringLiteral("search/query-mode"), false).toBool());
    top->addWidget(query_mode_);
    layout->addLayout(top);

    auto* saved = new QHBoxLayout;
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
        if (results_->selectedItems().isEmpty() || serverScope()) {
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
    connect(results_, &QListWidget::itemActivated, this, [this](QListWidgetItem*) {
        openResults(serverScope() ? LocalLibraryAction::new_list : LocalLibraryAction::append,
                    !serverScope());
    });
    loadSavedSearches();
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
         selected->scope != (databaseScope()  ? persistence::SavedSearchScope::library
                             : serverScope() ? persistence::SavedSearchScope::server
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
        QtConcurrent::run([database = database_path_, write = std::move(write),
                           remove]() -> core::Result<std::vector<persistence::SavedSearch>> {
            auto repository = persistence::ListRepository::open(database);
            if (!repository) {
                return std::unexpected(repository.error());
            }
            if (write) {
                auto result =
                    remove ? repository->remove_search(*write) : repository->save_search(*write);
                if (!result) {
                    return std::unexpected(result.error());
                }
            }
            return repository->load_saved_searches();
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
                .arg(search.scope == persistence::SavedSearchScope::library
                         ? QStringLiteral("Library database")
                     : search.scope == persistence::SavedSearchScope::server
                         ? QStringLiteral("Server library")
                         : QStringLiteral("Current tab"))
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
        scope_->setCurrentIndex(search.scope == persistence::SavedSearchScope::library ? 0
                                : search.scope == persistence::SavedSearchScope::server &&
                                        scope_->count() > 2
                                    ? 2
                                    : 1);
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
    search->scope = databaseScope()  ? persistence::SavedSearchScope::library
                    : serverScope() ? persistence::SavedSearchScope::server
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

bool SearchDialog::databaseScope() const { return scope_->currentIndex() == 0; }

bool SearchDialog::serverScope() const { return scope_->currentIndex() == 2; }

void SearchDialog::scheduleSearch() {
    updateSavedSearchButtons();
    debounce_->stop();
    results_->clear();
    result_rows_.clear();
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

void SearchDialog::startServerSearch(query::CompiledTkq compiled) {
    server_result_query_.reset();
    if (!server_scope_.run || (server_scope_.available && !server_scope_.available())) {
        status_->setText(
            QStringLiteral("No connected server supports structured queries; connect to a "
                           "current Melody server."));
        return;
    }
    searching_ = true;
    status_->setText(QStringLiteral("Searching the server library…"));
    const auto generation = generation_;
    server_scope_.run(
        compiled, [this, generation, compiled](const QStringList& labels, const int total,
                                               const QString& error) {
            searching_ = false;
            if (generation != generation_ || !serverScope()) {
                return;
            }
            results_->clear();
            result_rows_.clear();
            if (!error.isEmpty()) {
                error_->setText(error);
                error_->show();
                status_->setText(QStringLiteral("The server cannot run this query"));
                open_button_->setEnabled(false);
                return;
            }
            error_->hide();
            for (const auto& label : labels) {
                results_->addItem(label);
            }
            server_result_query_ = compiled;
            status_->setText(total == 1
                                 ? QStringLiteral("1 match")
                                 : QStringLiteral("%1 matches%2")
                                       .arg(total)
                                       .arg(total >= 500 ? QStringLiteral(" · first 500 shown")
                                                         : QString{}));
            open_button_->setEnabled(total > 0);
        });
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
    if (serverScope()) {
        startServerSearch(std::move(*compiled));
        return;
    }
    if (databaseScope()) {
        searching_ = true;
        watcher_.setFuture(
            QtConcurrent::run([database = database_path_,
                               shared = std::make_shared<query::CompiledTkq>(std::move(*compiled)),
                               token = cancellation_.token()]() {
                Outcome outcome;
                auto library = persistence::LocalLibrary::open(database);
                if (!library) {
                    outcome.error = displayText(library.error().message);
                    return outcome;
                }
                auto paths = library->filter_paths(*shared, token);
                if (!paths) {
                    outcome.error = displayText(paths.error().message);
                    return outcome;
                }
                auto cached = library->cached_tracks(*paths, token);
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
    watcher_.setFuture(QtConcurrent::run([rows = std::move(snapshot->rows),
                                          shared = std::make_shared<query::CompiledTkq>(
                                              std::move(*compiled)),
                                          needs_technicals,
                                          token = cancellation_.token()]() mutable {
        Outcome outcome;
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
                                .bit_rate = found->bit_rate > 0 ? found->bit_rate : probe->bit_rate,
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
            const auto descending = shared->sort->direction == query::TkqSortDirection::descending;
            std::ranges::stable_sort(keyed, [descending](const Keyed& left, const Keyed& right) {
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
    const auto total = result_rows_.size();
    const auto shown = std::min<std::size_t>(outcome.labels.size(),
                                             static_cast<std::size_t>(result_display_limit));
    for (std::size_t index = 0U; index < shown; ++index) {
        results_->addItem(displayText(outcome.labels[index]));
    }
    open_button_->setEnabled(total > 0U);
    QString text =
        QStringLiteral("%1 match%2").arg(total).arg(total == 1U ? QString{} : QStringLiteral("es"));
    if (total > shown) {
        text += QStringLiteral(" · showing first %1").arg(shown);
    }
    if (outcome.scanned > 0U) {
        text += QStringLiteral(" · scanned %1 file%2")
                    .arg(outcome.scanned)
                    .arg(outcome.scanned == 1U ? QString{} : QStringLiteral("s"));
    }
    status_->setText(text);
}

void SearchDialog::openResults(const LocalLibraryAction action, const bool selection_only) {
    if (serverScope()) {
        // Server results live in the MPD authority; the window opens them as
        // a committed server search tab.
        if (server_result_query_ && server_scope_.open) {
            server_scope_.open(*server_result_query_, result_query_);
        }
        return;
    }
    const auto name = QStringLiteral("Search: %1").arg(result_query_);
    std::vector<int> positions;
    if (selection_only) {
        for (const auto* item : results_->selectedItems()) {
            positions.push_back(results_->row(item));
        }
        std::ranges::sort(positions);
        if (positions.empty()) {
            return;
        }
    }
    std::vector<LocalTrackRow> rows;
    if (selection_only) {
        for (const auto position : positions) {
            if (position >= 0 && static_cast<std::size_t>(position) < result_rows_.size()) {
                rows.push_back(result_rows_[static_cast<std::size_t>(position)]);
            }
        }
    } else {
        rows = result_rows_;
    }
    if (!rows.empty()) {
        emit rowsRequested(name, std::move(rows), action);
    }
}

} // namespace trackknife::bench
