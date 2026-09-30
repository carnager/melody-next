// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_artwork_section.hpp"

#include "bench/cover_review.hpp"
#include "bench/cover_thumbnail.hpp"
#include "bench/preparation_feedback_dialog.hpp"

#include <QAbstractItemView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QTableView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <utility>

namespace trackknife::bench {
namespace {

constexpr int thumbnail_edge = 60;

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

// A table over one of the session's models, sharing its selection.
void attach(QTableView* table, QAbstractItemModel* model, QItemSelectionModel* selection) {
    table->setModel(model);
    auto* initial = table->selectionModel();
    table->setSelectionModel(selection);
    delete initial;
}

} // namespace

MetadataArtworkSection::MetadataArtworkSection(QWidget* parent)
    : QWidget(parent), session_(new ArtworkSession(this)) {
    setObjectName(QStringLiteral("bench-metadata-artwork-section"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(6);

    auto* status_row = new QHBoxLayout;
    status_row->setContentsMargins(0, 0, 0, 0);
    status_row->setSpacing(8);
    status_ = new QLabel(this);
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
    connect(stop_button_, &QPushButton::clicked, session_, &ArtworkSession::requestStop);
    status_row->addWidget(stop_button_);
    layout->addLayout(status_row);

    auto heading_font = status_->font();
    heading_font.setBold(true);

    auto* inventory_row = new QHBoxLayout;
    inventory_row->setContentsMargins(0, 0, 0, 0);
    inventory_row->setSpacing(6);
    inventory_row->addStretch(1);
    fetch_cover_button_ = new QPushButton(QStringLiteral("Fetch cover…"), this);
    fetch_cover_button_->setObjectName(QStringLiteral("bench-metadata-artwork-fetch-cover"));
    fetch_cover_button_->setToolTip(
        QStringLiteral("Choose a cover: an image beside the files, or one the Cover Art Archive "
                       "has for the release. A front cover replaces the existing one."));
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
    inventory_row->addWidget(add_button_);
    inventory_row->addWidget(copy_button_);
    inventory_row->addWidget(export_button_);
    inventory_row->addWidget(replace_button_);
    inventory_row->addWidget(remove_button_);
    layout->addLayout(inventory_row);
    auto* draft_buttons = new QHBoxLayout;
    draft_help_ = new QLabel(this);
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
    attach(pending_view_, session_->pending(), session_->pendingSelection());
    pending_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(undo_pending_button_, &QPushButton::clicked, session_,
            &ArtworkSession::undoSelectedPending);
    auto* undo_pending_shortcut = new QShortcut(QKeySequence::Delete, pending_view_);
    undo_pending_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undo_pending_shortcut, &QShortcut::activated, this, [this] {
        if (session_->canUndoSelected()) {
            session_->undoSelectedPending();
        }
    });
    pending_view_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    pending_view_->horizontalHeader()->setStretchLastSection(false);
    pending_view_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    pending_view_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    pending_view_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
    pending_view_->setColumnWidth(4, thumbnail_edge + 32);
    pending_view_->setColumnWidth(5, thumbnail_edge + 32);
    pending_view_->setIconSize(QSize(thumbnail_edge, thumbnail_edge));
    pending_view_->verticalHeader()->setDefaultSectionSize(thumbnail_edge + 8);
    pending_view_->setMaximumHeight(240);
    pending_view_->hide();
    layout->addWidget(pending_view_);
    connect(save_button_, &QPushButton::clicked, session_, &ArtworkSession::save);
    connect(discard_button_, &QPushButton::clicked, session_,
            &ArtworkSession::discardPendingChanges);

    items_ = new QTableView(this);
    items_->setObjectName(QStringLiteral("bench-metadata-artwork-items"));
    items_->setAccessibleName(QStringLiteral("Artwork inventory"));
    auto* delete_shortcut = new QShortcut(QKeySequence::Delete, items_);
    delete_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(delete_shortcut, &QShortcut::activated, this, [this] {
        if (session_->canRemove()) {
            session_->removeSelected();
        }
    });
    configure_table(items_);
    attach(items_, session_->items(), session_->itemSelection());
    items_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    items_->setIconSize(QSize(thumbnail_edge, thumbnail_edge));
    items_->verticalHeader()->setDefaultSectionSize(thumbnail_edge + 8);
    items_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    items_->horizontalHeader()->setStretchLastSection(true);
    items_->setColumnWidth(0, thumbnail_edge + 12);
    items_->setColumnWidth(2, 90);
    items_->setColumnWidth(3, 190);
    layout->addWidget(items_, 1);

    empty_state_ = new QLabel(this);
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
    issues_->setModel(session_->issues());
    issues_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    issues_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    issues_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    issues_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    issues_->setMaximumHeight(150);
    issues_layout->addWidget(issues_);
    issues_pane_->hide();
    layout->addWidget(issues_pane_);

    connect(fetch_cover_button_, &QPushButton::clicked, this,
            &MetadataArtworkSection::openCoverPicker);
    connect(add_button_, &QPushButton::clicked, this, &MetadataArtworkSection::promptAddition);
    connect(copy_button_, &QPushButton::clicked, session_, &ArtworkSession::copySelected);
    connect(export_button_, &QPushButton::clicked, this, &MetadataArtworkSection::promptExport);
    connect(replace_button_, &QPushButton::clicked, this,
            &MetadataArtworkSection::promptReplacement);
    connect(remove_button_, &QPushButton::clicked, session_, &ArtworkSession::removeSelected);

    connect(session_, &ArtworkSession::changed, this, &MetadataArtworkSection::sync);
    connect(session_, &ArtworkSession::operationRunningChanged, this,
            &MetadataArtworkSection::operationRunningChanged);
    connect(session_, &ArtworkSession::frontCoverChanged, this,
            &MetadataArtworkSection::frontCoverChanged);
    connect(session_, &ArtworkSession::pendingChangesChanged, this,
            &MetadataArtworkSection::pendingChangesChanged);
    connect(session_, &ArtworkSession::feedbackRequested, this,
            &MetadataArtworkSection::showFeedback);
    connect(session_, &ArtworkSession::folderImagesReviewRequested, this,
            [this](std::vector<metadata::FolderImageWritePlan> images) {
                reviewFolderImages(this, images, [session = QPointer{session_}] {
                    if (session) {
                        session->folderImagesReviewed(true);
                    }
                });
            });
    connect(session_, &ArtworkSession::pickerChanged, this, &MetadataArtworkSection::syncPicker);
    connect(session_, &ArtworkSession::pickerClosed, this, [this] {
        if (picker_dialog_ != nullptr) {
            picker_dialog_->close();
        }
    });
    sync();
}

MetadataArtworkSection::~MetadataArtworkSection() {
    delete session_;
    session_ = nullptr;
}

void MetadataArtworkSection::sync() {
    if (session_ == nullptr) {
        return;
    }
    const auto& session = *session_;
    setEnabled(!session.working());
    status_->setText(session.status());
    draft_help_->setText(session.draftHelp());
    save_button_->setVisible(session.saveVisible());
    pending_view_->setVisible(session.pendingVisible());
    empty_state_->setText(session.emptyText());
    empty_state_->setVisible(session.emptyVisible());
    issues_pane_->setVisible(session.issuesVisible());
    progress_bar_->setVisible(session.progressVisible());
    stop_button_->setVisible(session.progressVisible());
    stop_button_->setEnabled(session.canStop());
    if (session.progressVisible()) {
        progress_bar_->setRange(0, session.progressMaximum());
        progress_bar_->setValue(session.progressValue());
    }
    save_button_->setEnabled(session.canSave());
    discard_button_->setEnabled(session.canDiscard());
    undo_pending_button_->setEnabled(session.canUndoSelected());
    add_button_->setEnabled(session.canAdd());
    fetch_cover_button_->setEnabled(session.canFetch());
    copy_button_->setEnabled(session.canCopy());
    replace_button_->setEnabled(session.canReplace());
    remove_button_->setEnabled(session.canRemove());
    export_button_->setEnabled(session.canExport());
    const std::pair actions{session.frontEditable(), session.canFetch()};
    if (front_actions_ != actions) {
        front_actions_ = actions;
        emit frontActionsChanged(actions.first, actions.second);
    }
}

QWidget* MetadataArtworkSection::createCompactCover(QWidget* parent) {
    auto* pane = new QWidget(parent);
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* thumbnail = new CoverThumbnail(pane);
    auto* fetch = new QPushButton(QStringLiteral("Fetch cover"), pane);
    fetch->setObjectName(QStringLiteral("bench-metadata-cover-fetch"));
    layout->addWidget(thumbnail);
    layout->addWidget(fetch);
    layout->addStretch();
    connect(this, &MetadataArtworkSection::frontCoverChanged, thumbnail, &CoverThumbnail::setCover);
    connect(this, &MetadataArtworkSection::frontActionsChanged, pane,
            [thumbnail, fetch](bool editable, bool fetchable) {
                thumbnail->setProperty("cover-editable", editable);
                fetch->setEnabled(fetchable);
            });
    thumbnail->setProperty("cover-editable", session_->frontEditable());
    fetch->setEnabled(session_->canFetch());
    connect(thumbnail, &CoverThumbnail::fileDropped, session_, &ArtworkSession::stageFrontCover);
    connect(thumbnail, &CoverThumbnail::imagePasted, session_, &ArtworkSession::pasteFrontCover);
    connect(fetch, &QPushButton::clicked, this, &MetadataArtworkSection::openCoverPicker);
    connect(thumbnail, &QWidget::customContextMenuRequested, this,
            [this, thumbnail](const QPoint& point) {
                auto* menu = new QMenu(thumbnail);
                menu->setAttribute(Qt::WA_DeleteOnClose);
                auto* fetch_action = menu->addAction(QStringLiteral("Fetch cover"), this,
                                                     &MetadataArtworkSection::openCoverPicker);
                fetch_action->setEnabled(session_->canFetch());
                auto* choose = menu->addAction(QStringLiteral("Choose file…"), this, [this] {
                    const auto path = QFileDialog::getOpenFileName(
                        this, QStringLiteral("Choose front cover"), {},
                        QStringLiteral("Artwork images (*.png *.jpg *.jpeg)"));
                    if (!path.isEmpty()) {
                        session_->stageFrontCover(path);
                    }
                });
                choose->setEnabled(session_->frontEditable());
                auto* remove = menu->addAction(QStringLiteral("Remove"), session_,
                                               &ArtworkSession::removeFrontCover);
                remove->setEnabled(session_->canRemoveFront());
                menu->addSeparator();
                menu->addAction(QStringLiteral("Open Artwork tab"), this,
                                &MetadataArtworkSection::openArtworkRequested);
                menu->addAction(QStringLiteral("Cover settings…"), this,
                                &MetadataArtworkSection::coverSettingsRequested);
                menu->popup(thumbnail->mapToGlobal(point));
            });
    return pane;
}

// One picker for a cover: the images beside the files, then what the Cover
// Art Archive has for the release.
void MetadataArtworkSection::openCoverPicker() {
    if (!session_->openPicker()) {
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("bench-metadata-artwork-picker"));
    dialog->setWindowTitle(QStringLiteral("Choose a cover"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(600, 440);
    picker_dialog_ = dialog;
    auto* layout = new QVBoxLayout(dialog);
    auto* hint = new QLabel(
        QStringLiteral("A front cover replaces the files' existing one; other images are added "
                       "with their type."),
        dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* list = new QTreeWidget(dialog);
    list->setObjectName(QStringLiteral("bench-metadata-artwork-picker-list"));
    list->setHeaderLabels(
        {QString{}, QStringLiteral("From"), QStringLiteral("Type"), QStringLiteral("Details")});
    list->setRootIsDecorated(false);
    list->setAlternatingRowColors(true);
    list->setIconSize(QSize(72, 72));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->header()->setStretchLastSection(true);
    layout->addWidget(list, 1);
    picker_list_ = list;
    auto* status = new QLabel(dialog);
    status->setObjectName(QStringLiteral("bench-metadata-artwork-picker-status"));
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* use = buttons->addButton(QStringLiteral("Use this image"), QDialogButtonBox::ActionRole);
    use->setObjectName(QStringLiteral("bench-metadata-artwork-picker-use"));
    use->setEnabled(false);
    layout->addWidget(buttons);
    connect(list, &QTreeWidget::currentItemChanged, use,
            [use](QTreeWidgetItem* current) { use->setEnabled(current != nullptr); });
    const auto use_selected = [this, list] {
        if (auto* current = list->currentItem()) {
            session_->usePicker(list->indexOfTopLevelItem(current));
        }
    };
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(use, &QPushButton::clicked, dialog, use_selected);
    connect(list, &QTreeWidget::itemDoubleClicked, dialog,
            [use_selected](QTreeWidgetItem*, int) { use_selected(); });
    connect(dialog, &QObject::destroyed, this, [session = QPointer{session_}] {
        if (session) {
            session->closePicker();
        }
    });
    syncPicker();
    dialog->show();
}

void MetadataArtworkSection::syncPicker() {
    if (picker_dialog_ == nullptr || picker_list_ == nullptr) {
        return;
    }
    auto* list = picker_list_.data();
    const auto& rows = session_->pickerRows();
    // Rows only ever come; thumbnails trickle into those already there.
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const auto& picked = rows[static_cast<std::size_t>(row)];
        auto* item =
            row < list->topLevelItemCount() ? list->topLevelItem(row) : new QTreeWidgetItem(list);
        item->setText(1, picked.from);
        item->setText(2, picked.type);
        item->setText(3, picked.details);
        item->setToolTip(1, picked.tool_tip);
        if (!picked.thumbnail.isNull()) {
            item->setIcon(0, QIcon{QPixmap::fromImage(picked.thumbnail)});
        }
    }
    if (list->currentItem() == nullptr && session_->pickerSuggested() >= 0 &&
        session_->pickerSuggested() < list->topLevelItemCount()) {
        list->setCurrentItem(list->topLevelItem(session_->pickerSuggested()));
    }
    if (auto* status = picker_dialog_->findChild<QLabel*>(
            QStringLiteral("bench-metadata-artwork-picker-status"))) {
        status->setText(session_->pickerStatus());
    }
}

void MetadataArtworkSection::promptAddition() {
    const auto selected = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose artwork to add"), {},
        QStringLiteral("Artwork images (*.png *.jpg *.jpeg);;All files (*)"));
    if (selected.isEmpty()) {
        return;
    }
    const auto roles = ArtworkSession::addRoles();
    bool accepted = false;
    const auto selected_role =
        QInputDialog::getItem(this, QStringLiteral("Artwork role"), QStringLiteral("Store as:"),
                              roles, 0, false, &accepted);
    if (accepted) {
        session_->add(selected, static_cast<int>(roles.indexOf(selected_role)));
    }
}

void MetadataArtworkSection::promptReplacement() {
    session_->replace(QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose replacement artwork"), {},
        QStringLiteral("Artwork images (*.png *.jpg *.jpeg);;All files (*)")));
}

void MetadataArtworkSection::promptExport() {
    if (!session_->canExport()) {
        return;
    }
    session_->exportSelected(
        QFileDialog::getExistingDirectory(this, QStringLiteral("Choose artwork export directory")));
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
        sync();
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

} // namespace trackknife::bench
