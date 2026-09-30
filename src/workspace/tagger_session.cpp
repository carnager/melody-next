// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/tagger_session.hpp"

#include "bench/artwork_fitting.hpp"
#include "bench/metadata_dialog_helpers.hpp"
#include "bench/metadata_grid_model.hpp"
#include "bench/settings_keys.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/metadata/draft_document.hpp"
#include "trackknife/loudness/grouping.hpp"
#include "trackknife/metadata/field_suggestions.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "uicommon/debug_log.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace trackknife::bench {

namespace {

// ADR-0238: what the Actions popover was last set to.
constexpr auto remembered_save_tags_key = "properties/actions/save-tags";
constexpr auto remembered_rename_key = "properties/actions/rename-files";
constexpr auto remembered_move_key = "properties/actions/move-files";
constexpr auto remembered_layout_key = "properties/actions/naming-layout";
constexpr auto remembered_destination_prefix = "properties/actions/move-destination/";

constexpr auto properties_geometry_key = "workspace/metadata-properties-geometry-v1";
constexpr auto properties_metadata_splitter_key =
    "workspace/metadata-properties-metadata-splitter-v1";
constexpr auto properties_field_layouts_key = "workspace/metadata-field-layouts-v1";

constexpr std::size_t maximum_technical_probes = 512U;

[[nodiscard]] std::optional<std::size_t> parse_position_number(const std::string& text) {
    const auto slash = text.find('/');
    const auto digits = slash == std::string::npos ? text : text.substr(0U, slash);
    if (digits.empty() || digits.size() > 6U) {
        return std::nullopt;
    }
    std::size_t value = 0U;
    for (const auto character : digits) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::size_t>(character - '0');
    }
    return value == 0U ? std::nullopt : std::optional{value};
}

} // namespace

TaggerSession::TaggerSession(const std::size_t requested_item_count,
                             MetadataPropertiesSourceReader source_reader,
                             const std::span<const std::string_view> preferred_fields,
                             TaggerServices services, QObject* parent)
    : QObject(parent), source_reader_(std::move(source_reader)), services_(std::move(services)),
      requested_item_count_(requested_item_count) {
    summary_ = QStringLiteral("%1 %2 · preparing")
                   .arg(requested_item_count_)
                   .arg(pluralized(requested_item_count_, QStringLiteral("track"),
                                   QStringLiteral("tracks")));
    status_ = QStringLiteral("Read-only metadata preview · preparing selection");
    loading_text_ = QStringLiteral("Preparing metadata grid…");
    output_profile_status_ = QStringLiteral("Loading output profiles…");
    {
        const QSettings remembered;
        save_tags_ = remembered.value(QLatin1String(remembered_save_tags_key), true).toBool();
        wants_rename_ = remembered.value(QLatin1String(remembered_rename_key), false).toBool();
        wants_move_ = remembered.value(QLatin1String(remembered_move_key), false).toBool();
        const auto id_of = [&remembered](const QString& key) -> std::optional<core::StableId> {
            auto id = core::StableId::parse(remembered.value(key).toString().toStdString());
            return id ? std::optional{*id} : std::nullopt;
        };
        editing_output_layout_id_ = id_of(QLatin1String(remembered_layout_key));
        editing_destination_id_ = id_of(QLatin1String(remembered_destination_prefix) +
                                        services_.output_profile_store.destinations_key);
    }
    preferred_fields_.reserve(preferred_fields.size());
    for (const auto field : preferred_fields) {
        preferred_fields_.emplace_back(field);
    }

    selection_debounce_.setSingleShot(true);
    selection_debounce_.setInterval(40);
    connect(&selection_debounce_, &QTimer::timeout, this,
            &TaggerSession::updateSelectionProjection);
    apply_progress_timer_.setInterval(50);
    connect(&apply_progress_timer_, &QTimer::timeout, this, &TaggerSession::updateApplyProgress);
    replaygain_progress_timer_.setInterval(100);
    connect(&replaygain_progress_timer_, &QTimer::timeout, this, [this] {
        setStatus(QStringLiteral("Measuring loudness · %1 of %2 files · "
                                 "<a href=\"cancel-replaygain\">Stop</a>")
                      .arg(replaygain_completed_ ? replaygain_completed_->load() : 0U)
                      .arg(replaygain_total_),
                  true);
    });
    connect(&selection_watcher_, &QFutureWatcherBase::finished, this,
            &TaggerSession::finishSelection);
    connect(&automatic_watcher_, &QFutureWatcherBase::finished, this,
            &TaggerSession::finishAutomaticStage);
    connect(&replaygain_watcher_, &QFutureWatcherBase::finished, this,
            &TaggerSession::finishReplayGainScan);
    connect(&proposal_watcher_, &QFutureWatcherBase::finished, this,
            &TaggerSession::finishProposals);
    connect(&write_plan_watcher_, &QFutureWatcherBase::finished, this,
            &TaggerSession::finishWritePlan);
    connect(&metadata_apply_watcher_, &QFutureWatcherBase::finished, this,
            &TaggerSession::finishMetadataApply);
    connect(&file_apply_watcher_, &QFutureWatcherBase::finished, this,
            &TaggerSession::finishFileApply);
    connect(&technical_watcher_, &QFutureWatcherBase::finished, this, [this] {
        technical_probing_ = false;
        auto outcome = technical_watcher_.result();
        technical_pending_.erase(outcome.first);
        technical_cache_[outcome.first] = std::move(outcome.second);
        pumpTechnicalQueue();
        updateTechnicalSummary();
    });
}

TaggerSession::~TaggerSession() {
    write_plan_cancellation_.request_cancellation();
    apply_cancellation_.request_cancellation();
    if (write_plan_running_) {
        write_plan_watcher_.waitForFinished();
    }
    if (apply_running_) {
        metadata_apply_watcher_.waitForFinished();
        file_apply_watcher_.waitForFinished();
    }
    if (proposal_running_) {
        proposal_watcher_.waitForFinished();
    }
    if (automatic_stage_running_) {
        automatic_watcher_.waitForFinished();
    }
    replaygain_cancellation_.request_cancellation();
    if (replaygain_running_) {
        replaygain_watcher_.waitForFinished();
    }
    technical_cancellation_.request_cancellation();
    if (technical_probing_) {
        technical_watcher_.waitForFinished();
    }
    selection_watcher_.waitForFinished();
}

void TaggerSession::start() {
    loadTransformationCatalog();
    loadOutputProfiles();
    const metadata::StagedMetadataSelectionLimits limits;
    if (requested_item_count_ > limits.items) {
        summary_ = QStringLiteral("Properties unavailable");
        setStatus(QStringLiteral("Read-only metadata preview"));
        loading_text_ =
            QStringLiteral("The selection exceeds the %1-track limit").arg(limits.items);
        source_reader_ = {};
        emit changed();
        return;
    }
    sources_.reserve(requested_item_count_);
    audio_sources_->reserve(requested_item_count_);
    track_labels_.reserve(static_cast<qsizetype>(std::min(
        requested_item_count_, static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))));
    // ADR-0177: stored field sets are left as they are but not applied; every
    // field is shown.
    QTimer::singleShot(0, this, &TaggerSession::captureSources);
}

void TaggerSession::setArtwork(TaggerArtwork* artwork) {
    artwork_ = artwork;
    if (artwork_ != nullptr && grid_model_ != nullptr) {
        updateArtworkScope(selectedItems());
    }
}

void TaggerSession::setArtworkServices(ArtworkWritePlanApplierFactory applier_factory,
                                       ArtworkApplyObserver observer) {
    artwork_plan_applier_factory_ = std::move(applier_factory);
    artwork_apply_observer_ = std::move(observer);
}

ArtworkApplyObserver TaggerSession::artworkAppliedObserver() {
    const QPointer self{this};
    return [self](const operations::ArtworkApplyResult& result) {
        if (self) {
            self->artworkApplied(result);
        }
    };
}

void TaggerSession::artworkApplied(const operations::ArtworkApplyResult& result) {
    if (grid_model_ != nullptr) {
        for (const auto& source : result.sources) {
            if (source.state != operations::ArtworkApplySourceState::committed || !source.commit) {
                continue;
            }
            const auto& commit = *source.commit;
            const auto revised = grid_model_->advanceSourceRevision(
                commit.source_raw_path, commit.previous_revision, commit.published_revision);
            if (!revised) {
                showStickyStatus(display_utf8(revised.error().message));
            }
        }
        invalidateWritePlan();
    }
    if (artwork_apply_observer_) {
        artwork_apply_observer_(result);
    }
}

void TaggerSession::setArtworkOperationRunning(const bool running) {
    artwork_operation_running_ = running;
    emit changed();
}

void TaggerSession::artworkStateChanged() {
    artwork_operation_running_ = artwork_ != nullptr && artwork_->isBusy();
    updateDraftState(draft_count_, can_undo_, can_redo_);
}

std::optional<ArtworkCoverArtService> TaggerSession::coverArtService() {
    if (!services_.musicbrainz.fetch) {
        return std::nullopt;
    }
    const QPointer self{this};
    return ArtworkCoverArtService{
        .fetch_listing =
            [self](const QString& release_id,
                   std::function<void(core::Result<musicbrainz::CoverArtListing>)> completion) {
                const auto listing_url =
                    musicbrainz::build_cover_art_listing_url(release_id.toStdString());
                if (self.isNull() || !listing_url) {
                    completion(std::unexpected(
                        listing_url ? core::Error{.code = core::ErrorCode::cancelled,
                                                  .message = "the tag editor closed",
                                                  .context = {}}
                                    : listing_url.error()));
                    return;
                }
                self->services_.musicbrainz.fetch(
                    QString::fromStdString(*listing_url),
                    [completion](core::Result<QByteArray> body) {
                        if (!body) {
                            completion(std::unexpected(std::move(body.error())));
                            return;
                        }
                        completion(musicbrainz::parse_cover_art_listing(std::string_view{
                            body->constData(), static_cast<std::size_t>(body->size())}));
                    });
            },
        .fetch_bytes =
            [self](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                if (self.isNull()) {
                    return;
                }
                self->services_.musicbrainz.fetch(url, std::move(completion));
            },
        .store_image = [self](const QString& identity,
                              const QByteArray& bytes) -> core::Result<QString> {
            if (self.isNull()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::cancelled,
                    .message = "the tag editor closed",
                    .context = {},
                });
            }
            return self->storeCoverArtImage(identity, bytes);
        },
    };
}

// Field sets.

void TaggerSession::persistFieldLayouts() {
    if (!services_.layout_store.save) {
        return;
    }
    QJsonArray layouts;
    for (const auto& layout : field_layouts_) {
        QJsonArray fields;
        for (const auto& field : layout.fields) {
            fields.push_back(field);
        }
        layouts.push_back(QJsonObject{{QStringLiteral("id"), layout.id},
                                      {QStringLiteral("name"), layout.name},
                                      {QStringLiteral("fields"), fields}});
    }
    const QJsonObject root{{QStringLiteral("schema"), 1},
                           {QStringLiteral("active"), active_field_layout_id_},
                           {QStringLiteral("layouts"), layouts}};
    services_.layout_store.save(QString::fromLatin1(properties_field_layouts_key),
                                QJsonDocument(root).toJson(QJsonDocument::Compact), {});
}

QStringList TaggerSession::activeFieldLayoutFields() const {
    const auto found = std::ranges::find(field_layouts_, active_field_layout_id_, &FieldLayout::id);
    return found == field_layouts_.end() ? QStringList{} : found->fields;
}

void TaggerSession::selectFieldLayout(const QString& id) {
    const auto found = std::ranges::find(field_layouts_, id, &FieldLayout::id);
    active_field_layout_id_ = found == field_layouts_.end() ? QString{} : id;
    persistFieldLayouts();
    emit fieldLayoutsChanged();
}

int TaggerSession::saveFieldLayout(const QString& name, QStringList fields) {
    if (field_layouts_.size() >= 64U || name.trimmed().isEmpty()) {
        return -1;
    }
    fields.removeDuplicates();
    if (fields.isEmpty()) {
        return -1;
    }
    field_layouts_.push_back(FieldLayout{.id = QUuid::createUuid().toString(QUuid::WithoutBraces),
                                         .name = name.trimmed(),
                                         .fields = std::move(fields)});
    active_field_layout_id_ = field_layouts_.back().id;
    persistFieldLayouts();
    emit fieldLayoutsChanged();
    return static_cast<int>(field_layouts_.size()) - 1;
}

void TaggerSession::removeFieldLayout() {
    if (active_field_layout_id_.isEmpty()) {
        return;
    }
    std::erase_if(field_layouts_,
                  [this](const auto& layout) { return layout.id == active_field_layout_id_; });
    active_field_layout_id_.clear();
    persistFieldLayouts();
    emit fieldLayoutsChanged();
}

