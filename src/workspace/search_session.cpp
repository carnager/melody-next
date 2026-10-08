// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/search_session.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/persistence/tkq_row.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAbstractItemModel>
#include <QPointer>
#include <QSettings>
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

// The same line, formatted by a library from what it holds of a track: the
// first artist or "Unknown artist", the first track number, the title.
constexpr auto result_format = "$if2($getmulti(artist,0),Unknown artist) — "
                               "$if($if2($getmulti(tracknumber,0),$getmulti(track,0)),"
                               "$if2($getmulti(tracknumber,0),$getmulti(track,0)). ,)%title%";

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

SearchSession::SearchSession(const CatalogueSource& catalogues, TabAccess tab_access,
                             TechnicalsSink technicals_sink, std::vector<OtherLibrary> others,
                             QObject* parent)
    : QObject(parent), catalogues_(&catalogues), others_(std::move(others)),
      tab_access_(std::move(tab_access)), technicals_sink_(std::move(technicals_sink)),
      query_mode_(QSettings{}.value(QStringLiteral("search/query-mode"), false).toBool()),
      saved_status_(QStringLiteral("Loading saved searches…")) {
    scopes_.push_back({QStringLiteral("This computer"), QStringLiteral("local")});
    scopes_.push_back({QStringLiteral("Current tab"), QStringLiteral("tab")});
    // ADR-0227: another engine's library is searched there, and what is
    // found opens in a tab of that engine.
    for (const auto& other : others_) {
        scopes_.push_back({other.name.isEmpty() ? QStringLiteral("Remote library") : other.name,
                           other.engine.text()});
    }
    debounce_.setSingleShot(true);
    debounce_.setInterval(200);
    connect(&debounce_, &QTimer::timeout, this, &SearchSession::startSearch);
    connect(&watcher_, &QFutureWatcherBase::finished, this, &SearchSession::finishSearch);
    connect(&catalog_watcher_, &QFutureWatcherBase::finished, this,
            &SearchSession::finishSavedSearches);
    loadSavedSearches();
}

SearchSession::~SearchSession() {
    cancellation_.request_cancellation();
    if (searching_) {
        watcher_.waitForFinished();
    }
}

void SearchSession::setScope(const int index) {
    if (index < 0 || index >= static_cast<int>(scopes_.size()) || index == scope_) {
        return;
    }
    scope_ = index;
    scheduleSearch();
}

void SearchSession::setText(const QString& text) {
    if (text == text_) {
        return;
    }
    text_ = text;
    scheduleSearch();
}

void SearchSession::setQueryMode(const bool on) {
    if (on == query_mode_) {
        return;
    }
    query_mode_ = on;
    QSettings{}.setValue(QStringLiteral("search/query-mode"), on);
    error_.clear();
    scheduleSearch();
}

void SearchSession::followLibrary(const EngineKey& engine) {
    if (!databaseScope()) {
        return;
    }
    const auto wanted = std::ranges::find(scopes_, engine.text(), &Scope::value);
    if (wanted != scopes_.end()) {
        setScope(static_cast<int>(wanted - scopes_.begin()));
    }
}

void SearchSession::watchCurrentModel(QAbstractItemModel* model) {
    for (const auto& connection : current_model_connections_)
        disconnect(connection);
    current_model_connections_.clear();
    const auto changed_rows = [this] {
        if (!databaseScope())
            scheduleSearch();
    };
    if (model) {
        current_model_connections_.push_back(
            connect(model, &QAbstractItemModel::modelReset, this, changed_rows));
        current_model_connections_.push_back(
            connect(model, &QAbstractItemModel::rowsInserted, this, changed_rows));
        current_model_connections_.push_back(
            connect(model, &QAbstractItemModel::rowsRemoved, this, changed_rows));
        current_model_connections_.push_back(
            connect(model, &QAbstractItemModel::layoutChanged, this, changed_rows));
        current_model_connections_.push_back(connect(
            model, &QAbstractItemModel::dataChanged, this,
            [changed_rows](const QModelIndex& first, const QModelIndex&, const QList<int>& roles) {
                // Playback markers and lazy history-cell repaints are not source edits.
                if (roles.empty() || roles.contains(ui::track_rating_role) ||
                    (roles.contains(Qt::DisplayRole) &&
                     first.column() < ui::track_play_count_column))
                    changed_rows();
            }));
    }
    changed_rows();
}

