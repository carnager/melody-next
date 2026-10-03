// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_properties_dialog.hpp"

#include "bench/cover_review.hpp"
#include "bench/engine_folder_dialog.hpp"
#include "bench/file_scope_view.hpp"
#include "bench/identify_albums_dialog.hpp"
#include "bench/metadata_artwork_section.hpp"
#include "bench/metadata_dialog_helpers.hpp"
#include "bench/metadata_exact_value_dialog.hpp"
#include "bench/metadata_field_review_bar.hpp"
#include "bench/metadata_grid_model.hpp"
#include "bench/metadata_scalar_delegate.hpp"
#include "bench/metadata_transformation_dialog.hpp"
#include "bench/preparation_feedback_dialog.hpp"
#include "bench/settings_dialog.hpp"
#include "bench/themed_icon.hpp"
#include "bench/trackknife_style.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
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
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStringListModel>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

namespace trackknife::bench {

namespace {

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

[[nodiscard]] SettingsDialog::Page settingsPage(const TaggerSession::SettingsPage page) {
    switch (page) {
    case TaggerSession::SettingsPage::naming:
        return SettingsDialog::Page::naming;
    case TaggerSession::SettingsPage::covers:
        return SettingsDialog::Page::covers;
    case TaggerSession::SettingsPage::replaygain:
        return SettingsDialog::Page::replaygain;
    }
    return SettingsDialog::Page::naming;
}

// The destination list's last entry: not a destination, a folder to choose.
const QString choose_folder_item = QStringLiteral("choose-folder");

} // namespace

MetadataPropertiesDialog::MetadataPropertiesDialog(
    const std::size_t requested_item_count, MetadataPropertiesSourceReader source_reader,
    const std::span<const std::string_view> preferred_fields,
    MetadataWritePlanApplierFactory plan_applier_factory, MetadataApplyObserver apply_observer,
    MetadataTransformationStore transformation_store, OutputProfileStore output_profile_store,
    FilePublicationPlanApplierFactory file_plan_applier_factory,
    FilePublicationApplyObserver file_apply_observer, QWidget* parent,
    MetadataDialogLayoutStore layout_store, MusicBrainzLookupService musicbrainz,
    FileWorkTools tools)
    : MetadataPropertiesDialog(
          requested_item_count, std::move(source_reader), preferred_fields,
          TaggerServices{
              .plan_applier_factory = std::move(plan_applier_factory),
              .apply_observer = std::move(apply_observer),
              .transformation_store = std::move(transformation_store),
              .output_profile_store = std::move(output_profile_store),
              .file_plan_applier_factory = std::move(file_plan_applier_factory),
              .file_apply_observer = std::move(file_apply_observer),
              .layout_store = std::move(layout_store),
              .musicbrainz = std::move(musicbrainz),
              .tools = std::move(tools),
              .library_roots = {},
          },
          parent) {}

MetadataPropertiesDialog::MetadataPropertiesDialog(
    const std::size_t requested_item_count, MetadataPropertiesSourceReader source_reader,
    const std::span<const std::string_view> preferred_fields, TaggerServices services,
    QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("bench-metadata-properties"));
    setWindowTitle(QStringLiteral("Edit tags"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1'020, 620);
    session_ = new TaggerSession(requested_item_count, std::move(source_reader), preferred_fields,
                                 std::move(services), this);
    const QPointer self{this};
    session_->loadLayoutState(
        [self](QByteArray geometry) {
            if (self) {
                static_cast<void>(self->restoreGeometry(geometry));
            }
        },
        [self](QByteArray splitter) {
            if (!self) {
                return;
            }
            self->pending_metadata_splitter_state_ = std::move(splitter);
            if (self->metadata_splitter_ != nullptr) {
                static_cast<void>(
                    self->metadata_splitter_->restoreState(self->pending_metadata_splitter_state_));
                // A saved state carries its orientation: one from when the
                // files stood above the fields would stack them again.
                self->metadata_splitter_->setOrientation(Qt::Horizontal);
            }
        });

    // The tag editor's frame (ADR-0250): a shaded header and footer
    // across the window, the files in a shaded side pane, the fields beside
    // them with room around.
    root_layout_ = new QVBoxLayout(this);
    root_layout_->setContentsMargins(0, 0, 0, 0);
    root_layout_->setSpacing(0);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("bench-metadata-summary"));
    // ADR-0152/0183: selection summary and the read-only technical summary
    // share one header row; the technical text clips instead of wrapping
    // (full text in the tooltip) so it never adds rows or width.
    technical_status_ = new QLabel(this);
    technical_status_->setObjectName(QStringLiteral("bench-metadata-technical"));
    technical_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    technical_status_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    technical_status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    {
        auto font = summary_->font();
        font.setBold(true);
        summary_->setFont(font);
        auto quiet = technical_status_->palette();
        quiet.setColor(QPalette::WindowText, TrackknifeStyle::dim(quiet));
        technical_status_->setPalette(quiet);
    }
    auto* header = new Band(Band::Edge::bottom, this);
    header->setObjectName(QStringLiteral("bench-metadata-header"));
    auto* header_row = new QHBoxLayout(header);
    header_row->setContentsMargins(TrackknifeStyle::gap_large, 10, TrackknifeStyle::gap_large, 10);
    header_row->setSpacing(12);
    summary_->setParent(header);
    technical_status_->setParent(header);
    header_row->addWidget(summary_);
    header_row->addWidget(technical_status_, 1);
    root_layout_->addWidget(header);

    read_only_ = new QLabel(this);
    read_only_->setObjectName(QStringLiteral("bench-metadata-read-only"));
    read_only_->setAccessibleName(QStringLiteral("Metadata write capability"));
    read_only_->setTextFormat(Qt::PlainText);
    read_only_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    // ADR-0186: the apply options never render as their own surface; this
    // hidden panel's controls stand for the session's choices behind the
    // footer's Actions popover.
    auto* side_panel = new QWidget(this);
    side_panel->setObjectName(QStringLiteral("bench-metadata-side-panel"));
    side_panel->setMinimumWidth(260);
    transformation_panel_ = side_panel;
    transformation_panel_->hide();
    auto* side_layout = new QVBoxLayout(side_panel);
    side_layout->setContentsMargins(8, 6, 8, 6);
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
    // ADR-0237: destinations are folders on the engine the tracks are on;
    // for one elsewhere, it says whose.
    if (const auto engine = session_->destinationsOn(); !engine.isEmpty()) {
        destination_combo_->setPlaceholderText(QStringLiteral("None saved on %1 yet").arg(engine));
        destination_combo_->setToolTip(QStringLiteral("Move destinations on %1").arg(engine));
        manage_destinations_button->setToolTip(
            QStringLiteral("Create, change, or remove move destinations on %1").arg(engine));
    }
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
    // ADR-0147: per-track view of where each effective loudness value
    // comes from — draft, sidecar, CUE segment, or embedded tags.
    replaygain_provenance_button_ =
        new QPushButton(QStringLiteral("Loudness sources…"), side_panel);
    replaygain_provenance_button_->setObjectName(QStringLiteral("bench-replaygain-provenance"));
    replaygain_provenance_button_->setEnabled(false);
    connect(replaygain_provenance_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::showLoudnessProvenance);
    side_layout->addWidget(replaygain_provenance_button_);
    output_profile_status_ = new QLabel(side_panel);
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
    transformation_status_ = new QLabel(side_panel);
    transformation_status_->setObjectName(QStringLiteral("bench-metadata-transformation-status"));
    transformation_status_->setWordWrap(true);
    side_layout->addWidget(transformation_status_);
    transform_button_ = new QPushButton(QStringLiteral("Open script editor…"), side_panel);
    transform_button_->setObjectName(QStringLiteral("bench-metadata-transform"));
    transform_button_->setToolTip(
        QStringLiteral("Open the selected saved script, or create a new one"));
    transform_button_->setEnabled(false);
    side_layout->addWidget(transform_button_);

    // ADR-0185: profile management lives in Settings; these open it.
    connect(manage_layouts_button, &QPushButton::clicked, this,
            [this] { emit openSettingsRequested(SettingsDialog::Page::naming); });
    connect(manage_destinations_button, &QPushButton::clicked, this,
            [this] { emit openDestinationsRequested(); });

    loading_ = new QLabel(this);
    loading_->setObjectName(QStringLiteral("bench-metadata-loading"));
    loading_->setAlignment(Qt::AlignCenter);
    // "Stop" while the tags are being read.
    connect(loading_, &QLabel::linkActivated, this, [this](const QString& link) {
        if (link == QStringLiteral("stop") && session_ != nullptr) {
            session_->stopReading();
        }
    });
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
    identify_albums_button_ = new QPushButton(QStringLiteral("Identify albums…"), grid_tools_);
    identify_albums_button_->setObjectName(QStringLiteral("bench-metadata-identify-albums"));
    identify_albums_button_->setToolTip(
        QStringLiteral("Every album open here at once: grouped, looked up on MusicBrainz in the "
                       "background, and the clear matches staged by themselves"));
    identify_albums_button_->setEnabled(false);
    grid_tools_layout->addWidget(identify_albums_button_);
    auto* more_button = new QToolButton(grid_tools_);
    more_button->setObjectName(QStringLiteral("bench-metadata-more"));
    more_button->setText(QStringLiteral("More"));
    more_button->setPopupMode(QToolButton::InstantPopup);
    auto* more_menu = new QMenu(more_button);
    suggest_action_ = more_menu->addAction(QStringLiteral("Suggest album totals and artist"));
    suggest_action_->setToolTip(suggest_button_->toolTip());
    suggest_action_->setEnabled(false);
    connect(suggest_action_, &QAction::triggered, suggest_button_, &QPushButton::click);
    // ADR-0183: the field-set save/delete entries live here — their former
    // hidden-button triggers were reachable from nowhere.
    field_layout_save_action_ = more_menu->addAction(QStringLiteral("Save visible fields as set…"));
    field_layout_save_action_->setToolTip(
        QStringLiteral("Save the selected fields, or all currently visible fields, as a named "
                       "field set"));
    connect(field_layout_save_action_, &QAction::triggered, this,
            &MetadataPropertiesDialog::saveCurrentFieldLayout);
    field_layout_remove_action_ = more_menu->addAction(QStringLiteral("Delete current field set"));
    field_layout_remove_action_->setEnabled(false);
    connect(field_layout_remove_action_, &QAction::triggered, session_,
            &TaggerSession::removeFieldLayout);
    more_button->setMenu(more_menu);
    grid_tools_layout->addWidget(more_button);
    grid_tools_layout->addStretch(1);
    undo_button_ =
        new QPushButton(themedIcon(u"edit-undo|sp:SP_ArrowBack"), QString{}, grid_tools_);
    undo_button_->setFlat(true);
    undo_button_->setObjectName(QStringLiteral("bench-metadata-undo"));
    undo_button_->setAccessibleName(QStringLiteral("Undo"));
    undo_button_->setToolTip(QStringLiteral("Undo the last draft edit (Ctrl+Z)"));
    undo_button_->setShortcut(QKeySequence::Undo);
    undo_button_->setEnabled(false);
    grid_tools_layout->addWidget(undo_button_);
    redo_button_ =
        new QPushButton(themedIcon(u"edit-redo|sp:SP_ArrowForward"), QString{}, grid_tools_);
    redo_button_->setFlat(true);
    redo_button_->setObjectName(QStringLiteral("bench-metadata-redo"));
    redo_button_->setAccessibleName(QStringLiteral("Redo"));
    redo_button_->setToolTip(QStringLiteral("Redo the last undone draft edit (Ctrl+Shift+Z)"));
    redo_button_->setShortcut(QKeySequence::Redo);
    redo_button_->setEnabled(false);
    grid_tools_layout->addWidget(redo_button_);
    discard_button_ = new QPushButton(themedIcon(u"edit-clear|sp:SP_DialogDiscardButton"),
                                      QString{}, grid_tools_);
    discard_button_->setFlat(true);
    discard_button_->setObjectName(QStringLiteral("bench-metadata-discard"));
    discard_button_->setAccessibleName(QStringLiteral("Discard drafts"));
    discard_button_->setToolTip(QStringLiteral("Throw away every pending draft edit"));
    discard_button_->setEnabled(false);
    grid_tools_layout->addWidget(discard_button_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons_->setObjectName(QStringLiteral("bench-metadata-buttons"));
    // Apply is what this window is for: last, in the accent, after Close --
    // not ordered among Close by the platform's button-box rules.
    apply_plan_button_ = new QPushButton(QStringLiteral("Apply"), this);
    if (auto* close = buttons_->button(QDialogButtonBox::Close)) {
        close->setAutoDefault(false);
        close->setDefault(false);
    }
    apply_plan_button_->setObjectName(QStringLiteral("bench-metadata-apply-changes"));
    apply_plan_button_->setToolTip(QStringLiteral(
        "Recheck the files, then make every enabled change; problems stop the run and are shown"));
    apply_plan_button_->setEnabled(false);
    apply_plan_button_->setDefault(true);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(undo_button_, &QPushButton::clicked, session_, &TaggerSession::undo);
    connect(redo_button_, &QPushButton::clicked, session_, &TaggerSession::redo);
    connect(discard_button_, &QPushButton::clicked, session_, &TaggerSession::discardAll);
    connect(read_only_, &QLabel::linkActivated, this, [this](const QString& link) {
        if (link == QStringLiteral("export-replaygain")) {
            exportReplayGainResults();
            return;
        }
        session_->statusLinkActivated(link);
    });
    connect(add_field_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::promptAddField);
    connect(remove_field_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::removeSelectedFields);
    connect(edit_values_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::editCurrentValues);
    connect(field_layout_combo_, &QComboBox::currentIndexChanged, this,
            [this] { session_->selectFieldLayout(field_layout_combo_->currentData().toString()); });
    connect(suggest_button_, &QPushButton::clicked, session_, &TaggerSession::startProposals);
    connect(identify_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::startIdentify);
    connect(identify_albums_button_, &QPushButton::clicked, this, [this] {
        if (identify_albums_dialog_) {
            identify_albums_dialog_->raise();
            identify_albums_dialog_->activateWindow();
            return;
        }
        auto* dialog = new IdentifyAlbumsDialog(*session_, session_->services().musicbrainz, this);
        identify_albums_dialog_ = dialog;
        dialog->show();
    });
    connect(replaygain_scan_button_, &QPushButton::clicked, this,
            [this] { session_->startReplayGainScan(); });
    connect(replaygain_grouping_, &QComboBox::currentIndexChanged, session_,
            &TaggerSession::setReplayGainGrouping);
    connect(replaygain_expression_, &QLineEdit::textChanged, session_,
            &TaggerSession::setReplayGainExpression);
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
            &MetadataPropertiesDialog::sync);
    connect(transformation_list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        session_->toggleAutomaticScript(item->data(Qt::UserRole).toString(),
                                        item->checkState() == Qt::Checked);
    });
    connect(output_layout_combo_, &QComboBox::currentIndexChanged, session_,
            &TaggerSession::selectLayout);
    connect(destination_combo_, &QComboBox::currentIndexChanged, this,
            [this](const int index) { destinationActivated(destination_combo_, index); });
    connect(save_tags_check_, &QCheckBox::toggled, session_, &TaggerSession::setSaveTags);
    connect(rename_files_check_, &QCheckBox::toggled, session_, &TaggerSession::setRenameFiles);
    connect(move_files_check_, &QCheckBox::toggled, session_, &TaggerSession::setMoveFiles);
    connect(apply_plan_button_, &QPushButton::clicked, session_, &TaggerSession::startWritePlan);
    auto* footer = new Band(Band::Edge::top, this);
    footer->setObjectName(QStringLiteral("bench-metadata-footer"));
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(TrackknifeStyle::gap_large, 10, TrackknifeStyle::gap_large,
                                      10);
    footer_layout->setSpacing(TrackknifeStyle::gap);
    actions_button_ = new QToolButton(footer);
    actions_button_->setObjectName(QStringLiteral("bench-metadata-actions"));
    actions_button_->setText(QStringLiteral("Actions"));
    actions_button_->setToolTip(
        QStringLiteral("What Apply does: tags, renaming and moving, ReplayGain, scripts"));
    connect(actions_button_, &QToolButton::clicked, this,
            &MetadataPropertiesDialog::showActionsPopover);
    footer_layout->addWidget(actions_button_);
    apply_summary_ = new QLabel(footer);
    apply_summary_->setObjectName(QStringLiteral("bench-metadata-apply-summary"));
    apply_summary_->setTextFormat(Qt::PlainText);
    footer_layout->addWidget(apply_summary_);
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
    connect(apply_stop_button_, &QPushButton::clicked, session_, &TaggerSession::requestApplyStop);
    footer_layout->addWidget(apply_stop_button_);
    footer_layout->addWidget(buttons_);
    footer_layout->addWidget(apply_plan_button_);
    root_layout_->addWidget(footer);