void TaggerSession::loadLayoutState(std::function<void(QByteArray)> geometry,
                                    std::function<void(QByteArray)> splitter) {
    if (!services_.layout_store.load) {
        return;
    }
    const QPointer self{this};
    services_.layout_store.load(
        QString::fromLatin1(properties_geometry_key),
        [self, geometry = std::move(geometry)](QByteArray state, const QString& error) {
            if (self && error.isEmpty() && !state.isEmpty()) {
                geometry(std::move(state));
            }
        });
    services_.layout_store.load(
        QString::fromLatin1(properties_metadata_splitter_key),
        [self, splitter = std::move(splitter)](QByteArray state, const QString& error) {
            if (self && error.isEmpty() && !state.isEmpty()) {
                splitter(std::move(state));
            }
        });
}

void TaggerSession::storeLayoutState(const QByteArray& geometry, const QByteArray& splitter) {
    if (layout_state_saved_ || !services_.layout_store.save) {
        return;
    }
    layout_state_saved_ = true;
    services_.layout_store.save(QString::fromLatin1(properties_geometry_key), geometry, {});
    if (!splitter.isEmpty()) {
        services_.layout_store.save(QString::fromLatin1(properties_metadata_splitter_key),
                                    splitter, {});
    }
}

// The files.

void TaggerSession::captureSources() {
    constexpr auto capture_budget_ms = 4;
    if (capture_index_ >= requested_item_count_) {
        summary_ = QStringLiteral("Properties unavailable");
        setStatus(QStringLiteral("Read-only metadata preview"));
        loading_text_ = QStringLiteral("No tracks were selected");
        emit changed();
        return;
    }
    QElapsedTimer timer;
    timer.start();
    do {
        if (auto snapshot = source_reader_(capture_index_)) {
            snapshot->source.logical_track =
                snapshot->source.logical_track || snapshot->audio.range ||
                snapshot->audio.selection.stream_index || snapshot->audio.selection.subsong_index;
            sources_.push_back(std::move(snapshot->source));
            audio_sources_->push_back(snapshot->audio);
            track_labels_.push_back(std::move(snapshot->track_label));
        }
        ++capture_index_;
    } while (capture_index_ < requested_item_count_ && timer.elapsed() < capture_budget_ms);

    if (capture_index_ < requested_item_count_) {
        loading_text_ = QStringLiteral("Preparing metadata grid… %1/%2")
                            .arg(capture_index_)
                            .arg(requested_item_count_);
        emit changed();
        QTimer::singleShot(0, this, &TaggerSession::captureSources);
        return;
    }
    source_reader_ = {};
    if (sources_.empty()) {
        summary_ = QStringLiteral("Properties unavailable");
        setStatus(QStringLiteral("Read-only metadata preview"));
        loading_text_ = QStringLiteral("The selected tracks are no longer available");
        emit changed();
        return;
    }
    startSelection();
}

void TaggerSession::startSelection() {
    selection_watcher_.setFuture(QtConcurrent::run(
        [sources = std::move(sources_), preferred = std::move(preferred_fields_),
         access = services_.tools.access, token = technical_cancellation_.token()]() mutable {
            auto prepared =
                metadata::capture_uncached_metadata_sources(std::move(sources), access, token);
            if (!prepared) {
                return std::make_shared<SelectionResult>(std::unexpected(prepared.error()));
            }
            std::vector<std::string_view> preferred_views;
            preferred_views.reserve(preferred.size());
            for (const auto& field : preferred) {
                preferred_views.emplace_back(field);
            }
            return std::make_shared<SelectionResult>(
                metadata::StagedMetadataSelection::create(std::move(*prepared), preferred_views));
        }));
}

void TaggerSession::finishSelection() {
    const auto result = selection_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The selection task returned no result");
        summary_ = QStringLiteral("Properties unavailable");
        setStatus(QStringLiteral("Read-only metadata preview"));
        loading_text_ = message;
        emit changed();
        return;
    }
    buildGrid(std::move(**result));
}

void TaggerSession::buildGrid(metadata::StagedMetadataSelection selection) {
    const auto item_count = selection.item_count();
    const auto source_count = selection.distinct_source_count();
    const auto field_count = selection.field_count();
    const auto revision_count = selection.item_revision_count();
    loaded_item_count_ = item_count;
    selected_item_count_ = item_count;
    loaded_source_count_ = source_count;
    loaded_field_count_ = field_count;
    selection_summary_ =
        QStringLiteral("%1 of %2 files selected · %3 %4 · %5 fields")
            .arg(item_count)
            .arg(item_count)
            .arg(source_count)
            .arg(pluralized(source_count, QStringLiteral("source"), QStringLiteral("sources")))
            .arg(field_count);
    revision_summary_ = revision_count == item_count
                            ? QStringLiteral("source revisions captured")
                            : QStringLiteral("%1 rows have no captured source revision")
                                  .arg(item_count - revision_count);

    grid_model_ = new MetadataGridModel(std::move(selection), std::move(track_labels_), this);
    aggregate_model_ = new MetadataAggregateModel(grid_model_, this);
    file_selection_ = new QItemSelectionModel(grid_model_, this);
    updateDraftState(0, false, false);

    connect(file_selection_, &QItemSelectionModel::selectionChanged, this, [this] {
        selection_debounce_.start();
        updateTechnicalSummary();
    });
    connect(grid_model_, &MetadataGridModel::draftStateChanged, this,
            [this](const int patch_count, const bool can_undo, const bool can_redo) {
                sticky_status_.clear();
                invalidateWritePlan();
                updateDraftState(patch_count, can_undo, can_redo);
            });
    connect(grid_model_, &MetadataGridModel::editRejected, this, [this](const QString& message) {
        setStatus(QStringLiteral("Draft edit rejected · %1").arg(message));
        emit changed();
    });
    connect(aggregate_model_, &MetadataAggregateModel::editRejected, this,
            [this](const QString& message) {
                setStatus(QStringLiteral("Draft edit rejected · %1").arg(message));
                emit changed();
            });
    connect(aggregate_model_, &MetadataAggregateModel::selectionProjectionChanged, this,
            [this](const bool ready, const int selected_count) {
                if (ready) {
                    updateDraftState(draft_count_, can_undo_, can_redo_);
                    return;
                }
                setStatus(QStringLiteral("Preparing metadata for %1 selected %2…")
                              .arg(selected_count)
                              .arg(selected_count == 1 ? QStringLiteral("file")
                                                       : QStringLiteral("files")));
                emit changed();
            });
    emit gridReady();
    if (grid_model_->rowCount() > 0) {
        file_selection_->setCurrentIndex(grid_model_->index(0, 0), QItemSelectionModel::NoUpdate);
        const QItemSelection all{grid_model_->index(0, 0),
                                 grid_model_->index(grid_model_->rowCount() - 1,
                                                    grid_model_->columnCount() - 1)};
        file_selection_->select(all, QItemSelectionModel::ClearAndSelect);
        updateSelectionProjection();
    }
    stageAutomaticTransformations();
    updateTechnicalSummary();
    emit gridFilled();
    emit changed();
}

std::vector<std::size_t> TaggerSession::selectedItems() const {
    std::vector<std::size_t> selected_items;
    if (file_selection_ == nullptr) {
        return selected_items;
    }
    const auto rows = file_selection_->selectedRows(0);
    selected_items.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
        if (row.isValid() && row.row() >= 0) {
            selected_items.push_back(static_cast<std::size_t>(row.row()));
        }
    }
    std::ranges::sort(selected_items);
    return selected_items;
}

std::vector<std::size_t> TaggerSession::itemsOrAll() const {
    auto items = selectedItems();
    if (items.empty() && grid_model_ != nullptr) {
        items.reserve(grid_model_->selection().item_count());
        for (std::size_t item_index = 0U; item_index < grid_model_->selection().item_count();
             ++item_index) {
            items.push_back(item_index);
        }
    }
    return items;
}

void TaggerSession::updateSelectionProjection() {
    if (aggregate_model_ == nullptr || file_selection_ == nullptr) {
        return;
    }
    auto selected_items = selectedItems();
    selected_item_count_ = selected_items.size();
    selection_summary_ = QStringLiteral("%1 of %2 files selected · %3 %4 · %5 fields")
                             .arg(selected_item_count_)
                             .arg(loaded_item_count_)
                             .arg(loaded_source_count_)
                             .arg(pluralized(loaded_source_count_, QStringLiteral("source"),
                                             QStringLiteral("sources")))
                             .arg(loaded_field_count_);
    updateDraftState(draft_count_, can_undo_, can_redo_);
    updateArtworkScope(selected_items);
    aggregate_model_->setSelectedItems(std::move(selected_items));
    emit changed();
}

void TaggerSession::updateArtworkScope(const std::span<const std::size_t> selected_items) {
    if (artwork_ == nullptr || grid_model_ == nullptr) {
        return;
    }
    std::vector<MetadataArtworkScopeSource> scope;
    const auto bounded_source_capacity =
        std::min(selected_items.size(), metadata_artwork_source_limit + 1U);
    scope.reserve(bounded_source_capacity);
    std::unordered_map<std::string_view, std::size_t> source_positions;
    source_positions.reserve(bounded_source_capacity);
    bool source_limit_exceeded = false;
    for (const auto item_index : selected_items) {
        const auto& source = grid_model_->selection().source(item_index);
        const auto [position, inserted] = source_positions.emplace(source.raw_path, scope.size());
        if (!inserted) {
            auto& existing = scope[position->second];
            existing.occurrence_indexes.push_back(item_index);
            ++existing.occurrence_count;
            if (existing.captured_revision != source.source_revision) {
                existing.captured_revision_consistent = false;
            }
            continue;
        }
        const auto row = static_cast<int>(
            std::min(item_index, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        scope.push_back(MetadataArtworkScopeSource{
            .raw_path = source.raw_path,
            .captured_revision = source.source_revision,
            .label = grid_model_->trackLabel(row),
            .occurrence_indexes = {item_index},
            .occurrence_count = 1U,
            .captured_revision_consistent = true,
        });
        if (scope.size() > metadata_artwork_source_limit) {
            source_limit_exceeded = true;
            break;
        }
    }
    artwork_->setScope(std::move(scope), source_limit_exceeded);

    // Cover fetching needs one unambiguous release: every selected file must
    // carry the same MUSICBRAINZ_ALBUMID, draft or embedded — so an Identify
    // result enables it before Apply has run.
    std::optional<QString> release_id;
    auto release_consistent = !selected_items.empty();
    const auto release_column = grid_model_->fieldColumn(QStringLiteral("MUSICBRAINZ_ALBUMID"));
    if (release_consistent && release_column) {
        for (const auto item_index : selected_items) {
            const auto row = static_cast<int>(
                std::min(item_index, static_cast<std::size_t>(std::numeric_limits<int>::max())));
            const auto values = grid_model_->index(row, *release_column)
                                    .data(metadata_cell_values_role)
                                    .toStringList();
            const auto value = values.isEmpty() ? QString{} : values.front().trimmed();
            if (value.isEmpty() || (release_id && *release_id != value)) {
                release_consistent = false;
                break;
            }
            release_id = value;
        }
    }
    artwork_->setCoverArtRelease(release_consistent && release_column ? std::move(release_id)
                                                                      : std::nullopt);
}

core::Result<QString> TaggerSession::storeCoverArtImage(const QString& release_id,
                                                        const QByteArray& bytes) {
    const auto png = bytes.size() > 8 && bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a"));
    const auto jpeg = bytes.size() > 3 && static_cast<unsigned char>(bytes.at(0)) == 0xFFU &&
                      static_cast<unsigned char>(bytes.at(1)) == 0xD8U;
    if (!png && !jpeg) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "the Cover Art Archive image is neither PNG nor JPEG",
            .context = {},
        });
    }
    if (!cover_art_directory_) {
        auto directory = std::make_unique<QTemporaryDir>();
        if (!directory->isValid()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::io,
                .message = "no temporary directory holds the downloaded cover",
                .context = {},
            });
        }
        cover_art_directory_ = std::move(directory);
    }
    const auto path = cover_art_directory_->filePath(
        release_id + (png ? QStringLiteral("-front.png") : QStringLiteral("-front.jpg")));
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::io,
            .message = "the downloaded cover could not be stored",
            .context = {},
        });
    }
    file.close();
    return path;
}

// The status line.

void TaggerSession::setStatus(const QString& text, const bool rich) {
    status_ = text;
    status_rich_ = rich;
}

