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
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>

namespace trackknife::bench {

namespace {

[[nodiscard]] std::string encoded_utf8(const QString& text) {
    const auto bytes = text.toUtf8();
    return std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

} // namespace

OutputProfilesManagerWidget::OutputProfilesManagerWidget(OutputProfileStore store, QWidget* parent)
    : QWidget(parent), store_(std::move(store)), places_(store_.places) {
    if (places_.empty()) {
        // Only this computer's: the destinations the store itself holds.
        places_.push_back(DestinationPlace{
            .key = {},
            .name = store_.destinations_on.isEmpty() ? QStringLiteral("this computer")
                                                     : store_.destinations_on,
            .load =
                [load = store_.load](DestinationPlace::LoadCompletion completion) {
                    if (!load) {
                        completion({}, QStringLiteral("Profile storage is unavailable"));
                        return;
                    }
                    load([completion = std::move(completion)](
                             std::vector<persistence::SavedOutputLayoutProfile>,
                             std::vector<persistence::SavedDestinationProfile> destinations,
                             QString error) { completion(std::move(destinations), error); });
                },
            .save = store_.save_destination,
            .remove = store_.remove_destination,
            .folders = {},
            .copyable = {},
        });
    }
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
    sanitization_policy_->addItem(QStringLiteral("Linux filenames"), QStringLiteral("linux"));
    sanitization_policy_->addItem(QStringLiteral("Portable filenames"), QStringLiteral("portable"));
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
    for (const auto& place : places_) {
        place_list_->addItem(place.name, place.key);
    }
    place_list_->setEnabled(places_.size() > 1U);
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

    connect(layout_list_, &QComboBox::currentIndexChanged, this,
            &OutputProfilesManagerWidget::selectLayoutRow);
    connect(destination_list_, &QComboBox::currentIndexChanged, this,
            &OutputProfilesManagerWidget::selectDestinationRow);
    connect(layout_new_, &QPushButton::clicked, this, [this] {
        layout_list_->setCurrentIndex(-1);
        selectLayoutRow(-1);
        layout_name_->setFocus();
    });
    connect(destination_new_, &QPushButton::clicked, this, [this] {
        destination_list_->setCurrentIndex(-1);
        selectDestinationRow(-1);
        destination_name_->setFocus();
    });
    connect(layout_save_, &QPushButton::clicked, this, &OutputProfilesManagerWidget::saveLayout);
    connect(destination_save_, &QPushButton::clicked, this,
            &OutputProfilesManagerWidget::saveDestination);
    connect(layout_remove_, &QPushButton::clicked, this,
            &OutputProfilesManagerWidget::removeLayout);
    connect(destination_remove_, &QPushButton::clicked, this,
            &OutputProfilesManagerWidget::removeDestination);
    for (auto* edited : {layout_name_, basename_expression_, destination_name_}) {
        connect(edited, &QLineEdit::textChanged, this, &OutputProfilesManagerWidget::updateButtons);
    }
    connect(place_list_, &QComboBox::currentIndexChanged, this,
            &OutputProfilesManagerWidget::selectPlace);
    connect(destination_copy_, &QPushButton::clicked, this,
            &OutputProfilesManagerWidget::copyDestinations);
    connect(destination_browse_, &QPushButton::clicked, this, [this] {
        const auto& place = places_[static_cast<std::size_t>(place_)];
        if (place.folders) {
            auto* chooser =
                new EngineFolderDialog(place.name, place.folders, destination_root_raw_path_, this);
            connect(
                chooser, &EngineFolderDialog::folderChosen, this, [this](const QByteArray& chosen) {
                    destination_root_raw_path_.assign(chosen.constData(),
                                                      static_cast<std::size_t>(chosen.size()));
                    destination_root_->setText(
                        QString::fromStdString(core::display_raw_path(destination_root_raw_path_)));
                    updateButtons();
                });
            chooser->show();
            return;
        }
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
            QString::fromStdString(core::display_raw_path(destination_root_raw_path_)));
        updateButtons();
    });