    connect(session_, &TaggerSession::changed, this, &MetadataPropertiesDialog::sync);
    connect(session_, &TaggerSession::gridReady, this, &MetadataPropertiesDialog::buildGrid);
    connect(session_, &TaggerSession::gridFilled, this, &MetadataPropertiesDialog::fillGrid);
    connect(session_, &TaggerSession::scriptsChanged, this,
            &MetadataPropertiesDialog::rebuildScripts);
    connect(session_, &TaggerSession::outputProfilesChanged, this,
            &MetadataPropertiesDialog::rebuildOutputProfiles);
    connect(session_, &TaggerSession::fieldLayoutsChanged, this,
            &MetadataPropertiesDialog::rebuildFieldLayouts);
    connect(session_, &TaggerSession::feedbackRequested, this,
            &MetadataPropertiesDialog::showPreparationFeedback);
    connect(session_, &TaggerSession::folderImagesReviewRequested, this,
            [this](std::vector<metadata::FolderImageWritePlan> images) {
                reviewFolderImages(this, images, [session = QPointer{session_}] {
                    if (session) {
                        session->folderImagesReviewed(true);
                    }
                });
            });
    connect(session_, &TaggerSession::closeRequested, this, &QDialog::close);
    connect(session_, &TaggerSession::openSettingsRequested, this,
            [this](const TaggerSession::SettingsPage page) {
                emit openSettingsRequested(settingsPage(page));
            });
    connect(session_, &TaggerSession::openDestinationsRequested, this,
            &MetadataPropertiesDialog::openDestinationsRequested);
    connect(session_, &TaggerSession::statusMessage, this,
            &MetadataPropertiesDialog::statusMessage);
    sync();
    session_->start();
}

