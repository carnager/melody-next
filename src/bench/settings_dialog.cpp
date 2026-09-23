// SPDX-License-Identifier: GPL-3.0-only

#include "settings_dialog.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/shortcut_settings.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/audio/local_playback.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <utility>

namespace trackknife::bench {
namespace {
class SettingsPageDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto cell = option;
        initStyleOption(&cell, index);
        const auto base = cell.palette.color(QPalette::Base);
        const auto accent = cell.palette.color(QPalette::Highlight);
        const auto blend = [](int a, int b) { return (a * 7 + b) / 8; };
        cell.palette.setColor(QPalette::Highlight, QColor(blend(base.red(), accent.red()),
                                                          blend(base.green(), accent.green()),
                                                          blend(base.blue(), accent.blue())));
        cell.palette.setColor(QPalette::HighlightedText, cell.palette.color(QPalette::Text));
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        painter->save();
        painter->fillRect(option.rect, selected ? cell.palette.color(QPalette::Highlight) : base);
        auto font = cell.font;
        font.setBold(selected);
        painter->setFont(font);
        painter->setPen(cell.palette.color(QPalette::Text));
        painter->drawText(option.rect.adjusted(12, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                          cell.text);
        if (selected) {
            painter->fillRect(
                QRect(option.rect.left(), option.rect.top() + 5, 3, option.rect.height() - 10),
                accent);
        }
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.rheight() += 12;
        return size;
    }
};
} // namespace