// Event outcomes (staged results, unavailable Apply) must survive the
// asynchronous selection-projection refreshes that rewrite the footer; they
// stay until the next real draft change clears them.
void TaggerSession::showStickyStatus(const QString& text) {
    sticky_status_ = text;
    setStatus(text, text.contains(QStringLiteral("<a href")));
    emit changed();
}

void TaggerSession::updateDraftState(const int patch_count, const bool can_undo,
                                     const bool can_redo) {
    draft_count_ = patch_count;
    can_undo_ = can_undo;
    can_redo_ = can_redo;
    summary_ = patch_count == 0 ? selection_summary_
                                : QStringLiteral("%1 · %2 staged %3")
                                      .arg(selection_summary_)
                                      .arg(patch_count)
                                      .arg(patch_count == 1 ? QStringLiteral("change")
                                                            : QStringLiteral("changes"));
    if (!sticky_status_.isEmpty()) {
        setStatus(sticky_status_, sticky_status_.contains(QStringLiteral("<a href")));
    } else if (artwork_ != nullptr && artwork_->hasPendingChanges()) {
        setStatus(QStringLiteral("Artwork changes pending · Apply saves tags and covers together"));
    } else {
        setStatus(!save_tags_
                      ? QStringLiteral("Save tags is off · tag edits stay in the draft and "
                                       "Rename/Move uses the file's current tags")
                      : (patch_count == 0
                             ? QStringLiteral("No pending edits")
                             : QStringLiteral("Draft only · nothing is written until you apply")));
    }
    updateApplySummary();
    emit changed();
}

void TaggerSession::updateApplySummary() {
    QStringList parts;
    if (save_tags_ && draft_count_ > 0) {
        parts << QStringLiteral("tags");
    }
    if (artwork_ != nullptr && artwork_->hasPendingChanges()) {
        parts << QStringLiteral("covers");
    }
    if (rename_files_) {
        parts << QStringLiteral("rename");
    }
    if (move_files_) {
        parts << QStringLiteral("move");
    }
    apply_summary_ = parts.isEmpty()
                         ? QString{}
                         : QStringLiteral("Apply: %1").arg(parts.join(QStringLiteral(" · ")));
}

// Enablement.

bool TaggerSession::selectionReady() const {
    return aggregate_model_ != nullptr && aggregate_model_->summaryReady() &&
           aggregate_model_->selectedItemCount() > 0U;
}

bool TaggerSession::canAddField() const { return selectionReady() && !field_name_dialog_open_; }

bool TaggerSession::canRemoveFields() const { return selectionReady() && has_selected_fields_; }

bool TaggerSession::canEditValues() const {
    return !exact_values_dialog_open_ && selectionReady() && has_current_field_;
}

bool TaggerSession::canTransform() const {
    return !transformation_dialog_open_ && grid_model_ != nullptr && selectionReady() &&
           !exact_values_dialog_open_ && !field_name_dialog_open_ && !write_plan_running_ &&
           !apply_running_ && !artwork_operation_running_;
}

bool TaggerSession::canIdentify() const {
    return canSuggest() && static_cast<bool>(services_.musicbrainz.fetch) &&
           !identify_dialog_open_;
}

bool TaggerSession::canScanReplayGain() const { return canSuggest() && !replaygain_running_; }

bool TaggerSession::canApply() const {
    const auto has_metadata_effect = save_tags_ && draft_count_ > 0;
    const auto has_path_effect = rename_files_ || move_files_;
    return grid_model_ != nullptr &&
           (has_metadata_effect || has_path_effect ||
            (artwork_ != nullptr && artwork_->hasPendingChanges())) &&
           !transformation_catalog_loading_ && !write_plan_running_ && !apply_running_ &&
           !artwork_operation_running_;
}

bool TaggerSession::fileListEnabled() const {
    return !artwork_operation_running_ && (artwork_ == nullptr || !artwork_->hasPendingChanges());
}

void TaggerSession::setFieldSelection(const bool has_selected_fields,
                                      const bool has_current_field) {
    if (has_selected_fields_ == has_selected_fields && has_current_field_ == has_current_field) {
        return;
    }
    has_selected_fields_ = has_selected_fields;
    has_current_field_ = has_current_field;
    emit changed();
}

void TaggerSession::setFieldNameDialogOpen(const bool open) {
    field_name_dialog_open_ = open;
    emit changed();
}

void TaggerSession::setExactValuesDialogOpen(const bool open) {
    exact_values_dialog_open_ = open;
    emit changed();
}

void TaggerSession::setTransformationDialogOpen(const bool open) {
    transformation_dialog_open_ = open;
    emit changed();
}

void TaggerSession::setIdentifyDialogOpen(const bool open) {
    identify_dialog_open_ = open;
    emit changed();
}

// The draft.

void TaggerSession::undo() {
    if (grid_model_ != nullptr) {
        static_cast<void>(grid_model_->undo());
    }
}

void TaggerSession::redo() {
    if (grid_model_ != nullptr) {
        static_cast<void>(grid_model_->redo());
    }
}

void TaggerSession::discardAll() {
    if (grid_model_ != nullptr) {
        static_cast<void>(grid_model_->discardAll());
    }
}

void TaggerSession::statusLinkActivated(const QString& link) {
    if (link == QStringLiteral("undo-automatic")) {
        undo();
    }
    if (link == QStringLiteral("cancel-replaygain")) {
        replaygain_cancellation_.request_cancellation();
    }
    if (link == QStringLiteral("retry-replaygain") && !replaygain_retry_items_.empty()) {
        startReplayGainScan(replaygain_retry_items_);
    }
}

