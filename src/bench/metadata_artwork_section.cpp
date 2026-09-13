// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_artwork_section.hpp"

#include "bench/preparation_feedback_dialog.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"

#include <QAbstractItemView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTableView>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

void configure_table(QTableView* table) {
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideMiddle);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(24);
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

} // namespace

struct MetadataArtworkSection::BatchResult {
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

struct MetadataArtworkSection::ActionTarget {
    MetadataArtworkScopeSource scope;
    metadata::ArtworkInventoryItem item;
};

MetadataArtworkSection::MetadataArtworkSection(QWidget* parent)
    : QWidget(parent), watcher_(this), plan_watcher_(this), apply_watcher_(this),
      export_watcher_(this) {
    setObjectName(QStringLiteral("bench-metadata-artwork-section"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(6);

    auto* status_row = new QHBoxLayout;
    status_row->setContentsMargins(0, 0, 0, 0);
    status_row->setSpacing(8);
    status_ = new QLabel(QStringLiteral("Open Artwork to inspect the selected files"), this);
    status_->setObjectName(QStringLiteral("bench-metadata-artwork-status"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_row->addWidget(status_, 1);
    progress_bar_ = new QProgressBar(this);
    progress_bar_->setObjectName(QStringLiteral("bench-metadata-artwork-progress"));
    progress_bar_->setAccessibleName(QStringLiteral("Artwork operation progress"));
    progress_bar_->setFixedWidth(170);
    progress_bar_->hide();
    status_row->addWidget(progress_bar_);
    stop_button_ = new QPushButton(QStringLiteral("Stop"), this);
    stop_button_->setObjectName(QStringLiteral("bench-metadata-artwork-stop"));
    stop_button_->setToolTip(QStringLiteral("Stop after the files already in progress are safe"));
    stop_button_->setEnabled(false);
    stop_button_->hide();
    connect(stop_button_, &QPushButton::clicked, this, &MetadataArtworkSection::requestStop);
    status_row->addWidget(stop_button_);
    layout->addLayout(status_row);

    auto heading_font = status_->font();
    heading_font.setBold(true);

    auto* inventory_row = new QHBoxLayout;
    inventory_row->setContentsMargins(0, 0, 0, 0);
    inventory_row->setSpacing(6);
    inventory_row->addStretch(1);
    fetch_cover_button_ = new QPushButton(QStringLiteral("Fetch cover"), this);
    fetch_cover_button_->setObjectName(QStringLiteral("bench-metadata-artwork-fetch-cover"));
    fetch_cover_button_->setToolTip(
        QStringLiteral("Fetch the release's front cover from the Cover Art Archive and make it "
                       "every selected file's front picture — replacing an existing front, "
                       "adding one otherwise"));
    archive_button_ = new QPushButton(QStringLiteral("Covers…"), this);
    archive_button_->setObjectName(QStringLiteral("bench-metadata-artwork-covers"));
    archive_button_->setToolTip(
        QStringLiteral("Browse every Cover Art Archive image of the release — a Front choice "
                       "replaces the existing front cover, other types are added with their "
                       "role"));
    add_button_ = new QPushButton(QStringLiteral("Add image…"), this);
    add_button_->setObjectName(QStringLiteral("bench-metadata-artwork-add"));
    add_button_->setToolTip(QStringLiteral("Add one PNG or JPEG to every selected writable file"));
    copy_button_ = new QPushButton(QStringLiteral("Copy to Selection"), this);
    copy_button_->setObjectName(QStringLiteral("bench-metadata-artwork-copy"));
    copy_button_->setToolTip(
        QStringLiteral("Add the selected image to the other selected writable files"));
    export_button_ = new QPushButton(QStringLiteral("Export…"), this);
    export_button_->setObjectName(QStringLiteral("bench-metadata-artwork-export"));
    export_button_->setToolTip(
        QStringLiteral("Export selected encoded images without overwriting existing files"));
    replace_button_ = new QPushButton(QStringLiteral("Replace…"), this);
    replace_button_->setObjectName(QStringLiteral("bench-metadata-artwork-replace"));
    replace_button_->setToolTip(QStringLiteral(
        "Replace selected embedded covers; external image files are kept with one PNG or JPEG"));
    remove_button_ = new QPushButton(QStringLiteral("Remove"), this);
    remove_button_->setObjectName(QStringLiteral("bench-metadata-artwork-remove"));
    remove_button_->setToolTip(
        QStringLiteral("Remove selected embedded covers; external image files are kept"));
    inventory_row->addWidget(fetch_cover_button_);
    inventory_row->addWidget(archive_button_);
    inventory_row->addWidget(add_button_);
    inventory_row->addWidget(copy_button_);
    inventory_row->addWidget(export_button_);
    inventory_row->addWidget(replace_button_);
    inventory_row->addWidget(remove_button_);
    layout->addLayout(inventory_row);
    auto* draft_buttons = new QHBoxLayout;
    draft_help_ = new QLabel(
        QStringLiteral(
            "Select covers with Ctrl/Shift or Ctrl+A. Changes are saved only with Save artwork.\n"
            "External images are shared files: Remove edits embedded covers and keeps external "
            "files."),
        this);
    draft_help_->setWordWrap(true);
    draft_buttons->addWidget(draft_help_, 1);
    save_button_ = new QPushButton(QStringLiteral("Save artwork"), this);
    save_button_->setObjectName(QStringLiteral("bench-metadata-artwork-save"));
    discard_button_ = new QPushButton(QStringLiteral("Discard changes"), this);
    discard_button_->setObjectName(QStringLiteral("bench-metadata-artwork-discard"));
    undo_pending_button_ = new QPushButton(QStringLiteral("Undo selected"), this);
    undo_pending_button_->setObjectName(QStringLiteral("bench-metadata-artwork-undo-selected"));
    undo_pending_button_->setToolTip(QStringLiteral("Undo the selected pending changes"));
    draft_buttons->addWidget(undo_pending_button_);
    draft_buttons->addWidget(discard_button_);
    draft_buttons->addWidget(save_button_);
    layout->addLayout(draft_buttons);
    pending_view_ = new QTableView(this);
    pending_view_->setObjectName(QStringLiteral("bench-metadata-artwork-pending"));
    configure_table(pending_view_);
    pending_model_ = new QStandardItemModel(pending_view_);
    pending_model_->setHorizontalHeaderLabels({QStringLiteral("File"), QStringLiteral("Change"),
                                               QStringLiteral("Cover"),
                                               QStringLiteral("New image")});
    pending_view_->setModel(pending_model_);
    pending_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(pending_view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MetadataArtworkSection::updateActionButtons);
    connect(undo_pending_button_, &QPushButton::clicked, this,
            &MetadataArtworkSection::undoSelectedChanges);
    auto* undo_pending_shortcut = new QShortcut(QKeySequence::Delete, pending_view_);
    undo_pending_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undo_pending_shortcut, &QShortcut::activated, this, [this] {
        if (undo_pending_button_->isEnabled()) {
            undoSelectedChanges();
        }
    });
    pending_view_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    pending_view_->horizontalHeader()->setStretchLastSection(true);
    pending_view_->setMaximumHeight(160);
    pending_view_->hide();
    layout->addWidget(pending_view_);
    connect(save_button_, &QPushButton::clicked, this, &MetadataArtworkSection::savePendingChanges);
    connect(discard_button_, &QPushButton::clicked, this,
            &MetadataArtworkSection::discardPendingChanges);
    updateActionButtons();

    items_ = new QTableView(this);
    items_->setObjectName(QStringLiteral("bench-metadata-artwork-items"));
    items_->setAccessibleName(QStringLiteral("Artwork inventory"));
    auto* delete_shortcut = new QShortcut(QKeySequence::Delete, items_);
    delete_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(delete_shortcut, &QShortcut::activated, this, [this] {
        if (remove_button_->isEnabled()) {
            reviewRemoval();
        }
    });
    configure_table(items_);
    items_model_ = new QStandardItemModel(items_);
    items_model_->setHorizontalHeaderLabels({QString{}, QStringLiteral("File"),
                                             QStringLiteral("Role"), QStringLiteral("Image"),
                                             QStringLiteral("Source")});
    items_->setModel(items_model_);
    items_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    items_->setIconSize(QSize(thumbnail_edge, thumbnail_edge));
    items_->verticalHeader()->setDefaultSectionSize(thumbnail_edge + 8);
    items_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    items_->horizontalHeader()->setStretchLastSection(true);
    items_->setColumnWidth(0, thumbnail_edge + 12);
    items_->setColumnWidth(2, 90);
    items_->setColumnWidth(3, 190);
    layout->addWidget(items_, 1);

    empty_state_ = new QLabel(QStringLiteral("No artwork inventory loaded"), this);
    empty_state_->setObjectName(QStringLiteral("bench-metadata-artwork-empty"));
    empty_state_->setAlignment(Qt::AlignCenter);
    empty_state_->setTextFormat(Qt::PlainText);
    layout->addWidget(empty_state_);

    issues_pane_ = new QWidget(this);
    issues_pane_->setObjectName(QStringLiteral("bench-metadata-artwork-issues-pane"));
    auto* issues_layout = new QVBoxLayout(issues_pane_);
    issues_layout->setContentsMargins(0, 0, 0, 0);
    issues_layout->setSpacing(4);
    auto* issue_heading = new QLabel(QStringLiteral("Problems"), issues_pane_);
    issue_heading->setFont(heading_font);
    issues_layout->addWidget(issue_heading);
    issues_ = new QTableView(issues_pane_);
    issues_->setObjectName(QStringLiteral("bench-metadata-artwork-issues"));
    issues_->setAccessibleName(QStringLiteral("Artwork inventory read problems"));
    configure_table(issues_);
    issues_model_ = new QStandardItemModel(issues_);
    issues_model_->setHorizontalHeaderLabels({QStringLiteral("File"),
                                              QStringLiteral("Artwork source"),
                                              QStringLiteral("Type"), QStringLiteral("Problem")});
    issues_->setModel(issues_model_);
    issues_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    issues_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    issues_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    issues_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    issues_->setMaximumHeight(150);
    issues_layout->addWidget(issues_);
    issues_pane_->hide();
    layout->addWidget(issues_pane_);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(40);
    connect(debounce_, &QTimer::timeout, this, &MetadataArtworkSection::startInventory);
    connect(&watcher_, &QFutureWatcherBase::finished, this,
            &MetadataArtworkSection::finishInventory);
    connect(&plan_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataArtworkSection::finishReview);
    connect(&apply_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataArtworkSection::finishApply);
    connect(&export_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataArtworkSection::finishExport);
    connect(items_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateActionButtons(); });
    connect(fetch_cover_button_, &QPushButton::clicked, this,
            &MetadataArtworkSection::startCoverArtFetch);
    connect(archive_button_, &QPushButton::clicked, this,
            &MetadataArtworkSection::startArchivePicker);
    connect(add_button_, &QPushButton::clicked, this, &MetadataArtworkSection::promptAddition);
    connect(copy_button_, &QPushButton::clicked, this, &MetadataArtworkSection::reviewCopy);
    connect(export_button_, &QPushButton::clicked, this, &MetadataArtworkSection::promptExport);
    connect(replace_button_, &QPushButton::clicked, this,
            &MetadataArtworkSection::promptReplacement);
    connect(remove_button_, &QPushButton::clicked, this, &MetadataArtworkSection::reviewRemoval);
    apply_progress_timer_ = new QTimer(this);
    apply_progress_timer_->setInterval(50);
    connect(apply_progress_timer_, &QTimer::timeout, this,
            &MetadataArtworkSection::updateApplyProgress);
    export_progress_timer_ = new QTimer(this);
    export_progress_timer_->setInterval(50);
    connect(export_progress_timer_, &QTimer::timeout, this,
            &MetadataArtworkSection::updateExportProgress);
}

MetadataArtworkSection::~MetadataArtworkSection() {
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
}

void MetadataArtworkSection::setMutationServices(ArtworkWritePlanApplierFactory applier_factory,
                                                 ArtworkApplyObserver observer) {
    applier_factory_ = std::move(applier_factory);
    apply_observer_ = std::move(observer);
    updateActionButtons();
}

void MetadataArtworkSection::setCoverArtService(ArtworkCoverArtService service) {
    cover_service_ = std::move(service);
    updateActionButtons();
}

void MetadataArtworkSection::setCoverArtRelease(std::optional<QString> release_id) {
    if (cover_release_id_ == release_id) {
        return;
    }
    cover_release_id_ = std::move(release_id);
    updateActionButtons();
}

void MetadataArtworkSection::requestOperationCancellation() {
    mutation_cancellation_.request_cancellation();
}

void MetadataArtworkSection::setScope(std::vector<MetadataArtworkScopeSource> sources,
                                      const bool source_limit_exceeded) {
    if (scope_ == sources && source_limit_exceeded_ == source_limit_exceeded) {
        return;
    }
    scope_ = std::move(sources);
    source_limit_exceeded_ = source_limit_exceeded;
    ++generation_;
    cancellation_.request_cancellation();
    mutation_cancellation_.request_cancellation();
    if (debounce_ != nullptr) {
        debounce_->stop();
    }
    if (active_) {
        scheduleInventory();
    }
}

void MetadataArtworkSection::setActive(const bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    if (!active_) {
        cancellation_.request_cancellation();
        if (debounce_ != nullptr) {
            debounce_->stop();
        }
        return;
    }
    if (displayed_generation_ != generation_) {
        scheduleInventory();
    }
}

void MetadataArtworkSection::scheduleInventory() {
    if (!active_) {
        return;
    }
    if (scope_.empty()) {
        clearPresentation();
        status_->setText(QStringLiteral("Select at least one file to inspect artwork"));
        displayed_generation_ = generation_;
        return;
    }
    if (source_limit_exceeded_ || scope_.size() > metadata_artwork_source_limit) {
        clearPresentation();
        status_->setText(
            QStringLiteral("Artwork inventory is limited to %1 physical sources; narrow the "
                           "file selection")
                .arg(metadata_artwork_source_limit));
        displayed_generation_ = generation_;
        return;
    }
    status_->setText(
        QStringLiteral("Preparing artwork inventory for %1 %2…")
            .arg(scope_.size())
            .arg(scope_.size() == 1U ? QStringLiteral("source") : QStringLiteral("sources")));
    if (job_running_) {
        cancellation_.request_cancellation();
        return;
    }
    debounce_->start();
}

void MetadataArtworkSection::startInventory() {
    if (!active_ || job_running_ || scope_.empty() || source_limit_exceeded_ ||
        scope_.size() > metadata_artwork_source_limit) {
        return;
    }
    clearPresentation();
    cancellation_ = core::CancellationSource{};
    const auto token = cancellation_.token();
    job_generation_ = generation_;
    job_running_ = true;
    status_->setText(
        QStringLiteral("Reading artwork for %1 %2…")
            .arg(scope_.size())
            .arg(scope_.size() == 1U ? QStringLiteral("source") : QStringLiteral("sources")));
    watcher_.setFuture(
        QtConcurrent::run([scope = scope_, generation = job_generation_, token]() mutable {
            auto batch = std::make_shared<BatchResult>();
            batch->generation = generation;
            batch->sources.reserve(scope.size());
            for (auto& source : scope) {
                if (token.is_cancellation_requested()) {
                    batch->cancelled = true;
                    break;
                }
                auto read = metadata::read_local_artwork_inventory(
                    source.raw_path, metadata::default_artwork_inventory_policy(), token);
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
                        } else if (auto bytes = metadata::read_artwork_image_bytes(
                                       thumbnail_evidence(item), maximum_thumbnail_source_bytes,
                                       token)) {
                            const auto decoded =
                                QImage::fromData(bytes->data(), static_cast<int>(bytes->size()));
                            if (!decoded.isNull()) {
                                thumbnail =
                                    decoded.scaled(thumbnail_edge, thumbnail_edge,
                                                   Qt::KeepAspectRatio, Qt::SmoothTransformation);
                            }
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

void MetadataArtworkSection::finishInventory() {
    job_running_ = false;
    const auto result = watcher_.result();
    if (!result || !active_ || result->generation != generation_) {
        if (active_ && displayed_generation_ != generation_) {
            scheduleInventory();
        }
        return;
    }
    if (result->cancelled) {
        scheduleInventory();
        return;
    }
    present(*result);
    displayed_generation_ = result->generation;
    updateActionButtons();
}

void MetadataArtworkSection::clearPresentation() {
    items_model_->removeRows(0, items_model_->rowCount());
    issues_model_->removeRows(0, issues_model_->rowCount());
    action_targets_.clear();
    copy_targets_.clear();
    add_available_ = false;
    empty_state_->show();
    issues_pane_->hide();
    updateActionButtons();
}

void MetadataArtworkSection::present(const BatchResult& result) {
    clearPresentation();
    add_available_ = !result.sources.empty();
    std::size_t item_count = 0U;
    std::size_t issue_count = 0U;
    std::size_t read_only_count = 0U;

    for (const auto& source : result.sources) {
        const auto label = source_label(source.scope);
        const auto media_path =
            QString::fromStdString(core::escape_raw_path(source.scope.raw_path));
        if (!source.inventory) {
            add_available_ = false;
            ++read_only_count;
            const auto details = source.error ? error_tool_tip(*source.error)
                                              : QStringLiteral("The inventory returned no result");
            QList<QStandardItem*> issue_row;
            issue_row.reserve(4);
            issue_row.push_back(table_item(label, media_path));
            issue_row.push_back(table_item(media_path, media_path));
            issue_row.push_back(table_item(source.error ? error_code_name(source.error->code)
                                                        : QStringLiteral("Reader error")));
            issue_row.push_back(table_item(source.error ? display_utf8(source.error->message)
                                                        : QStringLiteral("No inventory result"),
                                           details));
            issues_model_->appendRow(issue_row);
            ++issue_count;
            continue;
        }

        const auto& inventory = *source.inventory;
        const auto changes_available =
            applier_factory_ && inventory.capabilities.embedded_readable &&
            metadata::is_qualified_artwork_adapter(inventory.embedded_adapter_name) &&
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
            QList<QStandardItem*> issue_row;
            issue_row.reserve(4);
            issue_row.push_back(table_item(label, media_path));
            issue_row.push_back(table_item(media_path, media_path));
            issue_row.push_back(table_item(QStringLiteral("View-only")));
            issue_row.push_back(table_item(reason));
            issues_model_->appendRow(issue_row);
            ++issue_count;
        }

        for (std::size_t index = 0U; index < inventory.items.size(); ++index) {
            const auto& artwork = inventory.items[index];
            const auto raw_artwork_path =
                QString::fromStdString(core::escape_raw_path(artwork.raw_source_path));
            const auto fingerprint = QString::fromStdString(
                metadata::artwork_fingerprint_hex(artwork.content_fingerprint));
            const auto embedded = artwork.provenance == metadata::ArtworkProvenance::embedded;

            auto* preview = new QStandardItem;
            preview->setEditable(false);
            if (index < source.thumbnails.size() && !source.thumbnails[index].isNull()) {
                preview->setData(source.thumbnails[index], Qt::DecorationRole);
            } else {
                preview->setText(QStringLiteral("—"));
                preview->setToolTip(QStringLiteral("No preview available"));
            }

            auto image = QStringLiteral("%1 · %2 · %3")
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

            QList<QStandardItem*> item_row;
            item_row.reserve(5);
            item_row.push_back(preview);
            item_row.push_back(table_item(label, media_path));
            item_row.push_back(
                table_item(title_case(display_utf8(metadata::artwork_role_name(artwork.role)))));
            item_row.push_back(table_item(image, image_tool_tip));
            item_row.push_back(table_item(origin, origin_tool_tip));
            items_model_->appendRow(item_row);
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
                QString::fromStdString(core::escape_raw_path(issue.raw_source_path));
            const auto details = error_tool_tip(issue.error);
            QList<QStandardItem*> issue_row;
            issue_row.reserve(4);
            issue_row.push_back(table_item(label, media_path));
            issue_row.push_back(table_item(raw_issue_path, raw_issue_path));
            issue_row.push_back(table_item(error_code_name(issue.error.code)));
            issue_row.push_back(table_item(display_utf8(issue.error.message), details));
            issues_model_->appendRow(issue_row);
            ++issue_count;
        }
    }

    empty_state_->setVisible(item_count == 0U);
    if (item_count == 0U) {
        empty_state_->setText(QStringLiteral("No artwork found for the selected files"));
    }
    issues_pane_->setVisible(issue_count > 0U);

    auto text =
        QStringLiteral("%1 %2 across %3 %4")
            .arg(item_count)
            .arg(item_count == 1U ? QStringLiteral("image") : QStringLiteral("images"))
            .arg(result.sources.size())
            .arg(result.sources.size() == 1U ? QStringLiteral("file") : QStringLiteral("files"));
    if (read_only_count > 0U) {
        text += QStringLiteral(" · %1 view-only").arg(read_only_count);
    }
    status_->setText(text);
    updateActionButtons();
}

void MetadataArtworkSection::updateActionButtons() {
    if (add_button_ == nullptr || copy_button_ == nullptr || export_button_ == nullptr ||
        replace_button_ == nullptr || remove_button_ == nullptr || fetch_cover_button_ == nullptr ||
        items_ == nullptr || items_->selectionModel() == nullptr) {
        return;
    }
    const auto selected = items_->selectionModel()->selectedRows();
    const auto actionable = std::ranges::any_of(selected, [this](const auto& row) {
        return row.row() >= 0 && static_cast<std::size_t>(row.row()) < action_targets_.size() &&
               action_targets_[static_cast<std::size_t>(row.row())].has_value();
    });
    const auto operation_idle =
        !plan_running_ && !apply_running_ && !export_running_ && !cover_fetch_running_;
    const auto mutation_idle =
        applier_factory_ && operation_idle && !job_running_ && displayed_generation_ == generation_;
    save_button_->setEnabled(hasPendingChanges() && mutation_idle);
    discard_button_->setEnabled(hasPendingChanges() && operation_idle);
    undo_pending_button_->setEnabled(operation_idle &&
                                     pending_view_->selectionModel()->hasSelection());
    add_button_->setEnabled(add_available_ && mutation_idle);
    const auto cover_ready =
        add_available_ && mutation_idle && coverServiceReady() && cover_release_id_.has_value();
    fetch_cover_button_->setEnabled(cover_ready);
    archive_button_->setEnabled(cover_ready && archive_dialog_.isNull());
    auto copy_available = add_available_ && selected.size() == 1;
    if (copy_available) {
        const auto row = selected.front().row();
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
    copy_button_->setEnabled(copy_available && mutation_idle);
    replace_button_->setEnabled(actionable && mutation_idle);
    remove_button_->setEnabled(actionable && mutation_idle);
    auto export_available = !selected.empty();
    for (const auto& row : selected) {
        if (row.row() < 0 || static_cast<std::size_t>(row.row()) >= copy_targets_.size()) {
            export_available = false;
            break;
        }
    }
    export_button_->setEnabled(export_available && operation_idle);
}

bool MetadataArtworkSection::coverServiceReady() const {
    return static_cast<bool>(cover_service_.fetch_listing) &&
           static_cast<bool>(cover_service_.fetch_bytes) &&
           static_cast<bool>(cover_service_.store_image);
}

void MetadataArtworkSection::startCoverArtFetch() {
    if (cover_fetch_running_ || plan_running_ || apply_running_ || !applier_factory_ ||
        !coverServiceReady() || !cover_release_id_) {
        return;
    }
    cover_fetch_running_ = true;
    status_->setText(QStringLiteral("Fetching the front cover from the Cover Art Archive…"));
    updateActionButtons();
    QPointer<MetadataArtworkSection> self{this};
    cover_service_.fetch_listing(
        *cover_release_id_, [self](core::Result<musicbrainz::CoverArtListing> listing) {
            if (self.isNull()) {
                return;
            }
            self->cover_fetch_running_ = false;
            if (!listing) {
                self->status_->setText(QStringLiteral("No cover was added · %1")
                                           .arg(display_utf8(listing.error().message)));
                self->updateActionButtons();
                return;
            }
            const auto front = musicbrainz::select_front_cover(*listing);
            if (!front) {
                self->status_->setText(
                    QStringLiteral("The Cover Art Archive has no front cover for this release"));
                self->updateActionButtons();
                return;
            }
            self->useArchiveImage(listing->images[*front]);
        });
}

void MetadataArtworkSection::startArchivePicker() {
    if (cover_fetch_running_ || plan_running_ || apply_running_ || !applier_factory_ ||
        !coverServiceReady() || !cover_release_id_ || !archive_dialog_.isNull()) {
        return;
    }
    cover_fetch_running_ = true;
    status_->setText(QStringLiteral("Loading the release's Cover Art Archive listing…"));
    updateActionButtons();
    QPointer<MetadataArtworkSection> self{this};
    cover_service_.fetch_listing(
        *cover_release_id_, [self](core::Result<musicbrainz::CoverArtListing> listing) {
            if (self.isNull()) {
                return;
            }
            self->cover_fetch_running_ = false;
            self->updateActionButtons();
            if (!listing) {
                self->status_->setText(QStringLiteral("No archive listing · %1")
                                           .arg(display_utf8(listing.error().message)));
                return;
            }
            if (listing->images.empty()) {
                self->status_->setText(
                    QStringLiteral("The Cover Art Archive has no images for this release"));
                return;
            }
            self->presentArchivePicker(std::move(*listing));
        });
}

namespace {

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

} // namespace

void MetadataArtworkSection::presentArchivePicker(musicbrainz::CoverArtListing listing) {
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("bench-metadata-artwork-archive"));
    dialog->setWindowTitle(QStringLiteral("Cover Art Archive images"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(560, 420);
    archive_dialog_ = dialog;
    auto* layout = new QVBoxLayout(dialog);
    auto* hint = new QLabel(
        QStringLiteral("Every archive image of this release · a Front choice replaces the "
                       "existing front cover, other types are added"),
        dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* list = new QTreeWidget(dialog);
    list->setObjectName(QStringLiteral("bench-metadata-artwork-archive-list"));
    list->setHeaderLabels(
        {QString{}, QStringLiteral("Type"), QStringLiteral("Approved"), QStringLiteral("Comment")});
    list->setRootIsDecorated(false);
    list->setAlternatingRowColors(true);
    list->setIconSize(QSize(72, 72));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->header()->setStretchLastSection(true);
    layout->addWidget(list, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* use = buttons->addButton(QStringLiteral("Use this image"), QDialogButtonBox::ActionRole);
    use->setObjectName(QStringLiteral("bench-metadata-artwork-archive-use"));
    use->setEnabled(false);
    layout->addWidget(buttons);

    auto images =
        std::make_shared<std::vector<musicbrainz::CoverArtImage>>(std::move(listing.images));
    for (std::size_t index = 0U; index < images->size(); ++index) {
        const auto& image = (*images)[index];
        auto* row = new QTreeWidgetItem(list);
        row->setData(0, Qt::UserRole, static_cast<qulonglong>(index));
        row->setText(1, archive_type_text(image));
        row->setText(2, image.approved ? QStringLiteral("Yes") : QStringLiteral("No"));
        row->setText(3, display_utf8(image.comment));
    }
    list->setCurrentItem(list->topLevelItem(0));
    use->setEnabled(true);

    // Thumbnails trickle in through the paced fetcher without blocking the
    // picker.
    QPointer<QTreeWidget> guarded_list{list};
    for (std::size_t index = 0U; index < images->size(); ++index) {
        const auto& thumbnail_url = (*images)[index].thumbnail_url;
        if (thumbnail_url.empty()) {
            continue;
        }
        cover_service_.fetch_bytes(
            QString::fromStdString(thumbnail_url),
            [guarded_list, index](core::Result<QByteArray> bytes) {
                if (guarded_list.isNull() || !bytes) {
                    return;
                }
                QImage thumbnail;
                if (!thumbnail.loadFromData(*bytes)) {
                    return;
                }
                if (auto* row = guarded_list->topLevelItem(static_cast<int>(index))) {
                    row->setIcon(0, QIcon{QPixmap::fromImage(thumbnail)});
                }
            });
    }

    QPointer<MetadataArtworkSection> self{this};
    const auto use_selected = [self, guarded_list, images,
                               dialog_pointer = QPointer<QDialog>{dialog}] {
        if (self.isNull() || guarded_list.isNull() || guarded_list->currentItem() == nullptr) {
            return;
        }
        const auto index = static_cast<std::size_t>(
            guarded_list->currentItem()->data(0, Qt::UserRole).toULongLong());
        if (index >= images->size()) {
            return;
        }
        const auto image = (*images)[index];
        if (!dialog_pointer.isNull()) {
            dialog_pointer->close();
        }
        self->useArchiveImage(image);
    };
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(use, &QPushButton::clicked, dialog, use_selected);
    connect(list, &QTreeWidget::itemDoubleClicked, dialog,
            [use_selected](QTreeWidgetItem*, int) { use_selected(); });
    dialog->show();
}

void MetadataArtworkSection::useArchiveImage(const musicbrainz::CoverArtImage& image) {
    if (cover_fetch_running_ || plan_running_ || apply_running_ || !coverServiceReady() ||
        !cover_release_id_) {
        return;
    }
    cover_fetch_running_ = true;
    const auto role = archive_image_role(image);
    status_->setText(QStringLiteral("Fetching the %1 image from the Cover Art Archive…")
                         .arg(archive_type_text(image)));
    updateActionButtons();
    const auto generation = generation_;
    const auto identity = QStringLiteral("%1-%2-%3")
                              .arg(*cover_release_id_, QString::fromStdString(image.id),
                                   display_utf8(metadata::artwork_role_name(role)));
    QPointer<MetadataArtworkSection> self{this};
    cover_service_.fetch_bytes(
        QString::fromStdString(image.image_url),
        [self, generation, identity, role](core::Result<QByteArray> bytes) {
            if (self.isNull()) {
                return;
            }
            self->cover_fetch_running_ = false;
            if (!bytes) {
                self->status_->setText(QStringLiteral("No image was added · %1")
                                           .arg(display_utf8(bytes.error().message)));
                self->updateActionButtons();
                return;
            }
            const auto stored = self->cover_service_.store_image(identity, *bytes);
            if (!stored) {
                self->status_->setText(QStringLiteral("No image was added · %1")
                                           .arg(display_utf8(stored.error().message)));
                self->updateActionButtons();
                return;
            }
            if (self->generation_ != generation) {
                self->status_->setText(
                    QStringLiteral("The selection changed while fetching; no image was added"));
                self->updateActionButtons();
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

void MetadataArtworkSection::promptAddition() {
    const auto selected = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose artwork to add"), {},
        QStringLiteral("Artwork images (*.png *.jpg *.jpeg);;All files (*)"));
    if (selected.isEmpty()) {
        return;
    }
    const QStringList roles{QStringLiteral("Front cover"), QStringLiteral("Back cover"),
                            QStringLiteral("Artist"),      QStringLiteral("Disc"),
                            QStringLiteral("Icon"),        QStringLiteral("Other")};
    bool accepted = false;
    const auto selected_role =
        QInputDialog::getItem(this, QStringLiteral("Artwork role"), QStringLiteral("Store as:"),
                              roles, 0, false, &accepted);
    if (!accepted) {
        return;
    }
    const auto role_index = roles.indexOf(selected_role);
    constexpr std::array role_values{
        metadata::ArtworkRole::front, metadata::ArtworkRole::back, metadata::ArtworkRole::artist,
        metadata::ArtworkRole::disc,  metadata::ArtworkRole::icon, metadata::ArtworkRole::other,
    };
    if (role_index < 0 || static_cast<std::size_t>(role_index) >= role_values.size()) {
        return;
    }
    const auto encoded = QFile::encodeName(selected);
    startReview(metadata::ArtworkWritePlanIntentKind::add,
                std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
                role_values[static_cast<std::size_t>(role_index)]);
}

void MetadataArtworkSection::promptReplacement() {
    const auto selected = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose replacement artwork"), {},
        QStringLiteral("Artwork images (*.png *.jpg *.jpeg);;All files (*)"));
    if (selected.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(selected);
    startReview(metadata::ArtworkWritePlanIntentKind::replace,
                std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
}

void MetadataArtworkSection::reviewRemoval() {
    startReview(metadata::ArtworkWritePlanIntentKind::remove, std::nullopt);
}

void MetadataArtworkSection::reviewCopy() {
    if (items_->selectionModel() == nullptr) {
        return;
    }
    const auto selected = items_->selectionModel()->selectedRows();
    if (selected.size() != 1 || selected.front().row() < 0 ||
        static_cast<std::size_t>(selected.front().row()) >= copy_targets_.size()) {
        return;
    }
    const auto& donor = copy_targets_[static_cast<std::size_t>(selected.front().row())].item;
    startReview(metadata::ArtworkWritePlanIntentKind::add, donor.raw_source_path, donor.role,
                donor.description,
                donor.provenance == metadata::ArtworkProvenance::embedded ? std::optional{donor}
                                                                          : std::nullopt);
}

void MetadataArtworkSection::promptExport() {
    if (export_running_ || items_->selectionModel() == nullptr) {
        return;
    }
    const auto selected = items_->selectionModel()->selectedRows();
    if (selected.empty()) {
        return;
    }
    const auto directory =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Choose artwork export directory"));
    if (directory.isEmpty()) {
        return;
    }
    const auto encoded_directory = QFile::encodeName(directory);
    const std::filesystem::path raw_directory{std::string{
        encoded_directory.constData(), static_cast<std::size_t>(encoded_directory.size())}};
    std::vector<operations::ArtworkExportRequest> requests;
    requests.reserve(static_cast<std::size_t>(selected.size()));
    for (qsizetype position = 0; position < selected.size(); ++position) {
        const auto row = selected[position].row();
        if (row < 0 || static_cast<std::size_t>(row) >= copy_targets_.size()) {
            continue;
        }
        const auto& source = copy_targets_[static_cast<std::size_t>(row)].item;
        const auto filename = "artwork-" + std::to_string(static_cast<std::size_t>(position) + 1U) +
                              "-" + std::string{metadata::artwork_role_name(source.role)} +
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
    status_->setText(QStringLiteral("Exporting · 0 of %1").arg(requests.size()));
    setProgressVisible(true, static_cast<int>(requests.size()));
    export_progress_timer_->start();
    updateActionButtons();
    const auto cancellation = mutation_cancellation_.token();
    const auto completed = export_completed_items_;
    export_watcher_.setFuture(
        QtConcurrent::run([requests = std::move(requests), cancellation, completed]() mutable {
            const operations::ArtworkExportProgressCallback progress =
                [completed](const operations::ArtworkExportProgress& update) {
                    completed->store(update.completed_items, std::memory_order_relaxed);
                };
            return std::make_shared<core::Result<operations::ArtworkExportResult>>(
                operations::export_artwork_items(requests, progress, cancellation));
        }));
}

void MetadataArtworkSection::updateExportProgress() {
    if (!export_running_ || !export_completed_items_) {
        return;
    }
    const auto completed = export_completed_items_->load(std::memory_order_relaxed);
    progress_bar_->setValue(static_cast<int>(completed));
    status_->setText(QStringLiteral("Exporting · %1 of %2%3")
                         .arg(completed)
                         .arg(progress_bar_->maximum())
                         .arg(stop_requested_ ? QStringLiteral(" · stopping…") : QString{}));
}

void MetadataArtworkSection::finishExport() {
    export_running_ = false;
    export_progress_timer_->stop();
    setProgressVisible(false);
    emit operationRunningChanged(false);
    const auto result = export_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The artwork export returned no result");
        status_->setText(QStringLiteral("Export failed · %1").arg(message));
        showFeedback(QStringLiteral("Export failed"), message, {});
    } else {
        const auto& outcome = **result;
        const auto exported = outcome.exported_item_count();
        status_->setText(
            QStringLiteral("Exported %1 %2")
                .arg(exported)
                .arg(exported == 1U ? QStringLiteral("image") : QStringLiteral("images")));
        if (exported < outcome.items.size()) {
            std::vector<PreparationFeedbackRow> rows;
            for (const auto& item : outcome.items) {
                if (item.state == operations::ArtworkExportItemState::exported) {
                    continue;
                }
                rows.push_back(PreparationFeedbackRow{
                    .file =
                        QString::fromStdString(core::escape_raw_path(item.destination_raw_path)),
                    .detail =
                        item.issue ? display_utf8(item.issue->message) : QStringLiteral("Stopped"),
                });
            }
            showFeedback(QStringLiteral("Export finished with problems"),
                         QStringLiteral("%1 exported · %2 not written. Existing files are never "
                                        "overwritten.")
                             .arg(exported)
                             .arg(outcome.items.size() - exported),
                         std::move(rows));
        }
    }
    export_completed_items_.reset();
    updateActionButtons();
}

void MetadataArtworkSection::showFeedback(const QString& window_title, const QString& summary,
                                          std::vector<PreparationFeedbackRow> rows) {
    if (feedback_dialog_ != nullptr) {
        feedback_dialog_->close();
    }
    auto* dialog = createPreparationFeedbackDialog(window_title, summary, rows, this);
    feedback_dialog_ = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (feedback_dialog_ == dialog) {
            feedback_dialog_ = nullptr;
        }
        updateActionButtons();
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MetadataArtworkSection::requestStop() {
    if (stop_requested_ || (!plan_running_ && !apply_running_ && !export_running_)) {
        return;
    }
    stop_requested_ = true;
    stop_button_->setEnabled(false);
    mutation_cancellation_.request_cancellation();
    status_->setText(QStringLiteral("Stopping after the files already in progress are safe…"));
}

void MetadataArtworkSection::setProgressVisible(const bool visible, const int total) {
    if (visible) {
        stop_requested_ = false;
        progress_bar_->setRange(0, total);
        progress_bar_->setValue(0);
    }
    progress_bar_->setVisible(visible);
    stop_button_->setVisible(visible);
    stop_button_->setEnabled(visible && !stop_requested_);
}

void MetadataArtworkSection::startReview(
    const metadata::ArtworkWritePlanIntentKind kind,
    std::optional<std::string> replacement_raw_path, const metadata::ArtworkRole added_role,
    std::string added_description, std::optional<metadata::ArtworkInventoryItem> embedded_donor) {
    if (!applier_factory_ || plan_running_ || apply_running_ ||
        items_->selectionModel() == nullptr) {
        return;
    }
    std::vector<metadata::ArtworkWritePlanIntent> intents;
    const auto selected = items_->selectionModel()->selectedRows();
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
    }
    if (kind != metadata::ArtworkWritePlanIntentKind::add) {
        for (const auto& row : selected) {
            if (row.row() < 0 || static_cast<std::size_t>(row.row()) >= action_targets_.size()) {
                continue;
            }
            const auto& target = action_targets_[static_cast<std::size_t>(row.row())];
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
        status_->setText(kind == metadata::ArtworkWritePlanIntentKind::add
                             ? QStringLiteral("Select at least one writable FLAC, MP3, or MP4 file")
                             : QStringLiteral("Select at least one writable embedded cover"));
        updateActionButtons();
        return;
    }
    const auto change_count = kind == metadata::ArtworkWritePlanIntentKind::add
                                  ? static_cast<qsizetype>(scope_.size())
                                  : selected.size();
    dispatchReview(std::move(intents), change_count);
}

void MetadataArtworkSection::dispatchReview(std::vector<metadata::ArtworkWritePlanIntent> intents,
                                            const qsizetype change_count) {
    Q_UNUSED(change_count);
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

void MetadataArtworkSection::setUnifiedApply(const bool enabled) {
    unified_apply_ = enabled;
    save_button_->setVisible(!enabled);
    if (enabled) {
        draft_help_->setText(
            QStringLiteral("Select covers with Ctrl/Shift or Ctrl+A. Apply saves tags and artwork "
                           "together. External image files stay on disk."));
    }
}

void MetadataArtworkSection::discardPendingChanges() {
    pending_intents_.clear();
    updatePendingPresentation();
}

void MetadataArtworkSection::undoSelectedChanges() {
    const auto selected = pending_view_->selectionModel()->selectedRows();
    std::set<std::string> targets;
    for (const auto& row : selected) {
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

void MetadataArtworkSection::updatePendingPresentation() {
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
            {table_item(QString::fromStdString(core::escape_raw_path(
                            std::filesystem::path{intent.raw_media_path}.filename().native())),
                        QString::fromStdString(core::escape_raw_path(intent.raw_media_path))),
             table_item(title_case(
                 display_utf8(metadata::artwork_write_plan_intent_kind_name(intent.kind)))),
             table_item(
                 intent.kind == metadata::ArtworkWritePlanIntentKind::add
                     ? title_case(display_utf8(metadata::artwork_role_name(intent.added_role)))
                     : QStringLiteral("Picture %1").arg(intent.target_ordinal + 1)),
             table_item(intent.replacement_raw_path ? QString::fromStdString(core::escape_raw_path(
                                                          *intent.replacement_raw_path))
                                                    : QString{})});
    }
    pending_view_->setVisible(hasPendingChanges());
    for (std::size_t row = 0; row < action_targets_.size(); ++row) {
        const auto& target = action_targets_[row];
        const auto removed =
            target && removals.contains({target->scope.raw_path, target->item.source_ordinal});
        for (int col = 0; col < items_model_->columnCount(); ++col) {
            auto* item = items_model_->item(static_cast<int>(row), col);
            auto font = item->font();
            font.setStrikeOut(removed);
            item->setFont(font);
        }
    }
    status_->setText(
        hasPendingChanges()
            ? (unified_apply_
                   ? QStringLiteral(
                         "%1 pending artwork changes · Apply saves tags and covers together")
                   : QStringLiteral("%1 pending artwork changes · review below, then Save artwork"))
                  .arg(pending_model_->rowCount())
            : QStringLiteral("No pending artwork changes"));
    emit pendingChangesChanged(hasPendingChanges());
    updateActionButtons();
}

void MetadataArtworkSection::savePendingChanges() {
    if (!save_button_->isEnabled()) {
        return;
    }
    const auto intents = pending_intents_;
    const auto change_count = pending_model_->rowCount();
    mutation_cancellation_.request_cancellation();
    mutation_cancellation_ = core::CancellationSource{};
    const auto cancellation = mutation_cancellation_.token();
    plan_running_ = true;
    setProgressVisible(true); // Indeterminate while the immutable plan is checked.
    emit operationRunningChanged(true);
    status_->setText(
        QStringLiteral("Checking %1 artwork %2 against fresh files…")
            .arg(change_count)
            .arg(change_count == 1 ? QStringLiteral("change") : QStringLiteral("changes")));
    updateActionButtons();
    plan_watcher_.setFuture(
        QtConcurrent::run([intents = std::move(intents), cancellation]() mutable {
            return std::make_shared<core::Result<metadata::ArtworkWritePlan>>(
                metadata::revalidate_artwork_write_plan(intents, cancellation));
        }));
}

// The fetched archive front becomes each file's front cover: replacing the
// existing embedded front picture where one exists, adding one otherwise —
// never stacking a second front.
void MetadataArtworkSection::reviewFetchedCover(const std::string& replacement_raw_path) {
    if (!applier_factory_ || plan_running_ || apply_running_) {
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
                if (!target || target->scope.raw_path != source.raw_path ||
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
        status_->setText(QStringLiteral("Select at least one writable FLAC, MP3, or MP4 file"));
        updateActionButtons();
        return;
    }
    dispatchReview(std::move(intents), static_cast<qsizetype>(scope_.size()));
}

void MetadataArtworkSection::finishReview() {
    plan_running_ = false;
    setProgressVisible(false);
    emit operationRunningChanged(false);
    const auto result = plan_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The artwork check returned no result");
        status_->setText(QStringLiteral("Nothing was changed · %1").arg(message));
        updateActionButtons();
        return;
    }
    auto plan = std::make_shared<const metadata::ArtworkWritePlan>(std::move(**result));
    if (plan->ready()) {
        startApply(std::move(plan));
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
                .file = QString::fromStdString(core::escape_raw_path(source.raw_media_path)),
                .detail = QStringLiteral("%1: %2").arg(
                    display_utf8(metadata::artwork_write_plan_issue_kind_name(issue.kind)),
                    display_utf8(issue.error.message)),
            });
        }
    }
    status_->setText(
        QStringLiteral("Nothing was changed · %1 %2")
            .arg(rows.size())
            .arg(rows.size() == 1U ? QStringLiteral("problem") : QStringLiteral("problems")));
    showFeedback(
        QStringLiteral("Artwork change blocked"),
        QStringLiteral("Nothing was changed. Fix the %1 below, then try again.")
            .arg(rows.size() == 1U ? QStringLiteral("problem") : QStringLiteral("problems")),
        std::move(rows));
    updateActionButtons();
}

void MetadataArtworkSection::startApply(std::shared_ptr<const metadata::ArtworkWritePlan> plan) {
    if (!plan || !plan->ready() || !applier_factory_ || apply_running_) {
        return;
    }
    auto applier = applier_factory_();
    if (!applier) {
        status_->setText(QStringLiteral("Artwork Apply is unavailable"));
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
    status_->setText(QStringLiteral("Saving artwork · 0 of %1").arg(total));
    setProgressVisible(true, static_cast<int>(total));
    updateActionButtons();
    apply_progress_timer_->start();

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

void MetadataArtworkSection::updateApplyProgress() {
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
    progress_bar_->setValue(static_cast<int>(completed));
    status_->setText(QStringLiteral("Saving artwork · %1 of %2%3")
                         .arg(completed)
                         .arg(total)
                         .arg(stop_requested_ ? QStringLiteral(" · stopping…") : QString{}));
}

void MetadataArtworkSection::finishApply() {
    apply_running_ = false;
    apply_progress_timer_->stop();
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
        status_->setText(QStringLiteral("Saving artwork failed · %1").arg(message));
        showFeedback(QStringLiteral("Saving artwork failed"), message, {});
    } else {
        const auto& outcome = **result;
        const auto saved = outcome.committed_source_count();
        if (saved == outcome.sources.size()) {
            status_->setText(
                QStringLiteral("Saved %1 artwork %2")
                    .arg(saved)
                    .arg(saved == 1U ? QStringLiteral("change") : QStringLiteral("changes")));
        } else {
            std::vector<PreparationFeedbackRow> rows;
            for (const auto& source_result : outcome.sources) {
                if (source_result.state == operations::ArtworkApplySourceState::committed) {
                    continue;
                }
                rows.push_back(PreparationFeedbackRow{
                    .file = QString::fromStdString(core::escape_raw_path(source_result.raw_path)),
                    .detail = source_result.issue ? display_utf8(source_result.issue->message)
                                                  : artwork_state_text(source_result.state),
                });
            }
            const auto stopped =
                outcome.cancelled_source_count() > 0U && outcome.failed_source_count() == 0U;
            status_->setText(QStringLiteral("%1 saved · %2 failed · %3 stopped")
                                 .arg(saved)
                                 .arg(outcome.failed_source_count())
                                 .arg(outcome.cancelled_source_count()));
            showFeedback(stopped ? QStringLiteral("Artwork save stopped")
                                 : QStringLiteral("Artwork saved with problems"),
                         QStringLiteral("%1 saved · %2 failed · %3 stopped. Saved changes are "
                                        "done; some files may be partially updated. Inspect the "
                                        "refreshed covers before retrying.")
                             .arg(saved)
                             .arg(outcome.failed_source_count())
                             .arg(outcome.cancelled_source_count()),
                         std::move(rows));
        }
    }
    apply_progress_state_.reset();
    updateActionButtons();
}

} // namespace trackknife::bench