bool SearchSession::databaseScope() const {
    return scopes_[static_cast<std::size_t>(scope_)].value != QStringLiteral("tab");
}

EngineKey SearchSession::scopeEngine() const {
    const auto& scope = scopes_[static_cast<std::size_t>(scope_)].value;
    return scope == QStringLiteral("tab") ? EngineKey::local() : EngineKey::fromText(scope);
}

const CatalogueSource* SearchSession::scopeCatalogues() const {
    const auto engine = scopeEngine();
    const auto other = std::ranges::find(others_, engine, &OtherLibrary::engine);
    return other != others_.end() ? other->catalogues : catalogues_;
}

QStringList SearchSession::savedNames() const {
    QStringList names{QStringLiteral("Saved searches…")};
    for (const auto& search : catalog_) {
        names.append(displayText(search.name));
    }
    return names;
}

QStringList SearchSession::savedTooltips() const {
    QStringList tips{QString{}};
    for (const auto& search : catalog_) {
        tips.append(
            QStringLiteral("%1 · %2\n%3")
                .arg(search.scope == persistence::SavedSearchScope::current_tab
                         ? QStringLiteral("Current tab")
                         : QStringLiteral("Library database"))
                .arg(search.dialect == "tkq-1" ? QStringLiteral("Query") : QStringLiteral("Words"))
                .arg(displayText(search.expression)));
    }
    return tips;
}

std::optional<persistence::SavedSearch> SearchSession::selectedSearch() const {
    const auto index = saved_index_ - 1;
    return index < 0 || static_cast<std::size_t>(index) >= catalog_.size()
               ? std::nullopt
               : std::optional{catalog_[static_cast<std::size_t>(index)]};
}

QString SearchSession::selectedName() const {
    const auto selected = selectedSearch();
    return selected ? displayText(selected->name) : QString{};
}

QString SearchSession::deleteQuestion() const {
    return QStringLiteral("Delete “%1” from saved searches? Existing result tabs remain available.")
        .arg(selectedName());
}

bool SearchSession::canSave() const { return savedAvailable() && !text_.trimmed().isEmpty(); }

bool SearchSession::canUpdate() const {
    const auto selected = selectedSearch();
    const bool differs =
        selected &&
        (selected->expression != utf8Bytes(text_.trimmed()) ||
         selected->dialect != (query_mode_ ? "tkq-1" : "words-1") ||
         selected->scope != (databaseScope() ? persistence::SavedSearchScope::library
                                             : persistence::SavedSearchScope::current_tab));
    return canSave() && differs;
}

bool SearchSession::canRenameOrDelete() const {
    return savedAvailable() && selectedSearch().has_value();
}