int TaggerSession::addField(const QString& name) {
    if (aggregate_model_ == nullptr) {
        return -1;
    }
    std::vector<int> selected_rows;
    if (file_selection_ != nullptr) {
        const auto indexes = file_selection_->selectedRows(0);
        selected_rows.reserve(static_cast<std::size_t>(indexes.size()));
        for (const auto& index : indexes) {
            selected_rows.push_back(index.row());
        }
    }
    const auto result = aggregate_model_->ensureField(name);
    if (!result) {
        setStatus(QStringLiteral("Field could not be added · %1")
                      .arg(display_utf8(result.error().message)));
        emit changed();
        return -1;
    }
    const auto trimmed_name = name.trimmed();
    const auto encoded_name = trimmed_name.toUtf8();
    const auto canonical_name = metadata::canonicalize_field_name(
        std::string_view{encoded_name.constData(), static_cast<std::size_t>(encoded_name.size())});
    std::erase_if(recent_field_names_, [&canonical_name](const std::string& recent) {
        return metadata::canonicalize_field_name(recent) == canonical_name;
    });
    recent_field_names_.insert(
        recent_field_names_.begin(),
        std::string{encoded_name.constData(), static_cast<std::size_t>(encoded_name.size())});
    constexpr auto maximum_recent_field_names = std::size_t{20U};
    if (recent_field_names_.size() > maximum_recent_field_names) {
        recent_field_names_.resize(maximum_recent_field_names);
    }
    if (file_selection_ != nullptr && grid_model_ != nullptr) {
        QItemSelection restored_selection;
        for (const auto row : selected_rows) {
            const auto track = grid_model_->index(row, 0);
            restored_selection.select(track, track);
        }
        file_selection_->select(restored_selection,
                                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    loaded_field_count_ = static_cast<std::size_t>(aggregate_model_->rowCount());
    updateSelectionProjection();
    return *result;
}

QStringList TaggerSession::fieldNameSuggestions(const QString& query) const {
    using metadata::MetadataFieldSuggestionCandidate;
    using metadata::MetadataFieldSuggestionKind;

    std::vector<MetadataFieldSuggestionCandidate> candidates;
    const auto catalog = metadata::metadata_field_suggestion_catalog();
    const auto present_count = grid_model_ == nullptr ? 0U : grid_model_->selection().field_count();
    candidates.reserve(present_count + recent_field_names_.size() + catalog.size());
    if (grid_model_ != nullptr) {
        const auto& selection = grid_model_->selection();
        for (std::size_t index = 0U; index < selection.field_count(); ++index) {
            const auto& field = selection.field(index);
            if (field.present_item_count > 0U) {
                candidates.push_back(MetadataFieldSuggestionCandidate{
                    .display_name = field.display_name,
                    .kind = MetadataFieldSuggestionKind::present,
                });
            }
        }
    }
    for (const auto& recent : recent_field_names_) {
        candidates.push_back(MetadataFieldSuggestionCandidate{
            .display_name = recent,
            .kind = MetadataFieldSuggestionKind::recent,
        });
    }
    candidates.insert(candidates.end(), catalog.begin(), catalog.end());

    const auto encoded = query.toUtf8();
    const auto suggestions = metadata::suggest_metadata_field_names(
        std::string_view{encoded.constData(), static_cast<std::size_t>(encoded.size())},
        candidates);
    QStringList display_names;
    display_names.reserve(static_cast<qsizetype>(suggestions.size()));
    for (const auto& suggestion : suggestions) {
        display_names.push_back(display_utf8(suggestion.display_name));
    }
    return display_names;
}

void TaggerSession::replaceValues(const int row, std::vector<std::string> values) {
    if (aggregate_model_ != nullptr) {
        static_cast<void>(aggregate_model_->replaceRowValues(row, std::move(values)));
    }
}

// Staging that introduces new fields inserts grid columns, and inserted
// columns are not part of the existing row selection — Qt then reports the
// rows as no longer fully selected and the fields pane projects an empty
// selection. Re-selecting the same rows keeps the view truthful.
bool TaggerSession::stageTransformation(const metadata::MetadataTransformationPreview& preview,
                                        const QStringList& step_sources) {
    if (grid_model_ == nullptr) {
        return false;
    }
    QList<int> selected_rows;
    if (file_selection_ != nullptr) {
        const auto rows = file_selection_->selectedRows();
        selected_rows.reserve(rows.size());
        for (const auto& row : rows) {
            selected_rows.push_back(row.row());
        }
    }
    const auto columns_before = grid_model_->columnCount();
    if (!grid_model_->stageTransformation(preview, step_sources)) {
        return false;
    }
    if (grid_model_->columnCount() != columns_before && file_selection_ != nullptr &&
        !selected_rows.isEmpty()) {
        QItemSelection restored;
        const auto last_column = grid_model_->columnCount() - 1;
        for (const auto row : selected_rows) {
            restored.select(grid_model_->index(row, 0), grid_model_->index(row, last_column));
        }
        file_selection_->select(restored, QItemSelectionModel::ClearAndSelect);
    }
    loaded_field_count_ = grid_model_->selection().field_count();
    updateSelectionProjection();
    return true;
}

// Scripts.

std::vector<TaggerSession::Script> TaggerSession::scripts() const {
    std::vector<Script> scripts;
    scripts.reserve(transformation_catalog_.size());
    for (const auto& entry : transformation_catalog_) {
        scripts.push_back(Script{.id = QString::fromStdString(entry.id.to_string()),
                                 .name = display_utf8(entry.chain.name),
                                 .automatic = entry.automatic});
    }
    return scripts;
}

QString TaggerSession::scriptStatus() const {
    if (transformation_catalog_loading_ && transformation_catalog_.empty()) {
        return QStringLiteral("Loading saved scripts…");
    }
    if (transformation_catalog_.empty()) {
        return {};
    }
    const auto automatic_count = std::ranges::count_if(
        transformation_catalog_, [](const auto& entry) { return entry.automatic; });
    return QStringLiteral("%1 of %2 checked · run in the order shown")
        .arg(automatic_count)
        .arg(transformation_catalog_.size());
}

void TaggerSession::reloadScripts(const std::optional<core::StableId> selected) {
    loadTransformationCatalog(selected);
}

void TaggerSession::loadTransformationCatalog(const std::optional<core::StableId> selected) {
    const auto rebuilt = [this](const std::optional<core::StableId>& selected_id) {
        std::ranges::sort(transformation_catalog_, [](const auto& left, const auto& right) {
            if (left.chain.name != right.chain.name) {
                return left.chain.name < right.chain.name;
            }
            return left.id.to_string() < right.id.to_string();
        });
        emit scriptsChanged(selected_id ? QString::fromStdString(selected_id->to_string())
                                        : QString{});
        emit changed();
    };
    if (!services_.transformation_store.load) {
        transformation_catalog_.clear();
        transformation_catalog_loading_ = false;
        rebuilt(std::nullopt);
        return;
    }
    transformation_catalog_loading_ = true;
    emit changed();
    const QPointer self{this};
    services_.transformation_store.load(
        [self, selected, rebuilt](std::vector<persistence::SavedMetadataTransformationChain> chains,
                                  QString error) mutable {
            if (!self) {
                return;
            }
            self->transformation_catalog_loading_ = false;
            if (!error.isEmpty()) {
                self->transformation_catalog_.clear();
                self->setStatus(
                    QStringLiteral("Could not load saved transformations · %1").arg(error));
                rebuilt(std::nullopt);
                return;
            }
            self->transformation_catalog_ = std::move(chains);
            rebuilt(selected);
            self->stageAutomaticTransformations();
        });
}

void TaggerSession::toggleAutomaticScript(const QString& script_id, const bool enabled) {
    const auto id = core::StableId::parse(script_id.toStdString());
    if (!id) {
        return;
    }
    if (transformation_catalog_loading_ || !services_.transformation_store.save) {
        emit scriptsChanged(script_id);
        return;
    }
    const auto found = std::ranges::find(transformation_catalog_, *id,
                                         &persistence::SavedMetadataTransformationChain::id);
    if (found == transformation_catalog_.end() || found->automatic == enabled) {
        return;
    }
    auto updated = *found;
    updated.automatic = enabled;
    auto retained_update = updated;
    transformation_catalog_loading_ = true;
    emit changed();
    const QPointer self{this};
    services_.transformation_store.save(
        std::move(updated), [self, updated = std::move(retained_update)](QString error) mutable {
            if (!self) {
                return;
            }
            self->transformation_catalog_loading_ = false;
            const auto updated_id = QString::fromStdString(updated.id.to_string());
            if (!error.isEmpty()) {
                self->setStatus(
                    QStringLiteral("Could not update automatic transformation · %1").arg(error));
                emit self->scriptsChanged(updated_id);
                emit self->changed();
                return;
            }
            const auto retained =
                std::ranges::find(self->transformation_catalog_, updated.id,
                                  &persistence::SavedMetadataTransformationChain::id);
            if (retained != self->transformation_catalog_.end()) {
                *retained = updated;
            }
            self->setStatus(QStringLiteral("%1 will %2stage its edits automatically.")
                                .arg(display_utf8(updated.chain.name),
                                     updated.automatic ? QString{} : QStringLiteral("no longer ")));
            emit self->scriptsChanged(updated_id);
            emit self->changed();
            if (updated.automatic) {
                self->stageAutomaticTransformations();
            }
        });
}

// Picard runs tagging scripts the moment new metadata arrives and saves
// exactly what it displays. Trackknife goes one step further per the
// user's model: automatic scripts also stage over plain local baselines,
// so every write is what the grid shows — never a hidden apply-time pass.
void TaggerSession::stageAutomaticTransformations() {
    if (grid_model_ == nullptr || automatic_stage_running_ || proposal_running_ ||
        write_plan_running_ || apply_running_) {
        return;
    }
    auto combined = combinedAutomaticChain();
    if (!combined || combined->chain.actions.empty()) {
        return;
    }
    automatic_step_sources_ = combined->step_sources;
    std::vector<std::size_t> items;
    items.reserve(grid_model_->selection().item_count());
    for (std::size_t item_index = 0U; item_index < grid_model_->selection().item_count();
         ++item_index) {
        items.push_back(item_index);
    }
    if (items.empty()) {
        return;
    }
    automatic_stage_running_ = true;
    auto selection = grid_model_->sharedSelection();
    auto draft = grid_model_->patches();
    automatic_watcher_.setFuture(QtConcurrent::run(
        [selection = std::move(selection), draft = std::move(draft), items = std::move(items),
         combined = std::move(combined->chain)]() mutable {
            using PreviewResult = core::Result<metadata::MetadataTransformationPreview>;
            return std::make_shared<PreviewResult>(metadata::plan_metadata_transformation(
                *selection, draft, items, std::move(combined)));
        }));
}

void TaggerSession::finishAutomaticStage() {
    automatic_stage_running_ = false;
    const auto result = automatic_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The script task returned no result");
        setStatus(QStringLiteral("Automatic scripts staged nothing · %1").arg(message));
        emit changed();
        return;
    }
    const auto& preview = **result;
    if (preview.cells.empty()) {
        return;
    }
    if (grid_model_ == nullptr || !stageTransformation(preview, automatic_step_sources_)) {
        return;
    }
    QStringList contributing;
    for (const auto& cell : preview.cells) {
        const auto step = static_cast<qsizetype>(cell.last_action_index);
        if (step < automatic_step_sources_.size()) {
            const auto name =
                automatic_step_sources_.at(step).section(QStringLiteral(" · step "), 0, 0);
            if (!contributing.contains(name)) {
                contributing.push_back(name);
            }
        }
    }
    const auto source_name =
        contributing.size() == 1 ? contributing.constFirst() : QStringLiteral("Automatic scripts");
    showStickyStatus(
        QStringLiteral("%1 staged %2 %3 across %4 %5 · <a href=\"undo-automatic\">Undo</a>")
            .arg(source_name.toHtmlEscaped())
            .arg(preview.cells.size())
            .arg(pluralized(preview.cells.size(), QStringLiteral("edit"), QStringLiteral("edits")))
            .arg(preview.changed_item_count)
            .arg(pluralized(preview.changed_item_count, QStringLiteral("file"),
                            QStringLiteral("files"))));
}

std::optional<TaggerSession::AutomaticChainPlan> TaggerSession::combinedAutomaticChain() {
    AutomaticChainPlan plan{
        .chain =
            metadata::MetadataTransformationChain{
                .schema_version = 1U,
                .name = "Automatic saved scripts",
                .actions = {},
            },
        .step_sources = {},
    };
    const metadata::MetadataTransformationLimits limits;
    for (const auto& saved : transformation_catalog_) {
        if (!saved.automatic) {
            continue;
        }
        if (saved.chain.actions.size() > limits.actions - plan.chain.actions.size()) {
            setStatus(QStringLiteral("Automatic scripts exceed the %1-step combined limit; "
                                     "disable or shorten a script.")
                          .arg(limits.actions));
            emit changed();
            return std::nullopt;
        }
        const auto name = display_utf8(saved.chain.name);
        for (std::size_t step = 0U; step < saved.chain.actions.size(); ++step) {
            plan.step_sources.push_back(QStringLiteral("%1 · step %2").arg(name).arg(step + 1U));
        }
        plan.chain.actions.insert(plan.chain.actions.end(), saved.chain.actions.begin(),
                                  saved.chain.actions.end());
    }
    return plan;
}

// Suggestions and MusicBrainz.

void TaggerSession::startProposals() {
    if (grid_model_ == nullptr || proposal_running_ || write_plan_running_ || apply_running_) {
        return;
    }
    auto items = itemsOrAll();
    if (items.size() < 2U) {
        setStatus(QStringLiteral("Suggestions need at least two files that share an album"));
        emit changed();
        return;
    }
    proposal_running_ = true;
    setStatus(QStringLiteral("Looking for suggestions across %1 files…").arg(items.size()));
    emit changed();
    auto selection = grid_model_->sharedSelection();
    auto draft = grid_model_->patches();
    proposal_watcher_.setFuture(QtConcurrent::run(
        [selection = std::move(selection), draft = std::move(draft), items = std::move(items)] {
            using PreviewResult = core::Result<metadata::MetadataTransformationPreview>;
            auto proposals = metadata::propose_selection_consistency(*selection, draft, items);
            if (!proposals) {
                return std::make_shared<PreviewResult>(
                    std::unexpected(std::move(proposals.error())));
            }
            return std::make_shared<PreviewResult>(
                metadata::metadata_proposal_preview(*selection, draft, *proposals, 0.75));
        }));
}

void TaggerSession::finishProposals() {
    proposal_running_ = false;
    const auto result = proposal_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The suggestion task returned no result");
        setStatus(QStringLiteral("No suggestions · %1").arg(message));
        emit changed();
        return;
    }
    const auto& preview = **result;
    if (preview.cells.empty()) {
        setStatus(QStringLiteral("No suggestions · the selected files already agree"));
        emit changed();
        return;
    }
    if (grid_model_ == nullptr ||
        !stageTransformation(preview, QStringList{display_utf8(preview.chain.name)})) {
        emit changed();
        return;
    }
    auto staged_status =
        QStringLiteral("Staged %1 %2 across %3 %4 from %5 · review the colored values, "
                       "then Apply")
            .arg(preview.cells.size())
            .arg(pluralized(preview.cells.size(), QStringLiteral("suggestion"),
                            QStringLiteral("suggestions")))
            .arg(preview.changed_item_count)
            .arg(pluralized(preview.changed_item_count, QStringLiteral("file"),
                            QStringLiteral("files")))
            .arg(display_utf8(preview.chain.name));
    staged_status += replayGainStatusLinks();
    showStickyStatus(staged_status);
    stageAutomaticTransformations();
}

std::optional<TaggerSession::Identify> TaggerSession::identifyRequest() const {
    if (grid_model_ == nullptr || proposal_running_ || identify_dialog_open_ ||
        !services_.musicbrainz.fetch) {
        return std::nullopt;
    }
    auto items = itemsOrAll();
    if (items.empty()) {
        return std::nullopt;
    }
    const auto& selection = grid_model_->selection();
    Identify request;
    request.descriptors.reserve(items.size());
    request.local_paths.reserve(items.size());
    for (const auto item_index : items) {
        const auto& source = selection.source(item_index);
        const auto& baseline = source.baseline;
        request.local_paths.push_back(QFile::decodeName(
            QByteArray{source.raw_path.data(), static_cast<qsizetype>(source.raw_path.size())}));
        musicbrainz::LocalTrackDescriptor descriptor{
            .title = baseline.first_effective_value("title").value_or(std::string{}),
            .artist = baseline.first_effective_value("artist").value_or(std::string{}),
            .album = baseline.first_effective_value("album").value_or(std::string{}),
            .track_number = {},
            .disc_number = {},
            .duration_ms = {},
        };
        if (const auto number = baseline.first_effective_value("tracknumber")) {
            descriptor.track_number = parse_position_number(*number);
        }
        if (const auto disc = baseline.first_effective_value("discnumber")) {
            descriptor.disc_number = parse_position_number(*disc);
        }
        if (request.release.isEmpty() && !descriptor.album.empty()) {
            request.release = display_utf8(descriptor.album);
        }
        if (request.artist.isEmpty()) {
            const auto album_artist = baseline.first_effective_value("albumartist");
            request.artist = display_utf8(
                album_artist && !album_artist->empty() ? *album_artist : descriptor.artist);
        }
        request.descriptors.push_back(std::move(descriptor));
    }
    request.items = std::move(items);
    return request;
}

void TaggerSession::applyMusicBrainzProposals(metadata::MetadataProposalSet proposals) {
    if (grid_model_ == nullptr || proposal_running_) {
        return;
    }
    proposal_running_ = true;
    setStatus(QStringLiteral("Matching the MusicBrainz release to the draft…"));
    emit changed();
    auto selection = grid_model_->sharedSelection();
    auto draft = grid_model_->patches();
    proposal_watcher_.setFuture(QtConcurrent::run(
        [selection = std::move(selection), draft = std::move(draft), set = std::move(proposals)] {
            using PreviewResult = core::Result<metadata::MetadataTransformationPreview>;
            return std::make_shared<PreviewResult>(
                metadata::metadata_proposal_preview(*selection, draft, set, 0.5));
        }));
}

// ReplayGain.

void TaggerSession::setReplayGainGrouping(const int index) {
    replaygain_grouping_ = std::clamp(index, 0, 4);
    emit changed();
}

void TaggerSession::setReplayGainExpression(const QString& expression) {
    replaygain_expression_ = expression;
}

