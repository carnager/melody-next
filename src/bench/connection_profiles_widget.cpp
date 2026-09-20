// SPDX-License-Identifier: GPL-3.0-only
#include "bench/connection_profiles_widget.hpp"
#include "trackknife/core/local_sources.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace trackknife::bench {
namespace {
QString pathText(const std::optional<std::string>& path) {
    return path ? QString::fromStdString(core::escape_raw_path(*path)) : QString{};
}
} // namespace
ConnectionProfilesWidget::ConnectionProfilesWidget(Profiles profiles, Writer writer,
                                                   QWidget* parent)
    : QWidget(parent), profiles_(std::move(profiles)), writer_(std::move(writer)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    selector_ = new QComboBox(this);
    selector_->setObjectName(QStringLiteral("settings-connection-profile"));
    selector_->setAccessibleName(tr("Saved connection"));
    selector_->setPlaceholderText(tr("New connection"));
    selector_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    layout->addWidget(selector_);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setVerticalSpacing(10);
    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("settings-connection-name"));
    host_ = new QLineEdit(this);
    host_->setObjectName(QStringLiteral("settings-connection-host"));
    host_->setPlaceholderText(tr("Host name, address, or Unix socket"));
    port_ = new QSpinBox(this);
    port_->setObjectName(QStringLiteral("settings-connection-port"));
    port_->setRange(1, 65535);
    root_ = new QLineEdit(this);
    root_->setObjectName(QStringLiteral("settings-connection-root"));
    root_->setPlaceholderText(tr("Optional local folder containing this server’s music"));
    auto* root_row = new QHBoxLayout;
    root_row->addWidget(root_, 1);
    auto* browse = new QPushButton(tr("Browse…"), this);
    root_row->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, tr("Local music folder"));
        if (!path.isEmpty())
            root_->setText(path);
    });
    startup_ = new QCheckBox(tr("Connect this profile on startup"), this);
    startup_->setObjectName(QStringLiteral("settings-connection-startup"));
    form->addRow(tr("Name:"), name_);
    form->addRow(tr("Host or socket:"), host_);
    form->addRow(tr("Port:"), port_);
    form->addRow(tr("Local music folder:"), root_row);
    form->addRow(QString{}, startup_);
    layout->addLayout(form);
    auto* buttons = new QHBoxLayout;
    auto* create = new QPushButton(tr("New"), this);
    create->setObjectName(QStringLiteral("settings-connection-new"));
    save_ = new QPushButton(tr("Save profile"), this);
    save_->setObjectName(QStringLiteral("settings-connection-save"));
    remove_ = new QPushButton(tr("Remove"), this);
    remove_->setObjectName(QStringLiteral("settings-connection-remove"));
    buttons->addWidget(create);
    buttons->addWidget(save_);
    buttons->addWidget(remove_);
    buttons->addStretch();
    layout->addLayout(buttons);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("settings-connection-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* note =
        new QLabel(tr("Profile changes save immediately. Use Connect to start a connection. "
                      "Passwords are entered there and kept only for the session."),
                   this);
    note->setWordWrap(true);
    layout->addWidget(note);
    connect(selector_, &QComboBox::currentIndexChanged, this, &ConnectionProfilesWidget::select);
    connect(create, &QPushButton::clicked, this, [this] {
        selector_->setCurrentIndex(-1);
        select(-1);
        name_->setFocus();
    });
    connect(save_, &QPushButton::clicked, this, [this] { persist(false); });
    connect(remove_, &QPushButton::clicked, this, [this] { persist(true); });
    connect(name_, &QLineEdit::textChanged, this, &ConnectionProfilesWidget::updateButtons);
    connect(host_, &QLineEdit::textChanged, this, &ConnectionProfilesWidget::updateButtons);
    rebuild(profiles_.empty() ? -1 : 0);
}
void ConnectionProfilesWidget::rebuild(int selected) {
    const QSignalBlocker block{selector_};
    selector_->clear();
    for (const auto& profile : profiles_)
        selector_->addItem(QString::fromStdString(profile.name));
    selector_->setCurrentIndex(selected);
    select(selected);
}
void ConnectionProfilesWidget::select(int row) {
    if (row < 0) {
        name_->clear();
        host_->setText(QStringLiteral("127.0.0.1"));
        port_->setValue(6600);
        root_->clear();
        startup_->setChecked(false);
    } else {
        const auto& profile = profiles_.at(static_cast<std::size_t>(row));
        name_->setText(QString::fromStdString(profile.name));
        host_->setText(QString::fromStdString(profile.host));
        port_->setValue(static_cast<int>(profile.port));
        root_->setText(pathText(profile.local_music_root));
        startup_->setChecked(profile.auto_connect);
    }
    updateButtons();
}
void ConnectionProfilesWidget::updateButtons() {
    save_->setEnabled(!saving_ && writer_ && !name_->text().trimmed().isEmpty() &&
                      !host_->text().trimmed().isEmpty());
    remove_->setEnabled(!saving_ && writer_ && selector_->currentIndex() >= 0);
}
void ConnectionProfilesWidget::persist(bool remove) {
    if (saving_ || !writer_)
        return;
    auto changed = profiles_;
    int row = selector_->currentIndex();
    if (remove) {
        if (row < 0)
            return;
        changed.erase(changed.begin() + row);
        row = changed.empty() ? -1 : 0;
    } else {
        auto profile = row >= 0 ? changed.at(static_cast<std::size_t>(row))
                                : persistence::ConnectionProfile{.id = core::StableId::random(),
                                                                 .name = {},
                                                                 .host = {},
                                                                 .port = 6600,
                                                                 .local_music_root = {},
                                                                 .auto_connect = false};
        profile.name = name_->text().trimmed().toStdString();
        profile.host = host_->text().trimmed().toStdString();
        profile.port = static_cast<unsigned>(port_->value());
        // Preserve raw path bytes when their escaped presentation was not edited.
        if (root_->text() != pathText(profile.local_music_root))
            profile.local_music_root =
                root_->text().isEmpty()
                    ? std::nullopt
                    : std::optional{QFile::encodeName(root_->text()).toStdString()};
        profile.auto_connect = startup_->isChecked();
        if (profile.auto_connect)
            for (auto& other : changed)
                other.auto_connect = false;
        if (row < 0) {
            row = static_cast<int>(changed.size());
            changed.push_back(profile);
        } else
            changed[static_cast<std::size_t>(row)] = profile;
    }
    saving_ = true;
    setEnabled(false);
    const QPointer self{this};
    auto retained = changed;
    writer_(std::move(changed), [self, changed = std::move(retained), row](QString error) {
        if (!self)
            return;
        self->saving_ = false;
        self->setEnabled(true);
        if (error.isEmpty()) {
            self->profiles_ = changed;
            self->rebuild(row);
        }
        self->status_->setText(error.isEmpty() ? self->tr("Connection profiles saved") : error);
        self->updateButtons();
    });
}
} // namespace trackknife::bench