void SearchSession::loadSavedSearches(std::optional<persistence::SavedSearch> write, bool remove) {
    if (catalog_busy_) {
        return;
    }
    catalog_busy_ = true;
    catalog_selection_ = write && !remove ? std::optional{write->id} : std::nullopt;
    saved_status_ = write ? QStringLiteral("Saving search definitions…")
                          : QStringLiteral("Loading saved searches…");
    emit savedChanged();
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

void SearchSession::finishSavedSearches() {
    catalog_busy_ = false;
    auto result = catalog_watcher_.result();
    if (!result) {
        saved_status_ = displayText(result.error().message);
        emit savedChanged();
        return;
    }
    catalog_ready_ = true;
    catalog_ = std::move(*result);
    saved_index_ = 0;
    for (std::size_t index = 0; index < catalog_.size(); ++index) {
        if (catalog_selection_ == catalog_[index].id) {
            saved_index_ = static_cast<int>(index) + 1;
        }
    }
    saved_status_ = QStringLiteral("%1 saved search%2 · select one to run it again")
                        .arg(catalog_.size())
                        .arg(catalog_.size() == 1U ? QString{} : QStringLiteral("es"));
    emit savedChanged();
}

void SearchSession::selectSaved(const int index) {
    saved_index_ = std::clamp(index, 0, static_cast<int>(catalog_.size()));
    emit savedChanged();
}

void SearchSession::useSaved(const int index) {
    selectSaved(index);
    if (index <= 0 || catalog_busy_ || static_cast<std::size_t>(index) > catalog_.size()) {
        return;
    }
    const auto& search = catalog_[static_cast<std::size_t>(index - 1)];
    text_ = displayText(search.expression);
    // A saved library search runs in whichever library is chosen.
    if (search.scope == persistence::SavedSearchScope::current_tab) {
        scope_ = 1;
    } else if (!databaseScope()) {
        scope_ = 0;
    }
    query_mode_ = search.dialect == "tkq-1";
    QSettings{}.setValue(QStringLiteral("search/query-mode"), query_mode_);
    error_.clear();
    scheduleSearch();
}

void SearchSession::saveAs(const QString& name) {
    if (!catalog_ready_ || catalog_busy_ || name.trimmed().isEmpty() || !compileInput()) {
        return;
    }
    persistence::SavedSearch search{
        .id = core::StableId::random(), .name = utf8Bytes(name.trimmed()), .expression = {}};
    search.expression = utf8Bytes(text_.trimmed());
    search.dialect = query_mode_ ? "tkq-1" : "words-1";
    search.scope = databaseScope() ? persistence::SavedSearchScope::library
                                   : persistence::SavedSearchScope::current_tab;
    loadSavedSearches(search);
}

void SearchSession::update() {
    if (!catalog_ready_ || catalog_busy_ || !compileInput()) {
        return;
    }
    auto search = selectedSearch();
    if (!search) {
        return;
    }
    search->expression = utf8Bytes(text_.trimmed());
    search->dialect = query_mode_ ? "tkq-1" : "words-1";
    search->scope = databaseScope() ? persistence::SavedSearchScope::library
                                    : persistence::SavedSearchScope::current_tab;
    loadSavedSearches(*search);
}

void SearchSession::rename(const QString& name) {
    auto search = selectedSearch();
    if (!search || catalog_busy_ || name.trimmed().isEmpty() ||
        utf8Bytes(name.trimmed()) == search->name) {
        return;
    }
    search->name = utf8Bytes(name.trimmed());
    loadSavedSearches(*search);
}

void SearchSession::remove() {
    const auto search = selectedSearch();
    if (!search || catalog_busy_) {
        return;
    }
    loadSavedSearches(*search, true);
}

std::vector<SearchSession::PresetGroup> SearchSession::presetGroups() {
    std::vector<PresetGroup> groups;
    const auto& presets = query::search_presets();
    for (std::size_t index = 0; index < presets.size(); ++index) {
        const auto& preset = presets[index];
        const auto source = query::preset_query(preset, preset.example);
        if (!source || !query::compile_tkq(*source))
            continue;
        const auto topic = displayText(std::string{preset.topic});
        if (groups.empty() || groups.back().topic != topic) {
            groups.push_back({topic, {}});
        }
        groups.back().presets.push_back({displayText(std::string{preset.title}),
                                         displayText(std::string{preset.id}),
                                         static_cast<int>(index)});
    }
    return groups;
}

SearchSession::PresetInput SearchSession::presetInput(const int index) {
    const auto& presets = query::search_presets();
    if (index < 0 || static_cast<std::size_t>(index) >= presets.size()) {
        return {};
    }
    const auto& preset = presets[static_cast<std::size_t>(index)];
    PresetInput input;
    input.kind = preset.input == query::PresetInput::none      ? PresetInput::Kind::none
                 : preset.input == query::PresetInput::integer ? PresetInput::Kind::integer
                                                               : PresetInput::Kind::text;
    input.title = displayText(std::string{preset.title});
    input.prompt = displayText(std::string{preset.prompt});
    input.value = displayText(std::string{preset.example});
    input.minimum = preset.minimum;
    input.maximum = preset.maximum;
    input.step = preset.id == "decade" ? 10 : 1;
    return input;
}

void SearchSession::usePreset(const int index, const QString& value) {
    const auto& presets = query::search_presets();
    if (index < 0 || static_cast<std::size_t>(index) >= presets.size()) {
        return;
    }
    const auto& preset = presets[static_cast<std::size_t>(index)];
    const auto given =
        preset.input == query::PresetInput::none ? displayText(std::string{preset.example}) : value;
    const auto source = query::preset_query(preset, utf8Bytes(given));
    if (!source) {
        error_ = displayText(source.error().message);
        emit changed();
        return;
    }
    // A built-in is a starting point, never an implicit edit to a saved search.
    saved_index_ = 0;
    emit savedChanged();
    query_mode_ = true;
    QSettings{}.setValue(QStringLiteral("search/query-mode"), true);
    error_.clear();
    text_ = displayText(*source);
    scheduleSearch();
}

void SearchSession::scheduleSearch() {
    debounce_.stop();
    results_.clear();
    result_rows_.clear();
    result_paths_.clear();
    result_artists_.clear();
    result_albums_.clear();
    ++generation_;
    cancellation_.request_cancellation();
    cancellation_ = core::CancellationSource{};
    if (text_.trimmed().isEmpty()) {
        status_.clear();
        error_.clear();
    } else {
        status_ = QStringLiteral("Searching…");
        debounce_.start();
    }
    emit resultsChanged();
    emit changed();
    emit savedChanged();
}

std::optional<query::CompiledTkq> SearchSession::compileInput() {
    const auto source = utf8Bytes(text_.trimmed());
    auto compiled =
        query_mode_ ? query::compile_tkq(source) : query::compile_tkq_word_search(source);
    if (!compiled) {
        error_ = displayText(compiled.error().message);
        status_ = QStringLiteral("Invalid query");
        emit changed();
        return std::nullopt;
    }
    if (!error_.isEmpty()) {
        error_.clear();
        emit changed();
    }
    return std::move(*compiled);
}

void SearchSession::startSearch() {
    if (searching_) {
        debounce_.start();
        return;
    }
    search_job_generation_ = generation_;
    auto compiled = compileInput();
    if (!compiled) {
        return;
    }
    result_query_ = text_.trimmed();
    result_engine_ = scopeEngine();
    if (databaseScope() && !query_mode_) {
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
            const auto find = [&](const persistence::LibraryEntryKind kind, const std::size_t limit)
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
            auto albums =
                artists ? find(persistence::LibraryEntryKind::album, grouped_limit) : std::nullopt;
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
                // The lines shown, formatted by the library: not every tag
                // of every match. ALL of 66,841 tracks fetched 122 MB of
                // them, and took 20 s to show a list of lines.
                auto found = catalogue.find(*shared, result_format,
                                            static_cast<std::size_t>(result_display_limit), token);
                if (!found) {
                    outcome.error = displayText(found.error().message);
                    return outcome;
                }
                outcome.labels.reserve(found->size());
                for (auto& track : *found) {
                    outcome.labels.push_back(std::move(track.text));
                }
                outcome.paths = std::move(*paths);
                return outcome;
            }));
        return;
    }
    auto snapshot = tab_access_ ? tab_access_() : std::nullopt;
    if (!snapshot) {
        status_ = QStringLiteral("No local tab is active.");
        results_.clear();
        result_rows_.clear();
        result_paths_.clear();
        emit resultsChanged();
        emit changed();
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

void SearchSession::finishSearch() {
    searching_ = false;
    if (search_job_generation_ != generation_) {
        return;
    }
    auto outcome = watcher_.result();
    if (!outcome.error.isEmpty()) {
        status_ = outcome.error;
        emit changed();
        return;
    }
    for (auto& [path, technicals] : outcome.probed) {
        if (technicals_sink_) {
            technicals_sink_(path, technicals);
        }
    }
    results_.clear();
    result_rows_ = std::move(outcome.rows);
    result_paths_ = std::move(outcome.paths);
    result_artists_ = std::move(outcome.artists);
    result_albums_ = std::move(outcome.albums);
    const auto add_item = [this](const QString& label, const persistence::LibraryEntryKind kind,
                                 const std::size_t index) {
        results_.push_back({.label = label, .heading = false, .kind = kind, .index = index});
    };
    const auto add_heading = [this](const QString& label) {
        results_.push_back({.label = label, .heading = true, .kind = {}, .index = 0U});
    };
    const auto total = result_rows_.empty() ? result_paths_.size() : result_rows_.size();
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
                         persistence::LibraryEntryKind::artist, index);
            }
        }
        if (!result_albums_.empty()) {
            add_heading(heading(QStringLiteral("Albums"), result_albums_.size(), grouped_limit));
            for (std::size_t index = 0U; index < result_albums_.size(); ++index) {
                const auto& album = result_albums_[index];
                add_item(QStringLiteral("%1 — %2").arg(displayText(album.artist),
                                                       displayText(album.label)),
                         persistence::LibraryEntryKind::album, index);
            }
        }
        if (total > 0U) {
            add_heading(heading(QStringLiteral("Tracks"), total, grouped_track_limit));
        }
    }
    const auto shown = std::min<std::size_t>(outcome.labels.size(),
                                             static_cast<std::size_t>(result_display_limit));
    for (std::size_t index = 0U; index < shown; ++index) {
        add_item(displayText(outcome.labels[index]), persistence::LibraryEntryKind::track, index);
    }
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
        text = QStringLiteral("%1 match%2")
                   .arg(total)
                   .arg(total == 1U ? QString{} : QStringLiteral("es"));
        if (total > shown) {
            text += QStringLiteral(" · showing first %1").arg(shown);
        }
    }
    if (outcome.scanned > 0U) {
        text += QStringLiteral(" · scanned %1 file%2")
                    .arg(outcome.scanned)
                    .arg(outcome.scanned == 1U ? QString{} : QStringLiteral("s"));
    }
    status_ = text;
    emit resultsChanged();
    emit changed();
}

