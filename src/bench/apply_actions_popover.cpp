// SPDX-License-Identifier: GPL-3.0-only
#include "bench/apply_actions_popover.hpp"

#include "bench/engine_folder_dialog.hpp"
#include "workspace/tagger_session.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QScreen>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace trackknife::bench {

QStringList replayGainGroupingNames() {
    return {QStringLiteral("Album by release"), QStringLiteral("Album merging discs"),
            QStringLiteral("Selection as one album"), QStringLiteral("Track gains only"),
            QStringLiteral("Group by expression")};
}

QString chooseFolderItem() { return QStringLiteral("choose-folder"); }

void chooseMoveFolder(QWidget* parent, TaggerSession& session) {
    const QPointer target{&session};
    const auto chosen = [target](const QByteArray& raw_path) {
        if (target && !raw_path.isEmpty()) {
            target->chooseMoveFolder(
                std::string{raw_path.constData(), static_cast<std::size_t>(raw_path.size())});
        }
    };
    for (const auto* holder : {parent, parent->window()}) {
        if (const auto given = holder->property("trackknife-move-folder").toByteArray();
            !given.isEmpty()) {
            chosen(given);
            return;
        }
    }
    const auto start = session.moveFolderStart();
    if (auto lister = session.moveFolderLister()) {
        auto* chooser =
            new EngineFolderDialog(session.destinationsOn(), std::move(lister), start, parent);
        QObject::connect(chooser, &EngineFolderDialog::folderChosen, parent, chosen);
        chooser->show();
        return;
    }
    const auto selected = QFileDialog::getExistingDirectory(
        parent, QStringLiteral("Move into folder"),
        QFile::decodeName(QByteArray{start.data(), static_cast<qsizetype>(start.size())}),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!selected.isEmpty()) {
        chosen(QFile::encodeName(selected));
    }
}

