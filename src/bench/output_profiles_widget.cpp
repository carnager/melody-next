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
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>

namespace trackknife::bench {

namespace {

// A destination's place and row, on its item.
constexpr int place_role = Qt::UserRole;
constexpr int row_role = Qt::UserRole + 1;

[[nodiscard]] QLabel* statusLabel(QWidget* parent) {
    auto* status = new QLabel(parent);
    status->setObjectName(QStringLiteral("bench-output-profiles-status"));
    status->setWordWrap(true);
    status->setForegroundRole(QPalette::PlaceholderText);
    return status;
}

} // namespace

OutputProfilesManager::OutputProfilesManager(OutputProfileStore store, QTabWidget* tabs)
    : QObject(tabs), session_(new ProfilesSession(std::move(store), this)), sections_(tabs) {
    setObjectName(QStringLiteral("bench-output-profiles-manager"));
    const auto expression_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    auto* sections = tabs;
    auto* layouts_box = new QWidget(sections);
    layouts_page_ = layouts_box;
    auto* layouts_row = new QVBoxLayout(layouts_box);
    layouts_row->setContentsMargins(16, 16, 16, 16);
    layouts_row->setSpacing(12);
    // ADR-0264: the saved layouts listed, the selected one edited below --
    // as the move destinations are. Global (ADR-0237): no engine.
    layout_list_ = new QTreeWidget(layouts_box);
    layout_list_->setObjectName(QStringLiteral("bench-output-layout-list"));
    layout_list_->setAccessibleName(QStringLiteral("Naming layouts"));
    layout_list_->setColumnCount(2);
    layout_list_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Pattern")});
    layout_list_->setRootIsDecorated(false);
    layout_list_->setItemsExpandable(false);
    layout_list_->setUniformRowHeights(true);
    layout_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout_list_->header()->setStretchLastSection(true);
    layout_list_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    layout_list_->setMinimumHeight(120);
    layouts_row->addWidget(layout_list_, 1);
    auto* layout_list_buttons = new QHBoxLayout;
    layout_new_ = new QPushButton(QStringLiteral("New"), layouts_box);
    layout_new_->setObjectName(QStringLiteral("bench-output-layout-new"));
    layout_remove_ = new QPushButton(QStringLiteral("Remove"), layouts_box);
    layout_remove_->setObjectName(QStringLiteral("bench-output-layout-remove"));
    layout_remove_->setToolTip(QStringLiteral("Remove the naming layout selected"));
    layout_list_buttons->addWidget(layout_new_);
    layout_list_buttons->addWidget(layout_remove_);
    layout_list_buttons->addStretch(1);
    layouts_row->addLayout(layout_list_buttons);
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
    layout_save_ = new QPushButton(QStringLiteral("Save layout"), layouts_box);
    layout_save_->setObjectName(QStringLiteral("bench-output-layout-save"));
    layout_buttons->addWidget(layout_save_);
    layout_buttons->addStretch(1);
    layout_form_holder->addLayout(layout_buttons);
    layouts_row->addLayout(layout_form_holder);
    layouts_status_ = statusLabel(layouts_box);
    layouts_row->addWidget(layouts_status_);
    sections->addTab(layouts_box, QStringLiteral("Naming layouts"));

    auto* destinations_box = new QWidget(sections);
    destinations_page_ = destinations_box;
    auto* destinations_row = new QVBoxLayout(destinations_box);
    destinations_row->setContentsMargins(16, 16, 16, 16);
    destinations_row->setSpacing(12);
    // ADR-0237/0264: a destination is a folder on one engine's machine;
    // every engine's are listed, each under its name.
    destination_list_ = new QTreeWidget(destinations_box);
    destination_list_->setObjectName(QStringLiteral("bench-destination-list"));
    destination_list_->setAccessibleName(QStringLiteral("Move destinations"));
    destination_list_->setColumnCount(2);
    destination_list_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Folder")});
    destination_list_->setRootIsDecorated(false);
    destination_list_->setItemsExpandable(false);
    destination_list_->setUniformRowHeights(true);
    destination_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    destination_list_->header()->setStretchLastSection(true);
    destination_list_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    destination_list_->setMinimumHeight(140);
    destinations_row->addWidget(destination_list_, 1);
    auto* list_buttons = new QHBoxLayout;
    destination_new_ = new QPushButton(QStringLiteral("New"), destinations_box);
    destination_new_->setObjectName(QStringLiteral("bench-destination-new"));
    destination_remove_ = new QPushButton(QStringLiteral("Remove"), destinations_box);
    destination_remove_->setObjectName(QStringLiteral("bench-destination-remove"));
    destination_remove_->setToolTip(QStringLiteral("Remove the move destination selected"));
    destination_copy_ = new QPushButton(destinations_box);
    destination_copy_->setObjectName(QStringLiteral("bench-destination-copy"));
    destination_copy_->setToolTip(
        QStringLiteral("Save this computer's destinations that lie under this engine's music "
                       "folder here too, as the engine names them"));
    destination_copy_->hide();
    list_buttons->addWidget(destination_new_);
    list_buttons->addWidget(destination_remove_);
    list_buttons->addStretch(1);
    list_buttons->addWidget(destination_copy_);
    destinations_row->addLayout(list_buttons);
    // A new one on which engine: asked when there is more than one.
    if (session_->placeCount() > 1) {
        auto* on = new QMenu(destination_new_);
        for (int place = 0; place < session_->placeCount(); ++place) {
            on->addAction(QStringLiteral("On %1").arg(session_->placeNames().at(place)), this,
                          [this, place] {
                              session_->newDestinationOn(place);
                              destination_name_->setFocus();
                          });
        }
        destination_new_->setMenu(on);
    }
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
    destinations_row->addLayout(destination_form);
    auto* save_row = new QHBoxLayout;
    destination_save_ = new QPushButton(QStringLiteral("Save destination"), destinations_box);
    destination_save_->setObjectName(QStringLiteral("bench-destination-save"));
    save_row->addWidget(destination_save_);
    save_row->addStretch(1);
    destinations_row->addLayout(save_row);
    destinations_status_ = statusLabel(destinations_box);
    destinations_row->addWidget(destinations_status_);
    sections->addTab(destinations_box, QStringLiteral("Move destinations"));

    connect(layout_list_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item) {
                if (!syncing_ && item != nullptr) {
                    session_->selectLayout(layout_list_->indexOfTopLevelItem(item));
                }
            });
    connect(destination_list_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item) {
                if (!syncing_ && item != nullptr && item->data(0, place_role).isValid()) {
                    session_->selectDestinationOn(item->data(0, place_role).toInt(),
                                                  item->data(0, row_role).toInt());
                }
            });
    connect(layout_new_, &QPushButton::clicked, this, [this] {
        session_->newLayout();
        layout_name_->setFocus();
    });
    connect(destination_new_, &QPushButton::clicked, this, [this] {
        if (destination_new_->menu() != nullptr) {
            return;
        }
        session_->newDestinationOn(0);
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
    connect(destination_copy_, &QPushButton::clicked, session_,
            &ProfilesSession::copyDestinations);
    connect(destination_browse_, &QPushButton::clicked, this, [this] {
        const auto& start = session_->destinationRootRawPath();
        if (const auto folders = session_->folders()) {
            auto* chooser =
                new EngineFolderDialog(session_->placeName(), folders, start, destinations_page_);
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
            destinations_page_, QStringLiteral("Choose move destination"), initial,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (selected.isEmpty()) {
            return;
        }
        const auto encoded = QFile::encodeName(selected);
        session_->chooseRoot(
            std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
    });
    connect(session_, &ProfilesSession::changed, this, &OutputProfilesManager::sync);
    connect(session_, &ProfilesSession::listsChanged, this,
            &OutputProfilesManager::rebuildLists);
    connect(session_, &ProfilesSession::profilesChanged, this,
            &OutputProfilesManager::profilesChanged);
    rebuildLists();
    sync();
}

void OutputProfilesManager::showNamingLayouts() { sections_->setCurrentWidget(layouts_page_); }

void OutputProfilesManager::showDestinationsOf(const QString& key) {
    if (const auto place = session_->placeOf(key); place >= 0) {
        session_->selectPlace(place);
    }
    sections_->setCurrentWidget(destinations_page_);
}

void OutputProfilesManager::rebuildLists() {
    const QSignalBlocker layout_blocker{layout_list_};
    const QSignalBlocker destination_blocker{destination_list_};
    layout_list_->clear();
    const auto layout_names = session_->layoutNames();
    for (int row = 0; row < layout_names.size(); ++row) {
        auto* item =
            new QTreeWidgetItem(layout_list_, {layout_names.at(row), session_->layoutPatternOn(row)});
        item->setToolTip(1, item->text(1));
    }
    destination_list_->clear();
    // Under each engine's name -- unless there is only this computer.
    const auto grouped = session_->placeCount() > 1;
    const auto places = session_->placeNames();
    for (int place = 0; place < session_->placeCount(); ++place) {
        QTreeWidgetItem* parent = nullptr;
        if (grouped) {
            auto name = places.at(place);
            if (!name.isEmpty()) {
                name[0] = name[0].toUpper();
            }
            parent = new QTreeWidgetItem(destination_list_, {name});
            parent->setFlags(Qt::ItemIsEnabled);
            parent->setFirstColumnSpanned(true);
            auto font = parent->font(0);
            font.setWeight(QFont::DemiBold);
            parent->setFont(0, font);
        }
        const auto add = [&](const QStringList& columns) {
            return parent != nullptr ? new QTreeWidgetItem(parent, columns)
                                     : new QTreeWidgetItem(destination_list_, columns);
        };
        const auto names = session_->destinationNamesOn(place);
        for (int row = 0; row < names.size(); ++row) {
            auto* item = add({names.at(row), session_->destinationRootOn(place, row)});
            item->setData(0, place_role, place);
            item->setData(0, row_role, row);
            item->setToolTip(1, item->text(1));
        }
        if (const auto note = session_->placeNote(place); !note.isEmpty() && grouped) {
            auto* item = add({note});
            item->setFlags(Qt::ItemIsEnabled);
            item->setForeground(0, destination_list_->palette().placeholderText());
        }
    }
    destination_list_->expandAll();
    sync();
}

void OutputProfilesManager::sync() {
    syncing_ = true;
    const auto show = [](QLineEdit* field, const QString& text) {
        if (field->text() != text) {
            field->setText(text);
            field->setCursorPosition(0);
        }
    };
    {
        const QSignalBlocker layout_blocker{layout_list_};
        if (auto* layout = layout_list_->topLevelItem(session_->layoutRow())) {
            layout_list_->setCurrentItem(layout);
        } else {
            layout_list_->clearSelection();
            layout_list_->setCurrentItem(nullptr);
        }
        QTreeWidgetItem* current = nullptr;
        const auto row = session_->destinationRow();
        for (QTreeWidgetItemIterator it{destination_list_}; *it != nullptr && row >= 0; ++it) {
            if ((*it)->data(0, place_role) == QVariant{session_->place()} &&
                (*it)->data(0, row_role) == QVariant{row}) {
                current = *it;
                break;
            }
        }
        if (current != nullptr) {
            destination_list_->setCurrentItem(current);
        } else {
            destination_list_->clearSelection();
            destination_list_->setCurrentItem(nullptr);
        }
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
    layouts_status_->setText(session_->status());
    destinations_status_->setText(session_->status());
    syncing_ = false;
    updateButtons();
}

void OutputProfilesManager::updateButtons() {
    const auto available = session_->available();
    for (auto* widget : std::initializer_list<QWidget*>{
             layout_list_, layout_name_, directory_expression_, basename_expression_,
             sanitization_policy_, destination_list_}) {
        widget->setEnabled(available);
    }
    for (auto* widget : std::initializer_list<QWidget*>{destination_name_, destination_root_}) {
        widget->setEnabled(session_->destinationsAvailable());
    }
    layout_new_->setEnabled(session_->canEditLayouts());
    layout_save_->setEnabled(session_->canSaveLayout());
    layout_remove_->setEnabled(session_->canRemoveLayout());
    destination_copy_->setEnabled(available);
    destination_browse_->setEnabled(session_->canEditDestinations());
    destination_new_->setEnabled(destination_new_->menu() != nullptr
                                     ? session_->available()
                                     : session_->canEditDestinations());
    destination_save_->setEnabled(session_->canSaveDestination());
    destination_remove_->setEnabled(session_->canRemoveDestination());
}

} // namespace trackknife::bench