// The M7 scan (ADR-0100): measure the selection on the bounded parallel
// graph and stage REPLAYGAIN_* values as ordinary colored draft edits — the
// grid is the write, exactly like every provider.
void TaggerSession::startReplayGainScan(std::vector<std::size_t> forced_items) {
    if (grid_model_ == nullptr || replaygain_running_ || proposal_running_ || apply_running_ ||
        write_plan_running_) {
        return;
    }
    auto items = std::move(forced_items);
    if (items.empty()) {
        items = itemsOrAll();
    }
    if (items.empty()) {
        return;
    }
    loudness::LoudnessGrouping grouping;
    switch (replaygain_grouping_) {
    case 0:
        grouping.mode = loudness::LoudnessGroupingMode::release;
        break;
    case 1:
        grouping.mode = loudness::LoudnessGroupingMode::release_merged_discs;
        break;
    case 2:
        grouping.mode = loudness::LoudnessGroupingMode::selection_album;
        break;
    case 3:
        grouping.mode = loudness::LoudnessGroupingMode::track;
        break;
    default:
        grouping.mode = loudness::LoudnessGroupingMode::format_expression;
        grouping.expression = replaygain_expression_.trimmed().toStdString();
        if (grouping.expression.empty()) {
            setStatus(QStringLiteral("Enter a tkfmt-1 grouping expression, e.g. %album%"));
            emit changed();
            return;
        }
        break;
    }
    replaygain_retry_items_.clear();

    replaygain_running_ = true;
    replaygain_cancellation_ = core::CancellationSource{};
    const auto cancellation = replaygain_cancellation_.token();
    replaygain_completed_ = std::make_shared<std::atomic_size_t>(0U);
    replaygain_total_ = items.size();
    replaygain_progress_timer_.start();
    setStatus(QStringLiteral("Measuring loudness · 0 of %1 files · "
                             "<a href=\"cancel-replaygain\">Stop</a>")
                  .arg(replaygain_total_),
              true);
    emit changed();

    auto selection = grid_model_->sharedSelection();
    auto draft = grid_model_->patches();
    const std::shared_ptr<const std::vector<MetadataPropertiesAudioSource>> audio_sources{
        audio_sources_};
    // ADR-0148/0149: captured before the worker starts.
    const QSettings stored;
    ReplayGainScanSettings settings;
    settings.grouping = std::move(grouping);
    settings.true_peak =
        stored.value(QLatin1String(SettingsKeys::replaygain_true_peak_key), false).toBool();
    settings.sidecar_only =
        stored.value(QLatin1String(SettingsKeys::replaygain_sidecar_only_key), false).toBool();
    replaygain_watcher_.setFuture(QtConcurrent::run(
        [selection = std::move(selection), draft = std::move(draft), items = std::move(items),
         audio_sources, settings = std::move(settings), completed = replaygain_completed_,
         cancellation, scanner = services_.tools.scanner] {
            return run_replaygain_scan(selection, draft, items, audio_sources, settings, completed,
                                       cancellation, scanner);
        }));
}

void TaggerSession::finishReplayGainScan() {
    replaygain_running_ = false;
    replaygain_progress_timer_.stop();
    const auto outcome = replaygain_watcher_.result();
    if (!outcome || !outcome->proposals) {
        const auto message = outcome ? display_utf8(outcome->proposals.error().message)
                                     : QStringLiteral("The loudness scan returned no result");
        setStatus(QStringLiteral("No ReplayGain values staged · %1").arg(message));
        emit changed();
        return;
    }
    replaygain_retry_items_ = outcome->retry_items;
    replaygain_export_rows_ = outcome->export_rows;
    if (!outcome->problems.empty()) {
        showFeedback(
            QStringLiteral("ReplayGain scan problems"),
            QStringLiteral("%1 %2 measured no usable loudness; every other file is staged.")
                .arg(outcome->problems.size())
                .arg(pluralized(outcome->problems.size(), QStringLiteral("file"),
                                QStringLiteral("files"))),
            std::vector<PreparationFeedbackRow>{outcome->problems});
    }
    if (outcome->proposals->items.empty()) {
        showStickyStatus(QStringLiteral("No ReplayGain values staged · nothing measurable in "
                                        "the selection%1")
                             .arg(replayGainStatusLinks()));
        return;
    }
    setStatus(status_);
    emit changed();
    applyMusicBrainzProposals(std::move(*outcome->proposals));
}

QString TaggerSession::replayGainStatusLinks() const {
    QString links;
    if (!replaygain_retry_items_.empty()) {
        links += QStringLiteral(" · <a href=\"retry-replaygain\">Retry %1 failed</a>")
                     .arg(replaygain_retry_items_.size());
    }
    if (!replaygain_export_rows_.isEmpty()) {
        links += QStringLiteral(" · <a href=\"export-replaygain\">Export results</a>");
    }
    return links;
}

void TaggerSession::exportReplayGainResults(const QString& path) {
    if (replaygain_export_rows_.isEmpty() || path.isEmpty()) {
        return;
    }
    QSaveFile output{path};
    if (!output.open(QIODevice::WriteOnly)) {
        showStickyStatus(QStringLiteral("Export failed · %1").arg(output.errorString()));
        return;
    }
    QString body = QStringLiteral(
        "track,file,integrated_lufs,track_gain_db,track_peak,album_key,album_gain_db,"
        "album_peak,status,peak_kind\n");
    body += replaygain_export_rows_.join(QLatin1Char('\n'));
    body += QLatin1Char('\n');
    const auto bytes = body.toUtf8();
    if (output.write(bytes) != bytes.size() || !output.commit()) {
        showStickyStatus(QStringLiteral("Export failed · %1").arg(output.errorString()));
        return;
    }
    const auto row_count = static_cast<std::size_t>(replaygain_export_rows_.size());
    showStickyStatus(QStringLiteral("Exported %1 %2 to %3%4")
                         .arg(row_count)
                         .arg(pluralized(row_count, QStringLiteral("row"), QStringLiteral("rows")))
                         .arg(path.toHtmlEscaped())
                         .arg(replayGainStatusLinks()));
}

// ADR-0147: per track, where each effective loudness value comes from —
// draft, sidecar, CUE segment, or embedded tags.
TaggerSession::Provenance TaggerSession::loudnessProvenance() const {
    Provenance provenance;
    if (grid_model_ == nullptr) {
        return provenance;
    }
    const auto items = itemsOrAll();
    const auto& selection = grid_model_->selection();
    const auto& patches = grid_model_->patches();
    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> loudness_fields{{
        {"replaygaintrackgain", "Track gain"},
        {"replaygaintrackpeak", "Track peak"},
        {"replaygainalbumgain", "Album gain"},
        {"replaygainalbumpeak", "Album peak"},
        {"r128trackgain", "R128 track"},
        {"r128albumgain", "R128 album"},
    }};
    provenance.headers << QStringLiteral("Track");
    for (const auto& [canonical, header] : loudness_fields) {
        provenance.headers << QString::fromUtf8(header.data(),
                                                static_cast<qsizetype>(header.size()));
    }
    for (const auto item_index : items) {
        QStringList row{grid_model_->trackLabel(static_cast<int>(item_index))};
        for (const auto& [canonical, header] : loudness_fields) {
            QString cell_text = QStringLiteral("—");
            if (const auto field_index = selection.field_index(canonical)) {
                if (const auto* patch = patches.patch(item_index, *field_index)) {
                    cell_text =
                        patch->kind == metadata::StagedMetadataPatchKind::remove_field
                            ? QStringLiteral("removed · draft")
                            : QStringLiteral("%1 · draft")
                                  .arg(display_utf8(patch->values.empty() ? std::string{}
                                                                          : patch->values.front()));
                } else if (const auto* cell = selection.cell(item_index, *field_index);
                           cell != nullptr && !cell->values.empty()) {
                    cell_text = QStringLiteral("%1 · %2").arg(
                        display_utf8(cell->values.front()),
                        display_utf8(
                            std::string{metadata::field_provenance_name(cell->provenance)}));
                }
            }
            row << cell_text;
        }
        provenance.rows.push_back(std::move(row));
    }
    return provenance;
}

// What Apply does.

bool TaggerSession::layoutsAvailable() const {
    return !output_profiles_loading_ && !output_layout_catalog_.empty();
}

bool TaggerSession::destinationsAvailable() const {
    return !output_profiles_loading_ && !destination_catalog_.empty();
}

bool TaggerSession::layoutReady() const {
    return editing_output_layout_id_.has_value() &&
           std::ranges::any_of(output_layout_catalog_, [this](const auto& entry) {
               return entry.id == *editing_output_layout_id_;
           });
}

bool TaggerSession::destinationReady() const {
    return editing_destination_id_.has_value() &&
           std::ranges::any_of(destination_catalog_, [this](const auto& entry) {
               return entry.id == *editing_destination_id_;
           });
}

bool TaggerSession::renameAvailable() const {
    return !output_profiles_loading_ && layoutReady() &&
           static_cast<bool>(services_.file_plan_applier_factory);
}

bool TaggerSession::moveAvailable() const { return renameAvailable() && destinationReady(); }

QString TaggerSession::renameTooltip() const {
    if (!services_.file_plan_applier_factory) {
        return QStringLiteral("File publication is unavailable");
    }
    return layoutReady() ? QStringLiteral("Generate a new basename with the saved layout")
                         : QStringLiteral("Select a saved naming layout first");
}

QString TaggerSession::moveTooltip() const {
    if (!services_.file_plan_applier_factory) {
        return QStringLiteral("File publication is unavailable");
    }
    return layoutReady() && destinationReady()
               ? QStringLiteral("Move below the saved destination using the saved layout")
               : QStringLiteral("Select a saved layout and destination first");
}

void TaggerSession::setSaveTags(const bool on) {
    if (save_tags_ == on) {
        return;
    }
    save_tags_ = on;
    invalidateWritePlan();
    updateDraftState(draft_count_, can_undo_, can_redo_);
}

void TaggerSession::chooseRename(const bool on) {
    wants_rename_ = on;
    setRenameFiles(on);
    rememberActionChoices();
}

void TaggerSession::chooseMove(const bool on) {
    wants_move_ = on;
    setMoveFiles(on);
    rememberActionChoices();
}

void TaggerSession::setRenameFiles(const bool on) {
    if (rename_files_ == on) {
        return;
    }
    rename_files_ = on;
    invalidateWritePlan();
}

void TaggerSession::setMoveFiles(const bool on) {
    if (move_files_ == on) {
        return;
    }
    move_files_ = on;
    invalidateWritePlan();
}

std::vector<TaggerSession::Choice> TaggerSession::layouts() const {
    std::vector<Choice> choices;
    choices.reserve(output_layout_catalog_.size());
    for (const auto& saved : output_layout_catalog_) {
        choices.push_back(Choice{.id = QString::fromStdString(saved.id.to_string()),
                                 .name = display_utf8(saved.profile.name)});
    }
    return choices;
}

std::vector<TaggerSession::Choice> TaggerSession::destinations() const {
    std::vector<Choice> choices;
    choices.reserve(destination_catalog_.size());
    for (const auto& saved : destination_catalog_) {
        choices.push_back(Choice{.id = QString::fromStdString(saved.id.to_string()),
                                 .name = display_utf8(saved.profile.name)});
    }
    return choices;
}

int TaggerSession::layoutIndex() const {
    if (!editing_output_layout_id_) {
        return -1;
    }
    const auto found = std::ranges::find(output_layout_catalog_, *editing_output_layout_id_,
                                         &persistence::SavedOutputLayoutProfile::id);
    return found == output_layout_catalog_.end()
               ? -1
               : static_cast<int>(std::distance(output_layout_catalog_.begin(), found));
}

int TaggerSession::destinationIndex() const {
    if (!editing_destination_id_) {
        return -1;
    }
    const auto found = std::ranges::find(destination_catalog_, *editing_destination_id_,
                                         &persistence::SavedDestinationProfile::id);
    return found == destination_catalog_.end()
               ? -1
               : static_cast<int>(std::distance(destination_catalog_.begin(), found));
}

void TaggerSession::selectLayout(const int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= output_layout_catalog_.size()) {
        editing_output_layout_id_.reset();
    } else {
        editing_output_layout_id_ = output_layout_catalog_[static_cast<std::size_t>(index)].id;
    }
    reconcileOutputProfileChoices();
    invalidateWritePlan();
}

void TaggerSession::selectDestination(const int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= destination_catalog_.size()) {
        editing_destination_id_.reset();
    } else {
        editing_destination_id_ = destination_catalog_[static_cast<std::size_t>(index)].id;
    }
    reconcileOutputProfileChoices();
    invalidateWritePlan();
}

void TaggerSession::reloadOutputProfiles() { loadOutputProfiles(); }

void TaggerSession::loadOutputProfiles() {
    output_profiles_loading_ = true;
    reconcileOutputProfileChoices();
    if (!services_.output_profile_store.load) {
        output_profiles_loading_ = false;
        output_profile_status_ = QStringLiteral("Output-profile persistence is unavailable");
        reconcileOutputProfileChoices();
        emit outputProfilesChanged();
        emit changed();
        return;
    }
    const QPointer self{this};
    services_.output_profile_store.load(
        [self](std::vector<persistence::SavedOutputLayoutProfile> layouts,
               std::vector<persistence::SavedDestinationProfile> destinations,
               QString error) mutable {
            if (!self) {
                return;
            }
            self->output_profiles_loading_ = false;
            if (!error.isEmpty()) {
                self->output_profile_status_ =
                    QStringLiteral("Could not load output profiles · %1").arg(error);
                self->reconcileOutputProfileChoices();
                emit self->outputProfilesChanged();
                emit self->changed();
                return;
            }
            self->output_layout_catalog_ = std::move(layouts);
            self->destination_catalog_ = std::move(destinations);
            // The one asked for, else the first: a layout or destination that
            // has gone is not a reason to have none.
            if (self->layoutIndex() < 0) {
                self->editing_output_layout_id_ =
                    self->output_layout_catalog_.empty()
                        ? std::nullopt
                        : std::optional{self->output_layout_catalog_.front().id};
            }
            if (self->destinationIndex() < 0) {
                self->editing_destination_id_ =
                    self->destination_catalog_.empty()
                        ? std::nullopt
                        : std::optional{self->destination_catalog_.front().id};
            }
            self->output_profile_status_ =
                QStringLiteral("%1 naming %2 · %3 move %4")
                    .arg(self->output_layout_catalog_.size())
                    .arg(self->output_layout_catalog_.size() == 1U ? QStringLiteral("layout")
                                                                   : QStringLiteral("layouts"))
                    .arg(self->destination_catalog_.size())
                    .arg(self->destination_catalog_.size() == 1U
                             ? QStringLiteral("destination")
                             : QStringLiteral("destinations"));
            self->reconcileOutputProfileChoices();
            emit self->outputProfilesChanged();
            emit self->changed();
        });
}