ApplyActionsPopover::ApplyActionsPopover(TaggerSession& session, Links links, QWidget* parent)
    : QFrame(parent, Qt::Popup), session_(&session) {
    setObjectName(QStringLiteral("bench-metadata-actions-popover"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    const auto link = [this](const QString& text, const QString& object_name, auto&& activated) {
        auto* label =
            new QLabel(QStringLiteral("<a href=\"#\">%1</a>").arg(text.toHtmlEscaped()), this);
        label->setObjectName(object_name);
        label->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        connect(label, &QLabel::linkActivated, this,
                [this, activated = std::forward<decltype(activated)>(activated)] {
                    close();
                    activated();
                });
        return label;
    };
    int row = 0;

    save_tags_ = new QCheckBox(QStringLiteral("Save tags"), this);
    save_tags_->setObjectName(QStringLiteral("bench-actions-save-tags"));
    connect(save_tags_, &QCheckBox::clicked, this, [this](const bool checked) {
        session_->setSaveTags(checked);
        session_->rememberActionChoices();
    });
    grid->addWidget(save_tags_, row++, 0, 1, 2);

    rename_ = new QCheckBox(QStringLiteral("Rename files"), this);
    rename_->setObjectName(QStringLiteral("bench-actions-rename-files"));
    connect(rename_, &QCheckBox::clicked, &session, &TaggerSession::chooseRename);
    layout_ = new QComboBox(this);
    layout_->setObjectName(QStringLiteral("bench-actions-layout"));
    layout_->setAccessibleName(QStringLiteral("Naming layout"));
    connect(layout_, &QComboBox::activated, this, [this](const int index) {
        session_->selectLayout(index);
        session_->rememberActionChoices();
    });
    grid->addWidget(rename_, row, 0);
    grid->addWidget(layout_, row++, 1);

    move_ = new QCheckBox(QStringLiteral("Move files"), this);
    move_->setObjectName(QStringLiteral("bench-actions-move-files"));
    connect(move_, &QCheckBox::clicked, &session, &TaggerSession::chooseMove);
    destination_ = new QComboBox(this);
    destination_->setObjectName(QStringLiteral("bench-actions-destination"));
    destination_->setAccessibleName(QStringLiteral("Move destination"));
    connect(destination_, &QComboBox::activated, this, [this](const int index) {
        if (destination_->itemData(index).toString() == chooseFolderItem()) {
            // Itself closed by the chooser; the window it is over chooses.
            auto* owner = parentWidget();
            close();
            chooseMoveFolder(owner, *session_);
            return;
        }
        session_->selectDestination(index);
        session_->rememberActionChoices();
    });
    grid->addWidget(move_, row, 0);
    grid->addWidget(destination_, row++, 1);
    auto* manage = new QHBoxLayout;
    manage->setSpacing(16);
    manage->addWidget(link(
        QStringLiteral("Manage naming layouts…"), QStringLiteral("bench-actions-manage-layouts"),
        [this] { emit session_->openSettingsRequested(TaggerSession::SettingsPage::naming); }));
    manage->addWidget(link(QStringLiteral("Manage move destinations…"),
                           QStringLiteral("bench-actions-manage-destinations"),
                           [this] { emit session_->openDestinationsRequested(); }));
    manage->addStretch(1);
    grid->addLayout(manage, row++, 1);

    // Measured when applying, and written with the rest (ADR-0262).
    replaygain_ = new QCheckBox(QStringLiteral("ReplayGain"), this);
    replaygain_->setObjectName(QStringLiteral("bench-actions-replaygain"));
    connect(replaygain_, &QCheckBox::clicked, &session, &TaggerSession::chooseReplayGain);
    grouping_ = new QComboBox(this);
    grouping_->setObjectName(QStringLiteral("bench-actions-replaygain-grouping"));
    grouping_->setAccessibleName(QStringLiteral("ReplayGain grouping"));
    grouping_->addItems(replayGainGroupingNames());
    connect(grouping_, &QComboBox::activated, this, [this](const int index) {
        session_->setReplayGainGrouping(index);
        if (index == 4) {
            bool accepted = false;
            const auto expression =
                QInputDialog::getText(parentWidget(), QStringLiteral("Group by expression"),
                                      QStringLiteral("tkfmt-1 expression:"), QLineEdit::Normal,
                                      session_->replayGainExpression(), &accepted);
            if (accepted && session_) {
                session_->setReplayGainExpression(expression);
            }
        }
    });
    skip_gain_ = new QCheckBox(QStringLiteral("Skip existing"), this);
    skip_gain_->setObjectName(QStringLiteral("bench-actions-replaygain-skip-existing"));
    skip_gain_->setToolTip(
        QStringLiteral("Leave albums alone whose files all have the gain already (tracks, for "
                       "track gains only)"));
    connect(skip_gain_, &QCheckBox::clicked, &session, &TaggerSession::chooseSkipExistingGain);
    auto* replaygain_row = new QHBoxLayout;
    replaygain_row->addWidget(grouping_, 1);
    replaygain_row->addWidget(skip_gain_);
    grid->addWidget(replaygain_, row, 0);
    grid->addLayout(replaygain_row, row++, 1);
    auto* replaygain_links = new QHBoxLayout;
    replaygain_links->setSpacing(16);
    if (links.loudness_sources) {
        auto* sources =
            link(QStringLiteral("Loudness sources…"),
                 QStringLiteral("bench-actions-loudness-sources"), links.loudness_sources);
        sources->setEnabled(session.canShowProvenance());
        replaygain_links->addWidget(sources);
    }
    replaygain_links->addWidget(link(
        QStringLiteral("ReplayGain settings…"), QStringLiteral("bench-actions-replaygain-settings"),
        [this] { emit session_->openSettingsRequested(TaggerSession::SettingsPage::replaygain); }));
    replaygain_links->addStretch(1);
    grid->addLayout(replaygain_links, row++, 1);

    auto* scripts = new QLabel(QStringLiteral("Scripts"), this);
    grid->addWidget(scripts, row, 0, Qt::AlignTop);
    auto* script_column = new QVBoxLayout;
    int index = 0;
    for (const auto& script : session.scripts()) {
        auto* choice = new QCheckBox(script.name, this);
        choice->setObjectName(QStringLiteral("bench-actions-script-%1").arg(index++));
        choice->setChecked(script.automatic);
        connect(choice, &QCheckBox::toggled, this, [this, id = script.id](const bool checked) {
            session_->toggleAutomaticScript(id, checked);
        });
        script_column->addWidget(choice);
    }
    if (links.script_editor) {
        auto* editor = link(QStringLiteral("Open script editor…"),
                            QStringLiteral("bench-actions-script-editor"), links.script_editor);
        editor->setEnabled(session.canTransform());
        script_column->addWidget(editor);
    }
    grid->addLayout(script_column, row++, 1);

    fillLists();
    connect(&session, &TaggerSession::changed, this, &ApplyActionsPopover::sync);
    connect(&session, &TaggerSession::outputProfilesChanged, this, &ApplyActionsPopover::fillLists);
}

void ApplyActionsPopover::fillLists() {
    if (session_ == nullptr) {
        return;
    }
    {
        const QSignalBlocker layout_blocker{layout_};
        const QSignalBlocker destination_blocker{destination_};
        layout_->clear();
        for (const auto& choice : session_->layouts()) {
            layout_->addItem(choice.name, choice.id);
        }
        layout_->setPlaceholderText(QStringLiteral("None saved yet"));
        destination_->clear();
        for (const auto& choice : session_->destinations()) {
            destination_->addItem(choice.name, choice.id);
        }
        destination_->addItem(QStringLiteral("Choose folder…"), chooseFolderItem());
        const auto engine = session_->destinationsOn();
        destination_->setToolTip(
            engine.isEmpty() ? QString{} : QStringLiteral("Move destinations on %1").arg(engine));
    }
    sync();
}

void ApplyActionsPopover::sync() {
    if (session_ == nullptr) {
        return;
    }
    const auto& session = *session_;
    const QSignalBlocker blockers[]{QSignalBlocker{save_tags_},   QSignalBlocker{rename_},
                                    QSignalBlocker{move_},        QSignalBlocker{layout_},
                                    QSignalBlocker{destination_}, QSignalBlocker{replaygain_},
                                    QSignalBlocker{grouping_},    QSignalBlocker{skip_gain_}};
    save_tags_->setChecked(session.saveTags());
    rename_->setChecked(session.renameFiles());
    rename_->setEnabled(session.renameAvailable());
    rename_->setToolTip(session.renameTooltip());
    move_->setChecked(session.moveFiles());
    move_->setEnabled(session.moveAvailable());
    move_->setToolTip(session.moveTooltip());
    layout_->setEnabled(session.layoutsAvailable());
    layout_->setCurrentIndex(session.layoutIndex());
    destination_->setEnabled(session.destinationsAvailable());
    destination_->setCurrentIndex(session.destinationIndex());
    replaygain_->setChecked(session.replayGainOnApply());
    grouping_->setCurrentIndex(session.replayGainGrouping());
    grouping_->setEnabled(session.replayGainOnApply());
    skip_gain_->setChecked(session.skipExistingGain());
    skip_gain_->setEnabled(session.replayGainOnApply());
}

void ApplyActionsPopover::showAt(QWidget* anchor) {
    adjustSize();
    // Above the button: the footer is at the window's foot. Below it when
    // there is no room above, and never past the screen's edges.
    const auto size = sizeHint();
    auto at = anchor->mapToGlobal(QPoint{0, -size.height()});
    if (const auto* screen = anchor->screen()) {
        const auto area = screen->availableGeometry();
        if (at.y() < area.top()) {
            at.setY(anchor->mapToGlobal(QPoint{0, anchor->height()}).y());
        }
        at.setX(
            std::clamp(at.x(), area.left(), std::max(area.left(), area.right() - size.width())));
    }
    move(at);
    show();
}

} // namespace trackknife::bench
