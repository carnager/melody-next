// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/metadata_artwork_section.hpp"
#include "bench/musicbrainz_identify_dialog.hpp"
#include "bench/output_profiles_widget.hpp"
#include "bench/preparation_feedback_dialog.hpp"
#include "bench/settings_dialog.hpp"
#include "workspace/tagger_session.hpp"

#include <QByteArray>
#include <QDialog>
#include <QPointer>
#include <QStringList>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

class QCloseEvent;
class QCheckBox;
class QFrame;
class QComboBox;
class QDialogButtonBox;
class QEvent;
class QLabel;
class QInputDialog;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QAction;
class QToolButton;
class QSplitter;
class QTabWidget;
class QTableView;
class QVBoxLayout;

namespace trackknife::bench {

class MetadataFieldReviewBar;

// The "Edit tags" window of the widgets workspace: a view over a
// TaggerSession, which decides everything it shows and does (ADR-0220).
class MetadataPropertiesDialog final : public QDialog {
    Q_OBJECT

  signals:
    // ADR-0183 addendum: the file list exists only after the asynchronous
    // grid build; sidebar hosting waits for this.
    void fileListConstructed();
    // ADR-0185/0186: the Actions menu and Edit buttons ask the bench
    // window to open Settings on a specific page.
    void openSettingsRequested(SettingsDialog::Page page);
    // The move destinations of the engine these tracks are on -- not the
    // naming layouts, which openSettingsRequested(naming) shows.
    void openDestinationsRequested();
    // Something worth a line in the window's status bar once this closes:
    // an update that went through but could not keep everything.
    void statusMessage(const QString& message);

  public:
    MetadataPropertiesDialog(std::size_t requested_item_count,
                             MetadataPropertiesSourceReader source_reader,
                             std::span<const std::string_view> preferred_fields,
                             MetadataWritePlanApplierFactory plan_applier_factory,
                             MetadataApplyObserver apply_observer,
                             MetadataTransformationStore transformation_store = {},
                             OutputProfileStore output_profile_store = {},
                             FilePublicationPlanApplierFactory file_plan_applier_factory = {},
                             FilePublicationApplyObserver file_apply_observer = {},
                             QWidget* parent = nullptr, MetadataDialogLayoutStore layout_store = {},
                             MusicBrainzLookupService musicbrainz = {}, FileWorkTools tools = {});
    MetadataPropertiesDialog(std::size_t requested_item_count,
                             MetadataPropertiesSourceReader source_reader,
                             std::span<const std::string_view> preferred_fields,
                             TaggerServices services, QWidget* parent = nullptr);
    ~MetadataPropertiesDialog() override;

    void setArtworkMutationServices(ArtworkWritePlanApplierFactory applier_factory,
                                    ArtworkApplyObserver observer);
    // ADR-0183 addendum: sidebar hosting of the file list by the bench
    // window; the dialog reclaims the widget on close.
    [[nodiscard]] QTableView* fileListView();
    void refreshFileListScope();
    // ADR-0185: refreshes the layout/destination selectors after Settings
    // edits profiles.
    void reloadOutputProfiles();

  private:
    // What the session says, shown.
    void sync();
    void buildGrid();
    void fillGrid();
    void rebuildScripts(const QString& selected);
    void rebuildOutputProfiles();
    void rebuildFieldLayouts();
    void noteFieldSelection();
    // ADR-0238: the Actions popover -- what Apply does, and with which
    // layout, destination, grouping and scripts -- all in view at once.
    void showActionsPopover();
    void syncActionsPopover();
    void startIdentify();
    void exportReplayGainResults();
    void showLoudnessProvenance();
    void showPreparationFeedback(const QString& window_title, const QString& summary,
                                 std::vector<PreparationFeedbackRow> rows, bool retry_offered);
    void promptAddField();
    void removeSelectedFields();
    void editCurrentValues();
    void promptTransformation(std::optional<core::StableId> initially_selected = std::nullopt,
                              bool preview_initially_selected = false);
    void saveCurrentFieldLayout();
    void persistLayoutState();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

    TaggerSession* session_{nullptr};
    QLabel* file_list_dir_{nullptr};
    QVBoxLayout* root_layout_{nullptr};
    QLabel* summary_{nullptr};
    QLabel* read_only_{nullptr};
    QLabel* apply_summary_{nullptr};
    QToolButton* actions_button_{nullptr};
    // Deletes itself when it closes.
    QPointer<QFrame> actions_popover_;
    QCheckBox* actions_save_tags_{nullptr};
    QCheckBox* actions_rename_{nullptr};
    QCheckBox* actions_move_{nullptr};
    QComboBox* actions_layout_{nullptr};
    QComboBox* actions_destination_{nullptr};
    QComboBox* actions_grouping_{nullptr};
    QPushButton* actions_scan_{nullptr};
    QLabel* loading_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
    QPushButton* undo_button_{nullptr};
    QPushButton* redo_button_{nullptr};
    QPushButton* discard_button_{nullptr};
    QPushButton* add_field_button_{nullptr};
    QPushButton* remove_field_button_{nullptr};
    QPushButton* edit_values_button_{nullptr};
    QComboBox* field_layout_combo_{nullptr};
    QLabel* field_layout_label_{nullptr};
    QAction* field_layout_save_action_{nullptr};
    QAction* field_layout_remove_action_{nullptr};
    QAction* suggest_action_{nullptr};
    QPushButton* suggest_button_{nullptr};
    QPushButton* identify_button_{nullptr};
    // ADR-0261: Identify albums… over every album open here.
    QPushButton* identify_albums_button_{nullptr};
    QPointer<QDialog> identify_albums_dialog_;
    QPushButton* transform_button_{nullptr};
    QWidget* transformation_panel_{nullptr};
    QWidget* grid_tools_{nullptr};
    QListWidget* transformation_list_{nullptr};
    QLabel* transformation_status_{nullptr};
    QCheckBox* save_tags_check_{nullptr};
    QCheckBox* rename_files_check_{nullptr};
    QCheckBox* move_files_check_{nullptr};
    QPushButton* replaygain_scan_button_{nullptr};
    QComboBox* replaygain_grouping_{nullptr};
    QLineEdit* replaygain_expression_{nullptr};
    QPushButton* replaygain_provenance_button_{nullptr};
    QComboBox* output_layout_combo_{nullptr};
    QComboBox* destination_combo_{nullptr};
    QLabel* output_profile_status_{nullptr};
    QPushButton* apply_plan_button_{nullptr};
    QProgressBar* apply_progress_bar_{nullptr};
    QPushButton* apply_stop_button_{nullptr};
    QTableView* fields_{nullptr};
    MetadataFieldReviewBar* field_review_bar_{nullptr};
    QPointer<QTableView> file_list_;
    QLabel* technical_status_{nullptr};
    QTabWidget* metadata_sections_{nullptr};
    MetadataArtworkSection* artwork_section_{nullptr};
    QSplitter* metadata_splitter_{nullptr};
    QPointer<QDialog> exact_values_dialog_;
    QPointer<QInputDialog> field_name_dialog_;
    QPointer<QDialog> transformation_dialog_;
    QPointer<QDialog> identify_dialog_;
    QPointer<QDialog> feedback_dialog_;
    QByteArray pending_metadata_splitter_state_;
};

} // namespace trackknife::bench