void TaggerSession::reconcileOutputProfileChoices() {
    if (!renameAvailable() && rename_files_) {
        setRenameFiles(false);
    }
    if (!moveAvailable() && move_files_) {
        setMoveFiles(false);
    }
    // ADR-0238: chosen before, and possible now.
    if (wants_rename_ && renameAvailable() && !rename_files_) {
        setRenameFiles(true);
    }
    if (wants_move_ && moveAvailable() && !move_files_) {
        setMoveFiles(true);
    }
    emit changed();
}

void TaggerSession::rememberActionChoices() const {
    QSettings settings;
    settings.setValue(QLatin1String(remembered_save_tags_key), save_tags_);
    settings.setValue(QLatin1String(remembered_rename_key), wants_rename_);
    settings.setValue(QLatin1String(remembered_move_key), wants_move_);
    if (editing_output_layout_id_) {
        settings.setValue(QLatin1String(remembered_layout_key),
                          QString::fromStdString(editing_output_layout_id_->to_string()));
    }
    // Destinations are the engine's: one remembered for each.
    if (editing_destination_id_) {
        settings.setValue(QLatin1String(remembered_destination_prefix) +
                              services_.output_profile_store.destinations_key,
                          QString::fromStdString(editing_destination_id_->to_string()));
    }
}

// Apply.

void TaggerSession::invalidateWritePlan() {
    ++write_plan_generation_;
    write_plan_cancellation_.request_cancellation();
    write_plan_cancellation_ = core::CancellationSource{};
    updateApplySummary();
    emit changed();
}

void TaggerSession::startWritePlan() {
    const auto artwork_intents = artwork_ != nullptr
                                     ? artwork_->pendingIntents()
                                     : std::vector<metadata::ArtworkWritePlanIntent>{};
    const operations::PreparationOperationSelection operation_selection{
        .save_tags = save_tags_ || !artwork_intents.empty(),
        .rename_files = rename_files_,
        .move_files = move_files_,
        .replaygain = false,
    };
    const auto cover_policy = SettingsKeys::artworkPolicy();
    const auto has_path_operation =
        operation_selection.rename_files || operation_selection.move_files;
    if (grid_model_ == nullptr || write_plan_running_ ||
        (!operation_selection.save_tags && !has_path_operation)) {
        return;
    }
    const auto refuse = [this](const QString& text) {
        setStatus(text);
        emit changed();
    };
    if (has_path_operation && !artwork_intents.empty() && cover_policy.write_folder_image) {
        refuse(QStringLiteral("Save folder covers before renaming or moving these files"));
        return;
    }
    std::optional<operations::OutputLayoutProfile> output_layout;
    std::optional<operations::DestinationProfile> destination;
    if (has_path_operation) {
        if (!editing_output_layout_id_) {
            refuse(QStringLiteral("Select a saved naming layout before applying"));
            return;
        }
        const auto layout = std::ranges::find(output_layout_catalog_, *editing_output_layout_id_,
                                              &persistence::SavedOutputLayoutProfile::id);
        if (layout == output_layout_catalog_.end()) {
            refuse(QStringLiteral("The selected naming layout is unavailable"));
            return;
        }
        output_layout = layout->profile;
        if (operation_selection.move_files) {
            if (!editing_destination_id_) {
                refuse(QStringLiteral("Select a saved move destination before applying"));
                return;
            }
            const auto selected_destination =
                std::ranges::find(destination_catalog_, *editing_destination_id_,
                                  &persistence::SavedDestinationProfile::id);
            if (selected_destination == destination_catalog_.end()) {
                refuse(QStringLiteral("The selected move destination is unavailable"));
                return;
            }
            destination = selected_destination->profile;
        }
    }
    auto draft = save_tags_ ? grid_model_->patches() : metadata::StagedMetadataPatchSet{};
    std::vector<std::size_t> items;
    items.reserve(grid_model_->selection().item_count());
    for (std::size_t item_index = 0U; item_index < grid_model_->selection().item_count();
         ++item_index) {
        items.push_back(item_index);
    }
    if (items.empty() || (draft.empty() && !has_path_operation && artwork_intents.empty())) {
        return;
    }

    ++write_plan_generation_;
    write_plan_job_generation_ = write_plan_generation_;
    write_plan_cancellation_.request_cancellation();
    write_plan_cancellation_ = core::CancellationSource{};
    const auto selection = grid_model_->sharedSelection();
    const auto cancellation = write_plan_cancellation_.token();
    const QSettings stored;
    const metadata::MetadataWritePlanOptions plan_options{
        .sidecar_loudness =
            stored.value(QLatin1String(SettingsKeys::replaygain_sidecar_only_key), false).toBool(),
        .true_peak_loudness =
            stored.value(QLatin1String(SettingsKeys::replaygain_true_peak_key), false).toBool()};
    write_plan_running_ = true;
    if (artwork_ != nullptr) {
        artwork_->setWorking(true);
    }
    setStatus(QStringLiteral("Checking files…"));
    emit changed();
    write_plan_watcher_.setFuture(QtConcurrent::run(
        [selection, draft = std::move(draft), items = std::move(items), operation_selection,
         output_layout = std::move(output_layout), destination = std::move(destination),
         cancellation, plan_options, artwork_intents, cover_policy,
         tools = services_.tools]() mutable {
            // WYSIWYG apply: the plan writes exactly the staged draft.
            // Automatic scripts already staged their edits into the grid.
            const auto metadata_context_change_count =
                (operation_selection.save_tags ? draft.patch_count() : 0U) + artwork_intents.size();
            std::optional<metadata::MetadataWritePlan> metadata_plan;
            if (operation_selection.save_tags && !draft.empty()) {
                auto revalidated = metadata::build_metadata_write_plan(
                    *selection, draft, tools.access, cancellation, plan_options);
                if (!revalidated) {
                    return std::make_shared<WritePlanResult>(
                        std::unexpected(std::move(revalidated.error())));
                }
                metadata_plan = std::move(*revalidated);
            }

            if (!artwork_intents.empty()) {
                // ADR-0237: images of this computer handed over first when the
                // engine writes; planned against the files where they are.
                auto staged = stageReplacements(artwork_intents, tools, cancellation);
                if (!staged) {
                    return std::make_shared<WritePlanResult>(std::unexpected(staged.error()));
                }
                auto art = operations::plan_artwork_storage(*staged, cover_policy, cancellation,
                                                            artworkFitterFor(tools), tools.artwork);
                if (!art) {
                    return std::make_shared<WritePlanResult>(std::unexpected(art.error()));
                }
                auto merged = metadata::merge_artwork_write_plan(
                    metadata_plan.value_or(metadata::MetadataWritePlan{}), std::move(*art));
                if (!merged) {
                    return std::make_shared<WritePlanResult>(std::unexpected(merged.error()));
                }
                metadata_plan = std::move(*merged);
            }
            std::optional<operations::OutputPathPlan> path_plan;
            std::optional<operations::OutputPathPreflight> path_preflight;
            if (operation_selection.rename_files || operation_selection.move_files) {
                const metadata::StagedMetadataPatchSet actual_source_tags;
                const auto& naming_selection = *selection;
                const auto& naming_context =
                    operation_selection.save_tags ? draft : actual_source_tags;
                auto documents = metadata::materialize_metadata_draft(
                    naming_selection, naming_context, items, cancellation);
                if (!documents) {
                    return std::make_shared<WritePlanResult>(
                        std::unexpected(std::move(documents.error())));
                }
                std::vector<operations::OutputPathPlanningItem> planning_items;
                planning_items.reserve(items.size());
                for (std::size_t position = 0U; position < items.size(); ++position) {
                    const auto item_index = items[position];
                    const auto& source = naming_selection.source(item_index);
                    if (!source.source_revision) {
                        return std::make_shared<WritePlanResult>(std::unexpected(core::Error{
                            .code = core::ErrorCode::conflict,
                            .message = "File path planning requires a fresh source revision "
                                       "for every selected track",
                            .context = {{.key = "item", .value = std::to_string(item_index)}},
                        }));
                    }
                    planning_items.push_back(operations::OutputPathPlanningItem{
                        .item_index = item_index,
                        .source_raw_path = source.raw_path,
                        .source_revision = *source.source_revision,
                        .final_metadata = std::move((*documents)[position]),
                    });
                }
                auto planned = operations::plan_output_paths(
                    planning_items,
                    operations::OutputPathOperationSelection{
                        .rename_files = operation_selection.rename_files,
                        .move_files = operation_selection.move_files,
                    },
                    std::move(*output_layout), std::move(destination), {}, cancellation);
                if (!planned) {
                    return std::make_shared<WritePlanResult>(
                        std::unexpected(std::move(planned.error())));
                }
                path_plan = std::move(*planned);
                if (path_plan->ready()) {
                    auto checked =
                        tools.preflight
                            ? tools.preflight(*path_plan, cancellation)
                            : operations::preflight_output_paths(*path_plan, cancellation);
                    if (!checked) {
                        return std::make_shared<WritePlanResult>(
                            std::unexpected(std::move(checked.error())));
                    }
                    path_preflight = std::move(*checked);
                }
            }
            return std::make_shared<WritePlanResult>(operations::assemble_preparation_plan(
                operation_selection, metadata_context_change_count, std::move(metadata_plan),
                std::move(path_plan), std::move(path_preflight)));
        }));
}

void TaggerSession::finishWritePlan() {
    const auto generation = write_plan_job_generation_;
    const auto result = write_plan_watcher_.result();
    write_plan_running_ = false;
    if (artwork_ != nullptr) {
        artwork_->setWorking(false);
    }
    if (generation != write_plan_generation_) {
        emit changed();
        return;
    }
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The preparation task returned no result");
        setStatus(QStringLiteral("Nothing was changed · %1").arg(message));
        emit changed();
        return;
    }

    auto plan = std::make_shared<const operations::PreparationPlan>(std::move(**result));
    if (plan->ready()) {
        std::vector<metadata::FolderImageWritePlan> folders;
        if (plan->metadata) {
            for (const auto& source : plan->metadata->sources) {
                if (source.artwork && source.artwork->folder_image) {
                    folders.push_back(*source.artwork->folder_image);
                }
            }
        }
        emit changed();
        if (folders.empty()) {
            startApply(std::move(plan));
            return;
        }
        pending_plan_ = std::move(plan);
        emit folderImagesReviewRequested(std::move(folders));
        return;
    }

    // Blocked: nothing was written; list only what needs attention.
    std::vector<PreparationFeedbackRow> rows;
    const auto add_row = [&rows](const std::string& raw_path, const QString& detail) {
        rows.push_back(PreparationFeedbackRow{
            .file = raw_path.empty() ? QStringLiteral("Selection")
                                     : QString::fromStdString(core::display_raw_path(raw_path)),
            .detail = detail,
        });
    };
    const auto metadata_issue = [](const auto& issue) {
        return QStringLiteral("%1: %2").arg(
            display_utf8(metadata::metadata_write_plan_issue_kind_name(issue.kind)),
            display_utf8(issue.error.message));
    };
    for (const auto& issue : plan->issues) {
        if (issue.blocking) {
            add_row({}, display_utf8(issue.message));
        }
    }
    if (plan->metadata) {
        for (const auto& source : plan->metadata->sources) {
            for (const auto& issue : source.issues) {
                if (issue.blocking) {
                    add_row(source.raw_path, metadata_issue(issue));
                }
            }
        }
        for (const auto& sheet : plan->metadata->cue_sheets) {
            for (const auto& issue : sheet.issues) {
                if (issue.blocking) {
                    add_row(sheet.raw_cue_path, metadata_issue(issue));
                }
            }
        }
        for (const auto& sidecar : plan->metadata->sidecars) {
            for (const auto& issue : sidecar.issues) {
                if (issue.blocking) {
                    add_row(sidecar.raw_audio_path, metadata_issue(issue));
                }
            }
        }
    }
    if (plan->output_paths) {
        for (const auto& issue : plan->output_paths->issues) {
            if (issue.blocking) {
                add_row(issue.source_raw_path.value_or(std::string{}),
                        QStringLiteral("%1: %2").arg(
                            display_utf8(operations::output_path_plan_issue_kind_name(issue.kind)),
                            display_utf8(issue.message)));
            }
        }
    }
    if (plan->path_preflight) {
        for (const auto& issue : plan->path_preflight->issues) {
            if (issue.blocking) {
                add_row(
                    issue.source_raw_path,
                    QStringLiteral("%1: %2").arg(
                        display_utf8(operations::output_path_preflight_issue_kind_name(issue.kind)),
                        display_utf8(issue.message)));
            }
        }
    }
    setStatus(
        QStringLiteral("Nothing was changed · %1 %2")
            .arg(rows.size())
            .arg(pluralized(rows.size(), QStringLiteral("problem"), QStringLiteral("problems"))));
    emit changed();
    const auto count = rows.size();
    showFeedback(
        QStringLiteral("Apply blocked"),
        QStringLiteral("Nothing was changed. Fix the %1 below, then apply again.")
            .arg(pluralized(count, QStringLiteral("problem"), QStringLiteral("problems"))),
        std::move(rows));
}