QString SearchSession::resultName() const {
    return QStringLiteral("Search: %1").arg(result_query_);
}

void SearchSession::openAll(const LocalLibraryAction action) {
    if (!result_rows_.empty()) {
        emit rowsRequested(resultName(), result_rows_, action, result_engine_);
        return;
    }
    if (!result_paths_.empty()) {
        resolveTracks(result_paths_, {}, {}, action);
    }
}

void SearchSession::openRows(std::vector<int> chosen, const LocalLibraryAction action) {
    // In list order, whatever the order they were selected in.
    std::ranges::sort(chosen);
    std::vector<LocalTrackRow> rows;
    std::vector<std::string> paths;
    std::vector<persistence::LibraryEntry> entries;
    for (const auto row : chosen) {
        if (row < 0 || row >= static_cast<int>(results_.size()) ||
            results_[static_cast<std::size_t>(row)].heading) {
            continue;
        }
        const auto& result = results_[static_cast<std::size_t>(row)];
        const auto index = result.index;
        switch (result.kind) {
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
            } else if (result_rows_.empty() && index < result_paths_.size()) {
                paths.push_back(result_paths_[index]);
            }
            break;
        case persistence::LibraryEntryKind::group:
            // Search finds artists, albums and tracks, never a view's level.
            break;
        }
    }
    if (!entries.empty() || !paths.empty()) {
        resolveTracks(std::move(paths), std::move(entries), std::move(rows), action);
        return;
    }
    if (!rows.empty()) {
        emit rowsRequested(resultName(), std::move(rows), action, result_engine_);
    }
}

