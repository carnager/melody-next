// SPDX-License-Identifier: GPL-3.0-only

#include "bench/output_profiles_widget.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/operations/output_path_plan.hpp"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>

namespace trackknife::bench {

OutputProfilesManagerWidget::OutputProfilesManagerWidget(OutputProfileStore store, QWidget* parent)
    : QWidget(parent), session_(new ProfilesSession(std::move(store), this)) {
    setObjectName(QStringLiteral("bench-output-profiles-manager"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    const auto expression_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    auto* sections = new QTabWidget(this);
    sections_ = sections;
    sections->setObjectName(QStringLiteral("bench-output-profile-sections"));
    root->addWidget(sections, 1);
    auto* layouts_box = new QWidget(sections);
    auto* layouts_row = new QVBoxLayout(layouts_box);
    layouts_row->setContentsMargins(16, 16, 16, 16);
    layouts_row->setSpacing(16);
    layout_list_ = new QComboBox(layouts_box);
    layout_list_->setObjectName(QStringLiteral("bench-output-layout-list"));
    layout_list_->setAccessibleName(QStringLiteral("Naming layout"));
    layout_list_->setPlaceholderText(QStringLiteral("New naming layout"));
    layout_list_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    layouts_row->addWidget(layout_list_);
    auto* layout_form_holder = new QVBoxLayout;
    auto* layout_form = new QFormLayout;
    layout_form->setVerticalSpacing(12);
    layout_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    layout_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout_name_ = new QLineEdit(layouts_box);
    layout_name_->setObjectName(QStringLiteral("bench-output-layout-name"));
    layout_name_->setPlaceholderText(QStringLiteral("For example: Album folders"));
    layout_form->addRow(QStringLiteral("Name:"), layout_name_);
    directory_expression_ = new QLineEdit(layouts_box);
    directory_expression_->setObjectName(
        QStringLiteral("bench-output-layout-directory-expression"));
    directory_expression_->setPlaceholderText(
        QStringLiteral("For example: %album artist%/%album%"));
    directory_expression_->setFont(expression_font);
    layout_form->addRow(QStringLiteral("Folders:"), directory_expression_);
    basename_expression_ = new QLineEdit(layouts_box);
    basename_expression_->setObjectName(QStringLiteral("bench-output-layout-basename-expression"));
    basename_expression_->setPlaceholderText(
        QStringLiteral("For example: %tracknumber% - %title%"));
    basename_expression_->setFont(expression_font);
    layout_form->addRow(QStringLiteral("Filename:"), basename_expression_);
    sanitization_policy_ = new QComboBox(layouts_box);
    sanitization_policy_->setObjectName(QStringLiteral("bench-output-layout-sanitization"));
    for (const auto& policy : ProfilesSession::sanitizationPolicies()) {
        sanitization_policy_->addItem(policy.label, policy.value);
    }
    sanitization_policy_->setToolTip(
        QStringLiteral("Portable replaces Windows-forbidden characters, trailing dots/spaces, "
                       "and reserved device names; Unicode spelling is preserved"));
    layout_form->addRow(QStringLiteral("Filename policy:"), sanitization_policy_);
    layout_form_holder->addLayout(layout_form);
    auto* layout_buttons = new QHBoxLayout;
    layout_new_ = new QPushButton(QStringLiteral("New"), layouts_box);
    layout_new_->setObjectName(QStringLiteral("bench-output-layout-new"));
    layout_save_ = new QPushButton(QStringLiteral("Save layout"), layouts_box);
    layout_save_->setObjectName(QStringLiteral("bench-output-layout-save"));
    layout_remove_ = new QPushButton(QStringLiteral("Remove"), layouts_box);
    layout_remove_->setObjectName(QStringLiteral("bench-output-layout-remove"));
    layout_buttons->addWidget(layout_new_);
    layout_buttons->addWidget(layout_save_);
    layout_buttons->addWidget(layout_remove_);
    layout_buttons->addStretch(1);
    layout_form_holder->addLayout(layout_buttons);
    layout_form_holder->addStretch(1);
    layouts_row->addLayout(layout_form_holder, 1);
    sections->addTab(layouts_box, QStringLiteral("Naming layouts"));

    auto* destinations_box = new QWidget(sections);
    auto* destinations_row = new QVBoxLayout(destinations_box);
    destinations_row->setContentsMargins(16, 16, 16, 16);
    destinations_row->setSpacing(16);
    // ADR-0237: a destination is a folder on one engine's machine; which one
    // is always in sight.
    auto* place_row = new QHBoxLayout;
    place_row->addWidget(new QLabel(QStringLiteral("Move destinations on"), destinations_box));
    place_list_ = new QComboBox(destinations_box);
    place_list_->setObjectName(QStringLiteral("bench-destination-engine"));
    place_list_->setAccessibleName(QStringLiteral("Engine the move destinations are on"));
    const auto places = session_->placeNames();
    for (int place = 0; place < places.size(); ++place) {
        place_list_->addItem(places.at(place), session_->placeKey(place));
    }
    place_row->addWidget(place_list_, 1);
    destination_copy_ = new QPushButton(destinations_box);
    destination_copy_->setObjectName(QStringLiteral("bench-destination-copy"));
    destination_copy_->setToolTip(
        QStringLiteral("Save this computer's destinations that lie under this engine's music "
                       "folder here too, as the engine names them"));
    destination_copy_->hide();
    place_row->addWidget(destination_copy_);
    destinations_row->addLayout(place_row);
    destination_list_ = new QComboBox(destinations_box);
    destination_list_->setObjectName(QStringLiteral("bench-destination-list"));
    destination_list_->setAccessibleName(QStringLiteral("Move destination"));
    destination_list_->setPlaceholderText(QStringLiteral("New move destination"));
    destination_list_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    destinations_row->addWidget(destination_list_);
    auto* destination_form_holder = new QVBoxLayout;
    auto* destination_form = new QFormLayout;
    destination_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    destination_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    destination_form->setVerticalSpacing(12);
    destination_name_ = new QLineEdit(destinations_box);
    destination_name_->setObjectName(QStringLiteral("bench-destination-name"));
    destination_name_->setPlaceholderText(QStringLiteral("For example: Music library"));
    destination_form->addRow(QStringLiteral("Name:"), destination_name_);
    auto* root_row = new QHBoxLayout;
    destination_root_ = new QLineEdit(destinations_box);
    destination_root_->setObjectName(QStringLiteral("bench-destination-root"));
    destination_root_->setPlaceholderText(QStringLiteral("Choose an absolute folder"));
    destination_browse_ = new QPushButton(QStringLiteral("Browse…"), destinations_box);
    destination_browse_->setObjectName(QStringLiteral("bench-destination-browse"));
    root_row->addWidget(destination_root_, 1);
    root_row->addWidget(destination_browse_);
    destination_form->addRow(QStringLiteral("Root:"), root_row);
    destination_form_holder->addLayout(destination_form);
    auto* destination_buttons = new QHBoxLayout;
    destination_new_ = new QPushButton(QStringLiteral("New"), destinations_box);
    destination_new_->setObjectName(QStringLiteral("bench-destination-new"));
    destination_save_ = new QPushButton(QStringLiteral("Save destination"), destinations_box);
    destination_save_->setObjectName(QStringLiteral("bench-destination-save"));
    destination_remove_ = new QPushButton(QStringLiteral("Remove"), destinations_box);
    destination_remove_->setObjectName(QStringLiteral("bench-destination-remove"));
    destination_buttons->addWidget(destination_new_);
    destination_buttons->addWidget(destination_save_);
    destination_buttons->addWidget(destination_remove_);
    destination_buttons->addStretch(1);
    destination_form_holder->addLayout(destination_buttons);
    destination_form_holder->addStretch(1);
    destinations_row->addLayout(destination_form_holder, 1);
    sections->addTab(destinations_box, QStringLiteral("Move destinations"));

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-output-profiles-status"));
    status_->setWordWrap(true);
    root->addWidget(status_);

    connect(layout_list_, &QComboBox::currentIndexChanged, session_,
            &ProfilesSession::selectLayout);
    connect(destination_list_, &QComboBox::currentIndexChanged, session_,
            &ProfilesSession::selectDestination);
    connect(layout_new_, &QPushButton::clicked, this, [this] {
        session_->newLayout();
        layout_name_->setFocus();
    });
    connect(destination_new_, &QPushButton::clicked, this, [this] {
        session_->newDestination();
        destination_name_->setFocus();
    });
    connect(layout_save_, &QPushButton::clicked, session_, &ProfilesSession::saveLayout);
    connect(destination_save_, &QPushButton::clicked, session_, &ProfilesSession::saveDestination);
    connect(layout_remove_, &QPushButton::clicked, session_, &ProfilesSession::removeLayout);
    connect(destination_remove_, &QPushButton::clicked, session_,
            &ProfilesSession::removeDestination);
    const auto typed = [this](QLineEdit* field, void (ProfilesSession::*set)(const QString&)) {
        connect(field, &QLineEdit::textChanged, this, [this, set](const QString& text) {
            if (!syncing_) {
                (session_->*set)(text);
            }
        });
    };
    typed(layout_name_, &ProfilesSession::setLayoutName);
    typed(directory_expression_, &ProfilesSession::setDirectoryExpression);
    typed(basename_expression_, &ProfilesSession::setBasenameExpression);
    typed(destination_name_, &ProfilesSession::setDestinationName);
    connect(sanitization_policy_, &QComboBox::currentIndexChanged, this, [this] {
        if (!syncing_) {
            session_->setSanitization(sanitization_policy_->currentData().toString());
        }
    });
    connect(place_list_, &QComboBox::currentIndexChanged, session_,
            &ProfilesSession::selectPlace);
    connect(destination_copy_, &QPushButton::clicked, session_,
            &ProfilesSession::copyDestinations);
    connect(destination_browse_, &QPushButton::clicked, this, [this] {
        const auto& start = session_->destinationRootRawPath();
        if (const auto folders = session_->folders()) {
            auto* chooser = new EngineFolderDialog(session_->placeName(), folders, start, this);
            connect(chooser, &EngineFolderDialog::folderChosen, session_,
                    [this](const QByteArray& chosen) {
                        session_->chooseRoot(
                            std::string{chosen.constData(), static_cast<std::size_t>(chosen.size())});
                    });
            chooser->show();
            return;
        }
        const auto initial =
            start.empty() ? QString{}
                          : QFile::decodeName(QByteArray{start.data(),
                                                         static_cast<qsizetype>(start.size())});
        const auto selected = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose move destination"), initial,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (selected.isEmpty()) {
            return;
        }
        const auto encoded = QFile::encodeName(selected);
        session_->chooseRoot(
            std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
    });
    connect(session_, &ProfilesSession::changed, this, &OutputProfilesManagerWidget::sync);
    connect(session_, &ProfilesSession::listsChanged, this,
            &OutputProfilesManagerWidget::rebuildLists);
    connect(session_, &ProfilesSession::profilesChanged, this,
            &OutputProfilesManagerWidget::profilesChanged);
    rebuildLists();
    sync();
}

void OutputProfilesManagerWidget::showNamingLayouts() { sections_->setCurrentIndex(0); }

void OutputProfilesManagerWidget::showDestinationsOf(const QString& key) {
    if (const auto place = session_->placeOf(key); place >= 0) {
        place_list_->setCurrentIndex(place);
    }
    sections_->setCurrentIndex(1);
}

void OutputProfilesManagerWidget::rebuildLists() {
    const QSignalBlocker layout_blocker{layout_list_};
    const QSignalBlocker destination_blocker{destination_list_};
    layout_list_->clear();
    layout_list_->addItems(session_->layoutNames());
    destination_list_->clear();
    destination_list_->addItems(session_->destinationNames());
    sync();
}

void OutputProfilesManagerWidget::sync() {
    syncing_ = true;
    const auto show = [](QLineEdit* field, const QString& text) {
        if (field->text() != text) {
            field->setText(text);
            field->setCursorPosition(0);
        }
    };
    {
        const QSignalBlocker layout_blocker{layout_list_};
        const QSignalBlocker destination_blocker{destination_list_};
        const QSignalBlocker place_blocker{place_list_};
        layout_list_->setCurrentIndex(session_->layoutRow());
        destination_list_->setCurrentIndex(session_->destinationRow());
        place_list_->setCurrentIndex(session_->place());
    }
    show(layout_name_, session_->layoutName());
    show(directory_expression_, session_->directoryExpression());
    show(basename_expression_, session_->basenameExpression());
    sanitization_policy_->setCurrentIndex(
        std::max(0, sanitization_policy_->findData(session_->sanitization())));
    show(destination_name_, session_->destinationName());
    show(destination_root_, session_->destinationRoot());
    destination_copy_->setText(
        QStringLiteral("Copy %1 from this computer").arg(session_->copyable()));
    destination_copy_->setVisible(session_->copyable() > 0);
    status_->setText(session_->status());
    syncing_ = false;
    updateButtons();
}

void OutputProfilesManagerWidget::updateButtons() {
    const auto available = session_->available();
    for (auto* widget : std::initializer_list<QWidget*>{
             layout_list_, layout_name_, directory_expression_, basename_expression_,
             sanitization_policy_, destination_list_, destination_name_, destination_root_}) {
        widget->setEnabled(available);
    }
    layout_new_->setEnabled(session_->canEditLayouts());
    layout_save_->setEnabled(session_->canSaveLayout());
    layout_remove_->setEnabled(session_->canRemoveLayout());
    place_list_->setEnabled(session_->canChoosePlace());
    destination_copy_->setEnabled(available);
    destination_browse_->setEnabled(session_->canEditDestinations());
    destination_new_->setEnabled(session_->canEditDestinations());
    destination_save_->setEnabled(session_->canSaveDestination());
    destination_remove_->setEnabled(session_->canRemoveDestination());
}

} // namespace trackknife::bench