void TaggerSession::folderImagesReviewed(const bool accepted) {
    auto plan = std::move(pending_plan_);
    if (accepted && plan) {
        startApply(std::move(plan));
    }
}

void TaggerSession::showFeedback(const QString& title, const QString& summary,
                                 std::vector<PreparationFeedbackRow> rows,
                                 const bool retry_offered) {
    emit feedbackRequested(title, summary, std::move(rows), retry_offered);
}

void TaggerSession::feedbackFinished() {
    if (apply_committed_) {
        QTimer::singleShot(0, this, &TaggerSession::closeRequested);
    } else {
        emit changed();
    }
}

void TaggerSession::requestApplyStop() {
    if (!apply_running_ || apply_stop_requested_) {
        return;
    }
    apply_stop_requested_ = true;
    apply_cancellation_.request_cancellation();
    setStatus(QStringLiteral("Stopping after the files already in progress are safe…"));
    emit changed();
}

void TaggerSession::startApply(std::shared_ptr<const operations::PreparationPlan> plan) {
    if (!plan || !plan->ready() || apply_running_) {
        return;
    }
    if (plan->has_path_operation()) {
        startFileApply(std::move(plan));
        return;
    }
    startMetadataApply(std::move(plan));
}

void TaggerSession::retryUnfinished() {
    auto plan = std::move(retry_plan_);
    if (plan) {
        startMetadataApply(std::move(plan));
    }
}

void TaggerSession::startMetadataApply(std::shared_ptr<const operations::PreparationPlan> plan) {
    if (!plan->metadata || !services_.plan_applier_factory) {
        showStickyStatus(QStringLiteral("Metadata Apply is unavailable"));
        return;
    }
    auto applier = services_.plan_applier_factory();
    if (!applier) {
        showStickyStatus(QStringLiteral("Metadata Apply is unavailable"));
        return;
    }
    active_metadata_plan_ = plan;
    retry_plan_.reset();
    apply_cancellation_.request_cancellation();
    apply_cancellation_ = core::CancellationSource{};
    apply_progress_state_ = std::make_shared<MetadataApplyProgressState>();
    apply_progress_state_->states.assign(plan->metadata->sources.size(),
                                         operations::MetadataApplySourceState::pending);
    apply_progress_state_->issues.resize(plan->metadata->sources.size());
    file_apply_progress_state_.reset();
    apply_running_ = true;
    if (artwork_ != nullptr) {
        artwork_->setWorking(true);
    }
    applying_file_paths_ = false;
    apply_stop_requested_ = false;
    apply_committed_ = metadata_had_commits_;
    const auto total = plan->metadata->sources.size();
    setStatus(QStringLiteral("Saving metadata · 0 of %1").arg(total));
    progress_maximum_ = static_cast<int>(total);
    progress_value_ = 0;
    apply_progress_timer_.start();
    emit changed();

    const auto cancellation = apply_cancellation_.token();
    const auto progress_state = apply_progress_state_;
    metadata_apply_watcher_.setFuture(
        QtConcurrent::run([plan = std::move(plan), applier = std::move(applier), progress_state,
                           cancellation]() mutable {
            const operations::MetadataApplyProgressCallback progress =
                [progress_state](const operations::MetadataApplyProgress& update) {
                    std::scoped_lock lock{progress_state->mutex};
                    if (update.source_index >= progress_state->states.size()) {
                        return;
                    }
                    progress_state->states[update.source_index] = update.state;
                    progress_state->issues[update.source_index] = update.issue;
                    progress_state->completed_sources = update.completed_sources;
                };
            return std::make_shared<core::Result<operations::MetadataApplyResult>>(
                applier(*plan->metadata, progress, cancellation));
        }));
}

void TaggerSession::startFileApply(std::shared_ptr<const operations::PreparationPlan> plan) {
    if (!plan->path_preflight || !services_.file_plan_applier_factory) {
        showStickyStatus(QStringLiteral("File publication Apply is unavailable"));
        return;
    }
    auto applier = services_.file_plan_applier_factory();
    if (!applier) {
        showStickyStatus(QStringLiteral("File publication Apply is unavailable"));
        return;
    }
    retry_plan_.reset();
    apply_cancellation_.request_cancellation();
    apply_cancellation_ = core::CancellationSource{};
    file_apply_progress_state_ = std::make_shared<FilePublicationApplyProgressState>();
    file_apply_progress_state_->states.assign(plan->path_preflight->sources.size(),
                                              operations::FilePublicationApplySourceState::pending);
    file_apply_progress_state_->issues.resize(plan->path_preflight->sources.size());
    apply_progress_state_.reset();
    apply_running_ = true;
    if (artwork_ != nullptr) {
        artwork_->setWorking(true);
    }
    applying_file_paths_ = true;
    apply_stop_requested_ = false;
    apply_committed_ = false;
    const auto total = plan->path_preflight->sources.size();
    setStatus(QStringLiteral("Updating files · 0 of %1").arg(total));
    progress_maximum_ = static_cast<int>(total);
    progress_value_ = 0;
    apply_progress_timer_.start();
    emit changed();

    const auto cancellation = apply_cancellation_.token();
    const auto progress_state = file_apply_progress_state_;
    file_apply_watcher_.setFuture(
        QtConcurrent::run([plan = std::move(plan), applier = std::move(applier), progress_state,
                           cancellation]() mutable {
            const operations::FilePublicationApplyProgressCallback progress =
                [progress_state](const operations::FilePublicationApplyProgress& update) {
                    std::scoped_lock lock{progress_state->mutex};
                    if (update.source_index >= progress_state->states.size()) {
                        return;
                    }
                    progress_state->states[update.source_index] = update.state;
                    progress_state->issues[update.source_index] = update.issue;
                    progress_state->completed_sources = update.completed_sources;
                };
            return std::make_shared<core::Result<operations::FilePublicationApplyResult>>(
                applier(*plan, progress, cancellation));
        }));
}

void TaggerSession::updateApplyProgress() {
    if (!apply_running_) {
        return;
    }
    std::size_t completed = 0U;
    std::size_t total = 0U;
    if (applying_file_paths_ && file_apply_progress_state_) {
        std::scoped_lock lock{file_apply_progress_state_->mutex};
        completed = file_apply_progress_state_->completed_sources;
        total = file_apply_progress_state_->states.size();
    } else if (apply_progress_state_) {
        std::scoped_lock lock{apply_progress_state_->mutex};
        completed = apply_progress_state_->completed_sources;
        total = apply_progress_state_->states.size();
    } else {
        return;
    }
    progress_value_ = static_cast<int>(completed);
    setStatus(QStringLiteral("%1 · %2 of %3%4")
                  .arg(applying_file_paths_ ? QStringLiteral("Updating files")
                                            : QStringLiteral("Saving metadata"))
                  .arg(completed)
                  .arg(total)
                  .arg(apply_stop_requested_ ? QStringLiteral(" · stopping…") : QString{}));
    emit changed();
}

void TaggerSession::finishMetadataApply() {
    apply_running_ = false;
    if (artwork_ != nullptr) {
        artwork_->setWorking(false);
    }
    apply_progress_timer_.stop();
    const auto result = metadata_apply_watcher_.result();
    if (result && *result) {
        metadata_had_commits_ = metadata_had_commits_ || (**result).committed_source_count() > 0U;
        apply_committed_ = metadata_had_commits_;
        if (services_.apply_observer) {
            services_.apply_observer(**result);
        }
    }
    ++write_plan_generation_;
    apply_progress_state_.reset();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The Apply task returned no result");
        setStatus(QStringLiteral("Saving metadata failed · %1").arg(message));
        emit changed();
        showFeedback(QStringLiteral("Saving metadata failed"), message, {});
        return;
    }
    const auto& outcome = **result;
    // ADR-0139/0141: committed CUE sheets and loudness sidecars count as
    // saved files; any failure keeps the editor open with the problem listed.
    const auto saved_sheets = static_cast<std::size_t>(
        std::ranges::count(outcome.cue_sheets, operations::MetadataApplySourceState::committed,
                           &operations::CueReplayGainApplyOutcome::state));
    const auto failed_sheets = static_cast<std::size_t>(
        std::ranges::count(outcome.cue_sheets, operations::MetadataApplySourceState::failed,
                           &operations::CueReplayGainApplyOutcome::state));
    const auto saved_sidecars = static_cast<std::size_t>(
        std::ranges::count(outcome.sidecars, operations::MetadataApplySourceState::committed,
                           &operations::LoudnessSidecarApplyOutcome::state));
    const auto failed_sidecars = static_cast<std::size_t>(
        std::ranges::count(outcome.sidecars, operations::MetadataApplySourceState::failed,
                           &operations::LoudnessSidecarApplyOutcome::state));
    const auto stopped_sheets = outcome.cue_sheets.size() - saved_sheets - failed_sheets +
                                outcome.sidecars.size() - saved_sidecars - failed_sidecars;
    apply_committed_ = apply_committed_ || saved_sheets > 0U || saved_sidecars > 0U;
    const auto saved = outcome.committed_source_count() + saved_sheets + saved_sidecars;
    if (saved == outcome.sources.size() + outcome.cue_sheets.size() + outcome.sidecars.size()) {
        if (artwork_ != nullptr) {
            artwork_->discardPendingChanges();
        }
        setStatus(QStringLiteral("Saved %1 %2").arg(saved).arg(
            pluralized(saved, QStringLiteral("file"), QStringLiteral("files"))));
        emit changed();
        QTimer::singleShot(0, this, &TaggerSession::closeRequested);
        return;
    }
    std::vector<PreparationFeedbackRow> rows;
    for (const auto& source : outcome.sources) {
        if (source.state == operations::MetadataApplySourceState::committed) {
            continue;
        }
        rows.push_back(PreparationFeedbackRow{
            .file = QString::fromStdString(core::display_raw_path(source.raw_path)),
            .detail =
                source.issue ? display_utf8(source.issue->message) : apply_state_text(source.state),
        });
    }
    for (const auto& sheet : outcome.cue_sheets) {
        if (sheet.state == operations::MetadataApplySourceState::committed) {
            continue;
        }
        rows.push_back(PreparationFeedbackRow{
            .file = QString::fromStdString(core::display_raw_path(sheet.raw_cue_path)),
            .detail =
                sheet.issue ? display_utf8(sheet.issue->message) : apply_state_text(sheet.state),
        });
    }
    for (const auto& sidecar : outcome.sidecars) {
        if (sidecar.state == operations::MetadataApplySourceState::committed) {
            continue;
        }
        rows.push_back(PreparationFeedbackRow{
            .file = QString::fromStdString(core::display_raw_path(sidecar.raw_audio_path)),
            .detail = sidecar.issue ? display_utf8(sidecar.issue->message)
                                    : apply_state_text(sidecar.state),
        });
    }
    const auto failed = outcome.failed_source_count() + failed_sheets + failed_sidecars;
    const auto stopped_count = outcome.cancelled_source_count() + stopped_sheets;
    const auto stopped = stopped_count > 0U && failed == 0U;
    const auto summary =
        QStringLiteral("%1 saved · %2 failed · %3 stopped. Saved files are done; the files below "
                       "need attention. Files with recovery problems may already have changed.")
            .arg(saved)
            .arg(failed)
            .arg(stopped_count);
    setStatus(QStringLiteral("%1 saved · %2 failed · %3 stopped")
                  .arg(saved)
                  .arg(failed)
                  .arg(stopped_count));
    // Retry the reviewed per-file intent, retaining its original revision and
    // fingerprints. Never reconstruct a retry from displayed or freshly read tags.
    retry_plan_.reset();
    if (active_metadata_plan_ && active_metadata_plan_->metadata && outcome.cue_sheets.empty() &&
        outcome.sidecars.empty()) {
        auto retry = std::make_shared<operations::PreparationPlan>(*active_metadata_plan_);
        std::erase_if(retry->metadata->sources, [&outcome](const auto& source) {
            const auto found = std::ranges::find(outcome.sources, source.raw_path,
                                                 &operations::MetadataApplySourceResult::raw_path);
            return found == outcome.sources.end() || found->commit ||
                   (found->state != operations::MetadataApplySourceState::failed &&
                    found->state != operations::MetadataApplySourceState::cancelled);
        });
        if (retry->ready() && !retry->metadata->sources.empty()) {
            retry_plan_ = std::move(retry);
        }
    }
    emit changed();
    showFeedback(stopped ? QStringLiteral("Save stopped") : QStringLiteral("Saved with problems"),
                 summary, std::move(rows), retry_plan_ != nullptr);
}