MetadataPropertiesDialog::~MetadataPropertiesDialog() {
    // The sole file view may currently be parented into the workspace sidebar.
    delete file_list_.data();
    session_->setArtwork(nullptr);
    delete session_;
    session_ = nullptr;
}

void MetadataPropertiesDialog::setArtworkMutationServices(
    ArtworkWritePlanApplierFactory applier_factory, ArtworkApplyObserver observer) {
    session_->setArtworkServices(std::move(applier_factory), std::move(observer));
    if (artwork_section_ != nullptr) {
        artwork_section_->setMutationServices(session_->artworkApplierFactory(),
                                              session_->artworkAppliedObserver());
    }
}

void MetadataPropertiesDialog::sync() {
    if (session_ == nullptr) {
        return;
    }
    const auto& session = *session_;
    summary_->setText(session.summary());
    read_only_->setTextFormat(session.statusRich() ? Qt::RichText : Qt::PlainText);
    read_only_->setText(session.status());
    read_only_->setToolTip(session.unreadableDetails());
    technical_status_->setText(session.technical());
    technical_status_->setToolTip(session.technical());
    apply_summary_->setText(session.applySummary());
    if (loading_ != nullptr) {
        loading_->setText(session.loadingText());
    }

    undo_button_->setEnabled(session.canUndo());
    redo_button_->setEnabled(session.canRedo());
    discard_button_->setEnabled(session.draftCount() > 0);
    add_field_button_->setEnabled(session.canAddField());
    remove_field_button_->setEnabled(session.canRemoveFields());
    edit_values_button_->setEnabled(session.canEditValues());

    const auto transform = session.canTransform();
    transform_button_->setEnabled(transform);
    suggest_button_->setEnabled(session.canSuggest());
    suggest_action_->setEnabled(session.canSuggest());
    identify_button_->setEnabled(session.canIdentify());
    identify_albums_button_->setEnabled(session.itemCount() > 0U &&
                                        static_cast<bool>(session.services().musicbrainz.fetch));
    replaygain_scan_button_->setEnabled(session.canScanReplayGain());
    replaygain_provenance_button_->setEnabled(session.canShowProvenance());
    transformation_list_->setEnabled(!session.scriptsLoading() && !session.scripts().empty() &&
                                     transform);
    transform_button_->setText(transformation_list_->currentItem() == nullptr
                                   ? QStringLiteral("Open script editor…")
                                   : QStringLiteral("Edit selected script…"));
    const auto script_status = session.scriptStatus();
    transformation_status_->setText(script_status);
    transformation_status_->setVisible(!script_status.isEmpty());

    {
        const QSignalBlocker save_blocker{save_tags_check_};
        const QSignalBlocker rename_blocker{rename_files_check_};
        const QSignalBlocker move_blocker{move_files_check_};
        const QSignalBlocker layout_blocker{output_layout_combo_};
        const QSignalBlocker destination_blocker{destination_combo_};
        const QSignalBlocker grouping_blocker{replaygain_grouping_};
        save_tags_check_->setChecked(session.saveTags());
        rename_files_check_->setEnabled(session.renameAvailable());
        rename_files_check_->setChecked(session.renameFiles());
        rename_files_check_->setToolTip(session.renameTooltip());
        move_files_check_->setEnabled(session.moveAvailable());
        move_files_check_->setChecked(session.moveFiles());
        move_files_check_->setToolTip(session.moveTooltip());
        output_layout_combo_->setEnabled(session.layoutsAvailable());
        destination_combo_->setEnabled(session.destinationsAvailable());
        if (output_layout_combo_->currentIndex() != session.layoutIndex()) {
            output_layout_combo_->setCurrentIndex(session.layoutIndex());
        }
        if (destination_combo_->currentIndex() != session.destinationIndex()) {
            destination_combo_->setCurrentIndex(session.destinationIndex());
        }
        replaygain_grouping_->setCurrentIndex(session.replayGainGrouping());
    }
    replaygain_expression_->setVisible(session.replayGainGrouping() == 4);
    output_profile_status_->setText(session.outputProfileStatus());

    apply_plan_button_->setEnabled(session.canApply());
    apply_progress_bar_->setVisible(session.progressVisible());
    apply_stop_button_->setVisible(session.progressVisible());
    apply_stop_button_->setEnabled(session.canStopApply());
    if (session.progressVisible()) {
        apply_progress_bar_->setRange(0, session.progressMaximum());
        apply_progress_bar_->setValue(session.progressValue());
    }
    if (file_list_ != nullptr && artwork_section_ != nullptr) {
        file_list_->setEnabled(session.fileListEnabled());
    }
    field_layout_remove_action_->setEnabled(!session.activeFieldLayout().isEmpty());
    if (metadata_splitter_ != nullptr) {
        // ADR-0261: Identify albums… is writing some of these files.
        metadata_splitter_->setEnabled(!session.writingElsewhere());
    }
    syncActionsPopover();
}

