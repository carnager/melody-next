// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_properties_dialog.hpp"

#include "bench/metadata_artwork_section.hpp"
#include "bench/metadata_dialog_helpers.hpp"
#include "bench/metadata_exact_value_dialog.hpp"
#include "bench/metadata_field_review_bar.hpp"
#include "bench/metadata_grid_model.hpp"
#include "bench/metadata_rule_script_import_dialog.hpp"
#include "bench/metadata_scalar_delegate.hpp"
#include "bench/metadata_transformation_dialog.hpp"
#include "bench/metadata_transformation_preview_model.hpp"
#include "bench/preparation_feedback_dialog.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/loudness/grouping.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/draft_document.hpp"
#include "trackknife/metadata/field_suggestions.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/proposal.hpp"
#include "trackknife/metadata/rule_script_import.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <QAbstractListModel>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QPaintEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

constexpr auto properties_geometry_key = "workspace/metadata-properties-geometry-v1";
constexpr auto properties_content_splitter_key =
    "workspace/metadata-properties-content-splitter-v1";
constexpr auto properties_metadata_splitter_key =
    "workspace/metadata-properties-metadata-splitter-v1";
constexpr auto properties_field_layouts_key = "workspace/metadata-field-layouts-v1";

class EmptyStateListWidget final : public QListWidget {
  public:
    explicit EmptyStateListWidget(QString empty_state, QWidget* parent)
        : QListWidget(parent), empty_state_(std::move(empty_state)) {
        setProperty("bench-empty-state-text", empty_state_);
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        QListWidget::paintEvent(event);
        if (count() != 0 || empty_state_.isEmpty()) {
            return;
        }
        QPainter painter{viewport()};
        painter.setClipRegion(event->region());
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                       QPalette::PlaceholderText));
        painter.drawText(viewport()->rect().adjusted(16, 16, -16, -16),
                         Qt::AlignCenter | Qt::TextWordWrap, empty_state_);
    }

  private:
    QString empty_state_;
};

} // namespace