SettingsDialog::SettingsDialog(QWidget* parent, OutputProfileStore profile_store,
                               std::function<QWidget*(QWidget*)> library_folders,
                               std::function<QWidget*(QWidget*)> connections,
                               std::function<QWidget*(QWidget*)> lastfm, QList<QAction*> shortcuts)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Settings"));
    setObjectName(QStringLiteral("bench-settings-dialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(940, 640);

    const QSettings settings;
    auto* root = new QVBoxLayout(this);
    auto* body = new QHBoxLayout;
    pages_ = new QListWidget(this);
    pages_->setObjectName(QStringLiteral("bench-settings-pages"));
    pages_->setMaximumWidth(150);
    pages_->setItemDelegate(new SettingsPageDelegate(pages_));
    pages_->setFrameShape(QFrame::NoFrame);
    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("bench-settings-stack"));
    body->addWidget(pages_);
    body->addWidget(stack_, 1);
    root->addLayout(body, 1);
    const auto add_page = [this](const QString& title, QWidget* page) {
        pages_->addItem(title);
        for (auto* form : page->findChildren<QFormLayout*>()) {
            if (form->rowWrapPolicy() != QFormLayout::WrapAllRows)
                form->setRowWrapPolicy(QFormLayout::WrapLongRows);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        }
        auto* scroll = new QScrollArea(stack_);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        stack_->addWidget(scroll);
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

    notifications_ = new QCheckBox(QStringLiteral("Track-change notifications"), general);
    notifications_->setObjectName(QStringLiteral("bench-settings-notifications"));
    notifications_->setToolTip(
        QStringLiteral("Show a notification when playback changes to another track."));
    notifications_->setChecked(
        settings.value(QStringLiteral("desktop/notifications"), false).toBool());
    general_form->addRow(QStringLiteral("Desktop:"), notifications_);
    notifications_background_ =
        new QCheckBox(QStringLiteral("Only while the app is in the background"), general);
    notifications_background_->setObjectName(
        QStringLiteral("bench-settings-notifications-background"));
    notifications_background_->setChecked(
        settings.value(QStringLiteral("desktop/notifications-background-only"), false).toBool());
    general_form->addRow(QString{}, notifications_background_);
    auto* test_notification = new QPushButton(QStringLiteral("Test notification"), general);
    test_notification->setObjectName(QStringLiteral("bench-settings-notification-test"));
    auto* notification_status = new QLabel(general);
    notification_status->setWordWrap(true);
    notification_status->setObjectName(QStringLiteral("bench-settings-notification-status"));
    general_form->addRow(test_notification, notification_status);
    auto* test_notifier = new DesktopNotifier(this);
    connect(test_notification, &QPushButton::clicked, this, [test_notifier, notification_status] {
        notification_status->setText(QStringLiteral("Sending…"));
        test_notifier->sendTest();
    });
    connect(test_notifier, &DesktopNotifier::deliveryFinished, this,
            [notification_status](const QString& error) {
                notification_status->setText(
                    error.isEmpty()
                        ? QStringLiteral("Accepted by your desktop. If no popup appears, check Do "
                                         "Not Disturb and desktop notification rules.")
                        : QStringLiteral("Notification failed: %1").arg(error));
            });

    panel_animations_ = new QCheckBox(QStringLiteral("Animate panel opening and closing"), general);
    panel_animations_->setObjectName(QStringLiteral("bench-settings-panel-animations"));
    panel_animations_->setChecked(
        settings.value(QStringLiteral("appearance/panel-animations"), true).toBool());
    general_form->addRow(QStringLiteral("Appearance:"), panel_animations_);
    add_page(QStringLiteral("General"), general);

    auto* playback = new QWidget(this);
    auto* playback_layout = new QVBoxLayout(playback);
    playback_layout->setSpacing(16);
    auto* playback_note = new QLabel(
        QStringLiteral(
            "These preferences apply to local playback. MPD uses the server’s playback settings."),
        playback);
    playback_note->setWordWrap(true);
    playback_layout->addWidget(playback_note);
    restore_playback_ = new QCheckBox(
        QStringLiteral("Restore local track and position on startup (paused)"), playback);
    restore_playback_->setObjectName(QStringLiteral("bench-settings-restore-playback"));
    restore_playback_->setChecked(
        settings.value(QLatin1String(restore_playback_key), false).toBool());
    playback_layout->addWidget(restore_playback_);
    auto* resume_note =
        new QLabel(QStringLiteral("Melody restores its own queue paused when the server restarts. "
                                  "Reconnecting this app never interrupts server playback."),
                   playback);
    resume_note->setWordWrap(true);
    playback_layout->addWidget(resume_note);
    auto* buffer_form = new QFormLayout;
    buffer_form->setVerticalSpacing(12);
    buffer_profile_ = new QComboBox(playback);
    buffer_profile_->setObjectName(QStringLiteral("bench-settings-buffer-profile"));
    buffer_profile_->addItem(QStringLiteral("Responsive"), QStringLiteral("responsive"));
    buffer_profile_->addItem(QStringLiteral("Balanced"), QStringLiteral("balanced"));
    buffer_profile_->addItem(QStringLiteral("Resilient"), QStringLiteral("resilient"));
    buffer_profile_->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
    buffer_form->addRow(QStringLiteral("Playback buffer:"), buffer_profile_);
    buffer_capacity_ = new QSpinBox(playback);
    buffer_capacity_->setObjectName(QStringLiteral("bench-settings-buffer-capacity"));
    buffer_capacity_->setRange(10, 10000);
    buffer_capacity_->setSuffix(QStringLiteral(" ms"));
    buffer_threshold_ = new QSpinBox(playback);
    buffer_threshold_->setObjectName(QStringLiteral("bench-settings-buffer-threshold"));
    buffer_threshold_->setRange(1, 10000);
    buffer_threshold_->setSuffix(QStringLiteral(" ms"));
    buffer_form->addRow(QStringLiteral("Capacity:"), buffer_capacity_);
    buffer_form->addRow(QStringLiteral("Start playback at:"), buffer_threshold_);
    playback_layout->addLayout(buffer_form);
    const auto update_buffer = [this] {
        const auto id = buffer_profile_->currentData().toString().toStdString();
        const auto preset = audio::playback_buffer_preset_from_id(id);
        if (preset) {
            const auto config = audio::playback_buffer_preset_config(*preset);
            buffer_capacity_->setValue(static_cast<int>(config.capacity.count()));
            buffer_threshold_->setValue(static_cast<int>(config.start_threshold.count()));
        }
        buffer_capacity_->setEnabled(!preset);
        buffer_threshold_->setEnabled(!preset);
    };
    connect(buffer_capacity_, &QSpinBox::valueChanged, buffer_threshold_, &QSpinBox::setMaximum);
    auto profile =
        settings.value(QStringLiteral("playback/buffer-profile"), QStringLiteral("balanced"))
            .toString();
    const int capacity = settings.value(QStringLiteral("playback/buffer-capacity-ms"), 750).toInt();
    const int threshold =
        settings.value(QStringLiteral("playback/buffer-start-threshold-ms"), 100).toInt();
    if (profile == QStringLiteral("custom") &&
        (capacity < 10 || capacity > 10000 || threshold < 1 || threshold > capacity))
        profile = QStringLiteral("balanced");
    buffer_capacity_->setValue(capacity);
    buffer_threshold_->setValue(threshold);
    const int profile_index = buffer_profile_->findData(profile);
    buffer_profile_->setCurrentIndex(profile_index >= 0 ? profile_index : 1);
    update_buffer();
    connect(buffer_profile_, &QComboBox::currentIndexChanged, this, update_buffer);
    auto* buffer_note = new QLabel(
        QStringLiteral("Responsive starts sooner; Resilient tolerates longer interruptions. "
                       "Buffer changes take effect on the next track."),
        playback);
    buffer_note->setWordWrap(true);
    playback_layout->addWidget(buffer_note);
    auto* preamp_form = new QFormLayout;
    preamp_form->setVerticalSpacing(12);
    const auto make_preamp = [&](const QString& name, const QString& key) {
        auto* spin = new QDoubleSpinBox(playback);
        spin->setObjectName(name);
        const double limit = audio::maximum_replay_gain_preamp_db;
        spin->setRange(-limit, limit);
        spin->setSingleStep(0.5);
        spin->setDecimals(1);
        spin->setSuffix(QStringLiteral(" dB"));
        spin->setValue(settings.value(key, 0.0).toDouble());
        return spin;
    };
    preamp_with_ = make_preamp(QStringLiteral("bench-settings-preamp-with"),
                               QStringLiteral("playback/rg-preamp-with"));
    preamp_without_ = make_preamp(QStringLiteral("bench-settings-preamp-without"),
                                  QStringLiteral("playback/rg-preamp-without"));
    preamp_form->addRow(QStringLiteral("Preamp with ReplayGain data:"), preamp_with_);
    preamp_form->addRow(QStringLiteral("Preamp without ReplayGain data:"), preamp_without_);
    playback_layout->addLayout(preamp_form);
    auto* preamp_note =
        new QLabel(QStringLiteral("Preamps apply when local ReplayGain is enabled. Choose Track, "
                                  "Album, or Automatic from the playback controls."),
                   playback);
    preamp_note->setWordWrap(true);
    playback_layout->addWidget(preamp_note);
    playback_layout->addStretch(1);
    add_page(QStringLiteral("Playback"), playback);

    auto* library = new QWidget(this);
    auto* library_layout = new QVBoxLayout(library);
    if (library_folders) {
        library_layout->addWidget(library_folders(library));
    } else {
        auto* note = new QLabel(
            QStringLiteral("Library folders are available from the running workspace."), library);
        note->setWordWrap(true);
        library_layout->addWidget(note);
        library_layout->addStretch(1);
    }
    // ADR-0220: which engine serves the catalogue and owns playback. Empty
    // means this process does both, which is the unchanged local behaviour --
    // stated here rather than left as a hand-edited setting, because
    // "is an engine in use" is otherwise unanswerable from the UI.
    auto* engine_form = new QFormLayout;
    engine_socket_ = new QLineEdit(library);
    engine_socket_->setObjectName(QStringLiteral("bench-settings-engine-socket"));
    engine_socket_->setPlaceholderText(
        QStringLiteral("empty: this process serves its own library and playback"));
    engine_socket_->setText(
        settings.value(QLatin1String(library_engine_socket_key), QString{}).toString());
    engine_form->addRow(QStringLiteral("Engine socket:"), engine_socket_);
    library_layout->addLayout(engine_form);
    auto* engine_note = new QLabel(
        QStringLiteral("The unix socket of a running trackknife engine (tkengine), which then owns "
                       "the library and playback. Takes effect when the workspace is reopened."),
        library);
    engine_note->setWordWrap(true);
    library_layout->addWidget(engine_note);

    add_page(QStringLiteral("Library"), library);

    auto* connections_page = new QWidget(this);
    auto* connections_layout = new QVBoxLayout(connections_page);
    connections_layout->setSpacing(18);
    if (connections)
        connections_layout->addWidget(connections(connections_page));
    else
        connections_layout->addWidget(new QLabel(
            QStringLiteral("Connection profiles are available from the running workspace."),
            connections_page));
    auto* fallback = new QFormLayout;
    fallback->addRow(QStringLiteral("Fallback music folder:"), root_row);
    connections_layout->addLayout(fallback);
    auto* fallback_note =
        new QLabel(QStringLiteral("Used when the connected profile has no local music folder."),
                   connections_page);
    fallback_note->setWordWrap(true);
    connections_layout->addWidget(fallback_note);
    connections_layout->addStretch(1);
    add_page(QStringLiteral("Connections"), connections_page);

    // --- Naming ------------------------------------------------------------
    auto* naming = new QWidget(this);
    auto* naming_layout = new QVBoxLayout(naming);
    if (profile_store.load) {
        auto* manager = new OutputProfilesManagerWidget(std::move(profile_store), naming);
        connect(manager, &OutputProfilesManagerWidget::profilesChanged, this,
                &SettingsDialog::outputProfilesChanged);
        naming_layout->addWidget(manager, 1);
    } else {
        auto* placeholder =
            new QLabel(QStringLiteral("Naming layouts and move destinations are managed from the "
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
    artwork_embed_->setChecked(settings.value(QLatin1String(artwork_embed_key), true).toBool());
    covers_layout->addWidget(artwork_embed_);
    artwork_folder_image_ =
        new QCheckBox(QStringLiteral("Write a front-cover image next to the tracks"), covers);
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
        QStringLiteral("Front covers use this storage policy on Apply. The filename extension "
                       "follows the image format (.jpg or .png). Folder replacements are reviewed "
                       "and retain recovery backups. Conversion has its own cover setting."),
        covers);
    covers_note->setWordWrap(true);
    covers_layout->addWidget(covers_note);
    covers_layout->addStretch(1);
    add_page(QStringLiteral("Covers"), covers);

    auto* metadata_services = new QWidget(this);
    auto* metadata_layout = new QVBoxLayout(metadata_services);
    metadata_layout->setSpacing(16);
    auto* musicbrainz_note =
        new QLabel(QStringLiteral("MusicBrainz text search works without an account or API key. "
                                  "Lookups start only when you request identification."),
                   metadata_services);
    musicbrainz_note->setWordWrap(true);
    metadata_layout->addWidget(musicbrainz_note);
    auto* key_form = new QFormLayout;
    acoustid_key_ = new QLineEdit(metadata_services);
    acoustid_key_->setObjectName(QStringLiteral("bench-settings-acoustid-key"));
    acoustid_key_->setEchoMode(QLineEdit::Password);
    acoustid_key_->setPlaceholderText(
        QStringLiteral("Client/application key for fingerprint lookup"));
    acoustid_key_->setText(settings.value(QLatin1String(acoustid_client_key)).toString());
    auto* key_row = new QHBoxLayout;
    key_row->addWidget(acoustid_key_, 1);
    auto* reveal = new QCheckBox(QStringLiteral("Show"), metadata_services);
    reveal->setObjectName(QStringLiteral("bench-settings-acoustid-show"));
    reveal->setAccessibleName(QStringLiteral("Show AcoustID client key"));
    connect(reveal, &QCheckBox::toggled, this, [this](bool show) {
        acoustid_key_->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });
    key_row->addWidget(reveal);
    key_form->addRow(QStringLiteral("AcoustID client key:"), key_row);
    metadata_layout->addLayout(key_form);
    auto* acoustid_note = new QLabel(
        QStringLiteral(
            "Audio fingerprint identification uses AcoustID and the fpcalc tool. "
            "Use an application/client key, not an AcoustID user key. "
            "The key is stored in your local application settings. Clear it to remove it."),
        metadata_services);
    acoustid_note->setWordWrap(true);
    metadata_layout->addWidget(acoustid_note);
    auto* acoustid_link =
        new QLabel(QStringLiteral("<a href=\"https://acoustid.org/new-application\">Register an "
                                  "application to get an AcoustID client key</a>"),
                   metadata_services);
    acoustid_link->setObjectName(QStringLiteral("bench-settings-acoustid-link"));
    acoustid_link->setOpenExternalLinks(true);
    acoustid_link->setTextInteractionFlags(Qt::TextBrowserInteraction);
    acoustid_link->setWordWrap(true);
    metadata_layout->addWidget(acoustid_link);
    auto* lastfm_form = new QFormLayout;
    lastfm_key_ = new QLineEdit(metadata_services);
    lastfm_key_->setObjectName(QStringLiteral("bench-settings-lastfm-key"));
    lastfm_key_->setEchoMode(QLineEdit::Password);
    lastfm_key_->setText(settings.value(QStringLiteral("lastfm/api-key")).toString());
    lastfm_form->addRow(QStringLiteral("Last.fm API key:"), lastfm_key_);
    metadata_layout->addLayout(lastfm_form);
    auto* lastfm_note = new QLabel(
        QStringLiteral(
            "Dynamic playlists can match Last.fm recommendations, loved tracks, top tracks, or "
            "tags "
            "to your library. Requests send the seed artist/track, username, or tag when you "
            "choose Refresh. "
            "The key is stored locally. Account authorization and scrobbling are separate under "
            "Last.fm settings. "
            "<a href=\"https://www.last.fm/api/account/create\">Get a Last.fm API key</a>"),
        metadata_services);
    lastfm_note->setWordWrap(true);
    lastfm_note->setOpenExternalLinks(true);
    lastfm_note->setTextInteractionFlags(Qt::TextBrowserInteraction);
    metadata_layout->addWidget(lastfm_note);
    metadata_layout->addStretch(1);
    add_page(QStringLiteral("Metadata services"), metadata_services);
    if (lastfm)
        add_page(QStringLiteral("Last.fm"), lastfm(this));
    if (!shortcuts.isEmpty()) {
        if (!lastfm)
            add_page(QStringLiteral("Last.fm"), new QWidget(this));
        shortcuts_ = new ShortcutSettings(shortcuts, this);
        add_page(tr("Shortcuts"), shortcuts_);
    }

    auto* save_note = new QLabel(this);
    save_note->setObjectName(QStringLiteral("bench-settings-save-note"));
    save_note->setWordWrap(true);
    root->addWidget(save_note);
    connect(pages_, &QListWidget::currentRowChanged, save_note, [save_note](int row) {
        const auto page = static_cast<Page>(row);
        if (page == Page::library)
            save_note->setText(
                QStringLiteral("Folder changes save immediately. Cancel does not undo them."));
        else if (page == Page::connections)
            save_note->setText(QStringLiteral("Save profile and Remove take effect immediately. "
                                              "The fallback folder uses Save below."));
        else if (page == Page::naming)
            save_note->setText(QStringLiteral("Save layout, Save destination, and Remove take "
                                              "effect immediately. Cancel does not undo them."));
        else
            save_note->setText(QStringLiteral("Preferences take effect when you choose Save."));
    });
    save_note->setText(QStringLiteral("Preferences take effect when you choose Save."));
    pages_->setCurrentRow(0);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("bench-settings-buttons"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (shortcuts_ && !shortcuts_->apply()) {
            showPage(Page::shortcuts);
            return;
        }
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void SettingsDialog::editCustomBuffer() {
    showPage(Page::playback);
    buffer_profile_->setCurrentIndex(buffer_profile_->findData(QStringLiteral("custom")));
    buffer_capacity_->setFocus();
}

void SettingsDialog::focusReplayGainPreamp() {
    showPage(Page::playback);
    preamp_with_->setFocus();
}

void SettingsDialog::showPage(const Page page) { pages_->setCurrentRow(static_cast<int>(page)); }

metadata::ArtworkStoragePolicy SettingsDialog::artworkPolicy() {
    const QSettings settings;
    return {.embed = settings.value(QLatin1String(artwork_embed_key), true).toBool(),
            .write_folder_image =
                settings.value(QLatin1String(artwork_folder_image_key), false).toBool(),
            .folder_image_name =
                QFile::encodeName(settings
                                      .value(QLatin1String(artwork_folder_image_name_key),
                                             QStringLiteral("cover.jpg"))
                                      .toString())
                    .toStdString(),
            .fetch_source = settings
                                .value(QLatin1String(artwork_fetch_source_key),
                                       QStringLiteral("coverartarchive"))
                                .toString()
                                .toStdString()};
}

void SettingsDialog::save() {
    QSettings settings;
    settings.setValue(QLatin1String(acoustid_client_key), acoustid_key_->text().trimmed());
    settings.setValue(QStringLiteral("lastfm/api-key"), lastfm_key_->text().trimmed());
    settings.setValue(QStringLiteral("appearance/panel-animations"),
                      panel_animations_->isChecked());
    settings.setValue(QStringLiteral("desktop/notifications"), notifications_->isChecked());
    settings.setValue(QStringLiteral("desktop/notifications-background-only"),
                      notifications_background_->isChecked());
    settings.setValue(QStringLiteral("playback/buffer-profile"), buffer_profile_->currentData());
    settings.setValue(QLatin1String(restore_playback_key), restore_playback_->isChecked());
    settings.setValue(QStringLiteral("playback/buffer-capacity-ms"), buffer_capacity_->value());
    settings.setValue(QStringLiteral("playback/buffer-start-threshold-ms"),
                      buffer_threshold_->value());
    settings.setValue(QStringLiteral("playback/rg-preamp-with"), preamp_with_->value());
    settings.setValue(QStringLiteral("playback/rg-preamp-without"), preamp_without_->value());
    settings.setValue(QLatin1String(startup_context_key), startup_->currentData().toString());
    settings.setValue(QLatin1String(music_root_key), music_root_->text().trimmed());
    settings.setValue(QLatin1String(library_engine_socket_key), engine_socket_->text().trimmed());
    settings.setValue(QLatin1String(replaygain_sidecar_only_key),
                      replaygain_sidecar_only_->isChecked());
    settings.setValue(QLatin1String(replaygain_true_peak_key), replaygain_true_peak_->isChecked());
    settings.setValue(QLatin1String(artwork_embed_key), artwork_embed_->isChecked());
    settings.setValue(QLatin1String(artwork_folder_image_key), artwork_folder_image_->isChecked());
    settings.setValue(QLatin1String(artwork_folder_image_name_key),
                      artwork_folder_image_name_->currentText().trimmed());
    settings.setValue(QLatin1String(artwork_fetch_source_key),
                      artwork_fetch_source_->currentData().toString());
}

} // namespace trackknife::bench