    reload();
}

void OutputProfilesManagerWidget::reload() {
    if (!store_.load) {
        status_->setText(QStringLiteral("Profile storage is unavailable"));
        updateButtons();
        return;
    }
    loading_ = true;
    updateButtons();
    const QPointer self{this};
    store_.load([self](std::vector<persistence::SavedOutputLayoutProfile> layouts,
                       std::vector<persistence::SavedDestinationProfile>, QString error) {
        if (!self) {
            return;
        }
        self->loading_ = false;
        if (!error.isEmpty()) {
            self->status_->setText(
                QStringLiteral("Could not load output profiles · %1").arg(error));
            self->updateButtons();
            return;
        }
        self->layouts_ = std::move(layouts);
        self->rebuildLists(self->editing_layout_id_, self->editing_destination_id_);
        self->reloadDestinations();
    });
}

void OutputProfilesManagerWidget::reloadDestinations() {
    const auto& place = places_[static_cast<std::size_t>(place_)];
    if (!place.load) {
        return;
    }
    loading_ = true;
    updateButtons();
    const QPointer self{this};
    const auto asked = place_;
    place.load([self, asked](std::vector<persistence::SavedDestinationProfile> destinations,
                             QString error) {
        if (!self || asked != self->place_) {
            return;
        }
        self->loading_ = false;
        const auto& shown = self->places_[static_cast<std::size_t>(asked)];
        if (!error.isEmpty()) {
            self->destinations_.clear();
            self->rebuildLists(self->editing_layout_id_, {});
            self->status_->setText(QStringLiteral("Could not load the move destinations on %1 · %2")
                                       .arg(shown.name, error));
            self->updateButtons();
            return;
        }
        std::ranges::sort(destinations, {},
                          [](const auto& profile) { return profile.profile.name; });
        self->destinations_ = std::move(destinations);
        self->rebuildLists(self->editing_layout_id_, self->editing_destination_id_);
        const auto copyable = shown.copyable ? shown.copyable() : decltype(shown.copyable()){};
        const auto missing = std::ranges::count_if(copyable, [&self](const auto& candidate) {
            return std::ranges::none_of(self->destinations_, [&candidate](const auto& held) {
                return held.profile.root_raw_path == candidate.profile.root_raw_path;
            });
        });
        self->destination_copy_->setText(QStringLiteral("Copy %1 from this computer").arg(missing));
        self->destination_copy_->setVisible(missing > 0);
        self->status_->setText(QStringLiteral("%1 naming %2 · %3 move %4 on %5")
                                   .arg(self->layouts_.size())
                                   .arg(self->layouts_.size() == 1U ? QStringLiteral("layout")
                                                                    : QStringLiteral("layouts"))
                                   .arg(self->destinations_.size())
                                   .arg(self->destinations_.size() == 1U
                                            ? QStringLiteral("destination")
                                            : QStringLiteral("destinations"))
                                   .arg(shown.name));
        self->updateButtons();
    });
}

void OutputProfilesManagerWidget::selectPlace(const int index) {
    if (index < 0 || index >= static_cast<int>(places_.size()) || index == place_) {
        return;
    }
    place_ = index;
    editing_destination_id_.reset();
    destinations_.clear();
    destination_copy_->hide();
    rebuildLists(editing_layout_id_, {});
    reloadDestinations();
}

void OutputProfilesManagerWidget::showDestinationsOf(const QString& key) {
    const auto found = std::ranges::find(places_, key, &DestinationPlace::key);
    if (found != places_.end()) {
        place_list_->setCurrentIndex(static_cast<int>(found - places_.begin()));
    }
    sections_->setCurrentIndex(1);
}

void OutputProfilesManagerWidget::copyDestinations() {
    const auto& place = places_[static_cast<std::size_t>(place_)];
    if (!place.copyable || !place.save || mutation_running_) {
        return;
    }
    std::vector<persistence::SavedDestinationProfile> missing;
    for (auto candidate : place.copyable()) {
        if (std::ranges::none_of(destinations_, [&candidate](const auto& held) {
                return held.profile.root_raw_path == candidate.profile.root_raw_path;
            })) {
            candidate.id = core::StableId::random();
            missing.push_back(std::move(candidate));
        }
    }
    if (missing.empty()) {
        return;
    }
    mutation_running_ = true;
    updateButtons();
    // One after another, then the list as the engine has it.
    auto remaining =
        std::make_shared<std::vector<persistence::SavedDestinationProfile>>(std::move(missing));
    auto next = std::make_shared<std::function<void(QString)>>();
    const QPointer self{this};
    const auto asked = place_;
    *next = [self, asked, remaining, next](QString error) {
        if (!self) {
            return;
        }
        if (!error.isEmpty() || remaining->empty() || asked != self->place_) {
            self->mutation_running_ = false;
            if (!error.isEmpty()) {
                self->status_->setText(
                    QStringLiteral("Could not copy a move destination · %1").arg(error));
            }
            self->reloadDestinations();
            emit self->profilesChanged();
            *next = {};
            return;
        }
        auto destination = std::move(remaining->back());
        remaining->pop_back();
        self->places_[static_cast<std::size_t>(asked)].save(std::move(destination), *next);
    };
    (*next)(QString{});
}

void OutputProfilesManagerWidget::rebuildLists(const std::optional<core::StableId> layout_id,
                                               const std::optional<core::StableId> destination_id) {
    const QSignalBlocker layout_blocker{layout_list_};
    const QSignalBlocker destination_blocker{destination_list_};
    layout_list_->clear();
    for (const auto& saved : layouts_) {
        layout_list_->addItem(displayText(saved.profile.name));
    }
    destination_list_->clear();
    for (const auto& saved : destinations_) {
        destination_list_->addItem(displayText(saved.profile.name));
    }
    const auto layout_row = [&]() -> int {
        if (!layout_id) {
            return layouts_.empty() ? -1 : 0;
        }
        const auto found =
            std::ranges::find(layouts_, *layout_id, &persistence::SavedOutputLayoutProfile::id);
        return found == layouts_.end() ? (layouts_.empty() ? -1 : 0)
                                       : static_cast<int>(found - layouts_.begin());
    }();
    const auto destination_row = [&]() -> int {
        if (!destination_id) {
            return destinations_.empty() ? -1 : 0;
        }
        const auto found = std::ranges::find(destinations_, *destination_id,
                                             &persistence::SavedDestinationProfile::id);
        return found == destinations_.end() ? (destinations_.empty() ? -1 : 0)
                                            : static_cast<int>(found - destinations_.begin());
    }();
    layout_list_->setCurrentIndex(layout_row);
    destination_list_->setCurrentIndex(destination_row);
    selectLayoutRow(layout_row);
    selectDestinationRow(destination_row);
}

void OutputProfilesManagerWidget::selectLayoutRow(const int row) {
    if (row < 0 || row >= static_cast<int>(layouts_.size())) {
        editing_layout_id_.reset();
        layout_name_->clear();
        directory_expression_->clear();
        basename_expression_->clear();
        sanitization_policy_->setCurrentIndex(0);
        updateButtons();
        return;
    }
    const auto& saved = layouts_[static_cast<std::size_t>(row)];
    editing_layout_id_ = saved.id;
    layout_name_->setText(displayText(saved.profile.name));
    directory_expression_->setText(displayText(saved.profile.relative_directory_expression));
    basename_expression_->setText(displayText(saved.profile.basename_expression));
    sanitization_policy_->setCurrentIndex(std::max(
        0, sanitization_policy_->findData(displayText(saved.profile.sanitization_policy.name))));
    directory_expression_->setCursorPosition(0);
    basename_expression_->setCursorPosition(0);
    updateButtons();
}

void OutputProfilesManagerWidget::selectDestinationRow(const int row) {
    if (row < 0 || row >= static_cast<int>(destinations_.size())) {
        editing_destination_id_.reset();
        destination_root_raw_path_.clear();
        destination_name_->clear();
        destination_root_->clear();
        updateButtons();
        return;
    }
    const auto& saved = destinations_[static_cast<std::size_t>(row)];
    editing_destination_id_ = saved.id;
    destination_root_raw_path_ = saved.profile.root_raw_path;
    destination_name_->setText(displayText(saved.profile.name));
    destination_root_->setText(
        QString::fromStdString(core::display_raw_path(destination_root_raw_path_)));
    updateButtons();
}

void OutputProfilesManagerWidget::saveLayout() {
    if (!store_.save_layout || mutation_running_) {
        return;
    }
    persistence::SavedOutputLayoutProfile saved{
        .id = editing_layout_id_.value_or(core::StableId::random()),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = encoded_utf8(layout_name_->text()),
                .dialect = {},
                .relative_directory_expression = encoded_utf8(directory_expression_->text()),
                .basename_expression = encoded_utf8(basename_expression_->text()),
                .sanitization_policy = {encoded_utf8(
                                            sanitization_policy_->currentData().toString()),
                                        1U},
            },
    };
    if (auto valid = operations::validate_output_layout_profile(saved.profile); !valid) {
        status_->setText(QStringLiteral("Naming layout is not valid · %1")
                             .arg(displayText(valid.error().message)));
        return;
    }
    mutation_running_ = true;
    updateButtons();
    const QPointer self{this};
    auto retained = saved;
    store_.save_layout(std::move(saved), [self, saved = std::move(retained)](QString error) {
        if (!self) {
            return;
        }
        self->mutation_running_ = false;
        if (!error.isEmpty()) {
            self->status_->setText(QStringLiteral("Could not save naming layout · %1").arg(error));
            self->updateButtons();
            return;
        }
        const auto found =
            std::ranges::find(self->layouts_, saved.id, &persistence::SavedOutputLayoutProfile::id);
        if (found == self->layouts_.end()) {
            self->layouts_.push_back(saved);
        } else {
            *found = saved;
        }
        std::ranges::sort(self->layouts_, {},
                          [](const auto& profile) { return profile.profile.name; });
        self->editing_layout_id_ = saved.id;
        self->rebuildLists(saved.id, self->editing_destination_id_);
        self->status_->setText(QStringLiteral("Naming layout saved"));
        emit self->profilesChanged();
    });
}