MetadataPropertiesDialog::MetadataPropertiesDialog(
    const std::size_t requested_item_count, MetadataPropertiesSourceReader source_reader,
    const std::span<const std::string_view> preferred_fields,
    MetadataWritePlanApplierFactory plan_applier_factory, MetadataApplyObserver apply_observer,
    MetadataTransformationStore transformation_store, OutputProfileStore output_profile_store,
    FilePublicationPlanApplierFactory file_plan_applier_factory,
    FilePublicationApplyObserver file_apply_observer, QWidget* parent,
    MetadataDialogLayoutStore layout_store, MusicBrainzLookupService musicbrainz)
    : QDialog(parent), selection_watcher_(this), write_plan_watcher_(this),
      metadata_apply_watcher_(this), file_apply_watcher_(this), output_example_watcher_(this),
      source_reader_(std::move(source_reader)),
      plan_applier_factory_(std::move(plan_applier_factory)),
      apply_observer_(std::move(apply_observer)),
      transformation_store_(std::move(transformation_store)),
      output_profile_store_(std::move(output_profile_store)),
      file_plan_applier_factory_(std::move(file_plan_applier_factory)),
      file_apply_observer_(std::move(file_apply_observer)), layout_store_(std::move(layout_store)),
      musicbrainz_(std::move(musicbrainz)), requested_item_count_(requested_item_count) {
    setObjectName(QStringLiteral("bench-metadata-properties"));
    setWindowTitle(QStringLiteral("Edit tags"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1'020, 620);
    restoreLayoutState();

    root_layout_ = new QVBoxLayout(this);
    root_layout_->setContentsMargins(10, 8, 10, 8);
    root_layout_->setSpacing(6);

    summary_ = new QLabel(QStringLiteral("%1 %2 · preparing")
                              .arg(requested_item_count_)
                              .arg(pluralized(requested_item_count_, QStringLiteral("track"),
                                              QStringLiteral("tracks"))),
                          this);
    summary_->setObjectName(QStringLiteral("bench-metadata-summary"));
    root_layout_->addWidget(summary_);
    // ADR-0152: read-only technical summary for the selected files, fed
    // by the bounded background prober; sits under the selection summary.
    technical_status_ = new QLabel(this);
    technical_status_->setObjectName(QStringLiteral("bench-metadata-technical"));
    technical_status_->setWordWrap(true);
    technical_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root_layout_->addWidget(technical_status_);

    read_only_ =
        new QLabel(QStringLiteral("Read-only metadata preview · preparing selection"), this);
    read_only_->setObjectName(QStringLiteral("bench-metadata-read-only"));
    read_only_->setAccessibleName(QStringLiteral("Metadata write capability"));
    read_only_->setTextFormat(Qt::PlainText);
    read_only_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* side_panel = new QWidget(this);
    side_panel->setObjectName(QStringLiteral("bench-metadata-side-panel"));
    side_panel->setMinimumWidth(260);
    side_panel->setMaximumWidth(380);
    transformation_panel_ = side_panel;
    transformation_panel_->hide();
    auto* side_layout = new QVBoxLayout(side_panel);
    // Only the splitter-facing edge is inset so the panel's right edge lines
    // up with the footer buttons below it.
    side_layout->setContentsMargins(8, 0, 0, 0);
    side_layout->setSpacing(6);

    const auto section_heading = [side_panel](const QString& text, const QString& object_name,
                                              const QString& tool_tip) {
        auto* heading = new QLabel(text, side_panel);
        heading->setObjectName(object_name);
        auto heading_font = heading->font();
        heading_font.setBold(true);
        heading->setFont(heading_font);
        heading->setToolTip(tool_tip);
        return heading;
    };

    side_layout->addWidget(section_heading(
        QStringLiteral("When you apply"), QStringLiteral("bench-preparation-actions-heading"),
        QStringLiteral("Apply rechecks the files and runs every checked action together; "
                       "problems stop the run before anything is written")));
    save_tags_check_ = new QCheckBox(QStringLiteral("Save tags"), side_panel);
    save_tags_check_->setObjectName(QStringLiteral("bench-preparation-save-tags"));
    save_tags_check_->setChecked(true);
    save_tags_check_->setToolTip(QStringLiteral("Write the drafted tag edits into the files"));
    side_layout->addWidget(save_tags_check_);
    rename_files_check_ = new QCheckBox(QStringLiteral("Rename files"), side_panel);
    rename_files_check_->setObjectName(QStringLiteral("bench-preparation-rename-files"));
    rename_files_check_->setEnabled(false);
    rename_files_check_->setToolTip(QStringLiteral("Choose a naming layout first"));
    side_layout->addWidget(rename_files_check_);
    auto* rename_row = new QHBoxLayout;
    rename_row->setContentsMargins(22, 0, 0, 0);
    rename_row->setSpacing(4);
    output_layout_combo_ = new QComboBox(side_panel);
    output_layout_combo_->setObjectName(QStringLiteral("bench-output-layout-profile"));
    output_layout_combo_->setAccessibleName(QStringLiteral("Saved naming layout"));
    output_layout_combo_->setPlaceholderText(QStringLiteral("None saved yet"));
    rename_row->addWidget(output_layout_combo_, 1);
    auto* manage_layouts_button = new QPushButton(QStringLiteral("Edit…"), side_panel);
    manage_layouts_button->setObjectName(QStringLiteral("bench-output-layout-manage"));
    manage_layouts_button->setToolTip(QStringLiteral("Create, change, or remove naming layouts"));
    rename_row->addWidget(manage_layouts_button);
    side_layout->addLayout(rename_row);
    move_files_check_ = new QCheckBox(QStringLiteral("Move files"), side_panel);
    move_files_check_->setObjectName(QStringLiteral("bench-preparation-move-files"));
    move_files_check_->setEnabled(false);
    move_files_check_->setToolTip(QStringLiteral("Choose a naming layout and a destination first"));
    side_layout->addWidget(move_files_check_);
    auto* move_row = new QHBoxLayout;
    move_row->setContentsMargins(22, 0, 0, 0);
    move_row->setSpacing(4);
    destination_combo_ = new QComboBox(side_panel);
    destination_combo_->setObjectName(QStringLiteral("bench-destination-profile"));
    destination_combo_->setAccessibleName(QStringLiteral("Saved move destination"));
    destination_combo_->setPlaceholderText(QStringLiteral("None saved yet"));
    move_row->addWidget(destination_combo_, 1);
    auto* manage_destinations_button = new QPushButton(QStringLiteral("Edit…"), side_panel);
    manage_destinations_button->setObjectName(QStringLiteral("bench-destination-manage"));
    manage_destinations_button->setToolTip(
        QStringLiteral("Create, change, or remove move destinations"));
    move_row->addWidget(manage_destinations_button);
    side_layout->addLayout(move_row);
    auto* replaygain_row = new QHBoxLayout;
    replaygain_row->setContentsMargins(0, 0, 0, 0);
    replaygain_row->setSpacing(4);
    replaygain_scan_button_ = new QPushButton(QStringLiteral("ReplayGain scan"), side_panel);
    replaygain_scan_button_->setObjectName(QStringLiteral("bench-replaygain-scan"));
    replaygain_scan_button_->setToolTip(
        QStringLiteral("Measure EBU R128 loudness for the selected files and stage the "
                       "ReplayGain tags as colored draft edits — nothing is written until "
                       "you apply"));
    replaygain_scan_button_->setEnabled(false);
    replaygain_row->addWidget(replaygain_scan_button_);
    replaygain_grouping_ = new QComboBox(side_panel);
    replaygain_grouping_->setObjectName(QStringLiteral("bench-replaygain-grouping"));
    replaygain_grouping_->setAccessibleName(QStringLiteral("ReplayGain album grouping"));
    replaygain_grouping_->addItem(QStringLiteral("Album by release"));
    replaygain_grouping_->addItem(QStringLiteral("Album merging discs"));
    replaygain_grouping_->addItem(QStringLiteral("Selection as one album"));
    replaygain_grouping_->addItem(QStringLiteral("Track gains only"));
    replaygain_grouping_->addItem(QStringLiteral("Group by expression"));
    replaygain_row->addWidget(replaygain_grouping_, 1);
    side_layout->addLayout(replaygain_row);
    replaygain_expression_ = new QLineEdit(side_panel);
    replaygain_expression_->setObjectName(QStringLiteral("bench-replaygain-expression"));
    replaygain_expression_->setPlaceholderText(QStringLiteral("tkfmt-1, e.g. %album%"));
    replaygain_expression_->hide();
    side_layout->addWidget(replaygain_expression_);
    // ADR-0146: the "never modify audio files" policy. CUE tracks keep
    // their sheet either way.
    replaygain_sidecar_only_ = new QCheckBox(QStringLiteral("Store in sidecar only"), side_panel);
    replaygain_sidecar_only_->setObjectName(QStringLiteral("bench-replaygain-sidecar-only"));
    replaygain_sidecar_only_->setToolTip(
        QStringLiteral("Write loudness into .tkmeta sidecars instead of audio-file tags, even "
                       "for writable formats; CUE tracks keep their sheet"));
    replaygain_sidecar_only_->setChecked(
        QSettings{}.value(QStringLiteral("replaygain/sidecar-only"), false).toBool());
    connect(replaygain_sidecar_only_, &QCheckBox::toggled, this, [](const bool enabled) {
        QSettings{}.setValue(QStringLiteral("replaygain/sidecar-only"), enabled);
    });
    side_layout->addWidget(replaygain_sidecar_only_);
    // ADR-0148: default stays the ReplayGain 2.0 sample peak; opting in
    // stores the oversampled true peak in the same REPLAYGAIN_*_PEAK
    // fields, the way foobar2000 and loudgain do.
    replaygain_true_peak_ =
        new QCheckBox(QStringLiteral("True peak as ReplayGain peak"), side_panel);
    replaygain_true_peak_->setObjectName(QStringLiteral("bench-replaygain-true-peak"));
    replaygain_true_peak_->setToolTip(
        QStringLiteral("Propose the oversampled inter-sample peak instead of the sample peak in "
                       "REPLAYGAIN_TRACK_PEAK and REPLAYGAIN_ALBUM_PEAK"));
    replaygain_true_peak_->setChecked(
        QSettings{}.value(QStringLiteral("replaygain/true-peak"), false).toBool());
    connect(replaygain_true_peak_, &QCheckBox::toggled, this, [](const bool enabled) {
        QSettings{}.setValue(QStringLiteral("replaygain/true-peak"), enabled);
    });
    side_layout->addWidget(replaygain_true_peak_);
    // ADR-0147: per-track view of where each effective loudness value
    // comes from — draft, sidecar, CUE segment, or embedded tags.
    replaygain_provenance_button_ =
        new QPushButton(QStringLiteral("Loudness sources…"), side_panel);
    replaygain_provenance_button_->setObjectName(QStringLiteral("bench-replaygain-provenance"));
    replaygain_provenance_button_->setEnabled(false);
    connect(replaygain_provenance_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::showLoudnessProvenance);
    side_layout->addWidget(replaygain_provenance_button_);
    output_profile_status_ = new QLabel(QStringLiteral("Loading output profiles…"), side_panel);
    output_profile_status_->setObjectName(QStringLiteral("bench-output-profile-status"));
    output_profile_status_->setWordWrap(true);
    side_layout->addWidget(output_profile_status_);

    auto* section_separator = new QFrame(side_panel);
    section_separator->setFrameShape(QFrame::HLine);
    section_separator->setFrameShadow(QFrame::Sunken);
    side_layout->addWidget(section_separator);

    side_layout->addWidget(section_heading(
        QStringLiteral("Scripts"), QStringLiteral("bench-metadata-scripts-heading"),
        QStringLiteral("Checked scripts stage their edits as colored drafts when files "
                       "load and when suggestions arrive; Apply writes exactly what the "
                       "grid shows")));
    transformation_list_ =
        new EmptyStateListWidget(QStringLiteral("No saved scripts yet"), side_panel);
    transformation_list_->setObjectName(QStringLiteral("bench-metadata-transformation-list"));
    transformation_list_->setAccessibleName(QStringLiteral("Saved tagging scripts"));
    transformation_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    transformation_list_->setAlternatingRowColors(true);
    transformation_list_->setEnabled(false);
    side_layout->addWidget(transformation_list_, 1);
    transformation_status_ = new QLabel(QStringLiteral("Loading saved scripts…"), side_panel);
    transformation_status_->setObjectName(QStringLiteral("bench-metadata-transformation-status"));
    transformation_status_->setWordWrap(true);
    side_layout->addWidget(transformation_status_);
    transform_button_ = new QPushButton(QStringLiteral("Open script editor…"), side_panel);
    transform_button_->setObjectName(QStringLiteral("bench-metadata-transform"));
    transform_button_->setToolTip(
        QStringLiteral("Open the selected saved script, or create a new one"));
    transform_button_->setEnabled(false);
    side_layout->addWidget(transform_button_);

    auto* layout_manager = new QDialog(this);
    layout_manager->setObjectName(QStringLiteral("bench-output-layout-manager"));
    layout_manager->setWindowTitle(QStringLiteral("Naming layouts"));
    layout_manager->setModal(false);
    layout_manager->setMinimumSize(560, 380);
    layout_manager->resize(820, 540);
    auto* layout_manager_box = new QVBoxLayout(layout_manager);
    auto* layout_manager_hint =
        new QLabel(QStringLiteral("Changes the layout currently selected in the tag editor. "
                                  "“New” starts a blank layout."),
                   layout_manager);
    layout_manager_hint->setWordWrap(true);
    layout_manager_box->addWidget(layout_manager_hint);
    auto* layout_form = new QFormLayout;
    layout_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    output_layout_name_ = new QLineEdit(layout_manager);
    output_layout_name_->setObjectName(QStringLiteral("bench-output-layout-name"));
    output_layout_name_->setPlaceholderText(QStringLiteral("For example: Album folders"));
    layout_form->addRow(QStringLiteral("Name:"), output_layout_name_);
    const auto expression_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    output_directory_expression_ = new QLineEdit(layout_manager);
    output_directory_expression_->setObjectName(
        QStringLiteral("bench-output-layout-directory-expression"));
    output_directory_expression_->setPlaceholderText(
        QStringLiteral("For example: %album artist%/%album%"));
    output_directory_expression_->setFont(expression_font);
    layout_form->addRow(QStringLiteral("Folders:"), output_directory_expression_);
    output_basename_expression_ = new QLineEdit(layout_manager);
    output_basename_expression_->setObjectName(
        QStringLiteral("bench-output-layout-basename-expression"));
    output_basename_expression_->setPlaceholderText(
        QStringLiteral("For example: %tracknumber% - %title%"));
    output_basename_expression_->setFont(expression_font);
    layout_form->addRow(QStringLiteral("Filename:"), output_basename_expression_);
    output_sanitization_policy_ = new QComboBox(layout_manager);
    output_sanitization_policy_->setObjectName(QStringLiteral("bench-output-layout-sanitization"));
    output_sanitization_policy_->addItem(QStringLiteral("Linux filenames"),
                                         QStringLiteral("linux"));
    output_sanitization_policy_->addItem(QStringLiteral("Portable filenames"),
                                         QStringLiteral("portable"));
    output_sanitization_policy_->setToolTip(
        QStringLiteral("Portable replaces Windows-forbidden characters, trailing dots/spaces, "
                       "and reserved device names; Unicode spelling is preserved"));
    layout_form->addRow(QStringLiteral("Filename policy:"), output_sanitization_policy_);
    layout_manager_box->addLayout(layout_form);
    output_layout_example_ =
        new QLabel(QStringLiteral("Preview: waiting for tracks…"), layout_manager);
    output_layout_example_->setObjectName(QStringLiteral("bench-output-layout-example"));
    output_layout_example_->setAccessibleName(QStringLiteral("Naming layout preview status"));
    output_layout_example_->setTextFormat(Qt::PlainText);
    output_layout_example_->setWordWrap(true);
    layout_manager_box->addWidget(output_layout_example_);
    output_layout_preview_ = new QTreeWidget(layout_manager);
    output_layout_preview_->setObjectName(QStringLiteral("bench-output-layout-preview"));
    output_layout_preview_->setAccessibleName(QStringLiteral("Naming layout live preview"));
    output_layout_preview_->setColumnCount(2);
    output_layout_preview_->setHeaderLabels(
        {QStringLiteral("Current name"), QStringLiteral("New path")});
    output_layout_preview_->setRootIsDecorated(false);
    output_layout_preview_->setAlternatingRowColors(true);
    output_layout_preview_->setUniformRowHeights(true);
    output_layout_preview_->setTextElideMode(Qt::ElideMiddle);
    output_layout_preview_->setSelectionMode(QAbstractItemView::NoSelection);
    output_layout_preview_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    output_layout_preview_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    output_layout_preview_->header()->setStretchLastSection(true);
    layout_manager_box->addWidget(output_layout_preview_, 1);
    auto* layout_buttons = new QHBoxLayout;
    output_layout_new_button_ = new QPushButton(QStringLiteral("New"), layout_manager);
    output_layout_new_button_->setObjectName(QStringLiteral("bench-output-layout-new"));
    output_layout_new_button_->setAutoDefault(false);
    output_layout_save_button_ = new QPushButton(QStringLiteral("Save"), layout_manager);
    output_layout_save_button_->setObjectName(QStringLiteral("bench-output-layout-save"));
    output_layout_save_button_->setAutoDefault(false);
    output_layout_remove_button_ = new QPushButton(QStringLiteral("Remove"), layout_manager);
    output_layout_remove_button_->setObjectName(QStringLiteral("bench-output-layout-remove"));
    output_layout_remove_button_->setAutoDefault(false);
    layout_buttons->addWidget(output_layout_new_button_);
    layout_buttons->addWidget(output_layout_save_button_);
    layout_buttons->addWidget(output_layout_remove_button_);
    layout_buttons->addStretch(1);
    auto* layout_manager_close = new QPushButton(QStringLiteral("Close"), layout_manager);
    layout_manager_close->setObjectName(QStringLiteral("bench-output-layout-manager-close"));
    layout_manager_close->setAutoDefault(false);
    layout_buttons->addWidget(layout_manager_close);
    layout_manager_box->addLayout(layout_buttons);
    connect(layout_manager_close, &QPushButton::clicked, layout_manager, &QDialog::close);
    connect(manage_layouts_button, &QPushButton::clicked, this, [this, layout_manager] {
        layout_manager->show();
        layout_manager->raise();
        layout_manager->activateWindow();
        scheduleOutputLayoutExample();
    });

    auto* destination_manager = new QDialog(this);
    destination_manager->setObjectName(QStringLiteral("bench-destination-manager"));
    destination_manager->setWindowTitle(QStringLiteral("Move destinations"));
    destination_manager->setModal(false);
    destination_manager->setMinimumWidth(460);
    auto* destination_manager_box = new QVBoxLayout(destination_manager);
    auto* destination_manager_hint =
        new QLabel(QStringLiteral("Changes the destination currently selected in the tag editor. "
                                  "“New” starts a blank destination."),
                   destination_manager);
    destination_manager_hint->setWordWrap(true);
    destination_manager_box->addWidget(destination_manager_hint);
    auto* destination_form = new QFormLayout;
    destination_name_ = new QLineEdit(destination_manager);
    destination_name_->setObjectName(QStringLiteral("bench-destination-name"));
    destination_name_->setPlaceholderText(QStringLiteral("For example: Music library"));
    destination_form->addRow(QStringLiteral("Name:"), destination_name_);
    auto* root_row = new QHBoxLayout;
    destination_root_ = new QLineEdit(destination_manager);
    destination_root_->setObjectName(QStringLiteral("bench-destination-root"));
    destination_root_->setPlaceholderText(QStringLiteral("Choose an absolute folder"));
    destination_browse_button_ = new QPushButton(QStringLiteral("Browse…"), destination_manager);
    destination_browse_button_->setObjectName(QStringLiteral("bench-destination-browse"));
    root_row->addWidget(destination_root_, 1);
    root_row->addWidget(destination_browse_button_);
    destination_form->addRow(QStringLiteral("Root:"), root_row);
    destination_manager_box->addLayout(destination_form);
    auto* destination_buttons = new QHBoxLayout;
    destination_new_button_ = new QPushButton(QStringLiteral("New"), destination_manager);
    destination_new_button_->setObjectName(QStringLiteral("bench-destination-new"));
    destination_new_button_->setAutoDefault(false);
    destination_save_button_ = new QPushButton(QStringLiteral("Save"), destination_manager);
    destination_save_button_->setObjectName(QStringLiteral("bench-destination-save"));
    destination_save_button_->setAutoDefault(false);
    destination_remove_button_ = new QPushButton(QStringLiteral("Remove"), destination_manager);
    destination_remove_button_->setObjectName(QStringLiteral("bench-destination-remove"));
    destination_remove_button_->setAutoDefault(false);
    destination_buttons->addWidget(destination_new_button_);
    destination_buttons->addWidget(destination_save_button_);
    destination_buttons->addWidget(destination_remove_button_);
    destination_buttons->addStretch(1);
    auto* destination_manager_close = new QPushButton(QStringLiteral("Close"), destination_manager);
    destination_manager_close->setObjectName(QStringLiteral("bench-destination-manager-close"));
    destination_manager_close->setAutoDefault(false);
    destination_buttons->addWidget(destination_manager_close);
    destination_manager_box->addLayout(destination_buttons);
    connect(destination_manager_close, &QPushButton::clicked, destination_manager, &QDialog::close);
    connect(manage_destinations_button, &QPushButton::clicked, destination_manager,
            [destination_manager] {
                destination_manager->show();
                destination_manager->raise();
                destination_manager->activateWindow();
            });

    loading_ = new QLabel(QStringLiteral("Preparing metadata grid…"), this);
    loading_->setObjectName(QStringLiteral("bench-metadata-loading"));
    loading_->setAlignment(Qt::AlignCenter);
    root_layout_->addWidget(loading_, 1);

    grid_tools_ = new QWidget(this);
    grid_tools_->setObjectName(QStringLiteral("bench-metadata-grid-tools"));
    grid_tools_->hide();
    auto* grid_tools_layout = new QHBoxLayout(grid_tools_);
    grid_tools_layout->setContentsMargins(0, 0, 0, 0);
    grid_tools_layout->setSpacing(6);
    add_field_button_ = new QPushButton(QStringLiteral("Add field…"), grid_tools_);
    add_field_button_->setObjectName(QStringLiteral("bench-metadata-add-field"));
    add_field_button_->setToolTip(QStringLiteral("Add an arbitrary metadata field (Insert)"));
    add_field_button_->setEnabled(false);
    grid_tools_layout->addWidget(add_field_button_);
    remove_field_button_ = new QPushButton(QStringLiteral("Remove field"), grid_tools_);
    remove_field_button_->setObjectName(QStringLiteral("bench-metadata-remove-field"));
    remove_field_button_->setToolTip(
        QStringLiteral("Remove the selected fields from the selected files (Delete)"));
    remove_field_button_->setEnabled(false);
    grid_tools_layout->addWidget(remove_field_button_);
    edit_values_button_ = new QPushButton(QStringLiteral("Edit values…"), grid_tools_);
    edit_values_button_->setObjectName(QStringLiteral("bench-metadata-edit-values"));
    edit_values_button_->setToolTip(
        QStringLiteral("Edit the exact ordered value list (Ctrl+Enter)"));
    edit_values_button_->setEnabled(false);
    grid_tools_layout->addWidget(edit_values_button_);
    field_layout_label_ = new QLabel(QStringLiteral("Fields:"), grid_tools_);
    field_layout_label_->setToolTip(
        QStringLiteral("Choose a saved set of metadata fields to show"));
    field_layout_label_->hide();
    grid_tools_layout->addWidget(field_layout_label_);
    field_layout_combo_ = new QComboBox(grid_tools_);
    field_layout_combo_->setObjectName(QStringLiteral("bench-metadata-field-layout"));
    field_layout_combo_->addItem(QStringLiteral("All fields"), QString{});
    field_layout_combo_->setToolTip(
        QStringLiteral("Show all metadata fields or a saved field set"));
    field_layout_combo_->hide();
    grid_tools_layout->addWidget(field_layout_combo_);
    field_layout_save_button_ =
        new QPushButton(QStringLiteral("Save visible fields as set…"), grid_tools_);
    field_layout_save_button_->setObjectName(QStringLiteral("bench-metadata-field-layout-save"));
    field_layout_save_button_->setToolTip(
        QStringLiteral("Save the selected fields, or all currently visible fields, as a named "
                       "field set"));
    field_layout_save_button_->hide();
    field_layout_remove_button_ = new QPushButton(QStringLiteral("Delete field set"), grid_tools_);
    field_layout_remove_button_->setObjectName(
        QStringLiteral("bench-metadata-field-layout-remove"));
    field_layout_remove_button_->setEnabled(false);
    field_layout_remove_button_->hide();
    suggest_button_ = new QPushButton(QStringLiteral("Suggest"), grid_tools_);
    suggest_button_->setObjectName(QStringLiteral("bench-metadata-suggest"));
    suggest_button_->setToolTip(
        QStringLiteral("Fill album artist and total tracks from agreement across the selected "
                       "files; suggestions become ordinary colored draft edits"));
    suggest_button_->setEnabled(false);
    suggest_button_->hide();
    identify_button_ = new QPushButton(QStringLiteral("Identify…"), grid_tools_);
    identify_button_->setObjectName(QStringLiteral("bench-metadata-identify"));
    identify_button_->setToolTip(
        QStringLiteral("Search MusicBrainz by artist and album — no MusicBrainz tags needed — "
                       "pick the exact release version, and stage the match as ordinary colored "
                       "draft edits"));
    identify_button_->setEnabled(false);
    grid_tools_layout->addWidget(identify_button_);
    auto* more_button = new QToolButton(grid_tools_);
    more_button->setObjectName(QStringLiteral("bench-metadata-more"));
    more_button->setText(QStringLiteral("More"));
    more_button->setPopupMode(QToolButton::InstantPopup);
    auto* more_menu = new QMenu(more_button);
    suggest_action_ = more_menu->addAction(QStringLiteral("Suggest album totals and artist"));
    suggest_action_->setToolTip(suggest_button_->toolTip());
    suggest_action_->setEnabled(false);
    connect(suggest_action_, &QAction::triggered, suggest_button_, &QPushButton::click);
    more_button->setMenu(more_menu);
    grid_tools_layout->addWidget(more_button);
    grid_tools_layout->addStretch(1);
    undo_button_ = new QPushButton(QStringLiteral("Undo"), grid_tools_);
    undo_button_->setObjectName(QStringLiteral("bench-metadata-undo"));
    undo_button_->setToolTip(QStringLiteral("Undo the last draft edit (Ctrl+Z)"));
    undo_button_->setShortcut(QKeySequence::Undo);
    undo_button_->setEnabled(false);
    grid_tools_layout->addWidget(undo_button_);
    redo_button_ = new QPushButton(QStringLiteral("Redo"), grid_tools_);
    redo_button_->setObjectName(QStringLiteral("bench-metadata-redo"));
    redo_button_->setToolTip(QStringLiteral("Redo the last undone draft edit (Ctrl+Shift+Z)"));
    redo_button_->setShortcut(QKeySequence::Redo);
    redo_button_->setEnabled(false);
    grid_tools_layout->addWidget(redo_button_);
    discard_button_ = new QPushButton(QStringLiteral("Discard"), grid_tools_);
    discard_button_->setObjectName(QStringLiteral("bench-metadata-discard"));
    discard_button_->setToolTip(QStringLiteral("Throw away every pending draft edit"));
    discard_button_->setEnabled(false);
    grid_tools_layout->addWidget(discard_button_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons_->setObjectName(QStringLiteral("bench-metadata-buttons"));
    apply_plan_button_ = buttons_->addButton(QStringLiteral("Apply"), QDialogButtonBox::ActionRole);
    apply_plan_button_->setObjectName(QStringLiteral("bench-metadata-apply-changes"));
    apply_plan_button_->setToolTip(QStringLiteral(
        "Recheck the files, then make every enabled change; problems stop the run and are shown"));
    apply_plan_button_->setEnabled(false);
    apply_plan_button_->setDefault(true);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(undo_button_, &QPushButton::clicked, this, [this] {
        if (grid_model_ != nullptr) {
            static_cast<void>(grid_model_->undo());
        }
    });
    connect(read_only_, &QLabel::linkActivated, this, [this](const QString& link) {
        if (link == QStringLiteral("undo-automatic") && grid_model_ != nullptr) {
            static_cast<void>(grid_model_->undo());
        }
        if (link == QStringLiteral("cancel-replaygain")) {
            replaygain_cancellation_.request_cancellation();
        }
        if (link == QStringLiteral("retry-replaygain") && !replaygain_retry_items_.empty()) {
            startReplayGainScan(replaygain_retry_items_);
        }
        if (link == QStringLiteral("export-replaygain") && !replaygain_export_rows_.isEmpty()) {
            exportReplayGainResults();
        }
    });
    connect(redo_button_, &QPushButton::clicked, this, [this] {
        if (grid_model_ != nullptr) {
            static_cast<void>(grid_model_->redo());
        }
    });
    connect(discard_button_, &QPushButton::clicked, this, [this] {
        if (grid_model_ != nullptr) {
            static_cast<void>(grid_model_->discardAll());
        }
    });
    connect(add_field_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::promptAddField);
    connect(remove_field_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::removeSelectedFields);
    connect(edit_values_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::editCurrentValues);
    connect(field_layout_combo_, &QComboBox::currentIndexChanged, this,
            &MetadataPropertiesDialog::applyCurrentFieldLayout);
    connect(field_layout_save_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::saveCurrentFieldLayout);
    connect(field_layout_remove_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::removeCurrentFieldLayout);
    connect(suggest_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::startProposals);
    connect(identify_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::startIdentify);
    connect(&automatic_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishAutomaticStage);
    connect(&replaygain_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishReplayGainScan);
    connect(replaygain_scan_button_, &QPushButton::clicked, this,
            [this] { startReplayGainScan(); });
    connect(replaygain_grouping_, &QComboBox::currentIndexChanged, this,
            [this](const int index) { replaygain_expression_->setVisible(index == 4); });
    connect(&proposal_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishProposals);
    connect(transform_button_, &QPushButton::clicked, this, [this] {
        std::optional<core::StableId> selected;
        if (const auto* item = transformation_list_->currentItem()) {
            if (const auto parsed =
                    core::StableId::parse(item->data(Qt::UserRole).toString().toStdString())) {
                selected = *parsed;
            }
        }
        promptTransformation(selected);
    });
    connect(transformation_list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                const auto selected =
                    core::StableId::parse(item->data(Qt::UserRole).toString().toStdString());
                if (selected) {
                    promptTransformation(*selected);
                }
            });
    connect(transformation_list_, &QListWidget::itemSelectionChanged, this,
            &MetadataPropertiesDialog::updateTransformationButton);
    connect(transformation_list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        const auto id = core::StableId::parse(item->data(Qt::UserRole).toString().toStdString());
        if (id) {
            toggleAutomaticTransformation(*id, item->checkState() == Qt::Checked);
        }
    });
    connect(output_layout_combo_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        selectOutputLayout(index);
        invalidateWritePlan();
    });
    connect(destination_combo_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        selectDestination(index);
        invalidateWritePlan();
    });
    connect(output_layout_new_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::newOutputLayout);
    connect(destination_new_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::newDestination);
    connect(output_layout_save_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::saveOutputLayout);
    connect(destination_save_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::saveDestination);
    connect(output_layout_remove_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::removeOutputLayout);
    connect(destination_remove_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::removeDestination);
    for (auto* editor : {output_layout_name_, output_directory_expression_,
                         output_basename_expression_, destination_name_, destination_root_}) {
        connect(editor, &QLineEdit::textChanged, this,
                &MetadataPropertiesDialog::updateOutputProfileButtons);
    }
    for (auto* expression : {output_directory_expression_, output_basename_expression_}) {
        connect(expression, &QLineEdit::textChanged, this,
                &MetadataPropertiesDialog::scheduleOutputLayoutExample);
    }
    connect(output_sanitization_policy_, &QComboBox::currentIndexChanged, this,
            &MetadataPropertiesDialog::scheduleOutputLayoutExample);
    connect(destination_root_, &QLineEdit::textEdited, this, [this](const QString& text) {
        const auto encoded = QFile::encodeName(text);
        destination_root_raw_path_.assign(encoded.constData(),
                                          static_cast<std::size_t>(encoded.size()));
    });
    connect(destination_browse_button_, &QPushButton::clicked, this, [this] {
        const auto initial = destination_root_raw_path_.empty()
                                 ? QString{}
                                 : QFile::decodeName(QByteArray{
                                       destination_root_raw_path_.data(),
                                       static_cast<qsizetype>(destination_root_raw_path_.size())});
        const auto selected = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose move destination"), initial,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (selected.isEmpty()) {
            return;
        }
        const auto encoded = QFile::encodeName(selected);
        destination_root_raw_path_.assign(encoded.constData(),
                                          static_cast<std::size_t>(encoded.size()));
        destination_root_->setText(
            QString::fromStdString(core::escape_raw_path(destination_root_raw_path_)));
        updateOutputProfileButtons();
    });
    connect(save_tags_check_, &QCheckBox::toggled, this, [this] {
        invalidateWritePlan();
        updateDraftState(draft_count_, undo_button_->isEnabled(), redo_button_->isEnabled());
        scheduleOutputLayoutExample();
    });
    for (auto* path_choice : {rename_files_check_, move_files_check_}) {
        connect(path_choice, &QCheckBox::toggled, this, [this] {
            invalidateWritePlan();
            updateWritePlanButton();
        });
    }
    connect(apply_plan_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::startWritePlan);
    connect(&write_plan_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishWritePlan);
    connect(&metadata_apply_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishMetadataApply);
    connect(&file_apply_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishFileApply);
    connect(&output_example_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishOutputLayoutExample);
    output_example_debounce_ = new QTimer(this);
    output_example_debounce_->setObjectName(QStringLiteral("bench-output-layout-example-timer"));
    output_example_debounce_->setSingleShot(true);
    output_example_debounce_->setInterval(250);
    connect(output_example_debounce_, &QTimer::timeout, this,
            &MetadataPropertiesDialog::startOutputLayoutExample);
    apply_progress_timer_ = new QTimer(this);
    apply_progress_timer_->setInterval(50);
    connect(apply_progress_timer_, &QTimer::timeout, this,
            &MetadataPropertiesDialog::updateApplyProgress);
    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("bench-metadata-footer"));
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(0, 0, 0, 0);
    footer_layout->setSpacing(8);
    footer_layout->addWidget(read_only_, 1);
    apply_progress_bar_ = new QProgressBar(footer);
    apply_progress_bar_->setObjectName(QStringLiteral("bench-metadata-apply-progress"));
    apply_progress_bar_->setAccessibleName(QStringLiteral("Apply progress"));
    apply_progress_bar_->setFixedWidth(170);
    apply_progress_bar_->setTextVisible(true);
    apply_progress_bar_->hide();
    footer_layout->addWidget(apply_progress_bar_);
    apply_stop_button_ = new QPushButton(QStringLiteral("Stop"), footer);
    apply_stop_button_->setObjectName(QStringLiteral("bench-metadata-apply-stop"));
    apply_stop_button_->setToolTip(
        QStringLiteral("Stop after the files already in progress are safe"));
    apply_stop_button_->hide();
    connect(apply_stop_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::requestApplyStop);
    footer_layout->addWidget(apply_stop_button_);
    footer_layout->addWidget(buttons_);
    root_layout_->addWidget(footer);
    loadTransformationCatalog();
    loadOutputProfiles();

    const metadata::StagedMetadataSelectionLimits limits;
    if (requested_item_count_ > limits.items) {
        summary_->setText(QStringLiteral("Properties unavailable"));
        read_only_->setText(QStringLiteral("Read-only metadata preview"));
        loading_->setText(
            QStringLiteral("The selection exceeds the %1-track limit").arg(limits.items));
        source_reader_ = {};
        return;
    }
    sources_.reserve(requested_item_count_);
    audio_sources_->reserve(requested_item_count_);
    track_labels_.reserve(static_cast<qsizetype>(std::min(
        requested_item_count_, static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))));
    preferred_fields_.reserve(preferred_fields.size());
    for (const auto field : preferred_fields) {
        preferred_fields_.emplace_back(field);
    }
    connect(&selection_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishSelection);
    QTimer::singleShot(0, this, &MetadataPropertiesDialog::captureSources);
}

