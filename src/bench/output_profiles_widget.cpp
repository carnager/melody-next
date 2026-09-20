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
#include <utility>

namespace trackknife::bench {

namespace {

[[nodiscard]] std::string encoded_utf8(const QString& text) {
    const auto bytes = text.toUtf8();
    return std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

} // namespace

OutputProfilesManagerWidget::OutputProfilesManagerWidget(OutputProfileStore store, QWidget* parent)
    : QWidget(parent), store_(std::move(store)) {
    setObjectName(QStringLiteral("bench-output-profiles-manager"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    const auto expression_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    auto* sections = new QTabWidget(this);
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
    connect(destination_browse_, &QPushButton::clicked, this, [this] {
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
                       std::vector<persistence::SavedDestinationProfile> destinations,
                       QString error) {
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
        self->destinations_ = std::move(destinations);
        self->rebuildLists(self->editing_layout_id_, self->editing_destination_id_);
        self->status_->setText(QStringLiteral("%1 naming %2 · %3 move %4")
                                   .arg(self->layouts_.size())
                                   .arg(self->layouts_.size() == 1U ? QStringLiteral("layout")
                                                                    : QStringLiteral("layouts"))
                                   .arg(self->destinations_.size())
                                   .arg(self->destinations_.size() == 1U
                                            ? QStringLiteral("destination")
                                            : QStringLiteral("destinations")));
    });
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
        QString::fromStdString(core::escape_raw_path(destination_root_raw_path_)));
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
    if (!store_.save_destination || mutation_running_) {
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
    store_.save_destination(std::move(saved), [self, saved = std::move(retained)](QString error) {
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
    if (!editing_destination_id_ || !store_.remove_destination || mutation_running_) {
        return;
    }
    const auto id = *editing_destination_id_;
    mutation_running_ = true;
    updateButtons();
    const QPointer self{this};
    store_.remove_destination(id, [self, id](QString error) {
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
    destination_browse_->setEnabled(available && bool{store_.save_destination});
    destination_new_->setEnabled(available && bool{store_.save_destination});
    destination_save_->setEnabled(available && bool{store_.save_destination} &&
                                  !destination_name_->text().isEmpty() &&
                                  !destination_root_raw_path_.empty());
    destination_remove_->setEnabled(available && bool{store_.remove_destination} &&
                                    editing_destination_id_.has_value());
}

} // namespace trackknife::bench