void OutputProfilesManagerWidget::saveDestination() {
    if (!places_[static_cast<std::size_t>(place_)].save || mutation_running_) {
        return;
    }
    persistence::SavedDestinationProfile saved{
        .id = editing_destination_id_.value_or(core::StableId::random()),
        .profile =
            operations::DestinationProfile{
                .schema_version = 1U,
                .name = encoded_utf8(destination_name_->text()),
                .root_raw_path = destination_root_raw_path_,
                .containment_policy = {"lexical-beneath-root", 1U},
            },
    };
    if (auto valid = operations::validate_destination_profile(saved.profile); !valid) {
        status_->setText(QStringLiteral("Move destination is not valid · %1")
                             .arg(displayText(valid.error().message)));
        return;
    }
    mutation_running_ = true;
    updateButtons();
    const QPointer self{this};
    auto retained = saved;
    places_[static_cast<std::size_t>(place_)].save(std::move(saved), [self,
                                                                      saved = std::move(retained)](
                                                                         QString error) {
        if (!self) {
            return;
        }
        self->mutation_running_ = false;
        if (!error.isEmpty()) {
            self->status_->setText(
                QStringLiteral("Could not save move destination · %1").arg(error));
            self->updateButtons();
            return;
        }
        const auto found = std::ranges::find(self->destinations_, saved.id,
                                             &persistence::SavedDestinationProfile::id);
        if (found == self->destinations_.end()) {
            self->destinations_.push_back(saved);
        } else {
            *found = saved;
        }
        std::ranges::sort(self->destinations_, {},
                          [](const auto& profile) { return profile.profile.name; });
        self->editing_destination_id_ = saved.id;
        self->rebuildLists(self->editing_layout_id_, saved.id);
        self->status_->setText(QStringLiteral("Move destination saved"));
        emit self->profilesChanged();
    });
}

