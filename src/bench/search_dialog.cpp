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
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
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
                           TechnicalsSink technicals_sink, QWidget* parent)
    : QDialog(parent), database_path_(std::move(database_path)), tab_access_(std::move(tab_access)),
      technicals_sink_(std::move(technicals_sink)) {
    setWindowTitle(QStringLiteral("Search"));
    setObjectName(QStringLiteral("bench-search-dialog"));
    setModal(false);
    resize(560, 480);

    auto* layout = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    scope_ = new QComboBox(this);
    scope_->setObjectName(QStringLiteral("bench-search-scope"));
    scope_->addItem(QStringLiteral("Library database"));
    scope_->addItem(QStringLiteral("Current tab"));
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
}

SearchDialog::~SearchDialog() {
    cancellation_.request_cancellation();
    if (searching_) {
        watcher_.waitForFinished();
    }
}

bool SearchDialog::databaseScope() const { return scope_->currentIndex() == 0; }

void SearchDialog::scheduleSearch() {
    ++generation_;
    cancellation_.request_cancellation();
    cancellation_ = core::CancellationSource{};
    if (input_->text().trimmed().isEmpty()) {
        results_->clear();
        result_paths_.clear();
        result_rows_.clear();
        open_button_->setEnabled(false);
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
    auto compiled = compileInput();
    if (!compiled) {
        return;
    }
    result_query_ = input_->text().trimmed();
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
                auto page = library->filter(*shared, 0U, 200U, token);
                if (!page) {
                    outcome.error = displayText(page.error().message);
                    return outcome;
                }
                outcome.more = page->more;
                auto paths = library->filter_paths(*shared, token);
                if (!paths) {
                    outcome.error = displayText(paths.error().message);
                    return outcome;
                }
                outcome.paths = std::move(*paths);
                for (const auto& entry : page->entries) {
                    outcome.labels.push_back(entry.label);
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
            const auto facts = persistence::make_tkq_row_facts(
                row.metadata, row.title, row.artist, row.album, row.duration_ms,
                row.technicals ? std::optional{persistence::TkqRowTechnicals{
                                     .codec = row.technicals->codec,
                                     .sample_rate = row.technicals->sample_rate,
                                     .bits = row.technicals->bits,
                                     .channels = row.technicals->channels}}
                               : std::nullopt);
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
    result_paths_ = std::move(outcome.paths);
    result_rows_ = std::move(outcome.rows);
    const auto total = databaseScope() ? result_paths_.size() : result_rows_.size();
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
    if (databaseScope()) {
        std::vector<std::string> paths;
        if (selection_only) {
            // Displayed rows map 1:1 onto the leading result paths.
            for (const auto position : positions) {
                if (position >= 0 && static_cast<std::size_t>(position) < result_paths_.size()) {
                    paths.push_back(result_paths_[static_cast<std::size_t>(position)]);
                }
            }
        } else {
            paths = result_paths_;
        }
        if (!paths.empty()) {
            emit resultsRequested(name, std::move(paths), action);
        }
        return;
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