void TaggerSession::finishFileApply() {
    apply_running_ = false;
    if (artwork_ != nullptr) {
        artwork_->setWorking(false);
    }
    apply_progress_timer_.stop();
    const auto result = file_apply_watcher_.result();
    if (result && *result) {
        apply_committed_ = (**result).committed_source_count() > 0U;
        if (services_.file_apply_observer) {
            services_.file_apply_observer(**result);
        }
    }
    ++write_plan_generation_;
    file_apply_progress_state_.reset();
    applying_file_paths_ = false;
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The Apply task returned no result");
        setStatus(QStringLiteral("Updating files failed · %1").arg(message));
        emit changed();
        showFeedback(QStringLiteral("Updating files failed"), message, {});
        return;
    }
    const auto& outcome = **result;
    const auto changed_count = outcome.committed_source_count();
    const auto unchanged = outcome.unchanged_source_count();
    // Limited destination filesystems publish successfully but may skip
    // preservation (ADR-0111): ownership on an NFS share, say, which is
    // refused for every file on every save and leaves nothing to do about
    // it. Said once in the status bar, per file in the debug log -- never a
    // window to close, which would teach closing windows unread.
    QStringList notes;
    for (const auto& source : outcome.sources) {
        if (!source.commit) {
            continue;
        }
        for (const auto& note : source.commit->notes) {
            const auto text = display_utf8(note);
            qCDebug(tkDebug).noquote()
                << QString::fromStdString(core::display_raw_path(source.source_raw_path)) << ":"
                << text;
            if (!notes.contains(text)) {
                notes.push_back(text);
            }
        }
    }
    if (changed_count + unchanged == outcome.sources.size()) {
        if (artwork_ != nullptr) {
            artwork_->discardPendingChanges();
        }
        const auto updated =
            QStringLiteral("Updated %1 %2")
                .arg(changed_count)
                .arg(pluralized(changed_count, QStringLiteral("file"), QStringLiteral("files")));
        setStatus(updated);
        emit changed();
        if (!notes.isEmpty()) {
            emit statusMessage(updated + QStringLiteral(". ") + notes.join(QStringLiteral(". ")) +
                               QStringLiteral("."));
        }
        QTimer::singleShot(0, this, &TaggerSession::closeRequested);
        return;
    }
    std::vector<PreparationFeedbackRow> rows;
    for (const auto& source : outcome.sources) {
        if (source.state == operations::FilePublicationApplySourceState::committed ||
            source.state == operations::FilePublicationApplySourceState::unchanged) {
            continue;
        }
        rows.push_back(PreparationFeedbackRow{
            .file = QString::fromStdString(core::display_raw_path(source.source_raw_path)),
            .detail = source.issue ? display_utf8(source.issue->message)
                                   : file_apply_state_text(source.state),
        });
    }
    const auto stopped =
        outcome.cancelled_source_count() > 0U && outcome.failed_source_count() == 0U;
    const auto summary =
        QStringLiteral("%1 updated · %2 failed · %3 stopped. Updated files are done; the files "
                       "below were not touched.")
            .arg(changed_count)
            .arg(outcome.failed_source_count())
            .arg(outcome.cancelled_source_count());
    setStatus(QStringLiteral("%1 updated · %2 failed · %3 stopped")
                  .arg(changed_count)
                  .arg(outcome.failed_source_count())
                  .arg(outcome.cancelled_source_count()));
    emit changed();
    showFeedback(stopped ? QStringLiteral("Update stopped") : QStringLiteral("Updated with problems"),
                 summary, std::move(rows));
}

// Closing.

TaggerSession::CloseAnswer TaggerSession::requestClose() {
    if (artwork_ != nullptr && artwork_->hasPendingChanges() && !artwork_->isBusy()) {
        return CloseAnswer::confirm_artwork;
    }
    if (artwork_operation_running_) {
        if (artwork_ != nullptr) {
            artwork_->requestOperationCancellation();
        }
        setStatus(QStringLiteral("Cancelling artwork work after in-flight files become safe…"));
        emit changed();
        return CloseAnswer::wait;
    }
    if (apply_running_) {
        apply_cancellation_.request_cancellation();
        setStatus(QStringLiteral("Cancelling Apply after in-flight sources become safe…"));
        emit changed();
        return CloseAnswer::wait;
    }
    if (apply_committed_ || draft_count_ == 0 || grid_model_ == nullptr) {
        return CloseAnswer::close;
    }
    return CloseAnswer::confirm_drafts;
}

void TaggerSession::closing(const bool discard) {
    write_plan_cancellation_.request_cancellation();
    if (discard && grid_model_ != nullptr) {
        static_cast<void>(grid_model_->discardAll());
    }
}

// ADR-0152: the read-only technical summary, fed by a bounded background
// prober with an editor-lifetime path cache.

void TaggerSession::pumpTechnicalQueue() {
    if (technical_probing_ || technical_queue_.empty()) {
        return;
    }
    const auto path = technical_queue_.front();
    technical_queue_.pop_front();
    technical_probing_ = true;
    technical_watcher_.setFuture(QtConcurrent::run(
        [path, probe = services_.tools.probe, token = technical_cancellation_.token()]()
            -> std::pair<std::string, std::optional<TechnicalInfo>> {
            auto facts = probe ? probe(path, token) : engine::probe_local_technicals(path, token);
            if (!facts) {
                return {path, std::nullopt};
            }
            TechnicalInfo info;
            info.codec = facts->codec;
            info.sample_rate = facts->sample_rate;
            info.bits = facts->bits;
            info.channels = facts->channels;
            info.bit_rate = facts->bit_rate;
            info.duration_ms = facts->duration_ms;
            return {path, info};
        }));
}

void TaggerSession::updateTechnicalSummary() {
    if (grid_model_ == nullptr) {
        return;
    }
    const auto items = itemsOrAll();
    const auto& selection = grid_model_->selection();
    if (items.empty()) {
        technical_.clear();
        emit changed();
        return;
    }
    std::vector<std::string> paths;
    std::set<std::string> seen;
    for (const auto item : items) {
        const auto& raw = selection.source(item).raw_path;
        if (seen.insert(raw).second) {
            paths.push_back(raw);
        }
    }
    for (const auto& path : paths) {
        if (technical_cache_.contains(path) || technical_pending_.contains(path)) {
            continue;
        }
        if (technical_cache_.size() + technical_pending_.size() >= maximum_technical_probes) {
            technical_truncated_ = true;
            break;
        }
        technical_queue_.push_back(path);
        technical_pending_.insert(path);
    }
    pumpTechnicalQueue();

    // Aggregate over analyzed paths: agreement shows the value,
    // disagreement shows "mixed", unknowns stay silent (ADR-0152).
    const auto merge_text = [](std::optional<std::string>& slot, bool& mixed,
                               const std::string& value) {
        if (value.empty()) {
            return;
        }
        if (!slot) {
            slot = value;
        } else if (*slot != value) {
            mixed = true;
        }
    };
    const auto merge_number = [](std::optional<std::int64_t>& slot, bool& mixed,
                                 const std::int64_t value) {
        if (value <= 0) {
            return;
        }
        if (!slot) {
            slot = value;
        } else if (*slot != value) {
            mixed = true;
        }
    };
    std::optional<std::string> codec;
    std::optional<std::int64_t> sample_rate;
    std::optional<std::int64_t> bits;
    std::optional<std::int64_t> channels;
    std::optional<std::int64_t> bit_rate;
    bool codec_mixed = false;
    bool rate_mixed = false;
    bool bits_mixed = false;
    bool channels_mixed = false;
    bool bit_rate_mixed = false;
    std::size_t analyzing = 0U;
    std::size_t failed = 0U;
    for (const auto& path : paths) {
        const auto found = technical_cache_.find(path);
        if (found == technical_cache_.end()) {
            if (technical_pending_.contains(path)) {
                ++analyzing;
            }
            continue;
        }
        if (!found->second) {
            ++failed;
            continue;
        }
        const auto& info = *found->second;
        merge_text(codec, codec_mixed, info.codec);
        merge_number(sample_rate, rate_mixed, info.sample_rate);
        merge_number(bits, bits_mixed, info.bits);
        merge_number(channels, channels_mixed, info.channels);
        merge_number(bit_rate, bit_rate_mixed, info.bit_rate);
    }

    // Duration sums per selected item once every involved path is known:
    // logical tracks convert their sample range at the stream's rate.
    bool duration_known = analyzing == 0U && failed == 0U;
    std::int64_t total_ms = 0;
    if (duration_known) {
        for (const auto item : items) {
            const auto found = technical_cache_.find(selection.source(item).raw_path);
            if (found == technical_cache_.end() || !found->second) {
                duration_known = false;
                break;
            }
            const auto& info = *found->second;
            const auto& audio = (*audio_sources_)[item];
            const auto start_ms = [&]() -> std::int64_t {
                if (!audio.range || info.sample_rate <= 0) {
                    return 0;
                }
                return (audio.range->start_sample / info.sample_rate) * 1'000 +
                       ((audio.range->start_sample % info.sample_rate) * 1'000) / info.sample_rate;
            }();
            if (audio.range && audio.range->end_sample && info.sample_rate > 0) {
                const auto samples = *audio.range->end_sample - audio.range->start_sample;
                total_ms += (samples / info.sample_rate) * 1'000 +
                            ((samples % info.sample_rate) * 1'000) / info.sample_rate;
            } else if (info.duration_ms >= 0) {
                total_ms += std::max<std::int64_t>(info.duration_ms - start_ms, 0);
            } else {
                duration_known = false;
                break;
            }
        }
    }

    QStringList parts;
    if (items.size() > 1U) {
        parts << tr("%1 tracks").arg(items.size());
    }
    if (codec_mixed) {
        parts << tr("mixed codecs");
    } else if (codec) {
        parts << QString::fromStdString(*codec).toUpper();
    }
    if (rate_mixed) {
        parts << tr("mixed rates");
    } else if (sample_rate) {
        parts << tr("%1 Hz").arg(*sample_rate);
    }
    if (bits_mixed) {
        parts << tr("mixed depths");
    } else if (bits) {
        parts << tr("%1 bit").arg(*bits);
    }
    if (channels_mixed) {
        parts << tr("mixed channels");
    } else if (channels) {
        parts << tr("%1 ch").arg(*channels);
    }
    if (bit_rate_mixed) {
        parts << tr("mixed bitrates");
    } else if (bit_rate) {
        parts << tr("%1 kbit/s").arg((*bit_rate + 500) / 1'000);
    }
    if (duration_known) {
        const auto seconds = total_ms / 1'000;
        const auto text = seconds >= 3'600
                              ? QStringLiteral("%1:%2:%3")
                                    .arg(seconds / 3'600)
                                    .arg((seconds % 3'600) / 60, 2, 10, QLatin1Char('0'))
                                    .arg(seconds % 60, 2, 10, QLatin1Char('0'))
                              : QStringLiteral("%1:%2")
                                    .arg(seconds / 60)
                                    .arg(seconds % 60, 2, 10, QLatin1Char('0'));
        parts << (items.size() > 1U ? tr("total %1").arg(text) : text);
    }
    if (analyzing > 0U) {
        parts << tr("analyzing %1…").arg(analyzing);
    }
    if (failed > 0U) {
        parts << tr("%1 unreadable").arg(failed);
    }
    if (technical_truncated_) {
        parts << tr("first %1 files").arg(maximum_technical_probes);
    }
    technical_ = parts.join(QStringLiteral(" · "));
    emit changed();
}

} // namespace trackknife::bench
