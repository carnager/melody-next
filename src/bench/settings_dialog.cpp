// SPDX-License-Identifier: GPL-3.0-only

#include "settings_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <utility>

namespace trackknife::bench {

SettingsDialog::SettingsDialog(QWidget* parent, OutputProfileStore profile_store)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Settings"));
    setObjectName(QStringLiteral("bench-settings-dialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(880, 520);

    const QSettings settings;
    auto* root = new QVBoxLayout(this);
    auto* body = new QHBoxLayout;
    pages_ = new QListWidget(this);
    pages_->setObjectName(QStringLiteral("bench-settings-pages"));
    pages_->setMaximumWidth(150);
    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("bench-settings-stack"));
    body->addWidget(pages_);
    body->addWidget(stack_, 1);
    root->addLayout(body, 1);
    const auto add_page = [this](const QString& title, QWidget* page) {
        pages_->addItem(title);
        stack_->addWidget(page);
    };
    connect(pages_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);

    // --- General -----------------------------------------------------------
    auto* general = new QWidget(this);
    auto* general_form = new QFormLayout(general);
    startup_ = new QComboBox(general);
    startup_->setObjectName(QStringLiteral("bench-settings-startup"));
    startup_->addItem(QStringLiteral("Local queue"), QStringLiteral("local"));
    startup_->addItem(QStringLiteral("MPD queue"), QStringLiteral("mpd"));
    const auto saved_context =
        settings.value(QLatin1String(startup_context_key), QStringLiteral("local")).toString();
    if (const auto position = startup_->findData(saved_context); position >= 0) {
        startup_->setCurrentIndex(position);
    }
    general_form->addRow(QStringLiteral("Start in:"), startup_);
    auto* root_row = new QHBoxLayout;
    music_root_ = new QLineEdit(general);
    music_root_->setObjectName(QStringLiteral("bench-settings-music-root"));
    music_root_->setText(settings.value(QLatin1String(music_root_key)).toString());
    music_root_->setPlaceholderText(QStringLiteral("MPD music folder, e.g. /mnt/nas/Music"));
    music_root_->setToolTip(
        QStringLiteral("The folder MPD serves its library from, as this machine sees it. "
                       "With it set, MPD selections can load as local files."));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), general);
    browse->setObjectName(QStringLiteral("bench-settings-music-root-browse"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose the MPD music folder"), music_root_->text());
        if (!chosen.isEmpty()) {
            music_root_->setText(chosen);
        }
    });
    root_row->addWidget(music_root_, 1);
    root_row->addWidget(browse);
    general_form->addRow(QStringLiteral("MPD music folder:"), root_row);
    add_page(QStringLiteral("General"), general);

    // --- Naming ------------------------------------------------------------
    auto* naming = new QWidget(this);
    auto* naming_layout = new QVBoxLayout(naming);
    if (profile_store.load) {
        auto* manager = new OutputProfilesManagerWidget(std::move(profile_store), naming);
        connect(manager, &OutputProfilesManagerWidget::profilesChanged, this,
                &SettingsDialog::outputProfilesChanged);
        naming_layout->addWidget(manager, 1);
    } else {
        auto* placeholder = new QLabel(
            QStringLiteral("Naming layouts and move destinations are managed from the "
                           "running application."),
            naming);
        placeholder->setWordWrap(true);
        naming_layout->addWidget(placeholder);
        naming_layout->addStretch(1);
    }
    add_page(QStringLiteral("Naming"), naming);

    // --- ReplayGain --------------------------------------------------------
    auto* replaygain = new QWidget(this);
    auto* replaygain_layout = new QVBoxLayout(replaygain);
    replaygain_sidecar_only_ =
        new QCheckBox(QStringLiteral("Store scan results in sidecar only"), replaygain);
    replaygain_sidecar_only_->setObjectName(QStringLiteral("bench-replaygain-sidecar-only"));
    replaygain_sidecar_only_->setToolTip(
        QStringLiteral("Keep ReplayGain values out of file tags; scans write the loudness "
                       "sidecar instead"));
    replaygain_sidecar_only_->setChecked(
        settings.value(QLatin1String(replaygain_sidecar_only_key), false).toBool());
    replaygain_layout->addWidget(replaygain_sidecar_only_);
    replaygain_true_peak_ =
        new QCheckBox(QStringLiteral("True peak as ReplayGain peak"), replaygain);
    replaygain_true_peak_->setObjectName(QStringLiteral("bench-replaygain-true-peak"));
    replaygain_true_peak_->setToolTip(
        QStringLiteral("Scan oversampled true peak instead of the plain sample peak"));
    replaygain_true_peak_->setChecked(
        settings.value(QLatin1String(replaygain_true_peak_key), false).toBool());
    replaygain_layout->addWidget(replaygain_true_peak_);
    replaygain_layout->addStretch(1);
    add_page(QStringLiteral("ReplayGain"), replaygain);

    // --- Covers ------------------------------------------------------------
    auto* covers = new QWidget(this);
    auto* covers_layout = new QVBoxLayout(covers);
    artwork_embed_ = new QCheckBox(QStringLiteral("Embed covers into the files"), covers);
    artwork_embed_->setObjectName(QStringLiteral("bench-artwork-embed"));
    artwork_embed_->setChecked(
        settings.value(QLatin1String(artwork_embed_key), true).toBool());
    covers_layout->addWidget(artwork_embed_);
    artwork_folder_image_ =
        new QCheckBox(QStringLiteral("Also write a folder image next to the tracks"), covers);
    artwork_folder_image_->setObjectName(QStringLiteral("bench-artwork-folder-image"));
    artwork_folder_image_->setChecked(
        settings.value(QLatin1String(artwork_folder_image_key), false).toBool());
    covers_layout->addWidget(artwork_folder_image_);
    auto* covers_form = new QFormLayout;
    artwork_folder_image_name_ = new QComboBox(covers);
    artwork_folder_image_name_->setObjectName(QStringLiteral("bench-artwork-folder-image-name"));
    artwork_folder_image_name_->setEditable(true);
    artwork_folder_image_name_->addItems(
        {QStringLiteral("cover.jpg"), QStringLiteral("folder.jpg")});
    artwork_folder_image_name_->setCurrentText(
        settings.value(QLatin1String(artwork_folder_image_name_key), QStringLiteral("cover.jpg"))
            .toString());
    covers_form->addRow(QStringLiteral("Folder image name:"), artwork_folder_image_name_);
    artwork_fetch_source_ = new QComboBox(covers);
    artwork_fetch_source_->setObjectName(QStringLiteral("bench-artwork-fetch-source"));
    artwork_fetch_source_->addItem(QStringLiteral("Cover Art Archive (front)"),
                                   QStringLiteral("coverartarchive"));
    covers_form->addRow(QStringLiteral("Fetch covers from:"), artwork_fetch_source_);
    covers_layout->addLayout(covers_form);
    auto* covers_note = new QLabel(
        QStringLiteral("The folder-image policy takes effect once the cover workflow lands; "
                       "embedding is today's behavior."),
        covers);
    covers_note->setWordWrap(true);
    covers_layout->addWidget(covers_note);
    covers_layout->addStretch(1);
    add_page(QStringLiteral("Covers"), covers);

    pages_->setCurrentRow(0);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("bench-settings-buttons"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void SettingsDialog::showPage(const Page page) {
    pages_->setCurrentRow(static_cast<int>(page));
}

void SettingsDialog::save() {
    QSettings settings;
    settings.setValue(QLatin1String(startup_context_key), startup_->currentData().toString());
    settings.setValue(QLatin1String(music_root_key), music_root_->text().trimmed());
    settings.setValue(QLatin1String(replaygain_sidecar_only_key),
                      replaygain_sidecar_only_->isChecked());
    settings.setValue(QLatin1String(replaygain_true_peak_key), replaygain_true_peak_->isChecked());
    settings.setValue(QLatin1String(artwork_embed_key), artwork_embed_->isChecked());
    settings.setValue(QLatin1String(artwork_folder_image_key),
                      artwork_folder_image_->isChecked());
    settings.setValue(QLatin1String(artwork_folder_image_name_key),
                      artwork_folder_image_name_->currentText().trimmed());
    settings.setValue(QLatin1String(artwork_fetch_source_key),
                      artwork_fetch_source_->currentData().toString());
}

} // namespace trackknife::bench