void MetadataPropertiesDialog::rebuildScripts(const QString& selected) {
    const QSignalBlocker blocker{transformation_list_};
    transformation_list_->clear();
    QListWidgetItem* selected_item = nullptr;
    for (const auto& script : session_->scripts()) {
        auto* item = new QListWidgetItem(script.name, transformation_list_);
        item->setData(Qt::UserRole, script.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(script.automatic ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(script.automatic
                             ? QStringLiteral("Staged automatically as colored draft edits")
                             : QStringLiteral("Not staged automatically"));
        if (!selected.isEmpty() && script.id == selected) {
            selected_item = item;
        }
    }
    if (selected_item != nullptr) {
        transformation_list_->setCurrentItem(selected_item);
    } else if (transformation_list_->count() > 0) {
        transformation_list_->setCurrentRow(0);
    }
    sync();
}

void MetadataPropertiesDialog::rebuildOutputProfiles() {
    {
        const QSignalBlocker layout_blocker{output_layout_combo_};
        const QSignalBlocker destination_blocker{destination_combo_};
        output_layout_combo_->clear();
        for (const auto& layout : session_->layouts()) {
            output_layout_combo_->addItem(layout.name, layout.id);
        }
        destination_combo_->clear();
        for (const auto& destination : session_->destinations()) {
            destination_combo_->addItem(destination.name, destination.id);
        }
        // ADR-0262: a folder that is not a saved destination.
        destination_combo_->addItem(QStringLiteral("Choose folder…"), choose_folder_item);
    }
    sync();
}

void MetadataPropertiesDialog::rebuildFieldLayouts() {
    {
        const QSignalBlocker blocker{field_layout_combo_};
        while (field_layout_combo_->count() > 1) {
            field_layout_combo_->removeItem(1);
        }
        for (const auto& layout : session_->fieldLayouts()) {
            field_layout_combo_->addItem(layout.name, layout.id);
        }
        field_layout_combo_->setCurrentIndex(
            std::max(0, field_layout_combo_->findData(session_->activeFieldLayout())));
    }
    const auto has_sets = !session_->fieldLayouts().empty();
    field_layout_label_->setVisible(has_sets);
    field_layout_combo_->setVisible(has_sets);
    if (field_review_bar_ != nullptr) {
        field_review_bar_->setLayoutFields(session_->activeFieldLayoutFields());
    }
    sync();
}

void MetadataPropertiesDialog::reloadOutputProfiles() { session_->reloadOutputProfiles(); }

QTableView* MetadataPropertiesDialog::fileListView() { return file_list_; }

void MetadataPropertiesDialog::buildGrid() {
    auto* grid_model = session_->gridModel();
    auto* aggregate_model = session_->aggregateModel();

    // ADR-0221: the tagger is its own window, so its file list lives beside
    // the field table rather than stacked above it. A vertical split spent
    // scarce height on a narrow path column; horizontal gives the paths a tall
    // column and the fields the full height.
    metadata_splitter_ = new QSplitter(Qt::Horizontal, this);
    metadata_splitter_->setObjectName(QStringLiteral("bench-metadata-splitter"));
    metadata_splitter_->setChildrenCollapsible(false);

    // The file list and its breadcrumb travel together as one splitter pane.
    auto* file_pane = new Band(Band::Edge::none, metadata_splitter_);
    file_pane->setObjectName(QStringLiteral("bench-metadata-files-pane"));
    auto* file_pane_layout = new QVBoxLayout(file_pane);
    file_pane_layout->setContentsMargins(TrackknifeStyle::gap, TrackknifeStyle::gap,
                                         TrackknifeStyle::gap, TrackknifeStyle::gap);
    file_pane_layout->setSpacing(TrackknifeStyle::gap_small);
    file_list_dir_ = new QLabel(file_pane);
    file_list_dir_->setObjectName(QStringLiteral("bench-metadata-files-dir"));
    file_list_dir_->setTextFormat(Qt::PlainText);
    file_list_dir_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    file_list_dir_->setContentsMargins(TrackknifeStyle::gap, TrackknifeStyle::gap_small, 0, 0);
    {
        auto quiet = file_list_dir_->palette();
        quiet.setColor(QPalette::WindowText, TrackknifeStyle::dim(quiet));
        file_list_dir_->setPalette(quiet);
        auto font = file_list_dir_->font();
        font.setPointSizeF(font.pointSizeF() * 0.9);
        file_list_dir_->setFont(font);
    }
    file_list_dir_->hide();
    file_pane_layout->addWidget(file_list_dir_);
    file_list_ = new FileScopeView(file_pane);
    file_list_->setObjectName(QStringLiteral("bench-metadata-files"));
    file_list_->setAccessibleName(QStringLiteral("Files included in metadata edit"));
    file_list_->setModel(grid_model);
    auto* initial_selection = file_list_->selectionModel();
    file_list_->setSelectionModel(session_->fileSelection());
    delete initial_selection;
    // On the pane's own shade, no frame and no stripes; the check boxes say
    // which files are in the edit, so rows show no second selection.
    file_list_->setAlternatingRowColors(false);
    file_list_->setShowGrid(false);
    file_list_->setFrameShape(QFrame::NoFrame);
    file_list_->viewport()->setAutoFillBackground(false);
    file_list_->setProperty(TrackknifeStyle::checks_show_selection, true);
    file_list_->verticalHeader()->setDefaultSectionSize(30);
    file_list_->setWordWrap(false);
    // Paths render relative to the selection's common folder, so a
    // single-album edit shows plain filenames; the folder itself is the
    // breadcrumb above. Eliding right keeps the start of the name visible.
    file_list_->setTextElideMode(Qt::ElideRight);
    file_list_->setSelectionBehavior(QAbstractItemView::SelectRows);
    file_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    file_list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    file_list_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    file_list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    file_list_->verticalHeader()->hide();
    file_list_->horizontalHeader()->hide();
    file_list_->setMinimumWidth(180);
    file_list_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (auto column = 1; column < grid_model->columnCount(); ++column) {
        file_list_->hideColumn(column);
    }
    file_pane_layout->addWidget(file_list_, 1);
    // The grid fills asynchronously; the breadcrumb follows whatever arrives.
    connect(grid_model, &QAbstractItemModel::modelReset, this,
            &MetadataPropertiesDialog::refreshFileListScope);
    connect(grid_model, &QAbstractItemModel::rowsInserted, this,
            &MetadataPropertiesDialog::refreshFileListScope);
    connect(grid_model, &QAbstractItemModel::rowsRemoved, this,
            &MetadataPropertiesDialog::refreshFileListScope);
    refreshFileListScope();
    connect(grid_model, &QAbstractItemModel::columnsInserted, file_list_,
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
    fields_->setModel(aggregate_model);
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
    fields_->verticalHeader()->setDefaultSectionSize(26);
    fields_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    fields_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    fields_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    fields_->setColumnWidth(0, 190);

    auto* fields_pane = new QWidget(metadata_splitter_);
    fields_pane->setObjectName(QStringLiteral("bench-metadata-fields-pane"));
    auto* fields_pane_layout = new QVBoxLayout(fields_pane);
    fields_pane_layout->setContentsMargins(0, TrackknifeStyle::gap_large, 0, 0);
    fields_pane_layout->setSpacing(TrackknifeStyle::gap);
    field_review_bar_ =
        new MetadataFieldReviewBar(fields_, aggregate_model, file_list_, fields_pane);
    field_review_bar_->setLayoutFields(session_->activeFieldLayoutFields());
    fields_pane_layout->addWidget(field_review_bar_);
    grid_tools_->setParent(fields_pane);
    fields_pane_layout->addWidget(grid_tools_);
    auto* fields_body = new QHBoxLayout;
    fields_body->addWidget(fields_, 1);
    fields_pane_layout->addLayout(fields_body, 1);
    grid_tools_->show();

    auto* sections_pane = new QWidget(metadata_splitter_);
    sections_pane->setObjectName(QStringLiteral("bench-metadata-sections-pane"));
    auto* sections_layout = new QVBoxLayout(sections_pane);
    sections_layout->setContentsMargins(TrackknifeStyle::gap_large, TrackknifeStyle::gap,
                                        TrackknifeStyle::gap_large, TrackknifeStyle::gap_large);
    sections_layout->setSpacing(0);
    metadata_sections_ = new QTabWidget(sections_pane);
    sections_layout->addWidget(metadata_sections_);
    metadata_sections_->setDocumentMode(true);
    metadata_sections_->setObjectName(QStringLiteral("bench-metadata-sections"));
    metadata_sections_->setAccessibleName(QStringLiteral("Metadata property sections"));
    // Artwork's optional draft and problem tables must scroll inside their
    // pane, not raise the splitter minimum and crush the selected-files list.
    metadata_sections_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    metadata_sections_->addTab(fields_pane, QStringLiteral("Fields"));
    artwork_section_ = new MetadataArtworkSection(metadata_sections_);
    artwork_section_->setFileWorkTools(session_->services().tools);
    fields_body->addWidget(artwork_section_->createCompactCover(fields_pane));
    connect(artwork_section_, &MetadataArtworkSection::openArtworkRequested, this,
            [this] { metadata_sections_->setCurrentWidget(artwork_section_); });
    connect(artwork_section_, &MetadataArtworkSection::coverSettingsRequested, this,
            [this] { emit openSettingsRequested(SettingsDialog::Page::covers); });
    artwork_section_->setActive(true);
    artwork_section_->setUnifiedApply(static_cast<bool>(session_->services().plan_applier_factory));
    artwork_section_->setMutationServices(session_->artworkApplierFactory(),
                                          session_->artworkAppliedObserver());
    if (auto service = session_->coverArtService()) {
        artwork_section_->setCoverArtService(std::move(*service));
    }
    const auto artwork_page =
        metadata_sections_->addTab(artwork_section_, QStringLiteral("Artwork"));
    connect(artwork_section_, &MetadataArtworkSection::operationRunningChanged, session_,
            &TaggerSession::setArtworkOperationRunning);
    connect(artwork_section_, &MetadataArtworkSection::pendingChangesChanged, session_,
            &TaggerSession::artworkStateChanged);
    connect(metadata_sections_, &QTabWidget::currentChanged, this,
            [this, artwork_page](const int index) {
                if (artwork_section_ != nullptr) {
                    artwork_section_->setActive(index == 0 || index == artwork_page);
                }
            });
    session_->setArtwork(&artwork_section_->session());

    // Two panes: the files with their folder above them, and the sections.
    // Adding the list itself took it out of its pane and left the folder
    // label standing as a column of its own.
    metadata_splitter_->addWidget(file_pane);
    metadata_splitter_->addWidget(sections_pane);
    metadata_splitter_->setStretchFactor(0, 1);
    metadata_splitter_->setStretchFactor(1, 3);
    metadata_splitter_->setSizes({170, 390});
    if (!pending_metadata_splitter_state_.isEmpty()) {
        static_cast<void>(metadata_splitter_->restoreState(pending_metadata_splitter_state_));
        // A saved state carries its orientation: one from when the files
        // stood above the fields would stack them again.
        metadata_splitter_->setOrientation(Qt::Horizontal);
    }
    transformation_panel_->setParent(this);
    transformation_panel_->hide();
    root_layout_->insertWidget(root_layout_->count() - 1, metadata_splitter_, 1);
    emit fileListConstructed();

    connect(fields_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MetadataPropertiesDialog::noteFieldSelection);
    connect(fields_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MetadataPropertiesDialog::noteFieldSelection);

    root_layout_->removeWidget(loading_);
    loading_->deleteLater();
    loading_ = nullptr;
}

void MetadataPropertiesDialog::fillGrid() {
    if (fields_ != nullptr && session_->aggregateModel()->rowCount() > 0) {
        fields_->setCurrentIndex(session_->aggregateModel()->index(0, 2));
    }
    noteFieldSelection();
}

void MetadataPropertiesDialog::noteFieldSelection() {
    if (fields_ == nullptr || fields_->selectionModel() == nullptr) {
        return;
    }
    session_->setFieldSelection(!fields_->selectionModel()->selectedRows(0).empty(),
                                fields_->currentIndex().isValid());
}

// ADR-0221: the file list belongs to this window. Rows render relative to the
// selection's common folder, which is shown once as a breadcrumb above them --
// a one-album edit then reads as plain filenames instead of repeating the same
// long path on every row.
void MetadataPropertiesDialog::refreshFileListScope() {
    if (file_list_ == nullptr) {
        return;
    }
    const auto common_dir = session_->commonFolder();
    if (file_list_dir_ != nullptr) {
        file_list_dir_->setText(common_dir);
        file_list_dir_->setToolTip(common_dir);
        file_list_dir_->setVisible(!common_dir.isEmpty());
    }
    file_list_->itemDelegate()->setProperty("relative-prefix", common_dir);
    file_list_->viewport()->update();
}

// ADR-0186: the Actions popover is the apply-options surface — its choices
// go to the session, preset lists select the saved profiles, and Manage
// entries open the matching Settings page.
void MetadataPropertiesDialog::showActionsPopover() {
    // Built afresh each time from the controls that show the state, so it
    // can never show anything else.
    delete actions_popover_.data();
    actions_popover_ = new QFrame(this, Qt::Popup);
    actions_popover_->setObjectName(QStringLiteral("bench-metadata-actions-popover"));
    actions_popover_->setAttribute(Qt::WA_DeleteOnClose);
    actions_popover_->setFrameShape(QFrame::StyledPanel);
    auto* grid = new QGridLayout(actions_popover_);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    const auto link = [this](const QString& text, const QString& object_name, auto&& activated) {
        auto* label = new QLabel(QStringLiteral("<a href=\"#\">%1</a>").arg(text.toHtmlEscaped()),
                                 actions_popover_);
        label->setObjectName(object_name);
        label->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        connect(label, &QLabel::linkActivated, this,
                [this, activated = std::forward<decltype(activated)>(activated)] {
                    actions_popover_->close();
                    activated();
                });
        return label;
    };
    const auto copy_items = [](QComboBox* from, QComboBox* to) {
        for (int index = 0; index < from->count(); ++index) {
            to->addItem(from->itemText(index), from->itemData(index));
        }
        to->setCurrentIndex(from->currentIndex());
        to->setPlaceholderText(from->placeholderText());
        to->setToolTip(from->toolTip());
    };
    int row = 0;

    actions_save_tags_ = new QCheckBox(QStringLiteral("Save tags"), actions_popover_);
    actions_save_tags_->setObjectName(QStringLiteral("bench-actions-save-tags"));
    connect(actions_save_tags_, &QCheckBox::clicked, this, [this](const bool checked) {
        session_->setSaveTags(checked);
        session_->rememberActionChoices();
    });
    grid->addWidget(actions_save_tags_, row++, 0, 1, 2);

    actions_rename_ = new QCheckBox(QStringLiteral("Rename files"), actions_popover_);
    actions_rename_->setObjectName(QStringLiteral("bench-actions-rename-files"));
    connect(actions_rename_, &QCheckBox::clicked, session_, &TaggerSession::chooseRename);
    actions_layout_ = new QComboBox(actions_popover_);
    actions_layout_->setObjectName(QStringLiteral("bench-actions-layout"));
    actions_layout_->setAccessibleName(QStringLiteral("Naming layout"));
    copy_items(output_layout_combo_, actions_layout_);
    connect(actions_layout_, &QComboBox::activated, this, [this](const int index) {
        session_->selectLayout(index);
        session_->rememberActionChoices();
    });
    grid->addWidget(actions_rename_, row, 0);
    grid->addWidget(actions_layout_, row++, 1);

    actions_move_ = new QCheckBox(QStringLiteral("Move files"), actions_popover_);
    actions_move_->setObjectName(QStringLiteral("bench-actions-move-files"));
    connect(actions_move_, &QCheckBox::clicked, session_, &TaggerSession::chooseMove);
    actions_destination_ = new QComboBox(actions_popover_);
    actions_destination_->setObjectName(QStringLiteral("bench-actions-destination"));
    actions_destination_->setAccessibleName(QStringLiteral("Move destination"));
    copy_items(destination_combo_, actions_destination_);
    connect(actions_destination_, &QComboBox::activated, this, [this](const int index) {
        destinationActivated(actions_destination_, index);
    });
    grid->addWidget(actions_move_, row, 0);
    grid->addWidget(actions_destination_, row++, 1);
    auto* manage = new QHBoxLayout;
    manage->setSpacing(16);
    manage->addWidget(link(QStringLiteral("Manage naming layouts…"),
                           QStringLiteral("bench-actions-manage-layouts"),
                           [this] { emit openSettingsRequested(SettingsDialog::Page::naming); }));
    manage->addWidget(link(QStringLiteral("Manage move destinations…"),
                           QStringLiteral("bench-actions-manage-destinations"),
                           [this] { emit openDestinationsRequested(); }));
    manage->addStretch(1);
    grid->addLayout(manage, row++, 1);

    // Measured when applying, and written with the rest.
    actions_replaygain_ = new QCheckBox(QStringLiteral("ReplayGain"), actions_popover_);
    actions_replaygain_->setObjectName(QStringLiteral("bench-actions-replaygain"));
    connect(actions_replaygain_, &QCheckBox::clicked, session_, &TaggerSession::chooseReplayGain);
    actions_grouping_ = new QComboBox(actions_popover_);
    actions_grouping_->setObjectName(QStringLiteral("bench-actions-replaygain-grouping"));
    actions_grouping_->setAccessibleName(QStringLiteral("ReplayGain grouping"));
    copy_items(replaygain_grouping_, actions_grouping_);
    connect(actions_grouping_, &QComboBox::activated, this, [this](const int index) {
        session_->setReplayGainGrouping(index);
        if (index == 4) {
            bool accepted = false;
            const auto expression = QInputDialog::getText(
                this, QStringLiteral("Group by expression"), QStringLiteral("tkfmt-1 expression:"),
                QLineEdit::Normal, session_->replayGainExpression(), &accepted);
            if (accepted) {
                replaygain_expression_->setText(expression);
            }
        }
    });
    actions_skip_gain_ =
        new QCheckBox(QStringLiteral("Skip existing"), actions_popover_);
    actions_skip_gain_->setObjectName(QStringLiteral("bench-actions-replaygain-skip-existing"));
    actions_skip_gain_->setToolTip(
        QStringLiteral("Leave albums alone whose files all have the gain already (tracks, for "
                       "track gains only)"));
    connect(actions_skip_gain_, &QCheckBox::clicked, session_,
            &TaggerSession::chooseSkipExistingGain);
    auto* replaygain_row = new QHBoxLayout;
    replaygain_row->addWidget(actions_grouping_, 1);
    replaygain_row->addWidget(actions_skip_gain_);
    grid->addWidget(actions_replaygain_, row, 0);
    grid->addLayout(replaygain_row, row++, 1);
    auto* replaygain_links = new QHBoxLayout;
    replaygain_links->setSpacing(16);
    auto* sources =
        link(QStringLiteral("Loudness sources…"), QStringLiteral("bench-actions-loudness-sources"),
             [this] { replaygain_provenance_button_->click(); });
    sources->setEnabled(replaygain_provenance_button_->isEnabled());
    replaygain_links->addWidget(sources);
    replaygain_links->addWidget(link(
        QStringLiteral("ReplayGain settings…"), QStringLiteral("bench-actions-replaygain-settings"),
        [this] { emit openSettingsRequested(SettingsDialog::Page::replaygain); }));
    replaygain_links->addStretch(1);
    grid->addLayout(replaygain_links, row++, 1);

    auto* scripts = new QLabel(QStringLiteral("Scripts"), actions_popover_);
    grid->addWidget(scripts, row, 0, Qt::AlignTop);
    auto* script_column = new QVBoxLayout;
    for (int index = 0; index < transformation_list_->count(); ++index) {
        auto* item = transformation_list_->item(index);
        auto* choice = new QCheckBox(item->text(), actions_popover_);
        choice->setObjectName(QStringLiteral("bench-actions-script-%1").arg(index));
        choice->setChecked(item->checkState() == Qt::Checked);
        connect(choice, &QCheckBox::toggled, this, [this, index](const bool checked) {
            if (auto* target = transformation_list_->item(index)) {
                target->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            }
        });
        script_column->addWidget(choice);
    }
    auto* editor =
        link(QStringLiteral("Open script editor…"), QStringLiteral("bench-actions-script-editor"),
             [this] { transform_button_->click(); });
    editor->setEnabled(transform_button_->isEnabled());
    script_column->addWidget(editor);
    grid->addLayout(script_column, row++, 1);

    syncActionsPopover();
    actions_popover_->adjustSize();
    // Above the button: the footer is at the window's foot. Below it when
    // there is no room above, and never past the screen's edges.
    const auto size = actions_popover_->sizeHint();
    auto at = actions_button_->mapToGlobal(QPoint{0, -size.height()});
    if (const auto* screen = actions_button_->screen()) {
        const auto area = screen->availableGeometry();
        if (at.y() < area.top()) {
            at.setY(actions_button_->mapToGlobal(QPoint{0, actions_button_->height()}).y());
        }
        at.setX(
            std::clamp(at.x(), area.left(), std::max(area.left(), area.right() - size.width())));
    }
    actions_popover_->move(at);
    actions_popover_->show();
}

void MetadataPropertiesDialog::syncActionsPopover() {
    if (actions_popover_ == nullptr || actions_save_tags_ == nullptr) {
        return;
    }
    const auto mirror = [](QCheckBox* shown, const QCheckBox* state) {
        const QSignalBlocker blocker{shown};
        shown->setChecked(state->isChecked());
        shown->setEnabled(state->isEnabled());
        shown->setToolTip(state->toolTip());
    };
    mirror(actions_save_tags_, save_tags_check_);
    mirror(actions_rename_, rename_files_check_);
    mirror(actions_move_, move_files_check_);
    actions_layout_->setEnabled(output_layout_combo_->isEnabled());
    actions_destination_->setEnabled(destination_combo_->isEnabled());
    {
        const QSignalBlocker replaygain_blocker{actions_replaygain_};
        const QSignalBlocker skip_blocker{actions_skip_gain_};
        actions_replaygain_->setChecked(session_->replayGainOnApply());
        actions_skip_gain_->setChecked(session_->skipExistingGain());
    }
    actions_grouping_->setEnabled(session_->replayGainOnApply());
    actions_skip_gain_->setEnabled(session_->replayGainOnApply());
}

void MetadataPropertiesDialog::destinationActivated(QComboBox* combo, const int index) {
    if (combo->itemData(index).toString() != choose_folder_item) {
        session_->selectDestination(index);
        session_->rememberActionChoices();
        return;
    }
    // Shown as it was until a folder is chosen.
    {
        const QSignalBlocker blocker{combo};
        combo->setCurrentIndex(session_->destinationIndex());
    }
    chooseMoveFolder();
}

void MetadataPropertiesDialog::chooseMoveFolder() {
    const QPointer session{session_};
    const auto chosen = [session](const QByteArray& raw_path) {
        if (session && !raw_path.isEmpty()) {
            session->chooseMoveFolder(
                std::string{raw_path.constData(), static_cast<std::size_t>(raw_path.size())});
        }
    };
    // Test seam: a folder given skips the chooser.
    if (const auto given = property("trackknife-move-folder").toByteArray(); !given.isEmpty()) {
        chosen(given);
        return;
    }
    const auto start = session_->moveFolderStart();
    if (auto lister = session_->moveFolderLister()) {
        auto* chooser = new EngineFolderDialog(session_->destinationsOn(), std::move(lister), start,
                                               this);
        connect(chooser, &EngineFolderDialog::folderChosen, this, chosen);
        chooser->show();
        return;
    }
    const auto selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Move into folder"),
        QFile::decodeName(QByteArray{start.data(), static_cast<qsizetype>(start.size())}),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!selected.isEmpty()) {
        chosen(QFile::encodeName(selected));
    }
}

void MetadataPropertiesDialog::startIdentify() {
    auto request = session_->identifyRequest();
    if (!request) {
        return;
    }
    auto* dialog = createMusicBrainzIdentifyDialog(
        session_->services().musicbrainz, std::move(request->descriptors),
        std::move(request->local_paths), std::move(request->items), request->artist,
        request->release,
        [session = QPointer{session_}](metadata::MetadataProposalSet proposals) {
            if (session) {
                session->applyMusicBrainzProposals(std::move(proposals));
            }
        },
        this);
    identify_dialog_ = dialog;
    session_->setIdentifyDialogOpen(true);
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (identify_dialog_ == dialog) {
            identify_dialog_ = nullptr;
            session_->setIdentifyDialogOpen(false);
        }
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MetadataPropertiesDialog::exportReplayGainResults() {
    if (!session_->hasReplayGainExport()) {
        return;
    }
    // Test seam: an explicit path skips the file dialog.
    auto path = property("trackknife-replaygain-export-path").toString();
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Export ReplayGain results"),
                                            QStringLiteral("replaygain-results.csv"),
                                            QStringLiteral("CSV files (*.csv)"));
    }
    session_->exportReplayGainResults(path);
}

void MetadataPropertiesDialog::showLoudnessProvenance() {
    const auto provenance = session_->loudnessProvenance();
    if (provenance.rows.empty()) {
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("bench-replaygain-provenance-dialog"));
    dialog->setWindowTitle(QStringLiteral("Loudness sources"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(dialog);
    auto* table = new QTableWidget(static_cast<int>(provenance.rows.size()),
                                   static_cast<int>(provenance.headers.size()), dialog);
    table->setObjectName(QStringLiteral("bench-replaygain-provenance-table"));
    table->setHorizontalHeaderLabels(provenance.headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->verticalHeader()->hide();
    table->setWordWrap(false);
    for (int row = 0; row < static_cast<int>(provenance.rows.size()); ++row) {
        const auto& cells = provenance.rows[static_cast<std::size_t>(row)];
        for (int column = 0; column < cells.size(); ++column) {
            table->setItem(row, column, new QTableWidgetItem(cells.at(column)));
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

void MetadataPropertiesDialog::showPreparationFeedback(const QString& window_title,
                                                       const QString& summary,
                                                       std::vector<PreparationFeedbackRow> rows,
                                                       const bool retry_offered) {
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
        session_->feedbackFinished();
    });
    // Retry the reviewed per-file intent, retaining its original revision and
    // fingerprints -- never rebuilt from displayed or freshly read tags.
    if (retry_offered) {
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("bench-preparation-feedback-buttons"));
        auto* retry_button = buttons->addButton(QStringLiteral("Retry failed / stopped files"),
                                                QDialogButtonBox::ActionRole);
        retry_button->setObjectName(QStringLiteral("bench-preparation-retry"));
        retry_button->setToolTip(
            QStringLiteral("Retry only unfinished files using the reviewed changes. Changed "
                           "files and unresolved recovery records remain blocked."));
        if (session_->applyCommitted()) {
            buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Close editor"));
        }
        connect(retry_button, &QPushButton::clicked, this, [this, dialog] {
            dialog->setProperty("retry-starting", true);
            dialog->close();
            session_->retryUnfinished();
        });
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MetadataPropertiesDialog::promptAddField() {
    if (field_name_dialog_ != nullptr) {
        field_name_dialog_->raise();
        field_name_dialog_->activateWindow();
        return;
    }
    if (!session_->selectionReady()) {
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
    completion_model->setStringList(session_->fieldNameSuggestions({}));
    connect(prompt, &QInputDialog::textValueChanged, this,
            [this, prompt, field_name, completion_model, completer](const QString& text) {
                completion_model->setStringList(session_->fieldNameSuggestions(text));
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
    session_->setFieldNameDialogOpen(true);
    connect(prompt, &QDialog::accepted, this, [this, prompt] {
        const auto row = session_->addField(prompt->textValue());
        if (row >= 0) {
            prompt->setProperty("trackknifeFieldRow", row);
        }
    });
    connect(prompt, &QDialog::finished, this, [this, prompt] {
        bool has_field_row = false;
        const auto field_row = prompt->property("trackknifeFieldRow").toInt(&has_field_row);
        if (field_name_dialog_ == prompt) {
            field_name_dialog_ = nullptr;
        }
        session_->setFieldNameDialogOpen(false);
        if (has_field_row && fields_ != nullptr) {
            field_review_bar_->revealField(field_row);
            const auto draft = session_->aggregateModel()->index(field_row, 2);
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
    if (fields_ == nullptr || fields_->selectionModel() == nullptr) {
        return;
    }
    static_cast<void>(
        session_->aggregateModel()->removeIndexes(fields_->selectionModel()->selectedRows(0)));
}

void MetadataPropertiesDialog::editCurrentValues() {
    if (exact_values_dialog_ != nullptr) {
        exact_values_dialog_->raise();
        exact_values_dialog_->activateWindow();
        return;
    }
    if (fields_ == nullptr || !session_->selectionReady()) {
        return;
    }
    const auto current = fields_->currentIndex();
    if (!current.isValid()) {
        return;
    }

    const auto exact = session_->exactValues(current.row());
    if (!exact) {
        return;
    }
    auto* editor =
        createMetadataExactValueDialog(exact->heading, exact->context, exact->values, this);
    exact_values_dialog_ = editor;
    session_->setExactValuesDialogOpen(true);
    const QPersistentModelIndex target{session_->aggregateModel()->index(exact->row, 2)};
    connect(editor, &QDialog::accepted, this, [this, editor, target] {
        if (target.isValid()) {
            session_->replaceValues(target.row(), metadataExactValueDialogValues(editor));
        }
    });
    connect(editor, &QDialog::finished, this, [this, editor] {
        if (exact_values_dialog_ == editor) {
            exact_values_dialog_ = nullptr;
        }
        session_->setExactValuesDialogOpen(false);
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
    if (!session_->canTransform()) {
        return;
    }
    auto items = session_->selectedItems();
    if (items.empty()) {
        return;
    }
    auto* grid_model = session_->gridModel();
    QStringList labels;
    labels.reserve(grid_model->rowCount());
    for (auto row = 0; row < grid_model->rowCount(); ++row) {
        labels.push_back(grid_model->trackLabel(row));
    }
    auto* dialog = createMetadataTransformationDialog(
        grid_model->sharedSelection(), grid_model->patches(), std::move(items), std::move(labels),
        [session = QPointer{session_}](const metadata::MetadataTransformationPreview& preview) {
            return session && session->stageTransformation(preview);
        },
        session_->services().transformation_store, this, initially_selected,
        preview_initially_selected, session_->services().layout_store);
    transformation_dialog_ = dialog;
    session_->setTransformationDialogOpen(true);
    connect(dialog, &QDialog::finished, this, [this, dialog, initially_selected] {
        if (transformation_dialog_ == dialog) {
            transformation_dialog_ = nullptr;
        }
        session_->setTransformationDialogOpen(false);
        session_->reloadScripts(initially_selected);
    });
    dialog->open();
}

void MetadataPropertiesDialog::saveCurrentFieldLayout() {
    if (fields_ == nullptr || field_review_bar_ == nullptr ||
        session_->fieldLayouts().size() >= 64U) {
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
    static_cast<void>(session_->saveFieldLayout(name, std::move(field_names)));
}

void MetadataPropertiesDialog::persistLayoutState() {
    session_->storeLayoutState(saveGeometry(), metadata_splitter_ != nullptr
                                                   ? metadata_splitter_->saveState()
                                                   : QByteArray{});
}

bool MetadataPropertiesDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == fields_ && event->type() == QEvent::KeyPress &&
        session_->gridModel() != nullptr) {
        const auto* key = static_cast<QKeyEvent*>(event);
        auto* grid_model = session_->gridModel();
        auto* aggregate_model = session_->aggregateModel();
        if (key->matches(QKeySequence::Undo)) {
            return grid_model->undo();
        }
        if (key->matches(QKeySequence::Redo)) {
            return grid_model->redo();
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
            return aggregate_model->removeIndexes(fields_->selectionModel()->selectedIndexes());
        }
        if (key->key() == Qt::Key_Backspace && key->modifiers() == Qt::ControlModifier) {
            return aggregate_model->revertIndexes(fields_->selectionModel()->selectedIndexes());
        }
    }
    return QDialog::eventFilter(watched, event);
}

void MetadataPropertiesDialog::reject() {
    // Escape must use the same unsaved-draft and cancellation checks as closing the tab.
    close();
}

void MetadataPropertiesDialog::closeEvent(QCloseEvent* event) {
    auto answer = session_->requestClose();
    if (answer == TaggerSession::CloseAnswer::confirm_artwork) {
        const auto discard =
            QMessageBox::warning(this, QStringLiteral("Discard artwork changes?"),
                                 QStringLiteral("The pending artwork changes have not been saved."),
                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (discard != QMessageBox::Discard) {
            event->ignore();
            return;
        }
        artwork_section_->discardPendingChanges();
        answer = session_->requestClose();
    }
    if (answer == TaggerSession::CloseAnswer::wait) {
        event->ignore();
        return;
    }
    if (answer == TaggerSession::CloseAnswer::confirm_drafts) {
        const auto count = session_->draftCount();
        const auto discard = QMessageBox::warning(
            this, QStringLiteral("Discard metadata draft?"),
            QStringLiteral("%1 staged %2 exist only in memory and have not been written to files.")
                .arg(count)
                .arg(count == 1 ? QStringLiteral("change") : QStringLiteral("changes")),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (discard != QMessageBox::Discard) {
            event->ignore();
            return;
        }
        session_->closing(true);
    } else {
        session_->closing(false);
    }
    persistLayoutState();
    event->accept();
}

} // namespace trackknife::bench