void OutputProfilesManagerWidget::removeLayout() {
    if (!editing_layout_id_ || !store_.remove_layout || mutation_running_) {
        return;
    }
    const auto id = *editing_layout_id_;
    mutation_running_ = true;
    updateButtons();
    const QPointer self{this};
    store_.remove_layout(id, [self, id](QString error) {
        if (!self) {
            return;
        }
        self->mutation_running_ = false;
        if (!error.isEmpty()) {
            self->status_->setText(
                QStringLiteral("Could not remove naming layout · %1").arg(error));
            self->updateButtons();
            return;
        }
        std::erase_if(self->layouts_, [id](const auto& saved) { return saved.id == id; });
        self->editing_layout_id_.reset();
        self->rebuildLists({}, self->editing_destination_id_);
        self->status_->setText(QStringLiteral("Naming layout removed"));
        emit self->profilesChanged();
    });
}

void OutputProfilesManagerWidget::removeDestination() {
    if (!editing_destination_id_ || !places_[static_cast<std::size_t>(place_)].remove ||
        mutation_running_) {
        return;
    }
    const auto id = *editing_destination_id_;
    mutation_running_ = true;
    updateButtons();
    const QPointer self{this};
    places_[static_cast<std::size_t>(place_)].remove(id, [self, id](QString error) {
        if (!self) {
            return;
        }
        self->mutation_running_ = false;
        if (!error.isEmpty()) {
            self->status_->setText(
                QStringLiteral("Could not remove move destination · %1").arg(error));
            self->updateButtons();
            return;
        }
        std::erase_if(self->destinations_, [id](const auto& saved) { return saved.id == id; });
        self->editing_destination_id_.reset();
        self->rebuildLists(self->editing_layout_id_, {});
        self->status_->setText(QStringLiteral("Move destination removed"));
        emit self->profilesChanged();
    });
}

void OutputProfilesManagerWidget::updateButtons() {
    const auto available = !loading_ && !mutation_running_;
    for (auto* widget : std::initializer_list<QWidget*>{
             layout_list_, layout_name_, directory_expression_, basename_expression_,
             sanitization_policy_, destination_list_, destination_name_, destination_root_}) {
        widget->setEnabled(available);
    }
    layout_new_->setEnabled(available && bool{store_.save_layout});
    layout_save_->setEnabled(available && bool{store_.save_layout} &&
                             !layout_name_->text().isEmpty() &&
                             !basename_expression_->text().isEmpty());
    layout_remove_->setEnabled(available && bool{store_.remove_layout} &&
                               editing_layout_id_.has_value());
    const auto& place = places_[static_cast<std::size_t>(place_)];
    place_list_->setEnabled(available && places_.size() > 1U);
    destination_copy_->setEnabled(available);
    destination_browse_->setEnabled(available && bool{place.save});
    destination_new_->setEnabled(available && bool{place.save});
    destination_save_->setEnabled(available && bool{place.save} &&
                                  !destination_name_->text().isEmpty() &&
                                  !destination_root_raw_path_.empty());
    destination_remove_->setEnabled(available && bool{place.remove} &&
                                    editing_destination_id_.has_value());
}

} // namespace trackknife::bench