MetadataPropertiesDialog::~MetadataPropertiesDialog() {
    write_plan_cancellation_.request_cancellation();
    apply_cancellation_.request_cancellation();
    output_example_cancellation_.request_cancellation();
    if (write_plan_running_) {
        write_plan_watcher_.waitForFinished();
    }
    if (apply_running_) {
        metadata_apply_watcher_.waitForFinished();
        file_apply_watcher_.waitForFinished();
    }
    if (output_example_running_) {
        output_example_watcher_.waitForFinished();
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
}

void MetadataPropertiesDialog::setArtworkMutationServices(
    ArtworkWritePlanApplierFactory applier_factory, ArtworkApplyObserver observer) {
    artwork_plan_applier_factory_ = std::move(applier_factory);
    artwork_apply_observer_ = std::move(observer);
    if (artwork_section_ != nullptr) {
        artwork_section_->setMutationServices(
            artwork_plan_applier_factory_, [this](const auto& result) { artworkApplied(result); });
    }
}

void MetadataPropertiesDialog::artworkApplied(const operations::ArtworkApplyResult& result) {
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

void MetadataPropertiesDialog::loadFieldLayouts() {
    const auto begin_capture = [this] {
        if (!active_field_layout_id_.isEmpty()) {
            const auto found =
                std::ranges::find(field_layouts_, active_field_layout_id_, &SavedFieldLayout::id);
            if (found != field_layouts_.end()) {
                std::vector<std::string> ordered;
                ordered.reserve(static_cast<std::size_t>(found->fields.size()) +
                                preferred_fields_.size());
                for (const auto& field : found->fields) {
                    ordered.push_back(encode_utf8(field));
                }
                ordered.insert(ordered.end(), preferred_fields_.begin(), preferred_fields_.end());
                preferred_fields_ = std::move(ordered);
            }
        }
        QTimer::singleShot(0, this, &MetadataPropertiesDialog::captureSources);
    };
    if (!layout_store_.load) {
        begin_capture();
        return;
    }
    const QPointer self{this};
    layout_store_.load(
        QString::fromLatin1(properties_field_layouts_key),
        [self, begin_capture](QByteArray state, const QString& error) mutable {
            if (!self) {
                return;
            }
            if (error.isEmpty() && !state.isEmpty()) {
                QJsonParseError parse_error;
                const auto document = QJsonDocument::fromJson(state, &parse_error);
                const auto root = document.object();
                if (parse_error.error == QJsonParseError::NoError && document.isObject() &&
                    root.value(QStringLiteral("schema")).toInt() == 1) {
                    self->active_field_layout_id_ = root.value(QStringLiteral("active")).toString();
                    const auto layouts = root.value(QStringLiteral("layouts")).toArray();
                    constexpr auto maximum_layouts = 64;
                    constexpr auto maximum_fields = 256;
                    for (const auto& value : layouts) {
                        if (self->field_layouts_.size() >= maximum_layouts || !value.isObject()) {
                            break;
                        }
                        const auto object = value.toObject();
                        SavedFieldLayout layout{.id = object.value(QStringLiteral("id")).toString(),
                                                .name =
                                                    object.value(QStringLiteral("name")).toString(),
                                                .fields = {}};
                        if (layout.id.isEmpty() || layout.name.trimmed().isEmpty()) {
                            continue;
                        }
                        for (const auto& field : object.value(QStringLiteral("fields")).toArray()) {
                            if (layout.fields.size() == maximum_fields || !field.isString()) {
                                break;
                            }
                            const auto name = field.toString().trimmed();
                            if (!name.isEmpty() &&
                                !layout.fields.contains(name, Qt::CaseInsensitive)) {
                                layout.fields.push_back(name);
                            }
                        }
                        if (!layout.fields.isEmpty()) {
                            self->field_layouts_.push_back(std::move(layout));
                        }
                    }
                }
            }
            {
                const QSignalBlocker blocker{self->field_layout_combo_};
                for (const auto& layout : self->field_layouts_) {
                    self->field_layout_combo_->addItem(layout.name, layout.id);
                }
                const auto index =
                    self->field_layout_combo_->findData(self->active_field_layout_id_);
                self->field_layout_combo_->setCurrentIndex(std::max(0, index));
            }
            const auto has_sets = !self->field_layouts_.empty();
            self->field_layout_label_->setVisible(has_sets);
            self->field_layout_combo_->setVisible(has_sets);
            begin_capture();
        });
}

void MetadataPropertiesDialog::persistFieldLayouts() {
    if (!layout_store_.save) {
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
    layout_store_.save(QString::fromLatin1(properties_field_layouts_key),
                       QJsonDocument(root).toJson(QJsonDocument::Compact), {});
}

void MetadataPropertiesDialog::applyCurrentFieldLayout() {
    if (field_layout_combo_ == nullptr) {
        return;
    }
    active_field_layout_id_ = field_layout_combo_->currentData().toString();
    QStringList fields;
    const auto found =
        std::ranges::find(field_layouts_, active_field_layout_id_, &SavedFieldLayout::id);
    if (found != field_layouts_.end()) {
        fields = found->fields;
    }
    if (field_review_bar_ != nullptr) {
        field_review_bar_->setLayoutFields(std::move(fields));
    }
    field_layout_remove_button_->setEnabled(found != field_layouts_.end());
    if (field_layout_remove_action_ != nullptr) {
        field_layout_remove_action_->setEnabled(found != field_layouts_.end());
    }
    persistFieldLayouts();
}

void MetadataPropertiesDialog::saveCurrentFieldLayout() {
    if (aggregate_model_ == nullptr || fields_ == nullptr || field_review_bar_ == nullptr ||
        field_layouts_.size() >= 64U) {
        return;
    }
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("Save field set"),
                              QStringLiteral("Field set name:"), QLineEdit::Normal, {}, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    QStringList field_names;
    for (const auto& index : fields_->selectionModel()->selectedRows(0)) {
        field_names.push_back(index.data(metadata_field_canonical_name_role).toString());
    }
    if (field_names.isEmpty()) {
        field_names = field_review_bar_->visibleFieldNames();
    }
    field_names.removeDuplicates();
    if (field_names.isEmpty()) {
        return;
    }
    SavedFieldLayout saved{.id = QUuid::createUuid().toString(QUuid::WithoutBraces),
                           .name = name,
                           .fields = std::move(field_names)};
    field_layouts_.push_back(saved);
    field_layout_combo_->addItem(saved.name, saved.id);
    field_layout_label_->show();
    field_layout_combo_->show();
    field_layout_combo_->setCurrentIndex(field_layout_combo_->count() - 1);
    persistFieldLayouts();
}

void MetadataPropertiesDialog::removeCurrentFieldLayout() {
    if (active_field_layout_id_.isEmpty()) {
        return;
    }
    std::erase_if(field_layouts_,
                  [this](const auto& layout) { return layout.id == active_field_layout_id_; });
    const auto index = field_layout_combo_->currentIndex();
    field_layout_combo_->removeItem(index);
    field_layout_combo_->setCurrentIndex(0);
    const auto has_sets = !field_layouts_.empty();
    field_layout_label_->setVisible(has_sets);
    field_layout_combo_->setVisible(has_sets);
    persistFieldLayouts();
}

void MetadataPropertiesDialog::restoreLayoutState() {
    if (!layout_store_.load) {
        return;
    }
    const QPointer self{this};
    layout_store_.load(QString::fromLatin1(properties_geometry_key),
                       [self](QByteArray state, const QString& error) {
                           if (self && error.isEmpty() && !state.isEmpty()) {
                               static_cast<void>(self->restoreGeometry(state));
                           }
                       });
    layout_store_.load(QString::fromLatin1(properties_content_splitter_key),
                       [self](QByteArray state, const QString& error) {
                           if (!self || !error.isEmpty() || state.isEmpty()) {
                               return;
                           }
                           self->pending_content_splitter_state_ = std::move(state);
                           if (self->content_splitter_ != nullptr) {
                               static_cast<void>(self->content_splitter_->restoreState(
                                   self->pending_content_splitter_state_));
                           }
                       });
    layout_store_.load(QString::fromLatin1(properties_metadata_splitter_key),
                       [self](QByteArray state, const QString& error) {
                           if (!self || !error.isEmpty() || state.isEmpty()) {
                               return;
                           }
                           self->pending_metadata_splitter_state_ = std::move(state);
                           if (self->metadata_splitter_ != nullptr) {
                               static_cast<void>(self->metadata_splitter_->restoreState(
                                   self->pending_metadata_splitter_state_));
                           }
                       });
}

void MetadataPropertiesDialog::persistLayoutState() {
    if (layout_state_saved_ || !layout_store_.save) {
        return;
    }
    layout_state_saved_ = true;
    layout_store_.save(QString::fromLatin1(properties_geometry_key), saveGeometry(), {});
    if (content_splitter_ != nullptr) {
        layout_store_.save(QString::fromLatin1(properties_content_splitter_key),
                           content_splitter_->saveState(), {});
    }
    if (metadata_splitter_ != nullptr) {
        layout_store_.save(QString::fromLatin1(properties_metadata_splitter_key),
                           metadata_splitter_->saveState(), {});
    }
}

void MetadataPropertiesDialog::captureSources() {
    constexpr auto capture_budget_ms = 4;
    if (capture_index_ >= requested_item_count_) {
        summary_->setText(QStringLiteral("Properties unavailable"));
        read_only_->setText(QStringLiteral("Read-only metadata preview"));
        loading_->setText(QStringLiteral("No tracks were selected"));
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
        loading_->setText(QStringLiteral("Preparing metadata grid… %1/%2")
                              .arg(capture_index_)
                              .arg(requested_item_count_));
        QTimer::singleShot(0, this, &MetadataPropertiesDialog::captureSources);
        return;
    }
    source_reader_ = {};
    if (sources_.empty()) {
        summary_->setText(QStringLiteral("Properties unavailable"));
        read_only_->setText(QStringLiteral("Read-only metadata preview"));
        loading_->setText(QStringLiteral("The selected tracks are no longer available"));
        return;
    }
    startSelection();
}

void MetadataPropertiesDialog::startSelection() {
    selection_watcher_.setFuture(
        QtConcurrent::run([sources = std::move(sources_), preferred = std::move(preferred_fields_),
                           token = technical_cancellation_.token()]() mutable {
            auto prepared = metadata::capture_uncached_metadata_sources(std::move(sources), token);
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

void MetadataPropertiesDialog::finishSelection() {
    const auto result = selection_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The selection task returned no result");
        summary_->setText(QStringLiteral("Properties unavailable"));
        read_only_->setText(QStringLiteral("Read-only metadata preview"));
        loading_->setText(message);
        return;
    }
    buildGrid(std::move(**result));
}

void MetadataPropertiesDialog::buildGrid(metadata::StagedMetadataSelection selection) {
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
    updateDraftState(0, false, false);

    content_splitter_ = new QSplitter(Qt::Horizontal, this);
    content_splitter_->setObjectName(QStringLiteral("bench-metadata-content-splitter"));
    content_splitter_->setChildrenCollapsible(false);
    metadata_splitter_ = new QSplitter(Qt::Vertical, content_splitter_);
    metadata_splitter_->setObjectName(QStringLiteral("bench-metadata-splitter"));
    metadata_splitter_->setChildrenCollapsible(false);

    file_list_ = new QTableView(metadata_splitter_);
    file_list_->setObjectName(QStringLiteral("bench-metadata-files"));
    file_list_->setAccessibleName(QStringLiteral("Files included in metadata edit"));
    grid_model_ = new MetadataGridModel(std::move(selection), std::move(track_labels_), file_list_);
    file_list_->setModel(grid_model_);
    file_list_->setAlternatingRowColors(true);
    file_list_->setShowGrid(false);
    file_list_->setWordWrap(false);
    file_list_->setTextElideMode(Qt::ElideMiddle);
    file_list_->setSelectionBehavior(QAbstractItemView::SelectRows);
    file_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    file_list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    file_list_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    file_list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    file_list_->verticalHeader()->hide();
    file_list_->verticalHeader()->setDefaultSectionSize(24);
    file_list_->setMinimumHeight(120);
    file_list_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (auto column = 1; column < grid_model_->columnCount(); ++column) {
        file_list_->hideColumn(column);
    }
    connect(grid_model_, &QAbstractItemModel::columnsInserted, file_list_,
            [this](const QModelIndex& parent, const int first, const int last) {
                if (parent.isValid()) {
                    return;
                }
                for (auto column = first; column <= last; ++column) {
                    file_list_->hideColumn(column);
                }
            });

    fields_ = new QTableView(metadata_splitter_);
    fields_->setObjectName(QStringLiteral("bench-metadata-fields"));
    fields_->setAccessibleName(QStringLiteral("Metadata fields with original and draft values"));
    aggregate_model_ = new MetadataAggregateModel(grid_model_, fields_);
    fields_->setModel(aggregate_model_);
    fields_->setAlternatingRowColors(true);
    fields_->setShowGrid(false);
    fields_->setWordWrap(false);
    fields_->setTextElideMode(Qt::ElideRight);
    fields_->setSelectionBehavior(QAbstractItemView::SelectRows);
    fields_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    fields_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                             QAbstractItemView::AnyKeyPressed);
    fields_->setItemDelegate(createMetadataScalarDelegate(fields_));
    fields_->installEventFilter(this);
    fields_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    fields_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    fields_->verticalHeader()->hide();
    fields_->verticalHeader()->setDefaultSectionSize(24);
    fields_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    fields_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    fields_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    fields_->setColumnWidth(0, 190);

    auto* fields_pane = new QWidget(metadata_splitter_);
    fields_pane->setObjectName(QStringLiteral("bench-metadata-fields-pane"));
    auto* fields_pane_layout = new QVBoxLayout(fields_pane);
    fields_pane_layout->setContentsMargins(0, 0, 0, 0);
    fields_pane_layout->setSpacing(4);
    field_review_bar_ =
        new MetadataFieldReviewBar(fields_, aggregate_model_, file_list_, fields_pane);
    if (const auto layout =
            std::ranges::find(field_layouts_, active_field_layout_id_, &SavedFieldLayout::id);
        layout != field_layouts_.end()) {
        field_review_bar_->setLayoutFields(layout->fields);
        field_layout_remove_button_->setEnabled(true);
        field_layout_remove_action_->setEnabled(true);
    }
    fields_pane_layout->addWidget(field_review_bar_);
    grid_tools_->setParent(fields_pane);
    fields_pane_layout->addWidget(grid_tools_);
    fields_pane_layout->addWidget(fields_, 1);
    grid_tools_->show();

    metadata_sections_ = new QTabWidget(metadata_splitter_);
    metadata_sections_->setObjectName(QStringLiteral("bench-metadata-sections"));
    metadata_sections_->setAccessibleName(QStringLiteral("Metadata property sections"));
    // Artwork's optional draft and problem tables must scroll inside their
    // pane, not raise the splitter minimum and crush the selected-files list.
    metadata_sections_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    metadata_sections_->addTab(fields_pane, QStringLiteral("Fields"));
    artwork_section_ = new MetadataArtworkSection(metadata_sections_);
    artwork_section_->setUnifiedApply(static_cast<bool>(plan_applier_factory_));
    artwork_section_->setMutationServices(artwork_plan_applier_factory_,
                                          [this](const auto& result) { artworkApplied(result); });
    if (musicbrainz_.fetch) {
        const QPointer self{this};
        artwork_section_->setCoverArtService(ArtworkCoverArtService{
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
                    self->musicbrainz_.fetch(
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
                [self](const QString& url,
                       std::function<void(core::Result<QByteArray>)> completion) {
                    if (self.isNull()) {
                        return;
                    }
                    self->musicbrainz_.fetch(url, std::move(completion));
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
        });
    }
    const auto artwork_page =
        metadata_sections_->addTab(artwork_section_, QStringLiteral("Artwork"));
    connect(artwork_section_, &MetadataArtworkSection::operationRunningChanged, this,
            [this](const bool running) {
                artwork_operation_running_ = running;
                file_list_->setEnabled(!artwork_operation_running_ &&
                                       !artwork_section_->hasPendingChanges());
                updateWritePlanButton();
                updateTransformationButton();
            });
    connect(
        artwork_section_, &MetadataArtworkSection::pendingChangesChanged, this, [this](const bool) {
            artwork_operation_running_ = artwork_section_->isBusy();
            file_list_->setEnabled(!artwork_operation_running_ &&
                                   !artwork_section_->hasPendingChanges());
            updateDraftState(draft_count_, undo_button_->isEnabled(), redo_button_->isEnabled());
        });
    connect(metadata_sections_, &QTabWidget::currentChanged, this,
            [this, artwork_page](const int index) {
                if (artwork_section_ != nullptr) {
                    artwork_section_->setActive(index == artwork_page);
                }
            });

    metadata_splitter_->addWidget(file_list_);
    metadata_splitter_->addWidget(metadata_sections_);
    metadata_splitter_->setStretchFactor(0, 1);
    metadata_splitter_->setStretchFactor(1, 3);
    metadata_splitter_->setSizes({170, 390});
    if (!pending_metadata_splitter_state_.isEmpty()) {
        static_cast<void>(metadata_splitter_->restoreState(pending_metadata_splitter_state_));
    }
    content_splitter_->addWidget(metadata_splitter_);
    content_splitter_->addWidget(transformation_panel_);
    transformation_panel_->show();
    content_splitter_->setStretchFactor(0, 1);
    content_splitter_->setStretchFactor(1, 0);
    content_splitter_->setSizes({760, 260});
    if (!pending_content_splitter_state_.isEmpty()) {
        static_cast<void>(content_splitter_->restoreState(pending_content_splitter_state_));
    }
    root_layout_->insertWidget(root_layout_->count() - 1, content_splitter_, 1);

    selection_debounce_ = new QTimer(this);
    selection_debounce_->setSingleShot(true);
    selection_debounce_->setInterval(40);
    connect(selection_debounce_, &QTimer::timeout, this,
            &MetadataPropertiesDialog::updateSelectionProjection);
    connect(file_list_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        scheduleSelectionProjection();
        scheduleOutputLayoutExample();
        updateTechnicalSummary();
    });
    connect(&technical_watcher_, &QFutureWatcherBase::finished, this, [this] {
        technical_probing_ = false;
        auto outcome = technical_watcher_.result();
        technical_pending_.erase(outcome.first);
        technical_cache_[outcome.first] = std::move(outcome.second);
        pumpTechnicalQueue();
        updateTechnicalSummary();
    });
    connect(fields_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        updateFieldButtons();
        updateEditValuesButton();
    });
    connect(fields_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateFieldButtons(); });
    connect(grid_model_, &MetadataGridModel::draftStateChanged, this,
            [this](const int patch_count, const bool can_undo, const bool can_redo) {
                sticky_status_.clear();
                invalidateWritePlan();
                updateDraftState(patch_count, can_undo, can_redo);
                scheduleOutputLayoutExample();
            });
    connect(grid_model_, &MetadataGridModel::editRejected, this, [this](const QString& message) {
        read_only_->setText(QStringLiteral("Draft edit rejected · %1").arg(message));
    });
    connect(aggregate_model_, &MetadataAggregateModel::editRejected, this,
            [this](const QString& message) {
                read_only_->setText(QStringLiteral("Draft edit rejected · %1").arg(message));
            });
    connect(aggregate_model_, &MetadataAggregateModel::selectionProjectionChanged, this,
            [this](const bool ready, const int selected_count) {
                if (ready) {
                    updateDraftState(draft_count_, undo_button_->isEnabled(),
                                     redo_button_->isEnabled());
                } else {
                    read_only_->setText(QStringLiteral("Preparing metadata for %1 selected %2…")
                                            .arg(selected_count)
                                            .arg(selected_count == 1 ? QStringLiteral("file")
                                                                     : QStringLiteral("files")));
                }
                updateFieldButtons();
                updateEditValuesButton();
                updateTransformationButton();
            });
    scheduleOutputLayoutExample();
    if (grid_model_->rowCount() > 0) {
        file_list_->selectionModel()->setCurrentIndex(grid_model_->index(0, 0),
                                                      QItemSelectionModel::NoUpdate);
        file_list_->selectAll();
        updateSelectionProjection();
    }
    if (aggregate_model_->rowCount() > 0) {
        fields_->setCurrentIndex(aggregate_model_->index(0, 2));
    }
    updateEditValuesButton();
    updateFieldButtons();
    updateTransformationButton();
    stageAutomaticTransformations();
    updateTechnicalSummary();

    root_layout_->removeWidget(loading_);
    loading_->deleteLater();
    loading_ = nullptr;
}

void MetadataPropertiesDialog::scheduleSelectionProjection() {
    if (selection_debounce_ != nullptr) {
        selection_debounce_->start();
    }
}

void MetadataPropertiesDialog::updateSelectionProjection() {
    if (file_list_ == nullptr || aggregate_model_ == nullptr ||
        file_list_->selectionModel() == nullptr) {
        return;
    }

    auto selected_items = selectedItemIndexes();
    selected_item_count_ = selected_items.size();
    selection_summary_ = QStringLiteral("%1 of %2 files selected · %3 %4 · %5 fields")
                             .arg(selected_item_count_)
                             .arg(loaded_item_count_)
                             .arg(loaded_source_count_)
                             .arg(pluralized(loaded_source_count_, QStringLiteral("source"),
                                             QStringLiteral("sources")))
                             .arg(loaded_field_count_);
    updateDraftState(draft_count_, undo_button_->isEnabled(), redo_button_->isEnabled());
    updateArtworkScope(selected_items);
    aggregate_model_->setSelectedItems(std::move(selected_items));
    updateTransformationButton();
    scheduleOutputLayoutExample();
}

void MetadataPropertiesDialog::updateArtworkScope(
    const std::span<const std::size_t> selected_items) {
    if (artwork_section_ == nullptr || grid_model_ == nullptr) {
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
    artwork_section_->setScope(std::move(scope), source_limit_exceeded);

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
    artwork_section_->setCoverArtRelease(
        release_consistent && release_column ? std::move(release_id) : std::nullopt);
}

core::Result<QString> MetadataPropertiesDialog::storeCoverArtImage(const QString& release_id,
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

void MetadataPropertiesDialog::updateDraftState(const int patch_count, const bool can_undo,
                                                const bool can_redo) {
    draft_count_ = patch_count;
    summary_->setText(patch_count == 0 ? selection_summary_
                                       : QStringLiteral("%1 · %2 staged %3")
                                             .arg(selection_summary_)
                                             .arg(patch_count)
                                             .arg(patch_count == 1 ? QStringLiteral("change")
                                                                   : QStringLiteral("changes")));
    if (!sticky_status_.isEmpty()) {
        read_only_->setTextFormat(
            sticky_status_.contains(QStringLiteral("<a href")) ? Qt::RichText : Qt::PlainText);
        read_only_->setText(sticky_status_);
    } else if (artwork_section_ && artwork_section_->hasPendingChanges()) {
        read_only_->setTextFormat(Qt::PlainText);
        read_only_->setText(
            QStringLiteral("Artwork changes pending · Apply saves tags and covers together"));
    } else {
        read_only_->setTextFormat(Qt::PlainText);
        read_only_->setText(
            save_tags_check_ != nullptr && !save_tags_check_->isChecked()
                ? QStringLiteral(
                      "Save tags is off · tag edits stay in the draft and Rename/Move uses the "
                      "file's current tags")
                : (patch_count == 0
                       ? QStringLiteral("No pending edits")
                       : QStringLiteral("Draft only · nothing is written until you apply")));
    }
    undo_button_->setEnabled(can_undo);
    redo_button_->setEnabled(can_redo);
    discard_button_->setEnabled(patch_count > 0);
    updateFieldButtons();
    updateTransformationButton();
    updateWritePlanButton();
}

void MetadataPropertiesDialog::updateFieldButtons() {
    if (add_field_button_ == nullptr || remove_field_button_ == nullptr) {
        return;
    }
    const auto selection_ready = aggregate_model_ != nullptr && aggregate_model_->summaryReady() &&
                                 aggregate_model_->selectedItemCount() > 0U;
    add_field_button_->setEnabled(selection_ready && field_name_dialog_ == nullptr);
    const auto has_fields = fields_ != nullptr && fields_->selectionModel() != nullptr &&
                            !fields_->selectionModel()->selectedRows(0).empty();
    remove_field_button_->setEnabled(selection_ready && has_fields);
}

void MetadataPropertiesDialog::updateEditValuesButton() {
    if (edit_values_button_ == nullptr) {
        return;
    }
    const auto enabled = exact_values_dialog_ == nullptr && aggregate_model_ != nullptr &&
                         aggregate_model_->summaryReady() &&
                         aggregate_model_->selectedItemCount() > 0U && fields_ != nullptr &&
                         fields_->currentIndex().isValid();
    edit_values_button_->setEnabled(enabled);
}

void MetadataPropertiesDialog::updateTransformationButton() {
    if (transform_button_ == nullptr) {
        return;
    }
    const auto enabled = transformation_dialog_ == nullptr && grid_model_ != nullptr &&
                         aggregate_model_ != nullptr && aggregate_model_->summaryReady() &&
                         aggregate_model_->selectedItemCount() > 0U &&
                         exact_values_dialog_ == nullptr && field_name_dialog_ == nullptr &&
                         !write_plan_running_ && !apply_running_ && !artwork_operation_running_;
    transform_button_->setEnabled(enabled);
    if (suggest_button_ != nullptr) {
        suggest_button_->setEnabled(enabled && !proposal_running_);
    }
    if (suggest_action_ != nullptr) {
        suggest_action_->setEnabled(enabled && !proposal_running_);
    }
    if (identify_button_ != nullptr) {
        identify_button_->setEnabled(enabled && !proposal_running_ &&
                                     static_cast<bool>(musicbrainz_.fetch) &&
                                     identify_dialog_ == nullptr);
    }
    if (replaygain_scan_button_ != nullptr) {
        replaygain_scan_button_->setEnabled(enabled && !proposal_running_ && !replaygain_running_);
        if (replaygain_provenance_button_ != nullptr) {
            replaygain_provenance_button_->setEnabled(grid_model_ != nullptr);
        }
    }
    if (transformation_list_ != nullptr) {
        transformation_list_->setEnabled(!transformation_catalog_loading_ &&
                                         !transformation_catalog_.empty() && enabled);
        transform_button_->setText(transformation_list_->currentItem() == nullptr
                                       ? QStringLiteral("Open script editor…")
                                       : QStringLiteral("Edit selected script…"));
    }
}

void MetadataPropertiesDialog::loadTransformationCatalog(
    const std::optional<core::StableId> selected) {
    if (!transformation_store_.load) {
        transformation_catalog_.clear();
        transformation_catalog_loading_ = false;
        rebuildTransformationCatalogControls();
        return;
    }
    transformation_catalog_loading_ = true;
    updateTransformationButton();
    const QPointer<MetadataPropertiesDialog> self{this};
    transformation_store_.load(
        [self, selected](std::vector<persistence::SavedMetadataTransformationChain> chains,
                         QString error) mutable {
            if (!self) {
                return;
            }
            self->transformation_catalog_loading_ = false;
            if (!error.isEmpty()) {
                self->transformation_catalog_.clear();
                self->rebuildTransformationCatalogControls();
                self->read_only_->setText(
                    QStringLiteral("Could not load saved transformations · %1").arg(error));
                return;
            }
            self->transformation_catalog_ = std::move(chains);
            self->rebuildTransformationCatalogControls(selected);
            self->stageAutomaticTransformations();
        });
}

void MetadataPropertiesDialog::rebuildTransformationCatalogControls(
    const std::optional<core::StableId> selected) {
    std::ranges::sort(transformation_catalog_, [](const auto& left, const auto& right) {
        if (left.chain.name != right.chain.name) {
            return left.chain.name < right.chain.name;
        }
        return left.id.to_string() < right.id.to_string();
    });
    const QSignalBlocker blocker{transformation_list_};
    transformation_list_->clear();
    QListWidgetItem* selected_item = nullptr;
    std::size_t automatic_count = 0U;
    for (const auto& entry : transformation_catalog_) {
        const auto name = display_utf8(entry.chain.name);
        const auto id = QString::fromStdString(entry.id.to_string());
        auto* item = new QListWidgetItem(name, transformation_list_);
        item->setData(Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(entry.automatic ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(entry.automatic
                             ? QStringLiteral("Staged automatically as colored draft edits")
                             : QStringLiteral("Not staged automatically"));
        if (selected && entry.id == *selected) {
            selected_item = item;
        }
        automatic_count += entry.automatic ? 1U : 0U;
    }
    if (selected_item != nullptr) {
        transformation_list_->setCurrentItem(selected_item);
    } else if (transformation_list_->count() > 0) {
        transformation_list_->setCurrentRow(0);
    }
    const auto has_saved_scripts = !transformation_catalog_.empty();
    transformation_status_->setVisible(has_saved_scripts);
    transformation_status_->setText(
        has_saved_scripts ? QStringLiteral("%1 of %2 checked · run in the order shown")
                                .arg(automatic_count)
                                .arg(transformation_catalog_.size())
                          : QString{});
    updateTransformationButton();
    updateWritePlanButton();
}

void MetadataPropertiesDialog::toggleAutomaticTransformation(const core::StableId id,
                                                             const bool enabled) {
    if (transformation_catalog_loading_ || !transformation_store_.save) {
        rebuildTransformationCatalogControls(id);
        return;
    }
    const auto found = std::ranges::find(transformation_catalog_, id,
                                         &persistence::SavedMetadataTransformationChain::id);
    if (found == transformation_catalog_.end() || found->automatic == enabled) {
        return;
    }
    auto updated = *found;
    updated.automatic = enabled;
    auto retained_update = updated;
    transformation_catalog_loading_ = true;
    updateTransformationButton();
    const QPointer<MetadataPropertiesDialog> self{this};
    transformation_store_.save(std::move(updated), [self, updated = std::move(retained_update)](
                                                       QString error) mutable {
        if (!self) {
            return;
        }
        self->transformation_catalog_loading_ = false;
        if (!error.isEmpty()) {
            self->rebuildTransformationCatalogControls(updated.id);
            self->read_only_->setText(
                QStringLiteral("Could not update automatic transformation · %1").arg(error));
            return;
        }
        const auto retained = std::ranges::find(self->transformation_catalog_, updated.id,
                                                &persistence::SavedMetadataTransformationChain::id);
        if (retained != self->transformation_catalog_.end()) {
            *retained = updated;
        }
        self->rebuildTransformationCatalogControls(updated.id);
        self->read_only_->setText(
            QStringLiteral("%1 will %2stage its edits automatically.")
                .arg(display_utf8(updated.chain.name),
                     updated.automatic ? QString{} : QStringLiteral("no longer ")));
        if (updated.automatic) {
            self->stageAutomaticTransformations();
        }
    });
}

void MetadataPropertiesDialog::loadOutputProfiles() {
    output_profiles_loading_ = true;
    updateOutputProfileButtons();
    if (!output_profile_store_.load) {
        output_profiles_loading_ = false;
        output_profile_status_->setText(
            QStringLiteral("Output-profile persistence is unavailable"));
        rebuildOutputProfileControls();
        return;
    }
    const QPointer self{this};
    output_profile_store_.load(
        [self](std::vector<persistence::SavedOutputLayoutProfile> layouts,
               std::vector<persistence::SavedDestinationProfile> destinations,
               QString error) mutable {
            if (!self) {
                return;
            }
            self->output_profiles_loading_ = false;
            if (!error.isEmpty()) {
                self->output_profile_status_->setText(
                    QStringLiteral("Could not load output profiles · %1").arg(error));
                self->rebuildOutputProfileControls();
                return;
            }
            self->output_layout_catalog_ = std::move(layouts);
            self->destination_catalog_ = std::move(destinations);
            self->rebuildOutputProfileControls(self->editing_output_layout_id_,
                                               self->editing_destination_id_);
            self->output_profile_status_->setText(
                QStringLiteral("%1 naming %2 · %3 move %4")
                    .arg(self->output_layout_catalog_.size())
                    .arg(self->output_layout_catalog_.size() == 1U ? QStringLiteral("layout")
                                                                   : QStringLiteral("layouts"))
                    .arg(self->destination_catalog_.size())
                    .arg(self->destination_catalog_.size() == 1U ? QStringLiteral("destination")
                                                                 : QStringLiteral("destinations")));
        });
}

void MetadataPropertiesDialog::rebuildOutputProfileControls(
    const std::optional<core::StableId> selected_layout,
    const std::optional<core::StableId> selected_destination) {
    const QSignalBlocker layout_blocker{output_layout_combo_};
    const QSignalBlocker destination_blocker{destination_combo_};
    output_layout_combo_->clear();
    for (const auto& saved : output_layout_catalog_) {
        output_layout_combo_->addItem(display_utf8(saved.profile.name),
                                      QString::fromStdString(saved.id.to_string()));
    }
    destination_combo_->clear();
    for (const auto& saved : destination_catalog_) {
        destination_combo_->addItem(display_utf8(saved.profile.name),
                                    QString::fromStdString(saved.id.to_string()));
    }
    const auto select_id = [](QComboBox* combo, const std::optional<core::StableId>& id) {
        if (!id) {
            return combo->count() > 0 ? 0 : -1;
        }
        return combo->findData(QString::fromStdString(id->to_string()));
    };
    const auto layout_index = select_id(output_layout_combo_, selected_layout);
    const auto destination_index = select_id(destination_combo_, selected_destination);
    output_layout_combo_->setCurrentIndex(layout_index);
    destination_combo_->setCurrentIndex(destination_index);
    selectOutputLayout(layout_index);
    selectDestination(destination_index);
    updateOutputProfileButtons();
}

void MetadataPropertiesDialog::selectOutputLayout(const int index) {
    if (index < 0 || index >= output_layout_combo_->count()) {
        editing_output_layout_id_.reset();
        output_layout_name_->clear();
        output_directory_expression_->clear();
        output_basename_expression_->clear();
        output_sanitization_policy_->setCurrentIndex(0);
        updateOutputProfileButtons();
        return;
    }
    const auto id =
        core::StableId::parse(output_layout_combo_->itemData(index).toString().toStdString());
    const auto found = id ? std::ranges::find(output_layout_catalog_, *id,
                                              &persistence::SavedOutputLayoutProfile::id)
                          : output_layout_catalog_.end();
    if (found == output_layout_catalog_.end()) {
        newOutputLayout();
        return;
    }
    editing_output_layout_id_ = found->id;
    output_layout_name_->setText(display_utf8(found->profile.name));
    output_directory_expression_->setText(
        display_utf8(found->profile.relative_directory_expression));
    output_basename_expression_->setText(display_utf8(found->profile.basename_expression));
    output_sanitization_policy_->setCurrentIndex(
        std::max(0, output_sanitization_policy_->findData(
                        display_utf8(found->profile.sanitization_policy.name))));
    updateOutputProfileButtons();
}

void MetadataPropertiesDialog::selectDestination(const int index) {
    if (index < 0 || index >= destination_combo_->count()) {
        editing_destination_id_.reset();
        destination_root_raw_path_.clear();
        destination_name_->clear();
        destination_root_->clear();
        updateOutputProfileButtons();
        return;
    }
    const auto id =
        core::StableId::parse(destination_combo_->itemData(index).toString().toStdString());
    const auto found =
        id ? std::ranges::find(destination_catalog_, *id, &persistence::SavedDestinationProfile::id)
           : destination_catalog_.end();
    if (found == destination_catalog_.end()) {
        newDestination();
        return;
    }
    editing_destination_id_ = found->id;
    destination_root_raw_path_ = found->profile.root_raw_path;
    destination_name_->setText(display_utf8(found->profile.name));
    destination_root_->setText(
        QString::fromStdString(core::escape_raw_path(destination_root_raw_path_)));
    updateOutputProfileButtons();
}

void MetadataPropertiesDialog::newOutputLayout() {
    editing_output_layout_id_.reset();
    output_layout_combo_->setCurrentIndex(-1);
    output_layout_name_->clear();
    output_directory_expression_->clear();
    output_basename_expression_->clear();
    output_sanitization_policy_->setCurrentIndex(0);
    output_layout_name_->setFocus();
    output_profile_status_->setText(
        QStringLiteral("Define a reusable relative folder and filename convention"));
    updateOutputProfileButtons();
}

void MetadataPropertiesDialog::newDestination() {
    editing_destination_id_.reset();
    destination_root_raw_path_.clear();
    destination_combo_->setCurrentIndex(-1);
    destination_name_->clear();
    destination_root_->clear();
    destination_name_->setFocus();
    output_profile_status_->setText(QStringLiteral("Define a named absolute root for Move files"));
    updateOutputProfileButtons();
}

void MetadataPropertiesDialog::saveOutputLayout() {
    if (!output_profile_store_.save_layout || output_profile_mutation_running_) {
        return;
    }
    persistence::SavedOutputLayoutProfile saved{
        .id = editing_output_layout_id_.value_or(core::StableId::random()),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = encode_utf8(output_layout_name_->text()),
                .dialect = {},
                .relative_directory_expression = encode_utf8(output_directory_expression_->text()),
                .basename_expression = encode_utf8(output_basename_expression_->text()),
                .sanitization_policy = {encode_utf8(
                                            output_sanitization_policy_->currentData().toString()),
                                        1U},
            },
    };
    if (auto valid = operations::validate_output_layout_profile(saved.profile); !valid) {
        output_profile_status_->setText(QStringLiteral("Naming layout is not valid · %1")
                                            .arg(display_utf8(valid.error().message)));
        return;
    }
    output_profile_mutation_running_ = true;
    updateOutputProfileButtons();
    const QPointer self{this};
    auto retained_saved = saved;
    output_profile_store_.save_layout(
        std::move(saved), [self, saved = std::move(retained_saved)](QString error) mutable {
            if (!self) {
                return;
            }
            self->output_profile_mutation_running_ = false;
            if (!error.isEmpty()) {
                self->output_profile_status_->setText(
                    QStringLiteral("Could not save naming layout · %1").arg(error));
                self->updateOutputProfileButtons();
                return;
            }
            const auto found = std::ranges::find(self->output_layout_catalog_, saved.id,
                                                 &persistence::SavedOutputLayoutProfile::id);
            if (found == self->output_layout_catalog_.end()) {
                self->output_layout_catalog_.push_back(saved);
            } else {
                *found = saved;
            }
            std::ranges::sort(self->output_layout_catalog_, {},
                              [](const auto& profile) { return profile.profile.name; });
            self->editing_output_layout_id_ = saved.id;
            self->rebuildOutputProfileControls(saved.id, self->editing_destination_id_);
            self->output_profile_status_->setText(QStringLiteral("Naming layout saved"));
        });
}

void MetadataPropertiesDialog::saveDestination() {
    if (!output_profile_store_.save_destination || output_profile_mutation_running_) {
        return;
    }
    persistence::SavedDestinationProfile saved{
        .id = editing_destination_id_.value_or(core::StableId::random()),
        .profile =
            operations::DestinationProfile{
                .schema_version = 1U,
                .name = encode_utf8(destination_name_->text()),
                .root_raw_path = destination_root_raw_path_,
                .containment_policy = {"lexical-beneath-root", 1U},
            },
    };
    if (auto valid = operations::validate_destination_profile(saved.profile); !valid) {
        output_profile_status_->setText(QStringLiteral("Move destination is not valid · %1")
                                            .arg(display_utf8(valid.error().message)));
        return;
    }
    output_profile_mutation_running_ = true;
    updateOutputProfileButtons();
    const QPointer self{this};
    auto retained_saved = saved;
    output_profile_store_.save_destination(
        std::move(saved), [self, saved = std::move(retained_saved)](QString error) mutable {
            if (!self) {
                return;
            }
            self->output_profile_mutation_running_ = false;
            if (!error.isEmpty()) {
                self->output_profile_status_->setText(
                    QStringLiteral("Could not save move destination · %1").arg(error));
                self->updateOutputProfileButtons();
                return;
            }
            const auto found = std::ranges::find(self->destination_catalog_, saved.id,
                                                 &persistence::SavedDestinationProfile::id);
            if (found == self->destination_catalog_.end()) {
                self->destination_catalog_.push_back(saved);
            } else {
                *found = saved;
            }
            std::ranges::sort(self->destination_catalog_, {},
                              [](const auto& profile) { return profile.profile.name; });
            self->editing_destination_id_ = saved.id;
            self->rebuildOutputProfileControls(self->editing_output_layout_id_, saved.id);
            self->output_profile_status_->setText(QStringLiteral("Move destination saved"));
        });
}

void MetadataPropertiesDialog::removeOutputLayout() {
    if (!editing_output_layout_id_ || !output_profile_store_.remove_layout ||
        output_profile_mutation_running_) {
        return;
    }
    const auto id = *editing_output_layout_id_;
    output_profile_mutation_running_ = true;
    updateOutputProfileButtons();
    const QPointer self{this};
    output_profile_store_.remove_layout(id, [self, id](QString error) {
        if (!self) {
            return;
        }
        self->output_profile_mutation_running_ = false;
        if (!error.isEmpty()) {
            self->output_profile_status_->setText(
                QStringLiteral("Could not remove naming layout · %1").arg(error));
            self->updateOutputProfileButtons();
            return;
        }
        std::erase_if(self->output_layout_catalog_,
                      [id](const auto& saved) { return saved.id == id; });
        self->editing_output_layout_id_.reset();
        self->rebuildOutputProfileControls({}, self->editing_destination_id_);
        self->output_profile_status_->setText(QStringLiteral("Naming layout removed"));
    });
}

void MetadataPropertiesDialog::removeDestination() {
    if (!editing_destination_id_ || !output_profile_store_.remove_destination ||
        output_profile_mutation_running_) {
        return;
    }
    const auto id = *editing_destination_id_;
    output_profile_mutation_running_ = true;
    updateOutputProfileButtons();
    const QPointer self{this};
    output_profile_store_.remove_destination(id, [self, id](QString error) {
        if (!self) {
            return;
        }
        self->output_profile_mutation_running_ = false;
        if (!error.isEmpty()) {
            self->output_profile_status_->setText(
                QStringLiteral("Could not remove move destination · %1").arg(error));
            self->updateOutputProfileButtons();
            return;
        }
        std::erase_if(self->destination_catalog_,
                      [id](const auto& saved) { return saved.id == id; });
        self->editing_destination_id_.reset();
        self->rebuildOutputProfileControls(self->editing_output_layout_id_, {});
        self->output_profile_status_->setText(QStringLiteral("Move destination removed"));
    });
}

void MetadataPropertiesDialog::updateOutputProfileButtons() {
    if (output_layout_save_button_ == nullptr) {
        return;
    }
    const auto available = !output_profiles_loading_ && !output_profile_mutation_running_;
    output_layout_combo_->setEnabled(available && !output_layout_catalog_.empty());
    destination_combo_->setEnabled(available && !destination_catalog_.empty());
    output_layout_name_->setEnabled(available);
    output_directory_expression_->setEnabled(available);
    output_basename_expression_->setEnabled(available);
    output_sanitization_policy_->setEnabled(available);
    destination_name_->setEnabled(available);
    destination_root_->setEnabled(available);
    destination_browse_button_->setEnabled(available && output_profile_store_.save_destination);
    output_layout_new_button_->setEnabled(available && output_profile_store_.save_layout);
    destination_new_button_->setEnabled(available && output_profile_store_.save_destination);
    output_layout_save_button_->setEnabled(available && output_profile_store_.save_layout &&
                                           !output_layout_name_->text().isEmpty() &&
                                           !output_basename_expression_->text().isEmpty());
    destination_save_button_->setEnabled(available && output_profile_store_.save_destination &&
                                         !destination_name_->text().isEmpty() &&
                                         !destination_root_raw_path_.empty());
    output_layout_remove_button_->setEnabled(available && output_profile_store_.remove_layout &&
                                             editing_output_layout_id_.has_value());
    destination_remove_button_->setEnabled(available && output_profile_store_.remove_destination &&
                                           editing_destination_id_.has_value());
    const auto layout_ready =
        editing_output_layout_id_.has_value() &&
        std::ranges::any_of(output_layout_catalog_, [this](const auto& entry) {
            return entry.id == *editing_output_layout_id_;
        });
    const auto destination_ready =
        editing_destination_id_.has_value() &&
        std::ranges::any_of(destination_catalog_, [this](const auto& entry) {
            return entry.id == *editing_destination_id_;
        });
    const auto publication_available = static_cast<bool>(file_plan_applier_factory_);
    rename_files_check_->setEnabled(available && layout_ready && publication_available);
    move_files_check_->setEnabled(available && layout_ready && destination_ready &&
                                  publication_available);
    rename_files_check_->setToolTip(
        publication_available
            ? (layout_ready ? QStringLiteral("Generate a new basename with the saved layout")
                            : QStringLiteral("Select a saved naming layout first"))
            : QStringLiteral("File publication is unavailable"));
    move_files_check_->setToolTip(
        publication_available
            ? (layout_ready && destination_ready
                   ? QStringLiteral("Move below the saved destination using the saved layout")
                   : QStringLiteral("Select a saved layout and destination first"))
            : QStringLiteral("File publication is unavailable"));
    if (!rename_files_check_->isEnabled() && rename_files_check_->isChecked()) {
        rename_files_check_->setChecked(false);
    }
    if (!move_files_check_->isEnabled() && move_files_check_->isChecked()) {
        move_files_check_->setChecked(false);
    }
}

void MetadataPropertiesDialog::scheduleOutputLayoutExample() {
    if (output_example_debounce_ == nullptr || output_layout_example_ == nullptr) {
        return;
    }
    ++output_example_generation_;
    output_example_cancellation_.request_cancellation();
    output_example_pending_ = false;
    output_example_debounce_->start();
}

void MetadataPropertiesDialog::startOutputLayoutExample() {
    if (output_example_running_) {
        output_example_pending_ = true;
        output_example_cancellation_.request_cancellation();
        return;
    }
    if (grid_model_ == nullptr) {
        output_layout_example_->setText(QStringLiteral("Preview: waiting for tracks…"));
        output_layout_preview_->clear();
        return;
    }
    // Selected tracks drive the preview; with no selection every track is
    // previewed so the layout can be judged before anything is applied.
    auto items = selectedItemIndexes();
    if (items.empty()) {
        items.reserve(grid_model_->selection().item_count());
        for (std::size_t item_index = 0U; item_index < grid_model_->selection().item_count();
             ++item_index) {
            items.push_back(item_index);
        }
    }
    if (items.empty()) {
        output_layout_example_->setText(QStringLiteral("Preview: waiting for tracks…"));
        output_layout_preview_->clear();
        return;
    }
    constexpr auto maximum_preview_items = std::size_t{100U};
    const auto item_count = items.size();
    const auto truncated = item_count > maximum_preview_items;
    if (truncated) {
        items.resize(maximum_preview_items);
    }

    auto selection = grid_model_->sharedSelection();
    auto draft =
        save_tags_check_->isChecked() ? grid_model_->patches() : metadata::StagedMetadataPatchSet{};
    auto layout = operations::OutputLayoutProfile{
        .schema_version = 1U,
        .name = "Live example",
        .dialect = {},
        .relative_directory_expression = encode_utf8(output_directory_expression_->text()),
        .basename_expression = encode_utf8(output_basename_expression_->text()),
        .sanitization_policy = {encode_utf8(output_sanitization_policy_->currentData().toString()),
                                1U},
    };
    output_example_job_generation_ = output_example_generation_;
    output_example_cancellation_ = core::CancellationSource{};
    const auto cancellation = output_example_cancellation_.token();
    output_example_running_ = true;
    output_layout_example_->setText(QStringLiteral("Preview: updating…"));

    output_example_watcher_.setFuture(QtConcurrent::run(
        [selection = std::move(selection), draft = std::move(draft), items = std::move(items),
         item_count, truncated, layout = std::move(layout), cancellation]() mutable {
            auto documents =
                metadata::materialize_metadata_draft(*selection, draft, items, cancellation);
            if (!documents) {
                return std::make_shared<OutputLayoutExampleResult>(
                    std::unexpected(std::move(documents.error())));
            }
            constexpr auto preview_root = "/trackknife-layout-example";
            std::vector<operations::OutputPathPlanningItem> planning_items;
            planning_items.reserve(items.size());
            for (std::size_t position = 0U; position < items.size(); ++position) {
                const auto item_index = items[position];
                const auto& source = selection->source(item_index);
                const auto revision = source.source_revision.value_or(core::LocalSourceRevision{
                    .device = 1U,
                    .inode = 1U,
                    .size = 1U,
                    .modification_time_seconds = 0,
                    .modification_time_nanoseconds = 0,
                });
                planning_items.push_back(operations::OutputPathPlanningItem{
                    .item_index = item_index,
                    .source_raw_path = source.raw_path,
                    .source_revision = revision,
                    .final_metadata = std::move((*documents)[position]),
                });
            }
            auto planned = operations::plan_output_paths(
                planning_items, {.rename_files = true, .move_files = true}, std::move(layout),
                operations::DestinationProfile{
                    .schema_version = 1U,
                    .name = "Live example",
                    .root_raw_path = preview_root,
                    .containment_policy = {"lexical-beneath-root", 1U},
                },
                {}, cancellation);
            if (!planned) {
                return std::make_shared<OutputLayoutExampleResult>(
                    std::unexpected(std::move(planned.error())));
            }
            const auto blocking = std::ranges::find_if(
                planned->issues, [](const auto& issue) { return issue.blocking; });
            if (blocking != planned->issues.end()) {
                return std::make_shared<OutputLayoutExampleResult>(std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = blocking->message,
                    .context = {},
                }));
            }
            OutputLayoutPreview preview{
                .rows = {},
                .item_count = item_count,
                .truncated = truncated,
            };
            preview.rows.reserve(planned->sources.size());
            constexpr std::string_view prefix = "/trackknife-layout-example/";
            for (const auto& planned_source : planned->sources) {
                auto relative_path = planned_source.target_raw_path;
                if (!relative_path.starts_with(prefix)) {
                    return std::make_shared<OutputLayoutExampleResult>(std::unexpected(core::Error{
                        .code = core::ErrorCode::invariant,
                        .message = "Naming layout preview escaped its preview root",
                        .context = {},
                    }));
                }
                relative_path.erase(0U, prefix.size());
                preview.rows.push_back(OutputLayoutPreviewRow{
                    .source_raw_path = planned_source.source_raw_path,
                    .target_relative_path = std::move(relative_path),
                });
            }
            return std::make_shared<OutputLayoutExampleResult>(std::move(preview));
        }));
}

void MetadataPropertiesDialog::finishOutputLayoutExample() {
    output_example_running_ = false;
    const auto result = output_example_watcher_.result();
    if (output_example_job_generation_ == output_example_generation_ && result) {
        if (*result) {
            const auto& preview = **result;
            output_layout_preview_->clear();
            for (const auto& row : preview.rows) {
                const auto source =
                    QString::fromStdString(core::escape_raw_path(row.source_raw_path));
                const auto target =
                    QString::fromStdString(core::escape_raw_path(row.target_relative_path));
                auto* item = new QTreeWidgetItem(output_layout_preview_,
                                                 {source.section(QChar{'/'}, -1), target});
                item->setToolTip(0, source);
                item->setToolTip(1, target);
            }
            output_layout_example_->setText(
                preview.truncated
                    ? QStringLiteral("Preview: first %1 of %2 tracks")
                          .arg(preview.rows.size())
                          .arg(preview.item_count)
                    : QStringLiteral("Preview: %1 %2")
                          .arg(preview.rows.size())
                          .arg(pluralized(preview.rows.size(), QStringLiteral("track"),
                                          QStringLiteral("tracks"))));
        } else if (result->error().code != core::ErrorCode::cancelled) {
            output_layout_preview_->clear();
            output_layout_example_->setText(
                QStringLiteral("Preview error: %1").arg(display_utf8(result->error().message)));
        }
    }
    if (output_example_pending_) {
        output_example_pending_ = false;
        startOutputLayoutExample();
    }
}

void MetadataPropertiesDialog::updateWritePlanButton() {
    if (apply_plan_button_ == nullptr) {
        return;
    }
    const auto has_metadata_effect = save_tags_check_->isChecked() && draft_count_ > 0;
    const auto has_path_effect = rename_files_check_->isChecked() || move_files_check_->isChecked();
    apply_plan_button_->setEnabled(grid_model_ != nullptr &&
                                   (has_metadata_effect || has_path_effect ||
                                    (artwork_section_ && artwork_section_->hasPendingChanges())) &&
                                   !transformation_catalog_loading_ && !write_plan_running_ &&
                                   !apply_running_ && !artwork_operation_running_);
    updateTransformationButton();
}

void MetadataPropertiesDialog::invalidateWritePlan() {
    ++write_plan_generation_;
    write_plan_cancellation_.request_cancellation();
    write_plan_cancellation_ = core::CancellationSource{};
    updateWritePlanButton();
}

void MetadataPropertiesDialog::startProposals() {
    if (grid_model_ == nullptr || proposal_running_ || write_plan_running_ || apply_running_) {
        return;
    }
    auto items = selectedItemIndexes();
    if (items.empty()) {
        items.reserve(grid_model_->selection().item_count());
        for (std::size_t item_index = 0U; item_index < grid_model_->selection().item_count();
             ++item_index) {
            items.push_back(item_index);
        }
    }
    if (items.size() < 2U) {
        read_only_->setText(
            QStringLiteral("Suggestions need at least two files that share an album"));
        return;
    }
    proposal_running_ = true;
    updateTransformationButton();
    read_only_->setText(
        QStringLiteral("Looking for suggestions across %1 files…").arg(items.size()));
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

void MetadataPropertiesDialog::finishProposals() {
    proposal_running_ = false;
    updateTransformationButton();
    const auto result = proposal_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The suggestion task returned no result");
        read_only_->setText(QStringLiteral("No suggestions · %1").arg(message));
        return;
    }
    const auto& preview = **result;
    if (preview.cells.empty()) {
        read_only_->setText(QStringLiteral("No suggestions · the selected files already agree"));
        return;
    }
    if (grid_model_ == nullptr || !stageTransformationPreservingSelection(
                                      preview, QStringList{display_utf8(preview.chain.name)})) {
        return;
    }
    loaded_field_count_ = grid_model_->selection().field_count();
    updateSelectionProjection();
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

// Picard runs tagging scripts the moment new metadata arrives and saves
// exactly what it displays. Trackknife goes one step further per the
// user's model: automatic scripts also stage over plain local baselines,
// so every write is what the grid shows — never a hidden apply-time pass.
void MetadataPropertiesDialog::stageAutomaticTransformations() {
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

void MetadataPropertiesDialog::finishAutomaticStage() {
    automatic_stage_running_ = false;
    const auto result = automatic_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The script task returned no result");
        read_only_->setText(QStringLiteral("Automatic scripts staged nothing · %1").arg(message));
        return;
    }
    const auto& preview = **result;
    if (preview.cells.empty()) {
        return;
    }
    if (grid_model_ == nullptr ||
        !stageTransformationPreservingSelection(preview, automatic_step_sources_)) {
        return;
    }
    loaded_field_count_ = grid_model_->selection().field_count();
    updateSelectionProjection();
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

std::optional<MetadataPropertiesDialog::AutomaticChainPlan>
MetadataPropertiesDialog::combinedAutomaticChain() const {
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
            read_only_->setText(
                QStringLiteral("Automatic scripts exceed the %1-step combined limit; disable or "
                               "shorten a script.")
                    .arg(limits.actions));
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

namespace {

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

void MetadataPropertiesDialog::startIdentify() {
    if (grid_model_ == nullptr || proposal_running_ || identify_dialog_ != nullptr ||
        !musicbrainz_.fetch) {
        return;
    }
    auto items = selectedItemIndexes();
    if (items.empty()) {
        items.reserve(grid_model_->selection().item_count());
        for (std::size_t item_index = 0U; item_index < grid_model_->selection().item_count();
             ++item_index) {
            items.push_back(item_index);
        }
    }
    if (items.empty()) {
        return;
    }
    const auto& selection = grid_model_->selection();
    std::vector<musicbrainz::LocalTrackDescriptor> descriptors;
    std::vector<QString> local_paths;
    descriptors.reserve(items.size());
    local_paths.reserve(items.size());
    QString initial_artist;
    QString initial_release;
    for (const auto item_index : items) {
        const auto& source = selection.source(item_index);
        const auto& baseline = source.baseline;
        local_paths.push_back(QFile::decodeName(
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
        if (initial_release.isEmpty() && !descriptor.album.empty()) {
            initial_release = display_utf8(descriptor.album);
        }
        if (initial_artist.isEmpty()) {
            const auto album_artist = baseline.first_effective_value("albumartist");
            initial_artist = display_utf8(
                album_artist && !album_artist->empty() ? *album_artist : descriptor.artist);
        }
        descriptors.push_back(std::move(descriptor));
    }
    openIdentifyDialog(std::move(descriptors), std::move(local_paths), std::move(items),
                       initial_artist, initial_release);
}

void MetadataPropertiesDialog::openIdentifyDialog(
    std::vector<musicbrainz::LocalTrackDescriptor> descriptors, std::vector<QString> local_paths,
    std::vector<std::size_t> items, QString initial_artist, QString initial_release) {
    auto* dialog = createMusicBrainzIdentifyDialog(
        musicbrainz_, std::move(descriptors), std::move(local_paths), std::move(items),
        initial_artist, initial_release,
        [this](metadata::MetadataProposalSet proposals) {
            applyMusicBrainzProposals(std::move(proposals));
        },
        this);
    identify_dialog_ = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (identify_dialog_ == dialog) {
            identify_dialog_ = nullptr;
        }
        updateTransformationButton();
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    updateTransformationButton();
}

// The M7 scan (ADR-0100): measure the selection on the bounded parallel
// graph and stage REPLAYGAIN_* values as ordinary colored draft edits — the
// grid is the write, exactly like every provider.
// Staging that introduces new fields inserts grid columns, and inserted
// columns are not part of the existing row selection — Qt then reports the
// rows as no longer fully selected and the fields pane projects an empty
// selection. Re-selecting the same rows keeps the view truthful.
// Event outcomes (staged results, unavailable Apply) must survive the
// asynchronous selection-projection refreshes that rewrite the footer;
// they stay until the next real draft change clears them.
void MetadataPropertiesDialog::showStickyStatus(const QString& text) {
    sticky_status_ = text;
    read_only_->setTextFormat(text.contains(QStringLiteral("<a href")) ? Qt::RichText
                                                                       : Qt::PlainText);
    read_only_->setText(text);
}

bool MetadataPropertiesDialog::stageTransformationPreservingSelection(
    const metadata::MetadataTransformationPreview& preview, const QStringList& step_sources) {
    if (grid_model_ == nullptr) {
        return false;
    }
    QList<int> selected_rows;
    if (file_list_ != nullptr && file_list_->selectionModel() != nullptr) {
        const auto rows = file_list_->selectionModel()->selectedRows();
        selected_rows.reserve(rows.size());
        for (const auto& row : rows) {
            selected_rows.push_back(row.row());
        }
    }
    const auto columns_before = grid_model_->columnCount();
    if (!grid_model_->stageTransformation(preview, step_sources)) {
        return false;
    }
    if (grid_model_->columnCount() != columns_before && file_list_ != nullptr &&
        file_list_->selectionModel() != nullptr && !selected_rows.isEmpty()) {
        QItemSelection restored;
        const auto last_column = grid_model_->columnCount() - 1;
        for (const auto row : selected_rows) {
            restored.select(grid_model_->index(row, 0), grid_model_->index(row, last_column));
        }
        file_list_->selectionModel()->select(restored, QItemSelectionModel::ClearAndSelect);
    }
    return true;
}

void MetadataPropertiesDialog::startReplayGainScan(std::vector<std::size_t> forced_items) {
    if (grid_model_ == nullptr || replaygain_running_ || proposal_running_ || apply_running_ ||
        write_plan_running_) {
        return;
    }
    auto items = std::move(forced_items);
    if (items.empty()) {
        items = selectedItemIndexes();
    }
    if (items.empty()) {
        items.reserve(grid_model_->selection().item_count());
        for (std::size_t item_index = 0U; item_index < grid_model_->selection().item_count();
             ++item_index) {
            items.push_back(item_index);
        }
    }
    if (items.empty()) {
        return;
    }
    loudness::LoudnessGrouping grouping;
    switch (replaygain_grouping_->currentIndex()) {
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
        grouping.expression = replaygain_expression_->text().trimmed().toStdString();
        if (grouping.expression.empty()) {
            read_only_->setText(
                QStringLiteral("Enter a tkfmt-1 grouping expression, e.g. %album%"));
            return;
        }
        break;
    }
    replaygain_retry_items_.clear();

    replaygain_running_ = true;
    replaygain_cancellation_ = core::CancellationSource{};
    const auto cancellation = replaygain_cancellation_.token();
    updateTransformationButton();
    auto completed = std::make_shared<std::atomic_size_t>(0U);
    const auto total = items.size();
    auto* progress_timer = new QTimer(this);
    progress_timer->setInterval(100);
    connect(progress_timer, &QTimer::timeout, this, [this, completed, total] {
        read_only_->setTextFormat(Qt::RichText);
        read_only_->setText(QStringLiteral("Measuring loudness · %1 of %2 files · "
                                           "<a href=\"cancel-replaygain\">Stop</a>")
                                .arg(completed->load())
                                .arg(total));
    });
    connect(&replaygain_watcher_, &QFutureWatcherBase::finished, progress_timer,
            &QObject::deleteLater);
    progress_timer->start();
    read_only_->setTextFormat(Qt::RichText);
    read_only_->setText(QStringLiteral("Measuring loudness · 0 of %1 files · "
                                       "<a href=\"cancel-replaygain\">Stop</a>")
                            .arg(total));

    auto selection = grid_model_->sharedSelection();
    auto draft = grid_model_->patches();
    const std::shared_ptr<const std::vector<MetadataPropertiesAudioSource>> audio_sources{
        audio_sources_};
    // ADR-0148/0149: captured before the worker starts; widgets stay on
    // the UI thread.
    const bool true_peak = replaygain_true_peak_ != nullptr && replaygain_true_peak_->isChecked();
    const bool sidecar_only =
        replaygain_sidecar_only_ != nullptr && replaygain_sidecar_only_->isChecked();
    ReplayGainScanSettings settings;
    settings.grouping = std::move(grouping);
    settings.true_peak = true_peak;
    settings.sidecar_only = sidecar_only;
    replaygain_watcher_.setFuture(QtConcurrent::run(
        [selection = std::move(selection), draft = std::move(draft), items = std::move(items),
         audio_sources, settings = std::move(settings), completed, cancellation] {
            return run_replaygain_scan(selection, draft, items, audio_sources, settings, completed,
                                       cancellation);
        }));
}

void MetadataPropertiesDialog::finishReplayGainScan() {
    replaygain_running_ = false;
    updateTransformationButton();
    const auto outcome = replaygain_watcher_.result();
    read_only_->setTextFormat(Qt::PlainText);
    if (!outcome || !outcome->proposals) {
        const auto message = outcome ? display_utf8(outcome->proposals.error().message)
                                     : QStringLiteral("The loudness scan returned no result");
        read_only_->setText(QStringLiteral("No ReplayGain values staged · %1").arg(message));
        return;
    }
    replaygain_retry_items_ = outcome->retry_items;
    replaygain_export_rows_ = outcome->export_rows;
    if (!outcome->problems.empty()) {
        showPreparationFeedback(
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
    applyMusicBrainzProposals(std::move(*outcome->proposals));
}

QString MetadataPropertiesDialog::replayGainStatusLinks() const {
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

void MetadataPropertiesDialog::exportReplayGainResults() {
    if (replaygain_export_rows_.isEmpty()) {
        return;
    }
    // Test seam: an explicit path skips the file dialog.
    auto path = property("trackknife-replaygain-export-path").toString();
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Export ReplayGain results"),
                                            QStringLiteral("replaygain-results.csv"),
                                            QStringLiteral("CSV files (*.csv)"));
    }
    if (path.isEmpty()) {
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

void MetadataPropertiesDialog::showLoudnessProvenance() {
    if (grid_model_ == nullptr) {
        return;
    }
    auto items = selectedItemIndexes();
    if (items.empty()) {
        items.reserve(grid_model_->selection().item_count());
        for (std::size_t item_index = 0U; item_index < grid_model_->selection().item_count();
             ++item_index) {
            items.push_back(item_index);
        }
    }
    if (items.empty()) {
        return;
    }
    const auto& selection = grid_model_->selection();
    const auto patches = grid_model_->patches();
    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> loudness_fields{{
        {"replaygaintrackgain", "Track gain"},
        {"replaygaintrackpeak", "Track peak"},
        {"replaygainalbumgain", "Album gain"},
        {"replaygainalbumpeak", "Album peak"},
        {"r128trackgain", "R128 track"},
        {"r128albumgain", "R128 album"},
    }};

    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("bench-replaygain-provenance-dialog"));
    dialog->setWindowTitle(QStringLiteral("Loudness sources"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(dialog);
    auto* table = new QTableWidget(static_cast<int>(items.size()),
                                   static_cast<int>(loudness_fields.size()) + 1, dialog);
    table->setObjectName(QStringLiteral("bench-replaygain-provenance-table"));
    QStringList headers{QStringLiteral("Track")};
    for (const auto& [canonical, header] : loudness_fields) {
        headers << QString::fromUtf8(header.data(), static_cast<qsizetype>(header.size()));
    }
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->verticalHeader()->hide();
    table->setWordWrap(false);
    for (int row = 0; row < static_cast<int>(items.size()); ++row) {
        const auto item_index = items[static_cast<std::size_t>(row)];
        table->setItem(row, 0,
                       new QTableWidgetItem(grid_model_->trackLabel(static_cast<int>(item_index))));
        for (int column = 0; column < static_cast<int>(loudness_fields.size()); ++column) {
            const auto& canonical = loudness_fields[static_cast<std::size_t>(column)].first;
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
            table->setItem(row, column + 1, new QTableWidgetItem(cell_text));
        }
    }
    table->resizeColumnsToContents();
    layout->addWidget(table);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->resize(640, 320);
    dialog->show();
}

void MetadataPropertiesDialog::applyMusicBrainzProposals(metadata::MetadataProposalSet proposals) {
    if (grid_model_ == nullptr || proposal_running_) {
        return;
    }
    proposal_running_ = true;
    updateTransformationButton();
    read_only_->setText(QStringLiteral("Matching the MusicBrainz release to the draft…"));
    auto selection = grid_model_->sharedSelection();
    auto draft = grid_model_->patches();
    proposal_watcher_.setFuture(QtConcurrent::run(
        [selection = std::move(selection), draft = std::move(draft), set = std::move(proposals)] {
            using PreviewResult = core::Result<metadata::MetadataTransformationPreview>;
            return std::make_shared<PreviewResult>(
                metadata::metadata_proposal_preview(*selection, draft, set, 0.5));
        }));
}

void MetadataPropertiesDialog::startWritePlan() {
    const auto artwork_intents = artwork_section_ ? artwork_section_->pendingIntents()
                                                  : std::vector<metadata::ArtworkWritePlanIntent>{};
    const operations::PreparationOperationSelection operation_selection{
        .save_tags = save_tags_check_->isChecked() || !artwork_intents.empty(),
        .rename_files = rename_files_check_->isChecked(),
        .move_files = move_files_check_->isChecked(),
        .replaygain = false,
    };
    const auto has_path_operation =
        operation_selection.rename_files || operation_selection.move_files;
    if (grid_model_ == nullptr || write_plan_running_ ||
        (!operation_selection.save_tags && !has_path_operation)) {
        return;
    }

    std::optional<operations::OutputLayoutProfile> output_layout;
    std::optional<operations::DestinationProfile> destination;
    if (has_path_operation) {
        if (!editing_output_layout_id_) {
            read_only_->setText(QStringLiteral("Select a saved naming layout before applying"));
            return;
        }
        const auto layout = std::ranges::find(output_layout_catalog_, *editing_output_layout_id_,
                                              &persistence::SavedOutputLayoutProfile::id);
        if (layout == output_layout_catalog_.end()) {
            read_only_->setText(QStringLiteral("The selected naming layout is unavailable"));
            return;
        }
        output_layout = layout->profile;
        if (operation_selection.move_files) {
            if (!editing_destination_id_) {
                read_only_->setText(
                    QStringLiteral("Select a saved move destination before applying"));
                return;
            }
            const auto selected_destination =
                std::ranges::find(destination_catalog_, *editing_destination_id_,
                                  &persistence::SavedDestinationProfile::id);
            if (selected_destination == destination_catalog_.end()) {
                read_only_->setText(QStringLiteral("The selected move destination is unavailable"));
                return;
            }
            destination = selected_destination->profile;
        }
    }
    auto draft =
        save_tags_check_->isChecked() ? grid_model_->patches() : metadata::StagedMetadataPatchSet{};
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
    const metadata::MetadataWritePlanOptions plan_options{
        .sidecar_loudness =
            replaygain_sidecar_only_ != nullptr && replaygain_sidecar_only_->isChecked(),
        .true_peak_loudness =
            replaygain_true_peak_ != nullptr && replaygain_true_peak_->isChecked()};
    write_plan_running_ = true;
    if (artwork_section_) {
        artwork_section_->setEnabled(false);
    }
    updateWritePlanButton();
    read_only_->setText(QStringLiteral("Checking files…"));
    write_plan_watcher_.setFuture(QtConcurrent::run(
        [selection, draft = std::move(draft), items = std::move(items), operation_selection,
         output_layout = std::move(output_layout), destination = std::move(destination),
         cancellation, plan_options, artwork_intents]() mutable {
            // WYSIWYG apply: the plan writes exactly the staged draft.
            // Automatic scripts already staged their edits into the grid.
            const auto metadata_context_change_count =
                (operation_selection.save_tags ? draft.patch_count() : 0U) + artwork_intents.size();
            std::optional<metadata::MetadataWritePlan> metadata_plan;
            if (operation_selection.save_tags && !draft.empty()) {
                auto revalidated = metadata::revalidate_metadata_write_plan(
                    *selection, draft, cancellation, plan_options);
                if (!revalidated) {
                    return std::make_shared<WritePlanResult>(
                        std::unexpected(std::move(revalidated.error())));
                }
                metadata_plan = std::move(*revalidated);
            }

            if (!artwork_intents.empty()) {
                auto art = metadata::revalidate_artwork_write_plan(artwork_intents, cancellation);
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
                    auto checked = operations::preflight_output_paths(*path_plan, cancellation);
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

void MetadataPropertiesDialog::finishWritePlan() {
    const auto generation = write_plan_job_generation_;
    const auto result = write_plan_watcher_.result();
    write_plan_running_ = false;
    if (artwork_section_) {
        artwork_section_->setEnabled(true);
    }
    updateWritePlanButton();
    if (generation != write_plan_generation_) {
        return;
    }
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The preparation task returned no result");
        read_only_->setText(QStringLiteral("Nothing was changed · %1").arg(message));
        return;
    }

    auto plan = std::make_shared<const operations::PreparationPlan>(std::move(**result));
    if (plan->ready()) {
        startApply(std::move(plan));
        return;
    }

    // Blocked: nothing was written; list only what needs attention.
    std::vector<PreparationFeedbackRow> rows;
    const auto add_row = [&rows](const std::string& raw_path, const QString& detail) {
        rows.push_back(PreparationFeedbackRow{
            .file = raw_path.empty() ? QStringLiteral("Selection")
                                     : QString::fromStdString(core::escape_raw_path(raw_path)),
            .detail = detail,
        });
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
                    add_row(
                        source.raw_path,
                        QStringLiteral("%1: %2").arg(
                            display_utf8(metadata::metadata_write_plan_issue_kind_name(issue.kind)),
                            display_utf8(issue.error.message)));
                }
            }
        }
        for (const auto& sheet : plan->metadata->cue_sheets) {
            for (const auto& issue : sheet.issues) {
                if (issue.blocking) {
                    add_row(
                        sheet.raw_cue_path,
                        QStringLiteral("%1: %2").arg(
                            display_utf8(metadata::metadata_write_plan_issue_kind_name(issue.kind)),
                            display_utf8(issue.error.message)));
                }
            }
        }
        for (const auto& sidecar : plan->metadata->sidecars) {
            for (const auto& issue : sidecar.issues) {
                if (issue.blocking) {
                    add_row(
                        sidecar.raw_audio_path,
                        QStringLiteral("%1: %2").arg(
                            display_utf8(metadata::metadata_write_plan_issue_kind_name(issue.kind)),
                            display_utf8(issue.error.message)));
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
    read_only_->setText(
        QStringLiteral("Nothing was changed · %1 %2")
            .arg(rows.size())
            .arg(pluralized(rows.size(), QStringLiteral("problem"), QStringLiteral("problems"))));
    showPreparationFeedback(
        QStringLiteral("Apply blocked"),
        QStringLiteral("Nothing was changed. Fix the %1 below, then apply again.")
            .arg(pluralized(rows.size(), QStringLiteral("problem"), QStringLiteral("problems"))),
        std::move(rows));
}

void MetadataPropertiesDialog::showPreparationFeedback(const QString& window_title,
                                                       const QString& summary,
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
        if (dialog->property("retry-starting").toBool()) {
            return;
        }
        if (apply_committed_) {
            QTimer::singleShot(0, this, &QDialog::close);
        } else {
            updateWritePlanButton();
        }
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MetadataPropertiesDialog::requestApplyStop() {
    if (!apply_running_ || apply_stop_requested_) {
        return;
    }
    apply_stop_requested_ = true;
    apply_stop_button_->setEnabled(false);
    apply_cancellation_.request_cancellation();
    read_only_->setText(QStringLiteral("Stopping after the files already in progress are safe…"));
}

void MetadataPropertiesDialog::setApplyProgressVisible(const bool visible) {
    apply_progress_bar_->setVisible(visible);
    apply_stop_button_->setVisible(visible);
    apply_stop_button_->setEnabled(visible && !apply_stop_requested_);
}

void MetadataPropertiesDialog::startApply(std::shared_ptr<const operations::PreparationPlan> plan) {
    if (!plan || !plan->ready() || apply_running_) {
        return;
    }
    if (plan->has_path_operation()) {
        startFileApply(std::move(plan));
        return;
    }
    startMetadataApply(std::move(plan));
}

void MetadataPropertiesDialog::startMetadataApply(
    std::shared_ptr<const operations::PreparationPlan> plan) {
    if (!plan->metadata || !plan_applier_factory_) {
        showStickyStatus(QStringLiteral("Metadata Apply is unavailable"));
        return;
    }
    auto applier = plan_applier_factory_();
    if (!applier) {
        showStickyStatus(QStringLiteral("Metadata Apply is unavailable"));
        return;
    }
    active_metadata_plan_ = plan;
    apply_cancellation_.request_cancellation();
    apply_cancellation_ = core::CancellationSource{};
    apply_progress_state_ = std::make_shared<MetadataApplyProgressState>();
    apply_progress_state_->states.assign(plan->metadata->sources.size(),
                                         operations::MetadataApplySourceState::pending);
    apply_progress_state_->issues.resize(plan->metadata->sources.size());
    file_apply_progress_state_.reset();
    apply_running_ = true;
    if (artwork_section_) {
        artwork_section_->setEnabled(false);
    }
    applying_file_paths_ = false;
    apply_stop_requested_ = false;
    apply_committed_ = metadata_had_commits_;
    updateWritePlanButton();
    const auto total = plan->metadata->sources.size();
    read_only_->setText(QStringLiteral("Saving metadata · 0 of %1").arg(total));
    apply_progress_bar_->setRange(0, static_cast<int>(total));
    apply_progress_bar_->setValue(0);
    setApplyProgressVisible(true);
    apply_progress_timer_->start();

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

void MetadataPropertiesDialog::startFileApply(
    std::shared_ptr<const operations::PreparationPlan> plan) {
    if (!plan->path_preflight || !file_plan_applier_factory_) {
        showStickyStatus(QStringLiteral("File publication Apply is unavailable"));
        return;
    }
    auto applier = file_plan_applier_factory_();
    if (!applier) {
        showStickyStatus(QStringLiteral("File publication Apply is unavailable"));
        return;
    }
    apply_cancellation_.request_cancellation();
    apply_cancellation_ = core::CancellationSource{};
    file_apply_progress_state_ = std::make_shared<FilePublicationApplyProgressState>();
    file_apply_progress_state_->states.assign(plan->path_preflight->sources.size(),
                                              operations::FilePublicationApplySourceState::pending);
    file_apply_progress_state_->issues.resize(plan->path_preflight->sources.size());
    apply_progress_state_.reset();
    apply_running_ = true;
    if (artwork_section_) {
        artwork_section_->setEnabled(false);
    }
    applying_file_paths_ = true;
    apply_stop_requested_ = false;
    apply_committed_ = false;
    updateWritePlanButton();
    const auto total = plan->path_preflight->sources.size();
    read_only_->setText(QStringLiteral("Updating files · 0 of %1").arg(total));
    apply_progress_bar_->setRange(0, static_cast<int>(total));
    apply_progress_bar_->setValue(0);
    setApplyProgressVisible(true);
    apply_progress_timer_->start();

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

void MetadataPropertiesDialog::updateApplyProgress() {
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
    apply_progress_bar_->setValue(static_cast<int>(completed));
    read_only_->setText(
        QStringLiteral("%1 · %2 of %3%4")
            .arg(applying_file_paths_ ? QStringLiteral("Updating files")
                                      : QStringLiteral("Saving metadata"))
            .arg(completed)
            .arg(total)
            .arg(apply_stop_requested_ ? QStringLiteral(" · stopping…") : QString{}));
}

void MetadataPropertiesDialog::finishMetadataApply() {
    apply_running_ = false;
    if (artwork_section_) {
        artwork_section_->setEnabled(true);
    }
    apply_progress_timer_->stop();
    setApplyProgressVisible(false);
    const auto result = metadata_apply_watcher_.result();
    if (result && *result) {
        metadata_had_commits_ = metadata_had_commits_ || (**result).committed_source_count() > 0U;
        apply_committed_ = metadata_had_commits_;
        if (apply_observer_) {
            apply_observer_(**result);
        }
    }
    ++write_plan_generation_;
    apply_progress_state_.reset();
    updateWritePlanButton();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The Apply task returned no result");
        read_only_->setText(QStringLiteral("Saving metadata failed · %1").arg(message));
        showPreparationFeedback(QStringLiteral("Saving metadata failed"), message, {});
        return;
    }
    const auto& outcome = **result;
    // ADR-0139/0141: committed CUE sheets and loudness sidecars count as
    // saved files; any failure keeps the dialog open with the problem
    // listed.
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
        if (artwork_section_) {
            artwork_section_->discardPendingChanges();
        }
        read_only_->setText(
            QStringLiteral("Saved %1 %2")
                .arg(saved)
                .arg(pluralized(saved, QStringLiteral("file"), QStringLiteral("files"))));
        QTimer::singleShot(0, this, &QDialog::close);
        return;
    }
    std::vector<PreparationFeedbackRow> rows;
    for (const auto& source : outcome.sources) {
        if (source.state == operations::MetadataApplySourceState::committed) {
            continue;
        }
        rows.push_back(PreparationFeedbackRow{
            .file = QString::fromStdString(core::escape_raw_path(source.raw_path)),
            .detail =
                source.issue ? display_utf8(source.issue->message) : apply_state_text(source.state),
        });
    }
    for (const auto& sheet : outcome.cue_sheets) {
        if (sheet.state == operations::MetadataApplySourceState::committed) {
            continue;
        }
        rows.push_back(PreparationFeedbackRow{
            .file = QString::fromStdString(core::escape_raw_path(sheet.raw_cue_path)),
            .detail =
                sheet.issue ? display_utf8(sheet.issue->message) : apply_state_text(sheet.state),
        });
    }
    for (const auto& sidecar : outcome.sidecars) {
        if (sidecar.state == operations::MetadataApplySourceState::committed) {
            continue;
        }
        rows.push_back(PreparationFeedbackRow{
            .file = QString::fromStdString(core::escape_raw_path(sidecar.raw_audio_path)),
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
    read_only_->setText(QStringLiteral("%1 saved · %2 failed · %3 stopped")
                            .arg(saved)
                            .arg(failed)
                            .arg(stopped_count));
    showPreparationFeedback(stopped ? QStringLiteral("Save stopped")
                                    : QStringLiteral("Saved with problems"),
                            summary, std::move(rows));
    // Retry the reviewed per-file intent, retaining its original revision and
    // fingerprints. Never reconstruct a retry from displayed or freshly read tags.
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
        if (retry->ready() && !retry->metadata->sources.empty() && feedback_dialog_) {
            auto* dialog = feedback_dialog_.data();
            auto* buttons = dialog->findChild<QDialogButtonBox*>(
                QStringLiteral("bench-preparation-feedback-buttons"));
            auto* retry_button = buttons->addButton(QStringLiteral("Retry failed / stopped files"),
                                                    QDialogButtonBox::ActionRole);
            retry_button->setObjectName(QStringLiteral("bench-preparation-retry"));
            retry_button->setToolTip(
                QStringLiteral("Retry only unfinished files using the reviewed changes. Changed "
                               "files and unresolved recovery records remain blocked."));
            if (apply_committed_) {
                buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Close editor"));
            }
            connect(retry_button, &QPushButton::clicked, this, [this, dialog, retry] {
                dialog->setProperty("retry-starting", true);
                dialog->close();
                startMetadataApply(retry);
            });
        }
    }
}

void MetadataPropertiesDialog::finishFileApply() {
    apply_running_ = false;
    if (artwork_section_) {
        artwork_section_->setEnabled(true);
    }
    apply_progress_timer_->stop();
    setApplyProgressVisible(false);
    const auto result = file_apply_watcher_.result();
    if (result && *result) {
        apply_committed_ = (**result).committed_source_count() > 0U;
        if (file_apply_observer_) {
            file_apply_observer_(**result);
        }
    }
    ++write_plan_generation_;
    file_apply_progress_state_.reset();
    applying_file_paths_ = false;
    updateWritePlanButton();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The Apply task returned no result");
        read_only_->setText(QStringLiteral("Updating files failed · %1").arg(message));
        showPreparationFeedback(QStringLiteral("Updating files failed"), message, {});
        return;
    }
    const auto& outcome = **result;
    const auto changed = outcome.committed_source_count();
    const auto unchanged = outcome.unchanged_source_count();
    // Limited destination filesystems publish successfully but may skip
    // preservation (ADR-0111); those notes surface problems-only instead of
    // silently closing.
    std::vector<PreparationFeedbackRow> note_rows;
    for (const auto& source : outcome.sources) {
        if (!source.commit) {
            continue;
        }
        for (const auto& note : source.commit->notes) {
            note_rows.push_back(PreparationFeedbackRow{
                .file = QString::fromStdString(core::escape_raw_path(source.source_raw_path)),
                .detail = display_utf8(note),
            });
        }
    }
    if (changed + unchanged == outcome.sources.size()) {
        if (artwork_section_) {
            artwork_section_->discardPendingChanges();
        }
        read_only_->setText(
            QStringLiteral("Updated %1 %2")
                .arg(changed)
                .arg(pluralized(changed, QStringLiteral("file"), QStringLiteral("files"))));
        if (!note_rows.empty()) {
            showPreparationFeedback(
                QStringLiteral("Updated with notes"),
                QStringLiteral("Every file updated; the destination filesystem could not "
                               "preserve everything."),
                std::move(note_rows));
            return;
        }
        QTimer::singleShot(0, this, &QDialog::close);
        return;
    }
    std::vector<PreparationFeedbackRow> rows = std::move(note_rows);
    for (const auto& source : outcome.sources) {
        if (source.state == operations::FilePublicationApplySourceState::committed ||
            source.state == operations::FilePublicationApplySourceState::unchanged) {
            continue;
        }
        rows.push_back(PreparationFeedbackRow{
            .file = QString::fromStdString(core::escape_raw_path(source.source_raw_path)),
            .detail = source.issue ? display_utf8(source.issue->message)
                                   : file_apply_state_text(source.state),
        });
    }
    const auto stopped =
        outcome.cancelled_source_count() > 0U && outcome.failed_source_count() == 0U;
    const auto summary =
        QStringLiteral("%1 updated · %2 failed · %3 stopped. Updated files are done; the files "
                       "below were not touched.")
            .arg(changed)
            .arg(outcome.failed_source_count())
            .arg(outcome.cancelled_source_count());
    read_only_->setText(QStringLiteral("%1 updated · %2 failed · %3 stopped")
                            .arg(changed)
                            .arg(outcome.failed_source_count())
                            .arg(outcome.cancelled_source_count()));
    showPreparationFeedback(stopped ? QStringLiteral("Update stopped")
                                    : QStringLiteral("Updated with problems"),
                            summary, std::move(rows));
}

QStringList MetadataPropertiesDialog::metadataFieldNameSuggestions(const QString& query) const {
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

std::vector<std::size_t> MetadataPropertiesDialog::selectedItemIndexes() const {
    std::vector<std::size_t> selected_items;
    if (file_list_ == nullptr || file_list_->selectionModel() == nullptr) {
        return selected_items;
    }
    const auto rows = file_list_->selectionModel()->selectedRows(0);
    selected_items.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
        if (row.isValid() && row.row() >= 0) {
            selected_items.push_back(static_cast<std::size_t>(row.row()));
        }
    }
    std::ranges::sort(selected_items);
    return selected_items;
}

void MetadataPropertiesDialog::promptAddField() {
    if (field_name_dialog_ != nullptr) {
        field_name_dialog_->raise();
        field_name_dialog_->activateWindow();
        return;
    }
    if (aggregate_model_ == nullptr || !aggregate_model_->summaryReady() ||
        aggregate_model_->selectedItemCount() == 0U) {
        return;
    }

    auto* prompt = new QInputDialog(this);
    prompt->setObjectName(QStringLiteral("bench-metadata-add-field-dialog"));
    prompt->setWindowTitle(QStringLiteral("Add metadata field"));
    prompt->setLabelText(QStringLiteral("Field name:"));
    prompt->setInputMode(QInputDialog::TextInput);
    prompt->setWindowModality(Qt::WindowModal);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    auto* field_name = prompt->findChild<QLineEdit*>();
    Q_ASSERT(field_name != nullptr);
    field_name->setObjectName(QStringLiteral("bench-metadata-add-field-name"));
    auto* completion_model = new QStringListModel(prompt);
    completion_model->setObjectName(QStringLiteral("bench-metadata-field-completions"));
    auto* completer = new QCompleter(completion_model, prompt);
    completer->setObjectName(QStringLiteral("bench-metadata-field-completer"));
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    completer->setMaxVisibleItems(12);
    field_name->setCompleter(completer);
    completion_model->setStringList(metadataFieldNameSuggestions({}));
    connect(prompt, &QInputDialog::textValueChanged, this,
            [this, prompt, field_name, completion_model, completer](const QString& text) {
                completion_model->setStringList(metadataFieldNameSuggestions(text));
                if (text.trimmed().isEmpty() || completion_model->rowCount() == 0) {
                    return;
                }
                QTimer::singleShot(0, prompt, [prompt, field_name, completer] {
                    if (prompt->isVisible() && field_name->hasFocus()) {
                        completer->complete();
                    }
                });
            });
    field_name_dialog_ = prompt;
    updateFieldButtons();
    updateTransformationButton();
    connect(prompt, &QDialog::accepted, this, [this, prompt] {
        std::vector<int> selected_rows;
        if (file_list_ != nullptr && file_list_->selectionModel() != nullptr) {
            const auto indexes = file_list_->selectionModel()->selectedRows(0);
            selected_rows.reserve(static_cast<std::size_t>(indexes.size()));
            for (const auto& index : indexes) {
                selected_rows.push_back(index.row());
            }
        }
        const auto result = aggregate_model_->ensureField(prompt->textValue());
        if (!result) {
            read_only_->setText(QStringLiteral("Field could not be added · %1")
                                    .arg(display_utf8(result.error().message)));
            return;
        }
        const auto trimmed_name = prompt->textValue().trimmed();
        const auto encoded_name = trimmed_name.toUtf8();
        const auto canonical_name = metadata::canonicalize_field_name(std::string_view{
            encoded_name.constData(), static_cast<std::size_t>(encoded_name.size())});
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
        if (file_list_ != nullptr && file_list_->selectionModel() != nullptr &&
            grid_model_ != nullptr) {
            QItemSelection restored_selection;
            for (const auto row : selected_rows) {
                const auto track = grid_model_->index(row, 0);
                restored_selection.select(track, track);
            }
            file_list_->selectionModel()->select(restored_selection,
                                                 QItemSelectionModel::ClearAndSelect |
                                                     QItemSelectionModel::Rows);
        }
        loaded_field_count_ = static_cast<std::size_t>(aggregate_model_->rowCount());
        updateSelectionProjection();
        prompt->setProperty("trackknifeFieldRow", *result);
    });
    connect(prompt, &QDialog::finished, this, [this, prompt] {
        bool has_field_row = false;
        const auto field_row = prompt->property("trackknifeFieldRow").toInt(&has_field_row);
        if (field_name_dialog_ == prompt) {
            field_name_dialog_ = nullptr;
        }
        updateFieldButtons();
        updateTransformationButton();
        if (has_field_row && fields_ != nullptr && aggregate_model_ != nullptr) {
            field_review_bar_->revealField(field_row);
            const auto draft = aggregate_model_->index(field_row, 2);
            fields_->setCurrentIndex(draft);
            fields_->selectionModel()->select(draft, QItemSelectionModel::ClearAndSelect |
                                                         QItemSelectionModel::Rows);
            fields_->scrollTo(draft, QAbstractItemView::PositionAtCenter);
            fields_->setFocus(Qt::OtherFocusReason);
        }
    });
    prompt->open();
}

void MetadataPropertiesDialog::removeSelectedFields() {
    if (aggregate_model_ == nullptr || fields_ == nullptr || fields_->selectionModel() == nullptr) {
        return;
    }
    static_cast<void>(aggregate_model_->removeIndexes(fields_->selectionModel()->selectedRows(0)));
}

void MetadataPropertiesDialog::editCurrentValues() {
    if (exact_values_dialog_ != nullptr) {
        exact_values_dialog_->raise();
        exact_values_dialog_->activateWindow();
        return;
    }
    if (grid_model_ == nullptr || aggregate_model_ == nullptr || fields_ == nullptr ||
        !aggregate_model_->summaryReady() || aggregate_model_->selectedItemCount() == 0U) {
        return;
    }

    const auto current = fields_->currentIndex();
    if (!current.isValid()) {
        return;
    }

    const auto field_index = static_cast<std::size_t>(current.row());
    const auto& field = grid_model_->selection().field(field_index);
    const auto value_index = aggregate_model_->index(current.row(), 2);
    const auto current_values = value_index.data(metadata_cell_values_role).toStringList();
    const auto heading =
        QStringLiteral("%1 — %2 selected %3")
            .arg(display_utf8(field.display_name))
            .arg(selected_item_count_)
            .arg(selected_item_count_ == 1U ? QStringLiteral("file") : QStringLiteral("files"));
    QString context;
    if (current_values.isEmpty()) {
        context = QStringLiteral(
            "The selected files do not currently share one exact value list. Values entered "
            "here replace this field on those files.");
    } else {
        context = QStringLiteral(
            "Edit the exact ordered value list applied to the selected files. Duplicates and "
            "empty values remain distinct.");
    }

    auto* editor = createMetadataExactValueDialog(heading, context, current_values, this);
    exact_values_dialog_ = editor;
    edit_values_button_->setEnabled(false);
    updateTransformationButton();
    const QPersistentModelIndex target{value_index};
    connect(editor, &QDialog::accepted, this, [this, editor, target] {
        if (!target.isValid()) {
            return;
        }
        static_cast<void>(aggregate_model_->replaceRowValues(
            target.row(), metadataExactValueDialogValues(editor)));
    });
    connect(editor, &QDialog::finished, this, [this, editor] {
        if (exact_values_dialog_ == editor) {
            exact_values_dialog_ = nullptr;
        }
        updateEditValuesButton();
        updateTransformationButton();
    });
    editor->open();
}

void MetadataPropertiesDialog::promptTransformation(
    const std::optional<core::StableId> initially_selected, const bool preview_initially_selected) {
    if (transformation_dialog_ != nullptr) {
        transformation_dialog_->raise();
        transformation_dialog_->activateWindow();
        return;
    }
    if (grid_model_ == nullptr || aggregate_model_ == nullptr ||
        !aggregate_model_->summaryReady() || aggregate_model_->selectedItemCount() == 0U ||
        exact_values_dialog_ != nullptr || field_name_dialog_ != nullptr || write_plan_running_ ||
        apply_running_ || artwork_operation_running_) {
        return;
    }
    auto items = selectedItemIndexes();
    if (items.empty()) {
        return;
    }
    QStringList labels;
    labels.reserve(grid_model_->rowCount());
    for (auto row = 0; row < grid_model_->rowCount(); ++row) {
        labels.push_back(grid_model_->trackLabel(row));
    }
    auto* dialog = createMetadataTransformationDialog(
        grid_model_->sharedSelection(), grid_model_->patches(), std::move(items), std::move(labels),
        [this](const metadata::MetadataTransformationPreview& preview) {
            if (grid_model_ == nullptr || !stageTransformationPreservingSelection(preview)) {
                return false;
            }
            loaded_field_count_ = grid_model_->selection().field_count();
            updateSelectionProjection();
            return true;
        },
        transformation_store_, this, initially_selected, preview_initially_selected, layout_store_);
    transformation_dialog_ = dialog;
    updateTransformationButton();
    connect(dialog, &QDialog::finished, this, [this, dialog, initially_selected] {
        if (transformation_dialog_ == dialog) {
            transformation_dialog_ = nullptr;
        }
        loadTransformationCatalog(initially_selected);
        updateTransformationButton();
    });
    dialog->open();
}

bool MetadataPropertiesDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == fields_ && event->type() == QEvent::KeyPress && grid_model_ != nullptr) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Undo)) {
            return grid_model_->undo();
        }
        if (key->matches(QKeySequence::Redo)) {
            return grid_model_->redo();
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            key->modifiers() == Qt::ControlModifier) {
            editCurrentValues();
            return true;
        }
        if (key->key() == Qt::Key_Insert && key->modifiers() == Qt::NoModifier) {
            promptAddField();
            return true;
        }
        if (key->key() == Qt::Key_Delete && key->modifiers() == Qt::NoModifier) {
            return aggregate_model_->removeIndexes(fields_->selectionModel()->selectedIndexes());
        }
        if (key->key() == Qt::Key_Backspace && key->modifiers() == Qt::ControlModifier) {
            return aggregate_model_->revertIndexes(fields_->selectionModel()->selectedIndexes());
        }
    }
    return QDialog::eventFilter(watched, event);
}

void MetadataPropertiesDialog::reject() {
    // Escape must use the same unsaved-draft and cancellation checks as closing the tab.
    close();
}

void MetadataPropertiesDialog::closeEvent(QCloseEvent* event) {
    if (artwork_section_ && artwork_section_->hasPendingChanges() && !artwork_section_->isBusy()) {
        const auto answer =
            QMessageBox::warning(this, QStringLiteral("Discard artwork changes?"),
                                 QStringLiteral("The pending artwork changes have not been saved."),
                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) {
            event->ignore();
            return;
        }
        artwork_section_->discardPendingChanges();
    }
    if (artwork_operation_running_) {
        artwork_section_->requestOperationCancellation();
        read_only_->setText(
            QStringLiteral("Cancelling artwork work after in-flight files become safe…"));
        event->ignore();
        return;
    }
    if (apply_running_) {
        apply_cancellation_.request_cancellation();
        read_only_->setText(
            QStringLiteral("Cancelling Apply after in-flight sources become safe…"));
        event->ignore();
        return;
    }
    if (apply_committed_) {
        write_plan_cancellation_.request_cancellation();
        persistLayoutState();
        event->accept();
        return;
    }
    if (draft_count_ == 0 || grid_model_ == nullptr) {
        write_plan_cancellation_.request_cancellation();
        persistLayoutState();
        event->accept();
        return;
    }
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Discard metadata draft?"),
        QStringLiteral("%1 staged %2 exist only in memory and have not been written to files.")
            .arg(draft_count_)
            .arg(draft_count_ == 1 ? QStringLiteral("change") : QStringLiteral("changes")),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Discard) {
        write_plan_cancellation_.request_cancellation();
        static_cast<void>(grid_model_->discardAll());
        persistLayoutState();
        event->accept();
    } else {
        event->ignore();
    }
}

void MetadataPropertiesDialog::pumpTechnicalQueue() {
    if (technical_probing_ || technical_queue_.empty()) {
        return;
    }
    const auto path = technical_queue_.front();
    technical_queue_.pop_front();
    technical_probing_ = true;
    technical_watcher_.setFuture(
        QtConcurrent::run([path, token = technical_cancellation_.token()]()
                              -> std::pair<std::string, std::optional<TechnicalInfo>> {
            auto probe = formats::probe_local_media(path, token);
            if (!probe || !probe->best_audio_stream) {
                return {path, std::nullopt};
            }
            const auto found = std::ranges::find(probe->audio_streams, *probe->best_audio_stream,
                                                 &formats::AudioStreamInfo::stream_index);
            if (found == probe->audio_streams.end()) {
                return {path, std::nullopt};
            }
            TechnicalInfo info;
            info.codec = found->codec_name;
            info.sample_rate = found->sample_rate;
            info.bits = formats::bits_per_sample_hint(found->sample_format);
            info.channels = found->channels;
            info.bit_rate = found->bit_rate > 0 ? found->bit_rate : probe->bit_rate;
            info.duration_ms = probe->duration_ms.value_or(-1);
            return {path, info};
        }));
}

void MetadataPropertiesDialog::updateTechnicalSummary() {
    if (technical_status_ == nullptr || grid_model_ == nullptr) {
        return;
    }
    auto items = selectedItemIndexes();
    const auto& selection = grid_model_->selection();
    if (items.empty()) {
        items.reserve(selection.item_count());
        for (std::size_t item_index = 0U; item_index < selection.item_count(); ++item_index) {
            items.push_back(item_index);
        }
    }
    if (items.empty()) {
        technical_status_->clear();
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
    constexpr std::size_t maximum_probes = 512U;
    for (const auto& path : paths) {
        if (technical_cache_.contains(path) || technical_pending_.contains(path)) {
            continue;
        }
        if (technical_cache_.size() + technical_pending_.size() >= maximum_probes) {
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
        parts << tr("first %1 files").arg(maximum_probes);
    }
    technical_status_->setText(parts.join(QStringLiteral(" · ")));
}

} // namespace trackknife::bench
