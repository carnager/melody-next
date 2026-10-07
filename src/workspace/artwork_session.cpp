// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/artwork_session.hpp"

#include "bench/artwork_fitting.hpp"
#include "bench/settings_keys.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/operations/artwork_export.hpp"
#include "uicommon/local_artwork.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImageReader>
#include <QPointer>
#include <QSaveFile>
#include <QStandardItem>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace trackknife::bench {

namespace {

[[nodiscard]] std::string artwork_target_key(const metadata::ArtworkWritePlanIntent& intent,
                                             const bool include_occurrence) {
    auto key = intent.raw_media_path + std::string(1, '\0');
    if (include_occurrence) {
        key += std::to_string(intent.occurrence_index) + std::string(1, '\0');
    }
    if (intent.kind == metadata::ArtworkWritePlanIntentKind::add) {
        key += "add" + std::string(1, '\0') + intent.replacement_raw_path.value_or("") +
               std::string(1, '\0') + std::to_string(static_cast<int>(intent.added_role)) +
               std::string(1, '\0') + intent.added_description;
    } else {
        key += std::to_string(intent.target_ordinal);
    }
    return key;
}

[[nodiscard]] QString display_utf8(const std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] QString title_case(QString text) {
    if (!text.isEmpty()) {
        text[0] = text[0].toUpper();
    }
    return text;
}

[[nodiscard]] QString error_code_name(const core::ErrorCode code) {
    switch (code) {
    case core::ErrorCode::cancelled:
        return QStringLiteral("Cancelled");
    case core::ErrorCode::invalid_argument:
        return QStringLiteral("Invalid input");
    case core::ErrorCode::not_found:
        return QStringLiteral("Not found");
    case core::ErrorCode::conflict:
        return QStringLiteral("Changed source");
    case core::ErrorCode::unsupported:
        return QStringLiteral("Unsupported");
    case core::ErrorCode::limit_exceeded:
        return QStringLiteral("Limit exceeded");
    case core::ErrorCode::io:
        return QStringLiteral("I/O error");
    case core::ErrorCode::backend:
        return QStringLiteral("Reader error");
    case core::ErrorCode::database:
        return QStringLiteral("Database error");
    case core::ErrorCode::invariant:
        return QStringLiteral("Internal error");
    case core::ErrorCode::unauthorized:
        return QStringLiteral("Not authorized");
    }
    return QStringLiteral("Error");
}

[[nodiscard]] QString error_tool_tip(const core::Error& error) {
    auto result = display_utf8(error.message);
    for (const auto& context : error.context) {
        result +=
            QStringLiteral("\n%1: %2").arg(display_utf8(context.key), display_utf8(context.value));
    }
    return result;
}

[[nodiscard]] QString source_label(const MetadataArtworkScopeSource& source) {
    if (source.occurrence_count <= 1U) {
        return source.label;
    }
    return QStringLiteral("%1 · %2 occurrences").arg(source.label).arg(source.occurrence_count);
}

[[nodiscard]] QStandardItem* table_item(const QString& text, const QString& tool_tip = {}) {
    auto* item = new QStandardItem(text);
    item->setEditable(false);
    if (!tool_tip.isEmpty()) {
        item->setToolTip(tool_tip);
    }
    return item;
}

[[nodiscard]] QString dimensions(const metadata::ArtworkInventoryItem& item) {
    if (!item.width || !item.height) {
        return QStringLiteral("Unknown");
    }
    return QStringLiteral("%1 × %2").arg(*item.width).arg(*item.height);
}

[[nodiscard]] QString mime_label(const std::string_view mime_type) {
    if (mime_type == "image/png") {
        return QStringLiteral("PNG");
    }
    if (mime_type == "image/jpeg") {
        return QStringLiteral("JPEG");
    }
    if (mime_type == "image/gif") {
        return QStringLiteral("GIF");
    }
    if (mime_type == "image/webp") {
        return QStringLiteral("WebP");
    }
    if (mime_type == "image/bmp") {
        return QStringLiteral("BMP");
    }
    if (mime_type == "image/tiff") {
        return QStringLiteral("TIFF");
    }
    return mime_type.empty() ? QStringLiteral("Unknown type") : display_utf8(mime_type);
}

[[nodiscard]] QString format_bytes(const std::uint64_t bytes) {
    if (bytes < 1024U) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024U * 1024U) {
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

constexpr int thumbnail_edge = 60;
// ADR-0251: thumbnails carry three pixels to each one they are shown at, so
// they are sharp on a Retina or 5K display (2x) and on 3x screens, and drawn
// down on an ordinary one.
constexpr int thumbnail_density = 3;

[[nodiscard]] QImage sharp_thumbnail(const QImage& image) {
    auto thumbnail =
        image.scaled(thumbnail_edge * thumbnail_density, thumbnail_edge * thumbnail_density,
                     Qt::KeepAspectRatio, Qt::SmoothTransformation);
    thumbnail.setDevicePixelRatio(thumbnail_density);
    return thumbnail;
}
constexpr std::uint64_t maximum_thumbnail_source_bytes = 16U * 1024U * 1024U;

[[nodiscard]] metadata::ArtworkImageFile
thumbnail_evidence(const metadata::ArtworkInventoryItem& item) {
    return metadata::ArtworkImageFile{
        .raw_path = item.raw_source_path,
        .source_revision = item.source_revision,
        .mime_type = item.mime_type,
        .width = item.width,
        .height = item.height,
        .byte_size = item.byte_size,
        .content_fingerprint = item.content_fingerprint,
        .embedded_source_ordinal = item.provenance == metadata::ArtworkProvenance::embedded
                                       ? std::optional{item.source_ordinal}
                                       : std::nullopt,
    };
}

[[nodiscard]] QString artwork_state_text(const operations::ArtworkApplySourceState state) {
    using State = operations::ArtworkApplySourceState;
    switch (state) {
    case State::pending:
        return QStringLiteral("Waiting");
    case State::running:
        return QStringLiteral("Saving");
    case State::committed:
        return QStringLiteral("Saved");
    case State::failed:
        return QStringLiteral("Failed");
    case State::cancelled:
        return QStringLiteral("Stopped");
    }
    return QStringLiteral("Unknown");
}

[[nodiscard]] std::string_view export_extension(const std::string_view mime_type) {
    if (mime_type == "image/png") {
        return ".png";
    }
    if (mime_type == "image/jpeg") {
        return ".jpg";
    }
    if (mime_type == "image/gif") {
        return ".gif";
    }
    if (mime_type == "image/webp") {
        return ".webp";
    }
    if (mime_type == "image/bmp") {
        return ".bmp";
    }
    if (mime_type == "image/tiff") {
        return ".tiff";
    }
    return ".bin";
}

[[nodiscard]] metadata::ArtworkRole archive_image_role(const musicbrainz::CoverArtImage& image) {
    const auto typed = [&image](const std::string_view type) {
        return std::ranges::find(image.types, type) != image.types.end();
    };
    if (image.front || typed("Front")) {
        return metadata::ArtworkRole::front;
    }
    if (image.back || typed("Back")) {
        return metadata::ArtworkRole::back;
    }
    if (typed("Medium")) {
        return metadata::ArtworkRole::disc;
    }
    return metadata::ArtworkRole::other;
}

[[nodiscard]] QString archive_type_text(const musicbrainz::CoverArtImage& image) {
    QStringList types;
    for (const auto& type : image.types) {
        types.push_back(display_utf8(type));
    }
    if (types.isEmpty()) {
        types.push_back(image.front ? QStringLiteral("Front") : QStringLiteral("Untyped"));
    }
    return types.join(QStringLiteral(" + "));
}

// A thumbnail of encoded image bytes, refusing oversized decode surfaces
// even for small compressed inputs.
[[nodiscard]] QImage decode_thumbnail(QByteArray encoded) {
    QBuffer buffer{&encoded};
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader{&buffer};
    const auto size = reader.size();
    if (!size.isValid() || static_cast<qint64>(size.width()) * size.height() > 32 * 1024 * 1024) {
        return {};
    }
    // Enough for the largest a cover is shown from it -- the tag editor's
    // 108 px -- at three device pixels to one (ADR-0251).
    constexpr int edge = 108 * thumbnail_density;
    reader.setScaledSize(size.scaled(edge, edge, Qt::KeepAspectRatio));
    return reader.read().scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace

struct ArtworkSession::BatchResult {
    struct SourceResult {
        MetadataArtworkScopeSource scope;
        std::optional<metadata::LocalArtworkInventory> inventory;
        // Parallel to inventory->items; null images mean no decodable preview.
        std::vector<QImage> thumbnails;
        std::optional<core::Error> error;
    };

    std::size_t generation{0U};
    std::vector<SourceResult> sources;
    bool cancelled{false};
};

ArtworkSession::ArtworkSession(QObject* parent)
    : QObject(parent), status_(QStringLiteral("Open Artwork to inspect the selected files")),
      draft_help_(QStringLiteral(
          "Select covers with Ctrl/Shift or Ctrl+A. Changes are saved only with Save artwork.\n"
          "External images are shared files: Remove edits embedded covers and keeps external "
          "files.")),
      empty_text_(QStringLiteral("No artwork inventory loaded")) {
    items_model_ = new QStandardItemModel(this);
    items_model_->setHorizontalHeaderLabels({QString{}, QStringLiteral("File"),
                                             QStringLiteral("Role"), QStringLiteral("Image"),
                                             QStringLiteral("Source")});
    item_selection_ = new QItemSelectionModel(items_model_, this);
    pending_model_ = new QStandardItemModel(this);
    pending_model_->setHorizontalHeaderLabels({QStringLiteral("File"), QStringLiteral("Change"),
                                               QStringLiteral("Cover"), QStringLiteral("New image"),
                                               QStringLiteral("Before"), QStringLiteral("After")});
    pending_selection_ = new QItemSelectionModel(pending_model_, this);
    issues_model_ = new QStandardItemModel(this);
    issues_model_->setHorizontalHeaderLabels({QStringLiteral("File"),
                                              QStringLiteral("Artwork source"),
                                              QStringLiteral("Type"), QStringLiteral("Problem")});
    connect(item_selection_, &QItemSelectionModel::selectionChanged, this,
            &ArtworkSession::changed);
    connect(pending_selection_, &QItemSelectionModel::selectionChanged, this,
            &ArtworkSession::changed);

    debounce_.setSingleShot(true);
    debounce_.setInterval(40);
    connect(&debounce_, &QTimer::timeout, this, &ArtworkSession::startInventory);
    connect(&watcher_, &QFutureWatcherBase::finished, this, &ArtworkSession::finishInventory);
    connect(&preview_watcher_, &QFutureWatcherBase::finished, this,
            &ArtworkSession::finishPendingPreviews);
    connect(&plan_watcher_, &QFutureWatcherBase::finished, this, &ArtworkSession::finishReview);
    connect(&apply_watcher_, &QFutureWatcherBase::finished, this, &ArtworkSession::finishApply);
    connect(&export_watcher_, &QFutureWatcherBase::finished, this, &ArtworkSession::finishExport);
    apply_progress_timer_.setInterval(50);
    connect(&apply_progress_timer_, &QTimer::timeout, this, &ArtworkSession::updateApplyProgress);
    export_progress_timer_.setInterval(50);
    connect(&export_progress_timer_, &QTimer::timeout, this, &ArtworkSession::updateExportProgress);
    connect(&paste_watcher_, &QFutureWatcher<core::Result<QString>>::finished, this, [this] {
        cover_fetch_running_ = false;
        emit operationRunningChanged(false);
        const auto result = paste_watcher_.result();
        if (result) {
            stageFrontCover(*result);
        } else {
            setStatus(display_utf8(result.error().message));
        }
        emit changed();
    });
}

ArtworkSession::~ArtworkSession() {
    paste_watcher_.waitForFinished();
    preview_cancellation_.request_cancellation();
    cancellation_.request_cancellation();
    mutation_cancellation_.request_cancellation();
    if (job_running_) {
        watcher_.waitForFinished();
    }
    if (plan_running_) {
        plan_watcher_.waitForFinished();
    }
    if (apply_running_) {
        apply_watcher_.waitForFinished();
    }
    if (export_running_) {
        export_watcher_.waitForFinished();
    }
    preview_watcher_.waitForFinished();
}

void ArtworkSession::setStatus(const QString& text) { status_ = text; }

void ArtworkSession::showFront(const QImage& image, const bool mixed) {
    shown_front_ = image;
    shown_front_mixed_ = mixed;
    emit frontCoverChanged(image, mixed);
}

void ArtworkSession::setWorking(const bool working) {
    working_ = working;
    emit changed();
}

// Enablement.

bool ArtworkSession::operationIdle() const {
    return !plan_running_ && !apply_running_ && !export_running_ && !cover_fetch_running_;
}

bool ArtworkSession::mutationIdle() const {
    return static_cast<bool>(applier_factory_) && operationIdle() && !job_running_ &&
           displayed_generation_ == generation_;
}

std::vector<int> ArtworkSession::selectedItemRows() const {
    std::vector<int> rows;
    for (const auto& index : item_selection_->selectedRows()) {
        rows.push_back(index.row());
    }
    return rows;
}

bool ArtworkSession::canSave() const { return hasPendingChanges() && mutationIdle(); }

bool ArtworkSession::canDiscard() const { return hasPendingChanges() && operationIdle(); }

bool ArtworkSession::canUndoSelected() const {
    return operationIdle() && pending_selection_->hasSelection();
}

bool ArtworkSession::canAdd() const { return add_available_ && mutationIdle(); }

bool ArtworkSession::canFetch() const {
    return add_available_ && mutationIdle() && !picker_open_ &&
           (localCoversOffered() || archiveReady());
}

bool ArtworkSession::canCopy() const {
    const auto selected = selectedItemRows();
    auto copy_available = add_available_ && selected.size() == 1U;
    if (copy_available) {
        const auto row = selected.front();
        copy_available = row >= 0 && static_cast<std::size_t>(row) < copy_targets_.size();
        if (copy_available) {
            const auto& donor = copy_targets_[static_cast<std::size_t>(row)].item;
            const auto supported =
                donor.mime_type == "image/png" || donor.mime_type == "image/jpeg";
            const auto destination_count =
                static_cast<std::size_t>(std::ranges::count_if(scope_, [&](const auto& source) {
                    return donor.provenance != metadata::ArtworkProvenance::embedded ||
                           source.raw_path != donor.raw_source_path;
                }));
            copy_available = supported && donor.width && donor.height && destination_count > 0U;
        }
    }
    return copy_available && mutationIdle();
}

bool ArtworkSession::canReplace() const {
    const auto selected = selectedItemRows();
    const auto actionable = std::ranges::any_of(selected, [this](const int row) {
        return row >= 0 && static_cast<std::size_t>(row) < action_targets_.size() &&
               action_targets_[static_cast<std::size_t>(row)].has_value();
    });
    return actionable && mutationIdle();
}

bool ArtworkSession::canExport() const {
    const auto selected = selectedItemRows();
    auto export_available = !selected.empty();
    for (const auto row : selected) {
        if (row < 0 || static_cast<std::size_t>(row) >= copy_targets_.size()) {
            export_available = false;
            break;
        }
    }
    return export_available && operationIdle();
}

bool ArtworkSession::canRemoveFront() const {
    return frontEditable() && SettingsKeys::artworkPolicy().embed;
}

// The front cover.

void ArtworkSession::stageFrontCover(const QString& path) {
    if (working_ || isBusy()) {
        return;
    }
    reviewFetchedCover(QFile::encodeName(path).toStdString());
}

void ArtworkSession::pasteFrontCover(const QImage& image) {
    if (working_ || isBusy() || image.isNull() ||
        static_cast<qint64>(image.width()) * image.height() > ui::maximum_artwork_pixels) {
        return;
    }
    const auto directory = coverDraftDirectory();
    cover_fetch_running_ = true;
    emit operationRunningChanged(true);
    emit changed();
    paste_watcher_.setFuture(QtConcurrent::run([image, directory]() -> core::Result<QString> {
        const auto fail = [] {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::io,
                            .message = "Could not cache the pasted cover (maximum 16 MiB)",
                            .context = {}});
        };
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG") ||
            bytes.size() > 16 * 1024 * 1024) {
            return fail();
        }
        if (!QDir{}.mkpath(directory)) {
            return fail();
        }
        const auto path = directory + QLatin1Char('/') +
                          QString::fromLatin1(
                              QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) +
                          QStringLiteral(".png");
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
            !file.commit()) {
            return fail();
        }
        return path;
    }));
}