void SearchSession::resolveTracks(std::vector<std::string> paths,
                                  std::vector<persistence::LibraryEntry> entries,
                                  std::vector<LocalTrackRow> picked,
                                  const LocalLibraryAction action) {
    // Artists and albums are resolved to their tracks first, then the
    // tracks a query found, read now that they are wanted; tracks picked
    // alongside them follow, in one request.
    auto* watcher = new QFutureWatcher<std::vector<LocalTrackRow>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, name = resultName(), action, picked = std::move(picked),
             engine = result_engine_]() mutable {
                watcher->deleteLater();
                auto resolved = watcher->result();
                resolved.insert(resolved.end(), std::make_move_iterator(picked.begin()),
                                std::make_move_iterator(picked.end()));
                if (!resolved.empty()) {
                    emit rowsRequested(name, std::move(resolved), action, engine);
                }
            });
    watcher->setFuture(QtConcurrent::run(
        [catalogues = scopeCatalogues(), entries = std::move(entries), paths = std::move(paths)] {
            std::vector<LocalTrackRow> found;
            auto handle = catalogues->open();
            const auto read = [&handle, &found](const std::vector<std::string>& files) {
                auto cached = handle->cached_tracks(files);
                if (!cached) {
                    return;
                }
                for (auto& track : *cached) {
                    found.push_back(cached_library_row(std::move(track)));
                }
            };
            for (const auto& entry : entries) {
                persistence::LibraryQuery request;
                request.kind = persistence::LibraryEntryKind::track;
                if (entry.kind == persistence::LibraryEntryKind::artist) {
                    request.artist = entry.key;
                } else {
                    request.album_key = entry.key;
                }
                if (auto files = handle->paths(request)) {
                    read(*files);
                }
            }
            if (!paths.empty()) {
                read(paths);
            }
            return found;
        }));
}

} // namespace trackknife::bench