void ArtworkSession::removeFrontCover() {
    if (working_ || isBusy() || !SettingsKeys::artworkPolicy().embed) {
        return;
    }
    std::erase_if(pending_intents_, [](const auto& intent) {
        return intent.kind == metadata::ArtworkWritePlanIntentKind::add &&
               intent.added_role == metadata::ArtworkRole::front;
    });
    item_selection_->clearSelection();
    for (std::size_t index = 0; index < action_targets_.size(); ++index) {
        if (action_targets_[index] &&
            action_targets_[index]->item.role == metadata::ArtworkRole::front) {
            item_selection_->select(items_model_->index(static_cast<int>(index), 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    removeSelected();
    updatePendingPresentation();
    showFront({}, false);
}

// Services.

void ArtworkSession::setMutationServices(ArtworkWritePlanApplierFactory applier_factory,
                                         ArtworkApplyObserver observer) {
    applier_factory_ = std::move(applier_factory);
    apply_observer_ = std::move(observer);
    emit changed();
}

void ArtworkSession::setCoverArtService(ArtworkCoverArtService service) {
    cover_service_ = std::move(service);
    emit changed();
}

void ArtworkSession::setCoverArtRelease(std::optional<QString> release_id) {
    if (cover_release_id_ == release_id) {
        return;
    }
    cover_release_id_ = std::move(release_id);
    emit changed();
}

void ArtworkSession::requestOperationCancellation() {
    mutation_cancellation_.request_cancellation();
}

void ArtworkSession::setUnifiedApply(const bool enabled) {
    unified_apply_ = enabled;
    if (enabled) {
        draft_help_ =
            QStringLiteral("Select covers with Ctrl/Shift or Ctrl+A. Apply saves tags and artwork "
                           "together. External image files stay on disk.");
    }
    emit changed();
}

// The inventory.

void ArtworkSession::refreshStoragePolicy() {
    if (isBusy()) {
        return;
    }
    ++generation_;
    scheduleInventory();
}

void ArtworkSession::setScope(std::vector<MetadataArtworkScopeSource> sources,
                              const bool source_limit_exceeded) {
    if (scope_ == sources && source_limit_exceeded_ == source_limit_exceeded) {
        return;
    }
    scope_ = std::move(sources);
    source_limit_exceeded_ = source_limit_exceeded;
    ++generation_;
    cancellation_.request_cancellation();
    mutation_cancellation_.request_cancellation();
    debounce_.stop();
    if (active_) {
        scheduleInventory();
    }
}

void ArtworkSession::setActive(const bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    if (!active_) {
        cancellation_.request_cancellation();
        debounce_.stop();
        return;
    }
    if (displayed_generation_ != generation_) {
        scheduleInventory();
    }
}

void ArtworkSession::scheduleInventory() {
    if (!active_) {
        return;
    }
    if (scope_.empty()) {
        clearPresentation();
        setStatus(QStringLiteral("Select at least one file to inspect artwork"));
        displayed_generation_ = generation_;
        emit changed();
        return;
    }
    if (source_limit_exceeded_ || scope_.size() > metadata_artwork_source_limit) {
        clearPresentation();
        setStatus(QStringLiteral("Artwork inventory is limited to %1 physical sources; narrow the "
                                 "file selection")
                      .arg(metadata_artwork_source_limit));
        displayed_generation_ = generation_;
        emit changed();
        return;
    }
    setStatus(QStringLiteral("Preparing artwork inventory for %1 %2…")
                  .arg(scope_.size())
                  .arg(scope_.size() == 1U ? QStringLiteral("source") : QStringLiteral("sources")));
    emit changed();
    if (job_running_) {
        cancellation_.request_cancellation();
        return;
    }
    debounce_.start();
}

void ArtworkSession::startInventory() {
    if (!active_ || job_running_ || scope_.empty() || source_limit_exceeded_ ||
        scope_.size() > metadata_artwork_source_limit) {
        return;
    }
    clearPresentation();
    cancellation_ = core::CancellationSource{};
    const auto token = cancellation_.token();
    job_generation_ = generation_;
    job_running_ = true;
    setStatus(QStringLiteral("Reading artwork for %1 %2…")
                  .arg(scope_.size())
                  .arg(scope_.size() == 1U ? QStringLiteral("source") : QStringLiteral("sources")));
    emit changed();
    auto inventory_policy = metadata::default_artwork_inventory_policy();
    const auto storage = SettingsKeys::artworkPolicy();
    auto image_name = std::filesystem::path{storage.folder_image_name};
    if (!image_name.has_parent_path() && !image_name.empty()) {
        for (const auto* extension : {".jpg", ".png"}) {
            image_name.replace_extension(extension);
            if (std::ranges::none_of(inventory_policy.external_patterns, [&](const auto& pattern) {
                    return pattern.raw_basename == image_name.native();
                })) {
                inventory_policy.external_patterns.push_back(
                    {.raw_basename = image_name.native(), .role = metadata::ArtworkRole::front});
            }
        }
    }
    watcher_.setFuture(QtConcurrent::run([scope = scope_, generation = job_generation_, token,
                                          inventory_policy, artwork = tools_.artwork]() mutable {
        auto batch = std::make_shared<BatchResult>();
        batch->generation = generation;
        batch->sources.reserve(scope.size());
        for (auto& source : scope) {
            if (token.is_cancellation_requested()) {
                batch->cancelled = true;
                break;
            }
            auto read = artwork.inventory(source.raw_path, inventory_policy, token);
            if (!read && read.error().code == core::ErrorCode::cancelled) {
                batch->cancelled = true;
                break;
            }
            BatchResult::SourceResult result{
                .scope = std::move(source), .inventory = {}, .thumbnails = {}, .error = {}};
            if (read) {
                result.inventory = std::move(*read);
                result.thumbnails.reserve(result.inventory->items.size());
                for (const auto& item : result.inventory->items) {
                    if (token.is_cancellation_requested()) {
                        batch->cancelled = true;
                        break;
                    }
                    QImage thumbnail;
                    if (item.duplicate_of && *item.duplicate_of < result.thumbnails.size()) {
                        thumbnail = result.thumbnails[*item.duplicate_of];
                    } else if (auto bytes =
                                   artwork.image_bytes(thumbnail_evidence(item),
                                                       maximum_thumbnail_source_bytes, token)) {
                        thumbnail = decode_thumbnail(
                            QByteArray{reinterpret_cast<const char*>(bytes->data()),
                                       static_cast<qsizetype>(bytes->size())});
                    }
                    result.thumbnails.push_back(std::move(thumbnail));
                }
            } else {
                result.error = std::move(read.error());
            }
            batch->sources.push_back(std::move(result));
            if (batch->cancelled) {
                break;
            }
        }
        return batch;
    }));
}

void ArtworkSession::finishInventory() {
    job_running_ = false;
    const auto result = watcher_.result();
    if (!result || !active_ || result->generation != generation_) {
        if (active_ && displayed_generation_ != generation_) {
            scheduleInventory();
        }
        emit changed();
        return;
    }
    if (result->cancelled) {
        scheduleInventory();
        return;
    }
    present(*result);
    // The cover shown is the one the files carry. An image beside them is
    // the cover only where covers are kept there and nowhere else;
    // otherwise it is something Fetch offers.
    const auto storage = SettingsKeys::artworkPolicy();
    const auto folder_only = !storage.embed && storage.write_folder_image;
    for (const auto& source : result->sources) {
        if (!source.inventory) {
            continue;
        }
        for (std::size_t index = 0; index < source.inventory->items.size(); ++index) {
            const auto& item = source.inventory->items[index];
            if (item.role != metadata::ArtworkRole::front ||
                item.provenance != metadata::ArtworkProvenance::external ||
                (item.mime_type != "image/jpeg" && item.mime_type != "image/png") ||
                std::ranges::any_of(local_covers_, [&item](const LocalCover& known) {
                    return known.raw_path == item.raw_source_path;
                })) {
                continue;
            }
            auto details = QStringLiteral("Beside the files");
            if (item.width && item.height) {
                details += QStringLiteral(" · %1×%2").arg(*item.width).arg(*item.height);
            }
            local_covers_.push_back(LocalCover{
                .raw_path = item.raw_source_path,
                .thumbnail = index < source.thumbnails.size() ? source.thumbnails[index] : QImage{},
                .details = details,
            });
        }
    }
    front_image_ = {};
    front_mixed_ = false;
    std::optional<core::ContentFingerprint> first;
    bool have_source = false;
    for (const auto& source : result->sources) {
        std::optional<core::ContentFingerprint> fingerprint;
        if (source.inventory) {
            for (std::size_t index = 0; index < source.inventory->items.size(); ++index) {
                if (source.inventory->items[index].role != metadata::ArtworkRole::front ||
                    (!folder_only && source.inventory->items[index].provenance ==
                                         metadata::ArtworkProvenance::external)) {
                    continue;
                }
                fingerprint = source.inventory->items[index].content_fingerprint;
                if (!have_source && index < source.thumbnails.size()) {
                    front_image_ = source.thumbnails[index];
                }
                break;
            }
        }
        if (have_source && first != fingerprint) {
            front_mixed_ = true;
        }
        if (!have_source) {
            first = fingerprint;
        }
        have_source = true;
    }
    showFront(front_image_, front_mixed_);
    displayed_generation_ = result->generation;
    emit changed();
}

void ArtworkSession::clearPresentation() {
    items_model_->removeRows(0, items_model_->rowCount());
    issues_model_->removeRows(0, issues_model_->rowCount());
    action_targets_.clear();
    copy_targets_.clear();
    local_covers_.clear();
    add_available_ = false;
    empty_visible_ = true;
    issues_visible_ = false;
}

void ArtworkSession::present(const BatchResult& result) {
    clearPresentation();
    add_available_ = !result.sources.empty();
    std::size_t item_count = 0U;
    std::size_t issue_count = 0U;
    std::size_t read_only_count = 0U;
    const auto policy = SettingsKeys::artworkPolicy();
    const auto folder_only = !policy.embed && policy.write_folder_image;

    for (const auto& source : result.sources) {
        const auto label = source_label(source.scope);
        const auto media_path =
            QString::fromStdString(core::display_raw_path(source.scope.raw_path));
        if (!source.inventory) {
            add_available_ = false;
            ++read_only_count;
            const auto details = source.error ? error_tool_tip(*source.error)
                                              : QStringLiteral("The inventory returned no result");
            issues_model_->appendRow(
                {table_item(label, media_path), table_item(media_path, media_path),
                 table_item(source.error ? error_code_name(source.error->code)
                                         : QStringLiteral("Reader error")),
                 table_item(source.error ? display_utf8(source.error->message)
                                         : QStringLiteral("No inventory result"),
                            details)});
            ++issue_count;
            continue;
        }

        const auto& inventory = *source.inventory;
        const auto changes_available =
            applier_factory_ &&
            (folder_only ||
             (inventory.capabilities.embedded_readable &&
              metadata::is_qualified_artwork_adapter(inventory.embedded_adapter_name))) &&
            source.scope.captured_revision_consistent && source.scope.captured_revision &&
            *source.scope.captured_revision == inventory.media_revision;
        if (!changes_available) {
            add_available_ = false;
            ++read_only_count;
            // Only a blocked file earns a row; the old always-on capability
            // table restated this for every file.
            QString reason;
            if (!inventory.capabilities.embedded_readable ||
                !metadata::is_qualified_artwork_adapter(inventory.embedded_adapter_name)) {
                reason = QStringLiteral(
                    "Artwork changes need a FLAC, MP3, or MP4 file; this file is view-only");
            } else if (!source.scope.captured_revision_consistent ||
                       !source.scope.captured_revision ||
                       *source.scope.captured_revision != inventory.media_revision) {
                reason = QStringLiteral(
                    "The file changed since the tag editor opened; reopen to edit artwork");
            } else {
                reason = QStringLiteral("Artwork changes are unavailable");
            }
            issues_model_->appendRow({table_item(label, media_path),
                                      table_item(media_path, media_path),
                                      table_item(QStringLiteral("View-only")), table_item(reason)});
            ++issue_count;
        }

        for (std::size_t index = 0U; index < inventory.items.size(); ++index) {
            const auto& artwork = inventory.items[index];
            const auto raw_artwork_path =
                QString::fromStdString(core::display_raw_path(artwork.raw_source_path));
            const auto fingerprint = QString::fromStdString(
                metadata::artwork_fingerprint_hex(artwork.content_fingerprint));
            const auto embedded = artwork.provenance == metadata::ArtworkProvenance::embedded;

            auto* preview = new QStandardItem;
            preview->setEditable(false);
            if (index < source.thumbnails.size() && !source.thumbnails[index].isNull()) {
                preview->setData(sharp_thumbnail(source.thumbnails[index]), Qt::DecorationRole);
            } else {
                preview->setText(QStringLiteral("—"));
                preview->setToolTip(QStringLiteral("No preview available"));
            }

            const auto image = QStringLiteral("%1 · %2 · %3")
                                   .arg(mime_label(artwork.mime_type), dimensions(artwork),
                                        format_bytes(artwork.byte_size));
            auto image_tool_tip =
                QStringLiteral("%1 · %2 bytes\nSHA-256: %3")
                    .arg(artwork.mime_type.empty() ? QStringLiteral("Unknown MIME")
                                                   : display_utf8(artwork.mime_type))
                    .arg(artwork.byte_size)
                    .arg(fingerprint);
            if (!artwork.description.empty()) {
                image_tool_tip +=
                    QStringLiteral("\nDescription: %1").arg(display_utf8(artwork.description));
            }

            auto origin = embedded ? QStringLiteral("Embedded") : QStringLiteral("External");
            if (artwork.duplicate_of) {
                origin += QStringLiteral(" · same image as row %1").arg(*artwork.duplicate_of + 1U);
            }
            auto origin_tool_tip = raw_artwork_path;
            if (!artwork.native_type.empty()) {
                origin_tool_tip +=
                    QStringLiteral("\nNative type: %1").arg(display_utf8(artwork.native_type));
            }
            origin_tool_tip += QStringLiteral("\nPicture %1").arg(artwork.source_ordinal + 1U);

            items_model_->appendRow(
                {preview, table_item(label, media_path),
                 table_item(title_case(display_utf8(metadata::artwork_role_name(artwork.role)))),
                 table_item(image, image_tool_tip), table_item(origin, origin_tool_tip)});
            if (changes_available && embedded) {
                action_targets_.push_back(ActionTarget{.scope = source.scope, .item = artwork});
            } else {
                action_targets_.push_back(std::nullopt);
            }
            copy_targets_.push_back(ActionTarget{.scope = source.scope, .item = artwork});
            ++item_count;
        }

        for (const auto& issue : inventory.issues) {
            const auto raw_issue_path =
                QString::fromStdString(core::display_raw_path(issue.raw_source_path));
            issues_model_->appendRow(
                {table_item(label, media_path), table_item(raw_issue_path, raw_issue_path),
                 table_item(error_code_name(issue.error.code)),
                 table_item(display_utf8(issue.error.message), error_tool_tip(issue.error))});
            ++issue_count;
        }
    }

    empty_visible_ = item_count == 0U;
    if (item_count == 0U) {
        empty_text_ = QStringLiteral("No artwork found for the selected files");
    }
    issues_visible_ = issue_count > 0U;

    auto text =
        QStringLiteral("%1 %2 across %3 %4")
            .arg(item_count)
            .arg(item_count == 1U ? QStringLiteral("image") : QStringLiteral("images"))
            .arg(result.sources.size())
            .arg(result.sources.size() == 1U ? QStringLiteral("file") : QStringLiteral("files"));
    if (read_only_count > 0U) {
        text += QStringLiteral(" · %1 view-only").arg(read_only_count);
    }
    setStatus(text);
}

// The cover picker: the images beside the files, then what the Cover Art
// Archive has for the release.

bool ArtworkSession::coverServiceReady() const {
    return SettingsKeys::artworkPolicy().fetch_source == "coverartarchive" &&
           static_cast<bool>(cover_service_.fetch_listing) &&
           static_cast<bool>(cover_service_.fetch_bytes) &&
           static_cast<bool>(cover_service_.store_image);
}

bool ArtworkSession::localCoversOffered() const {
    // Offered for the files; where covers live only beside them, the image
    // there already is the cover.
    return SettingsKeys::artworkPolicy().embed && !local_covers_.empty();
}

bool ArtworkSession::archiveReady() const {
    return coverServiceReady() && cover_release_id_.has_value();
}

bool ArtworkSession::openPicker() {
    if (cover_fetch_running_ || plan_running_ || apply_running_ || !applier_factory_ ||
        picker_open_ || (!localCoversOffered() && !archiveReady())) {
        return false;
    }
    picker_open_ = true;
    const auto picker = ++picker_generation_;
    picker_rows_.clear();
    picker_status_.clear();
    picker_suggested_ = -1;
    picker_images_ = std::make_shared<std::vector<musicbrainz::CoverArtImage>>();
    picker_locals_ = localCoversOffered() ? local_covers_ : std::vector<LocalCover>{};
    for (std::size_t index = 0U; index < picker_locals_.size(); ++index) {
        const auto& local = picker_locals_[index];
        picker_rows_.push_back(PickerRow{
            .kind = QStringLiteral("local"),
            .index = index,
            .thumbnail = local.thumbnail,
            .from = QFileInfo{QFile::decodeName(local.raw_path.c_str())}.fileName(),
            .type = QStringLiteral("Front"),
            .details = local.details,
            .tool_tip = QString::fromStdString(core::display_raw_path(local.raw_path)),
        });
    }
    if (!picker_rows_.empty()) {
        picker_suggested_ = 0;
    }

    const QPointer self{this};
    if (archiveReady()) {
        picker_status_ = QStringLiteral("Asking the Cover Art Archive…");
        cover_service_.fetch_listing(
            *cover_release_id_, [self, picker](core::Result<musicbrainz::CoverArtListing> listing) {
                if (self.isNull() || !self->picker_open_ || self->picker_generation_ != picker) {
                    return;
                }
                if (!listing || listing->images.empty()) {
                    self->picker_status_ =
                        listing ? QStringLiteral("The Cover Art Archive has no images for this "
                                                 "release")
                                : QStringLiteral("The Cover Art Archive could not be asked · %1")
                                      .arg(display_utf8(listing.error().message));
                    emit self->pickerChanged();
                    return;
                }
                self->picker_status_.clear();
                *self->picker_images_ = std::move(listing->images);
                const auto& images = *self->picker_images_;
                // The archive's own front first, as it would have been fetched.
                const auto front =
                    musicbrainz::select_front_cover(musicbrainz::CoverArtListing{.images = images});
                const bool had_rows = !self->picker_rows_.empty();
                for (std::size_t index = 0U; index < images.size(); ++index) {
                    const auto& image = images[index];
                    auto details = display_utf8(image.comment);
                    if (!image.approved) {
                        details += (details.isEmpty() ? QString{} : QStringLiteral(" · ")) +
                                   QStringLiteral("not approved");
                    }
                    const auto row = self->picker_rows_.size();
                    self->picker_rows_.push_back(PickerRow{
                        .kind = QStringLiteral("archive"),
                        .index = index,
                        .thumbnail = {},
                        .from = QStringLiteral("Cover Art Archive"),
                        .type = archive_type_text(image),
                        .details = details,
                        .tool_tip = {},
                    });
                    if (!had_rows && front && index == *front) {
                        self->picker_suggested_ = static_cast<int>(row);
                    }
                    // Thumbnails trickle in through the paced fetcher.
                    if (image.thumbnail_url.empty()) {
                        continue;
                    }
                    self->cover_service_.fetch_bytes(
                        QString::fromStdString(image.thumbnail_url),
                        [self, picker, row](core::Result<QByteArray> bytes) {
                            QImage thumbnail;
                            if (self.isNull() || !self->picker_open_ ||
                                self->picker_generation_ != picker ||
                                row >= self->picker_rows_.size() || !bytes ||
                                !thumbnail.loadFromData(*bytes)) {
                                return;
                            }
                            self->picker_rows_[row].thumbnail = std::move(thumbnail);
                            emit self->pickerChanged();
                        });
                }
                if (self->picker_suggested_ < 0 && !self->picker_rows_.empty()) {
                    self->picker_suggested_ = 0;
                }
                emit self->pickerChanged();
            });
    } else if (picker_rows_.empty()) {
        picker_status_ = QStringLiteral("No images were found");
    }
    emit pickerChanged();
    emit changed();
    return true;
}

void ArtworkSession::closePicker() {
    if (!picker_open_) {
        return;
    }
    picker_open_ = false;
    ++picker_generation_;
    emit pickerClosed();
    emit changed();
}

void ArtworkSession::usePicker(const int row) {
    if (!picker_open_ || row < 0 || static_cast<std::size_t>(row) >= picker_rows_.size()) {
        return;
    }
    const auto chosen = picker_rows_[static_cast<std::size_t>(row)];
    const auto locals = picker_locals_;
    const auto images = picker_images_;
    closePicker();
    if (chosen.kind == QStringLiteral("local") && chosen.index < locals.size()) {
        reviewFetchedCover(locals[chosen.index].raw_path);
    } else if (chosen.kind == QStringLiteral("archive") && images &&
               chosen.index < images->size()) {
        useArchiveImage((*images)[chosen.index]);
    }
}

void ArtworkSession::useArchiveImage(const musicbrainz::CoverArtImage& image) {
    if (cover_fetch_running_ || plan_running_ || apply_running_ || !coverServiceReady() ||
        !cover_release_id_) {
        return;
    }
    cover_fetch_running_ = true;
    const auto role = archive_image_role(image);
    setStatus(QStringLiteral("Fetching the %1 image from the Cover Art Archive…")
                  .arg(archive_type_text(image)));
    emit changed();
    const auto generation = generation_;
    const auto identity = QStringLiteral("%1-%2-%3")
                              .arg(*cover_release_id_, QString::fromStdString(image.id),
                                   display_utf8(metadata::artwork_role_name(role)));
    const QPointer self{this};
    cover_service_.fetch_bytes(
        QString::fromStdString(image.image_url),
        [self, generation, identity, role](core::Result<QByteArray> bytes) {
            if (self.isNull()) {
                return;
            }
            self->cover_fetch_running_ = false;
            const auto refused = [&self](const QString& text) {
                self->setStatus(text);
                emit self->changed();
            };
            if (!bytes) {
                refused(QStringLiteral("No image was added · %1")
                            .arg(display_utf8(bytes.error().message)));
                return;
            }
            const auto stored = self->cover_service_.store_image(identity, *bytes);
            if (!stored) {
                refused(QStringLiteral("No image was added · %1")
                            .arg(display_utf8(stored.error().message)));
                return;
            }
            if (self->generation_ != generation) {
                refused(QStringLiteral("The selection changed while fetching; no image was added"));
                return;
            }
            const auto encoded = QFile::encodeName(*stored);
            std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
            if (role == metadata::ArtworkRole::front) {
                self->reviewFetchedCover(raw_path);
            } else {
                self->startReview(metadata::ArtworkWritePlanIntentKind::add, std::move(raw_path),
                                  role);
            }
        });
}

// Staging.

QStringList ArtworkSession::addRoles() {
    return {QStringLiteral("Front cover"), QStringLiteral("Back cover"), QStringLiteral("Artist"),
            QStringLiteral("Disc"),        QStringLiteral("Icon"),       QStringLiteral("Other")};
}

void ArtworkSession::add(const QString& path, const int role_index) {
    constexpr std::array role_values{
        metadata::ArtworkRole::front, metadata::ArtworkRole::back, metadata::ArtworkRole::artist,
        metadata::ArtworkRole::disc,  metadata::ArtworkRole::icon, metadata::ArtworkRole::other,
    };
    if (path.isEmpty() || role_index < 0 ||
        static_cast<std::size_t>(role_index) >= role_values.size()) {
        return;
    }
    const auto encoded = QFile::encodeName(path);
    startReview(metadata::ArtworkWritePlanIntentKind::add,
                std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
                role_values[static_cast<std::size_t>(role_index)]);
}

void ArtworkSession::replace(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(path);
    startReview(metadata::ArtworkWritePlanIntentKind::replace,
                std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
}

void ArtworkSession::removeSelected() {
    startReview(metadata::ArtworkWritePlanIntentKind::remove, std::nullopt);
}

void ArtworkSession::copySelected() {
    const auto selected = selectedItemRows();
    if (selected.size() != 1U || selected.front() < 0 ||
        static_cast<std::size_t>(selected.front()) >= copy_targets_.size()) {
        return;
    }
    const auto& donor = copy_targets_[static_cast<std::size_t>(selected.front())].item;
    startReview(metadata::ArtworkWritePlanIntentKind::add, donor.raw_source_path, donor.role,
                donor.description,
                donor.provenance == metadata::ArtworkProvenance::embedded ? std::optional{donor}
                                                                          : std::nullopt);
}

void ArtworkSession::startReview(const metadata::ArtworkWritePlanIntentKind kind,
                                 std::optional<std::string> replacement_raw_path,
                                 const metadata::ArtworkRole added_role,
                                 std::string added_description,
                                 std::optional<metadata::ArtworkInventoryItem> embedded_donor) {
    if (!applier_factory_ || plan_running_ || apply_running_) {
        return;
    }
    std::vector<metadata::ArtworkWritePlanIntent> intents;
    if (kind == metadata::ArtworkWritePlanIntentKind::add) {
        for (const auto& source : scope_) {
            if (embedded_donor && source.raw_path == embedded_donor->raw_source_path) {
                continue;
            }
            for (const auto occurrence_index : source.occurrence_indexes) {
                intents.push_back(metadata::ArtworkWritePlanIntent{
                    .occurrence_index = occurrence_index,
                    .raw_media_path = source.raw_path,
                    .expected_media_revision = source.captured_revision_consistent
                                                   ? source.captured_revision
                                                   : std::nullopt,
                    .target_ordinal = 0U,
                    .expected_target_fingerprint = {},
                    .kind = kind,
                    .replacement_raw_path = replacement_raw_path,
                    .added_role = added_role,
                    .added_description = added_description,
                    .replacement_embedded_source = embedded_donor,
                });
            }
        }
    } else {
        for (const auto row : selectedItemRows()) {
            if (row < 0 || static_cast<std::size_t>(row) >= action_targets_.size()) {
                continue;
            }
            const auto& target = action_targets_[static_cast<std::size_t>(row)];
            if (!target) {
                continue;
            }
            for (const auto occurrence_index : target->scope.occurrence_indexes) {
                intents.push_back(metadata::ArtworkWritePlanIntent{
                    .occurrence_index = occurrence_index,
                    .raw_media_path = target->scope.raw_path,
                    .expected_media_revision = target->scope.captured_revision_consistent
                                                   ? target->scope.captured_revision
                                                   : std::nullopt,
                    .target_ordinal = target->item.source_ordinal,
                    .expected_target_fingerprint = target->item.content_fingerprint,
                    .kind = kind,
                    .replacement_raw_path = replacement_raw_path,
                    .added_role = added_role,
                    .added_description = added_description,
                    .replacement_embedded_source = std::nullopt,
                });
            }
        }
    }
    if (intents.empty()) {
        setStatus(kind == metadata::ArtworkWritePlanIntentKind::add
                      ? QStringLiteral("Select at least one writable FLAC, MP3, or MP4 file")
                      : QStringLiteral("Select at least one writable embedded cover"));
        emit changed();
        return;
    }
    dispatchReview(std::move(intents));
}

void ArtworkSession::dispatchReview(std::vector<metadata::ArtworkWritePlanIntent> intents) {
    std::unordered_map<std::string, std::size_t> positions;
    positions.reserve(pending_intents_.size() + intents.size());
    for (std::size_t index = 0; index < pending_intents_.size(); ++index) {
        positions.emplace(artwork_target_key(pending_intents_[index], true), index);
    }
    for (auto& intent : intents) {
        const auto [position, inserted] =
            positions.emplace(artwork_target_key(intent, true), pending_intents_.size());
        if (inserted) {
            pending_intents_.push_back(std::move(intent));
        } else {
            pending_intents_[position->second] = std::move(intent);
        }
    }
    updatePendingPresentation();
}

// The fetched archive front becomes each file's front cover: replacing the
// existing embedded front picture where one exists, adding one otherwise —
// never stacking a second front.
void ArtworkSession::reviewFetchedCover(const std::string& replacement_raw_path) {
    if (!applier_factory_ || plan_running_ || apply_running_ || job_running_ ||
        displayed_generation_ != generation_ || !add_available_) {
        return;
    }
    const auto policy = SettingsKeys::artworkPolicy();
    if (!policy.embed && !policy.write_folder_image) {
        setStatus(QStringLiteral("Enable a cover destination in Cover settings"));
        emit changed();
        return;
    }
    std::erase_if(pending_intents_, [](const auto& intent) {
        return intent.kind == metadata::ArtworkWritePlanIntentKind::add &&
               intent.added_role == metadata::ArtworkRole::front;
    });
    std::vector<metadata::ArtworkWritePlanIntent> intents;
    for (const auto& source : scope_) {
        for (const auto occurrence_index : source.occurrence_indexes) {
            for (const auto& target : action_targets_) {
                if (!policy.embed || !target || target->scope.raw_path != source.raw_path ||
                    target->item.role != metadata::ArtworkRole::front) {
                    continue;
                }
                intents.push_back(metadata::ArtworkWritePlanIntent{
                    .occurrence_index = occurrence_index,
                    .raw_media_path = source.raw_path,
                    .expected_media_revision = source.captured_revision,
                    .target_ordinal = target->item.source_ordinal,
                    .expected_target_fingerprint = target->item.content_fingerprint,
                    .kind = metadata::ArtworkWritePlanIntentKind::remove,
                    .replacement_raw_path = std::nullopt,
                    .added_role = metadata::ArtworkRole::front,
                    .added_description = {},
                    .replacement_embedded_source = std::nullopt,
                });
            }
            intents.push_back(metadata::ArtworkWritePlanIntent{
                .occurrence_index = occurrence_index,
                .raw_media_path = source.raw_path,
                .expected_media_revision = source.captured_revision,
                .target_ordinal = 0U,
                .expected_target_fingerprint = {},
                .kind = metadata::ArtworkWritePlanIntentKind::add,
                .replacement_raw_path = replacement_raw_path,
                .added_role = metadata::ArtworkRole::front,
                .added_description = {},
                .replacement_embedded_source = std::nullopt,
            });
        }
    }
    if (intents.empty()) {
        setStatus(QStringLiteral("Select at least one writable FLAC, MP3, or MP4 file"));
        emit changed();
        return;
    }
    dispatchReview(std::move(intents));
}

void ArtworkSession::discardPendingChanges() {
    pending_intents_.clear();
    updatePendingPresentation();
    showFront(front_image_, front_mixed_);
}

void ArtworkSession::undoSelectedPending() {
    std::set<std::string> targets;
    for (const auto& row : pending_selection_->selectedRows()) {
        if (row.row() >= 0 && static_cast<std::size_t>(row.row()) < pending_rows_.size()) {
            targets.insert(
                artwork_target_key(pending_rows_[static_cast<std::size_t>(row.row())], false));
        }
    }
    std::erase_if(pending_intents_, [&](const auto& intent) {
        return targets.contains(artwork_target_key(intent, false));
    });
    updatePendingPresentation();
}

void ArtworkSession::updatePendingPresentation() {
    ++preview_generation_;
    preview_cancellation_.request_cancellation();
    pending_model_->removeRows(0, pending_model_->rowCount());
    pending_rows_.clear();
    std::set<std::string> displayed;
    std::set<std::pair<std::string, std::size_t>> removals;
    for (const auto& intent : pending_intents_) {
        if (intent.kind == metadata::ArtworkWritePlanIntentKind::remove) {
            removals.emplace(intent.raw_media_path, intent.target_ordinal);
        }
        if (!displayed.insert(artwork_target_key(intent, false)).second) {
            continue;
        }
        pending_rows_.push_back(intent);
        pending_model_->appendRow(
            {table_item(QString::fromStdString(core::display_raw_path(
                            std::filesystem::path{intent.raw_media_path}.filename().native())),
                        QString::fromStdString(core::display_raw_path(intent.raw_media_path))),
             table_item(title_case(
                 display_utf8(metadata::artwork_write_plan_intent_kind_name(intent.kind)))),
             table_item(
                 intent.kind == metadata::ArtworkWritePlanIntentKind::add
                     ? title_case(display_utf8(metadata::artwork_role_name(intent.added_role)))
                     : QStringLiteral("Picture %1").arg(intent.target_ordinal + 1)),
             table_item(intent.replacement_raw_path ? QString::fromStdString(core::display_raw_path(
                                                          *intent.replacement_raw_path))
                                                    : QString{}),
             table_item(intent.kind == metadata::ArtworkWritePlanIntentKind::add
                            ? QStringLiteral("None")
                            : QStringLiteral("Unavailable")),
             table_item(intent.kind == metadata::ArtworkWritePlanIntentKind::remove
                            ? QStringLiteral("Removed")
                            : QStringLiteral("Loading…"))});
        if (intent.kind != metadata::ArtworkWritePlanIntentKind::add) {
            for (std::size_t index = 0; index < action_targets_.size(); ++index) {
                const auto& target = action_targets_[index];
                if (target && target->scope.raw_path == intent.raw_media_path &&
                    target->item.provenance == metadata::ArtworkProvenance::embedded &&
                    target->item.source_ordinal == intent.target_ordinal &&
                    target->item.content_fingerprint == intent.expected_target_fingerprint) {
                    auto* before = pending_model_->item(pending_model_->rowCount() - 1, 4);
                    const auto image =
                        items_model_->index(static_cast<int>(index), 0).data(Qt::DecorationRole);
                    if (!image.isNull()) {
                        before->setText({});
                        before->setData(image, Qt::DecorationRole);
                    }
                    break;
                }
            }
        }
    }
    startPendingPreviews();
    for (std::size_t row = 0; row < action_targets_.size(); ++row) {
        const auto& target = action_targets_[row];
        const auto removed =
            target && removals.contains({target->scope.raw_path, target->item.source_ordinal});
        for (int column = 0; column < items_model_->columnCount(); ++column) {
            auto* item = items_model_->item(static_cast<int>(row), column);
            auto font = item->font();
            font.setStrikeOut(removed);
            item->setFont(font);
        }
    }
    setStatus(
        hasPendingChanges()
            ? (unified_apply_
                   ? QStringLiteral(
                         "%1 pending artwork changes · Apply saves tags and covers together")
                   : QStringLiteral("%1 pending artwork changes · review below, then Save artwork"))
                  .arg(pending_model_->rowCount())
            : QStringLiteral("No pending artwork changes"));
    emit pendingChangesChanged(hasPendingChanges());
    emit changed();
}

void ArtworkSession::startPendingPreviews() {
    if (preview_running_ || pending_rows_.empty()) {
        return;
    }
    preview_running_ = true;
    preview_job_generation_ = preview_generation_;
    preview_cancellation_ = core::CancellationSource{};
    preview_watcher_.setFuture(QtConcurrent::run([rows = pending_rows_,
                                                  token = preview_cancellation_.token(),
                                                  artwork = tools_.artwork] {
        std::vector<QImage> images(rows.size());
        std::unordered_map<std::string, QImage> cache;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (token.is_cancellation_requested()) {
                break;
            }
            const auto& intent = rows[index];
            if (intent.kind == metadata::ArtworkWritePlanIntentKind::remove) {
                continue;
            }
            const auto key =
                intent.replacement_embedded_source
                    ? intent.replacement_embedded_source->raw_source_path + std::string(1, '\0') +
                          std::to_string(intent.replacement_embedded_source->source_ordinal)
                    : intent.replacement_raw_path.value_or("");
            if (const auto found = cache.find(key); found != cache.end()) {
                images[index] = found->second;
                continue;
            }
            auto evidence =
                intent.replacement_embedded_source
                    ? core::Result<metadata::ArtworkImageFile>{thumbnail_evidence(
                          *intent.replacement_embedded_source)}
                    : metadata::read_artwork_image_file(intent.replacement_raw_path.value_or(""),
                                                        maximum_thumbnail_source_bytes, token);
            QImage thumbnail;
            if (evidence) {
                // A picture copied from another track is the engine's; one
                // picked or downloaded is this computer's.
                auto bytes =
                    intent.replacement_embedded_source
                        ? artwork.image_bytes(*evidence, maximum_thumbnail_source_bytes, token)
                        : metadata::read_artwork_image_bytes(*evidence,
                                                             maximum_thumbnail_source_bytes, token);
                if (bytes && !token.is_cancellation_requested()) {
                    thumbnail =
                        decode_thumbnail(QByteArray{reinterpret_cast<const char*>(bytes->data()),
                                                    static_cast<qsizetype>(bytes->size())});
                }
            }
            cache.emplace(key, thumbnail);
            images[index] = std::move(thumbnail);
        }
        return images;
    }));
}

void ArtworkSession::finishPendingPreviews() {
    preview_running_ = false;
    if (preview_job_generation_ != preview_generation_) {
        startPendingPreviews();
        return;
    }
    const auto images = preview_watcher_.result();
    for (std::size_t index = 0; index < images.size() && index < pending_rows_.size(); ++index) {
        if (pending_rows_[index].kind == metadata::ArtworkWritePlanIntentKind::remove) {
            continue;
        }
        auto* item = pending_model_->item(static_cast<int>(index), 5);
        item->setText(images[index].isNull() ? QStringLiteral("Unavailable") : QString{});
        item->setData(images[index].isNull() ? QImage{} : sharp_thumbnail(images[index]),
                      Qt::DecorationRole);
        if (pending_rows_[index].kind == metadata::ArtworkWritePlanIntentKind::add &&
            pending_rows_[index].added_role == metadata::ArtworkRole::front) {
            showFront(images[index], false);
        }
        item->setToolTip(
            images[index].isNull()
                ? QStringLiteral(
                      "Preview unavailable. Apply will validate the image before writing.")
                : QStringLiteral("Image to embed when you Apply"));
    }
    emit changed();
}

// Export.

void ArtworkSession::exportSelected(const QString& directory) {
    if (export_running_ || directory.isEmpty()) {
        return;
    }
    const auto selected = selectedItemRows();
    if (selected.empty()) {
        return;
    }
    const auto encoded_directory = QFile::encodeName(directory);
    const std::filesystem::path raw_directory{std::string{
        encoded_directory.constData(), static_cast<std::size_t>(encoded_directory.size())}};
    std::vector<operations::ArtworkExportRequest> requests;
    requests.reserve(selected.size());
    for (std::size_t position = 0; position < selected.size(); ++position) {
        const auto row = selected[position];
        if (row < 0 || static_cast<std::size_t>(row) >= copy_targets_.size()) {
            continue;
        }
        const auto& source = copy_targets_[static_cast<std::size_t>(row)].item;
        const auto filename = "artwork-" + std::to_string(position + 1U) + "-" +
                              std::string{metadata::artwork_role_name(source.role)} +
                              std::string{export_extension(source.mime_type)};
        requests.push_back(operations::ArtworkExportRequest{
            .source = source,
            .destination_raw_path = (raw_directory / filename).native(),
        });
    }
    if (requests.empty()) {
        return;
    }
    mutation_cancellation_.request_cancellation();
    mutation_cancellation_ = core::CancellationSource{};
    export_completed_items_ = std::make_shared<std::atomic_size_t>(0U);
    export_running_ = true;
    emit operationRunningChanged(true);
    setStatus(QStringLiteral("Exporting · 0 of %1").arg(requests.size()));
    setProgressVisible(true, static_cast<int>(requests.size()));
    export_progress_timer_.start();
    emit changed();
    const auto cancellation = mutation_cancellation_.token();
    const auto completed = export_completed_items_;
    operations::ArtworkExportOptions export_options;
    if (tools_.stage) {
        // Read through the engine; written here, where the user chose.
        export_options.image_bytes = tools_.artwork.image_bytes;
    }
    export_watcher_.setFuture(
        QtConcurrent::run([requests = std::move(requests), cancellation, completed,
                           export_options = std::move(export_options)]() mutable {
            const operations::ArtworkExportProgressCallback progress =
                [completed](const operations::ArtworkExportProgress& update) {
                    completed->store(update.completed_items, std::memory_order_relaxed);
                };
            return std::make_shared<core::Result<operations::ArtworkExportResult>>(
                operations::export_artwork_items(requests, progress, cancellation, export_options));
        }));
}

void ArtworkSession::updateExportProgress() {
    if (!export_running_ || !export_completed_items_) {
        return;
    }
    const auto completed = export_completed_items_->load(std::memory_order_relaxed);
    progress_value_ = static_cast<int>(completed);
    setStatus(QStringLiteral("Exporting · %1 of %2%3")
                  .arg(completed)
                  .arg(progress_maximum_)
                  .arg(stop_requested_ ? QStringLiteral(" · stopping…") : QString{}));
    emit changed();
}

void ArtworkSession::finishExport() {
    export_running_ = false;
    export_progress_timer_.stop();
    setProgressVisible(false);
    emit operationRunningChanged(false);
    const auto result = export_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The artwork export returned no result");
        setStatus(QStringLiteral("Export failed · %1").arg(message));
        emit changed();
        emit feedbackRequested(QStringLiteral("Export failed"), message, {});
    } else {
        const auto& outcome = **result;
        const auto exported = outcome.exported_item_count();
        setStatus(QStringLiteral("Exported %1 %2")
                      .arg(exported)
                      .arg(exported == 1U ? QStringLiteral("image") : QStringLiteral("images")));
        emit changed();
        if (exported < outcome.items.size()) {
            std::vector<PreparationFeedbackRow> rows;
            for (const auto& item : outcome.items) {
                if (item.state == operations::ArtworkExportItemState::exported) {
                    continue;
                }
                rows.push_back(PreparationFeedbackRow{
                    .file =
                        QString::fromStdString(core::display_raw_path(item.destination_raw_path)),
                    .detail =
                        item.issue ? display_utf8(item.issue->message) : QStringLiteral("Stopped"),
                });
            }
            emit feedbackRequested(QStringLiteral("Export finished with problems"),
                                   QStringLiteral("%1 exported · %2 not written. Existing files "
                                                  "are never overwritten.")
                                       .arg(exported)
                                       .arg(outcome.items.size() - exported),
                                   std::move(rows));
        }
    }
    export_completed_items_.reset();
    emit changed();
}

// Saving.

void ArtworkSession::requestStop() {
    if (stop_requested_ || (!plan_running_ && !apply_running_ && !export_running_)) {
        return;
    }
    stop_requested_ = true;
    mutation_cancellation_.request_cancellation();
    setStatus(QStringLiteral("Stopping after the files already in progress are safe…"));
    emit changed();
}

void ArtworkSession::setProgressVisible(const bool visible, const int total) {
    if (visible) {
        stop_requested_ = false;
        progress_maximum_ = total;
        progress_value_ = 0;
    }
    progress_visible_ = visible;
}

void ArtworkSession::save() {
    if (!canSave()) {
        return;
    }
    const auto intents = pending_intents_;
    const auto policy = SettingsKeys::artworkPolicy();
    const auto change_count = pending_model_->rowCount();
    mutation_cancellation_.request_cancellation();
    mutation_cancellation_ = core::CancellationSource{};
    const auto cancellation = mutation_cancellation_.token();
    plan_running_ = true;
    setProgressVisible(true); // Indeterminate while the immutable plan is checked.
    emit operationRunningChanged(true);
    setStatus(QStringLiteral("Checking %1 artwork %2 against fresh files…")
                  .arg(change_count)
                  .arg(change_count == 1 ? QStringLiteral("change") : QStringLiteral("changes")));
    emit changed();
    plan_watcher_.setFuture(QtConcurrent::run(
        [intents = std::move(intents), cancellation, policy, tools = tools_]() mutable {
            // ADR-0237: images of this computer handed over first, when the
            // engine writes; then planned against the files where they are.
            auto staged = stageReplacements(std::move(intents), tools, cancellation, policy);
            if (!staged) {
                return std::make_shared<core::Result<metadata::ArtworkWritePlan>>(
                    std::unexpected(std::move(staged.error())));
            }
            return std::make_shared<core::Result<metadata::ArtworkWritePlan>>(
                operations::plan_artwork_storage(*staged, policy, cancellation,
                                                 artworkFitterFor(tools), tools.artwork));
        }));
}

void ArtworkSession::finishReview() {
    plan_running_ = false;
    setProgressVisible(false);
    emit operationRunningChanged(false);
    const auto result = plan_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The artwork check returned no result");
        setStatus(QStringLiteral("Nothing was changed · %1").arg(message));
        emit changed();
        return;
    }
    auto plan = std::make_shared<const metadata::ArtworkWritePlan>(std::move(**result));
    if (plan->ready()) {
        std::vector<metadata::FolderImageWritePlan> folders;
        for (const auto& source : plan->sources) {
            if (source.folder_image) {
                folders.push_back(*source.folder_image);
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
    for (const auto& source : plan->sources) {
        for (const auto& issue : source.issues) {
            if (!issue.blocking) {
                continue;
            }
            rows.push_back(PreparationFeedbackRow{
                .file = QString::fromStdString(core::display_raw_path(source.raw_media_path)),
                .detail = QStringLiteral("%1: %2").arg(
                    display_utf8(metadata::artwork_write_plan_issue_kind_name(issue.kind)),
                    display_utf8(issue.error.message)),
            });
        }
    }
    const auto count = rows.size();
    setStatus(QStringLiteral("Nothing was changed · %1 %2")
                  .arg(count)
                  .arg(count == 1U ? QStringLiteral("problem") : QStringLiteral("problems")));
    emit changed();
    emit feedbackRequested(
        QStringLiteral("Artwork change blocked"),
        QStringLiteral("Nothing was changed. Fix the %1 below, then try again.")
            .arg(count == 1U ? QStringLiteral("problem") : QStringLiteral("problems")),
        std::move(rows));
}

void ArtworkSession::folderImagesReviewed(const bool accepted) {
    auto plan = std::move(pending_plan_);
    if (accepted && plan) {
        startApply(std::move(plan));
    }
}

void ArtworkSession::startApply(std::shared_ptr<const metadata::ArtworkWritePlan> plan) {
    if (!plan || !plan->ready() || !applier_factory_ || apply_running_) {
        return;
    }
    auto applier = applier_factory_();
    if (!applier) {
        setStatus(QStringLiteral("Artwork Apply is unavailable"));
        emit changed();
        return;
    }
    mutation_cancellation_.request_cancellation();
    mutation_cancellation_ = core::CancellationSource{};
    apply_progress_state_ = std::make_shared<ArtworkApplyProgressState>();
    apply_progress_state_->states.assign(plan->sources.size(),
                                         operations::ArtworkApplySourceState::pending);
    apply_progress_state_->issues.resize(plan->sources.size());
    apply_running_ = true;
    emit operationRunningChanged(true);
    const auto total = plan->sources.size();
    setStatus(QStringLiteral("Saving artwork · 0 of %1").arg(total));
    setProgressVisible(true, static_cast<int>(total));
    apply_progress_timer_.start();
    emit changed();

    const auto cancellation = mutation_cancellation_.token();
    const auto progress_state = apply_progress_state_;
    apply_watcher_.setFuture(
        QtConcurrent::run([plan = std::move(plan), applier = std::move(applier), progress_state,
                           cancellation]() mutable {
            const operations::ArtworkApplyProgressCallback progress =
                [progress_state](const operations::ArtworkApplyProgress& update) {
                    std::scoped_lock lock{progress_state->mutex};
                    if (update.source_index >= progress_state->states.size()) {
                        return;
                    }
                    progress_state->states[update.source_index] = update.state;
                    progress_state->issues[update.source_index] = update.issue;
                    progress_state->completed_sources = update.completed_sources;
                };
            return std::make_shared<core::Result<operations::ArtworkApplyResult>>(
                applier(*plan, progress, cancellation));
        }));
}

void ArtworkSession::updateApplyProgress() {
    if (!apply_running_ || !apply_progress_state_) {
        return;
    }
    std::size_t completed = 0U;
    std::size_t total = 0U;
    {
        std::scoped_lock lock{apply_progress_state_->mutex};
        completed = apply_progress_state_->completed_sources;
        total = apply_progress_state_->states.size();
    }
    progress_value_ = static_cast<int>(completed);
    setStatus(QStringLiteral("Saving artwork · %1 of %2%3")
                  .arg(completed)
                  .arg(total)
                  .arg(stop_requested_ ? QStringLiteral(" · stopping…") : QString{}));
    emit changed();
}

void ArtworkSession::finishApply() {
    apply_running_ = false;
    apply_progress_timer_.stop();
    setProgressVisible(false);
    emit operationRunningChanged(false);
    const auto result = apply_watcher_.result();
    if (result && *result) {
        discardPendingChanges();
        if (apply_observer_) {
            apply_observer_(**result);
        }
        auto revised = false;
        for (const auto& source_result : (**result).sources) {
            if (!source_result.commit) {
                continue;
            }
            for (auto& source : scope_) {
                if (source.raw_path != source_result.commit->source_raw_path) {
                    continue;
                }
                source.captured_revision = source_result.commit->published_revision;
                source.captured_revision_consistent = true;
                revised = true;
            }
        }
        if (revised) {
            ++generation_;
            cancellation_.request_cancellation();
            scheduleInventory();
        }
    }
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The artwork Apply task returned no result");
        setStatus(QStringLiteral("Saving artwork failed · %1").arg(message));
        emit changed();
        emit feedbackRequested(QStringLiteral("Saving artwork failed"), message, {});
    } else {
        const auto& outcome = **result;
        const auto saved = outcome.committed_source_count();
        if (saved == outcome.sources.size()) {
            setStatus(QStringLiteral("Saved %1 artwork %2")
                          .arg(saved)
                          .arg(saved == 1U ? QStringLiteral("change") : QStringLiteral("changes")));
            emit changed();
        } else {
            std::vector<PreparationFeedbackRow> rows;
            for (const auto& source_result : outcome.sources) {
                if (source_result.state == operations::ArtworkApplySourceState::committed) {
                    continue;
                }
                rows.push_back(PreparationFeedbackRow{
                    .file = QString::fromStdString(core::display_raw_path(source_result.raw_path)),
                    .detail = source_result.issue ? display_utf8(source_result.issue->message)
                                                  : artwork_state_text(source_result.state),
                });
            }
            const auto stopped =
                outcome.cancelled_source_count() > 0U && outcome.failed_source_count() == 0U;
            setStatus(QStringLiteral("%1 saved · %2 failed · %3 stopped")
                          .arg(saved)
                          .arg(outcome.failed_source_count())
                          .arg(outcome.cancelled_source_count()));
            emit changed();
            emit feedbackRequested(
                stopped ? QStringLiteral("Artwork save stopped")
                        : QStringLiteral("Artwork saved with problems"),
                QStringLiteral("%1 saved · %2 failed · %3 stopped. Saved changes are done; some "
                               "files may be partially updated. Inspect the refreshed covers "
                               "before retrying.")
                    .arg(saved)
                    .arg(outcome.failed_source_count())
                    .arg(outcome.cancelled_source_count()),
                std::move(rows));
        }
    }
    apply_progress_state_.reset();
    emit changed();
}

} // namespace trackknife::bench
