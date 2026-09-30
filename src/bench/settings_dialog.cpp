// SPDX-License-Identifier: GPL-3.0-only

#include "settings_dialog.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/shortcut_settings.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/audio/local_playback.hpp"
#include "trackknife/discovery/mdns.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QSysInfo>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
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

SettingsDialog::~SettingsDialog() = default;

SettingsDialog::SettingsDialog(QWidget* parent, OutputProfileStore profile_store,
                               std::function<QWidget*(QWidget*)> library_folders,
                               std::function<QWidget*(QWidget*)> lastfm, QList<QAction*> shortcuts)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Settings"));
    setObjectName(QStringLiteral("bench-settings-dialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(940, 640);

    session_ = new SettingsSession(this);
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
            // Fields as wide as what goes in them, not the whole page: a
            // number or a choice stays short, text gets room for a path.
            for (int row = 0; row < form->rowCount(); ++row) {
                auto* item = form->itemAt(row, QFormLayout::FieldRole);
                auto* field = item != nullptr ? item->widget() : nullptr;
                if (qobject_cast<QAbstractSpinBox*>(field) != nullptr ||
                    qobject_cast<QComboBox*>(field) != nullptr) {
                    field->setMaximumWidth(260);
                } else if (qobject_cast<QLineEdit*>(field) != nullptr ||
                           qobject_cast<QKeySequenceEdit*>(field) != nullptr) {
                    field->setMaximumWidth(460);
                }
            }
        }
        // The page says what it is at its top.
        auto* framed = new QWidget(stack_);
        auto* framed_layout = new QVBoxLayout(framed);
        framed_layout->setContentsMargins(12, 8, 12, 8);
        framed_layout->setSpacing(10);
        auto* heading = new QLabel(title, framed);
        heading->setObjectName(QStringLiteral("bench-settings-page-title"));
        auto heading_font = heading->font();
        heading_font.setPointSizeF(heading_font.pointSizeF() * 1.3);
        heading_font.setWeight(QFont::DemiBold);
        heading->setFont(heading_font);
        framed_layout->addWidget(heading);
        framed_layout->addWidget(page, 1);
        auto* scroll = new QScrollArea(stack_);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(framed);
        stack_->addWidget(scroll);
    };
    connect(pages_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);

    // --- General -----------------------------------------------------------
    auto* general = new QWidget(this);
    auto* general_form = new QFormLayout(general);

    notifications_ = new QCheckBox(QStringLiteral("Track-change notifications"), general);
    notifications_->setObjectName(QStringLiteral("bench-settings-notifications"));
    notifications_->setToolTip(
        QStringLiteral("Show a notification when playback changes to another track."));
    bind(notifications_, "desktop/notifications");
    general_form->addRow(QStringLiteral("Desktop:"), notifications_);
    notifications_background_ =
        new QCheckBox(QStringLiteral("Only while the app is in the background"), general);
    notifications_background_->setObjectName(
        QStringLiteral("bench-settings-notifications-background"));
    bind(notifications_background_, "desktop/notifications-background-only");
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
    bind(panel_animations_, "appearance/panel-animations");
    general_form->addRow(QStringLiteral("Appearance:"), panel_animations_);
    // ADR-0233: the lists as tabs above the tracks, or as a pane beside them.
    lists_display_ = new QComboBox(general);
    lists_display_->setObjectName(QStringLiteral("bench-settings-lists-display"));
    bind(lists_display_, SettingsSession::listsDisplays(), "appearance/lists-display");
    general_form->addRow(QStringLiteral("Show lists as:"), lists_display_);
    add_page(QStringLiteral("General"), general);

    auto* playback = new QWidget(this);
    auto* playback_layout = new QVBoxLayout(playback);
    playback_layout->setSpacing(16);
    auto* resume_note =
        new QLabel(QStringLiteral("The engine plays the music and keeps its queue: closing this "
                                  "window never interrupts it, and an engine that restarts "
                                  "restores its queue, paused."),
                   playback);
    resume_note->setWordWrap(true);
    resume_note->setForegroundRole(QPalette::PlaceholderText);
    playback_layout->addWidget(resume_note);
    auto* buffer_form = new QFormLayout;
    buffer_form->setVerticalSpacing(12);
    buffer_profile_ = new QComboBox(playback);
    buffer_profile_->setObjectName(QStringLiteral("bench-settings-buffer-profile"));
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
    bind(buffer_capacity_, "playback/buffer-capacity-ms");
    bind(buffer_threshold_, "playback/buffer-start-threshold-ms");
    bind(buffer_profile_, SettingsSession::bufferProfiles(), "playback/buffer-profile");
    syncs_.push_back([this] {
        const auto custom = session_->bufferCustom();
        buffer_capacity_->setEnabled(custom);
        buffer_threshold_->setEnabled(custom);
        buffer_threshold_->setMaximum(buffer_capacity_->value());
    });
    auto* buffer_note = new QLabel(
        QStringLiteral("Responsive starts sooner; Resilient tolerates longer interruptions. "
                       "Buffer changes take effect on the next track."),
        playback);
    buffer_note->setWordWrap(true);
    buffer_note->setForegroundRole(QPalette::PlaceholderText);
    playback_layout->addWidget(buffer_note);
    auto* preamp_form = new QFormLayout;
    preamp_form->setVerticalSpacing(12);
    const auto make_preamp = [&](const QString& name, const char* key) {
        auto* spin = new QDoubleSpinBox(playback);
        spin->setObjectName(name);
        const auto limit = SettingsSession::maximumPreamp();
        spin->setRange(-limit, limit);
        spin->setSingleStep(0.5);
        spin->setDecimals(1);
        spin->setSuffix(QStringLiteral(" dB"));
        bind(spin, key);
        return spin;
    };
    preamp_with_ =
        make_preamp(QStringLiteral("bench-settings-preamp-with"), "playback/rg-preamp-with");
    preamp_without_ =
        make_preamp(QStringLiteral("bench-settings-preamp-without"), "playback/rg-preamp-without");
    preamp_form->addRow(QStringLiteral("Preamp with ReplayGain data:"), preamp_with_);
    preamp_form->addRow(QStringLiteral("Preamp without ReplayGain data:"), preamp_without_);
    playback_layout->addLayout(preamp_form);
    auto* preamp_note =
        new QLabel(QStringLiteral("Preamps apply when local ReplayGain is enabled. Choose Track, "
                                  "Album, or Automatic from the playback controls."),
                   playback);
    preamp_note->setWordWrap(true);
    preamp_note->setForegroundRole(QPalette::PlaceholderText);
    playback_layout->addWidget(preamp_note);
    playback_layout->addStretch(1);
    add_page(QStringLiteral("Playback"), playback);

    auto* library = new QWidget(this);
    auto* library_layout = new QVBoxLayout(library);
    auto* folders = new QGroupBox(QStringLiteral("Music folders"), library);
    auto* folders_layout = new QVBoxLayout(folders);
    if (library_folders) {
        folders_layout->addWidget(library_folders(folders));
    } else {
        auto* note = new QLabel(
            QStringLiteral("Library folders are available from the running workspace."), folders);
        note->setWordWrap(true);
        note->setForegroundRole(QPalette::PlaceholderText);
        folders_layout->addWidget(note);
    }
    library_layout->addWidget(folders, 1);

    // Ratings in the files: written, their backup copy (ADR-0245), and read
    // from other players' tags.
    auto* ratings = new QGroupBox(QStringLiteral("Ratings"), library);
    auto* ratings_form = new QFormLayout(ratings);
    ratings_in_tags_ = new QCheckBox(QStringLiteral("Write track ratings into the files"), ratings);
    ratings_in_tags_->setObjectName(QStringLiteral("bench-settings-ratings-in-tags"));
    ratings_in_tags_->setToolTip(
        QStringLiteral("Ratings stay in each engine's library either way. With this on, each "
                       "engine also writes a track's rating into its files as FMPS_RATING, "
                       "which other players read. Album ratings are not written."));
    bind(ratings_in_tags_, ratings_in_tags_key);
    ratings_form->addRow(QStringLiteral("In the files:"), ratings_in_tags_);

    auto* backup_row = new QHBoxLayout;
    rating_backup_ = new QCheckBox(QStringLiteral("Also write to the tag"), ratings);
    rating_backup_->setObjectName(QStringLiteral("bench-settings-rating-backup"));
    rating_backup_->setToolTip(
        QStringLiteral("A second copy of each rating, as its plain 0-10 number, in a tag other "
                       "players leave alone -- so a player that rewrites FMPS_RATING or POPM "
                       "cannot lose it. A tagging script's Convert rating step (scale 0-10) "
                       "brings it back."));
    bind(rating_backup_, rating_backup_key);
    rating_backup_tag_ = new QLineEdit(ratings);
    rating_backup_tag_->setObjectName(QStringLiteral("bench-settings-rating-backup-tag"));
    rating_backup_tag_->setMaximumWidth(260);
    bind(rating_backup_tag_, rating_backup_tag_key);
    backup_row->addWidget(rating_backup_);
    backup_row->addWidget(rating_backup_tag_, 1);
    backup_row->addStretch(1);
    auto* backup_label = new QLabel(QStringLiteral("Backup copy:"), ratings);
    ratings_form->addRow(backup_label, backup_row);
    // Under the field: why the name will not do, or what an official tag
    // loses.
    rating_backup_note_ = new QLabel(ratings);
    rating_backup_note_->setObjectName(QStringLiteral("bench-settings-rating-backup-note"));
    rating_backup_note_->setWordWrap(true);
    rating_backup_note_->setForegroundRole(QPalette::PlaceholderText);
    ratings_form->addRow(QString{}, rating_backup_note_);

    rating_tag_scale_ = new QComboBox(ratings);
    rating_tag_scale_->setObjectName(QStringLiteral("bench-settings-rating-tag-scale"));
    rating_tag_scale_->setToolTip(
        QStringLiteral("Ratings other players left in your files are taken into the library. "
                       "FMPS_RATING and MP3 POPM always are; a plain RATING tag has no agreed "
                       "scale, so it is read only on the one chosen here."));
    bind(rating_tag_scale_, SettingsSession::ratingScales(), rating_tag_scale_key);
    ratings_form->addRow(QStringLiteral("Other players' RATING tags:"), rating_tag_scale_);
    library_layout->addWidget(ratings);

    const auto refresh_backup = [this, backup_label] {
        const auto writing = ratings_in_tags_->isChecked();
        rating_backup_->setEnabled(writing);
        backup_label->setEnabled(writing);
        rating_backup_tag_->setEnabled(writing && rating_backup_->isChecked());
        const auto note = SettingsSession::ratingBackupNote(rating_backup_tag_->text());
        rating_backup_note_->setText(note);
        rating_backup_note_->setVisible(writing && rating_backup_->isChecked() &&
                                        !note.isEmpty());
    };
    connect(ratings_in_tags_, &QCheckBox::toggled, this, refresh_backup);
    connect(rating_backup_, &QCheckBox::toggled, this, refresh_backup);
    connect(rating_backup_tag_, &QLineEdit::textChanged, this, refresh_backup);
    refresh_backup();
    add_page(QStringLiteral("Library"), library);

    // --- Engine ------------------------------------------------------------
    auto* engine = new QWidget(this);
    auto* engine_layout = new QVBoxLayout(engine);
    auto* engine_intro = new QLabel(
        QStringLiteral("melodyd plays the music and keeps the library. Trackknife starts this "
                       "computer's, and it plays on after the window closes."),
        engine);
    engine_intro->setWordWrap(true);
    engine_intro->setForegroundRole(QPalette::PlaceholderText);
    engine_layout->addWidget(engine_intro);

    // ADR-0223: one password for the engines on this network, as melodyd has
    // one: what this computer's engine asks of others when shared, and what
    // this computer gives the remote engine unless that one differs.
    auto* password_form = new QFormLayout;
    engine_password_ = new QLineEdit(engine);
    engine_password_->setObjectName(QStringLiteral("bench-settings-engine-password"));
    engine_password_->setEchoMode(QLineEdit::Password);
    engine_password_->setPlaceholderText(QStringLiteral("the same on every engine and agent"));
    engine_password_->setToolTip(QStringLiteral(
        "Needed to share this computer's engine, and given to the remote engine unless it has a "
        "password of its own below. It travels unencrypted: across an untrusted network, use "
        "WireGuard or a TLS proxy."));
    bind(engine_password_, engine_password_key);
    password_form->addRow(QStringLiteral("Password:"), engine_password_);
    engine_layout->addLayout(password_form);

    // ADR-0226/0228: this computer's engine, shared on the network so output
    // agents -- the bedside speaker -- can play what it plays.
    auto* sharing = new QGroupBox(QStringLiteral("This computer's engine"), engine);
    auto* sharing_form = new QFormLayout(sharing);
    show_local_library_ = new QCheckBox(QStringLiteral("Show this computer's library"), sharing);
    show_local_library_->setObjectName(QStringLiteral("bench-settings-show-local-library"));
    show_local_library_->setToolTip(QStringLiteral(
        "Hide it when all your music is in the remote engine's library. Files on this computer "
        "still open and play here."));
    bind(show_local_library_, library_show_local_key);
    sharing_form->addRow(show_local_library_);
    engine_share_ = new QCheckBox(QStringLiteral("Share on the network"), sharing);
    engine_share_->setObjectName(QStringLiteral("bench-settings-engine-share"));
    engine_share_->setToolTip(QStringLiteral(
        "Lets output agents play this computer's music, and other Trackknife windows control it"));
    bind(engine_share_, engine_share_key);
    sharing_form->addRow(engine_share_);
    engine_listen_ = new QLineEdit(sharing);
    engine_listen_->setObjectName(QStringLiteral("bench-settings-engine-listen"));
    bind(engine_listen_, engine_listen_key);
    engine_listen_->setToolTip(
        QStringLiteral("host:port; 0.0.0.0 listens on every network this computer is on"));
    sharing_form->addRow(QStringLiteral("Address:"), engine_listen_);
    engine_stream_port_ = new QSpinBox(sharing);
    engine_stream_port_->setObjectName(QStringLiteral("bench-settings-engine-stream-port"));
    engine_stream_port_->setRange(1, 65535);
    bind(engine_stream_port_, engine_stream_port_key);
    engine_stream_port_->setToolTip(
        QStringLiteral("Where agents without a copy of the music fetch it, on the same address"));
    sharing_form->addRow(QStringLiteral("Stream port:"), engine_stream_port_);
    engine_music_root_ = new QLineEdit(sharing);
    engine_music_root_->setObjectName(QStringLiteral("bench-settings-engine-music-root"));
    engine_music_root_->setPlaceholderText(QStringLiteral("optional"));
    engine_music_root_->setToolTip(QStringLiteral(
        "Agents started with their own --music-root are sent paths relative to this folder; "
        "agents without one stream"));
    bind(engine_music_root_, engine_music_root_key);
    auto* music_root_row = new QHBoxLayout;
    music_root_row->addWidget(engine_music_root_, 1);
    auto* music_root_browse = new QPushButton(QStringLiteral("Browse…"), sharing);
    connect(music_root_browse, &QPushButton::clicked, this, [this] {
        const auto chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Music root"),
                                                              engine_music_root_->text());
        if (!chosen.isEmpty()) {
            engine_music_root_->setText(chosen);
        }
    });
    music_root_row->addWidget(music_root_browse);
    sharing_form->addRow(QStringLiteral("Music root:"), music_root_row);
    // What to run on the machine with the speakers, kept in step with the
    // fields: the one thing this page is for, spelled out.
    engine_agent_command_ = new QLabel(sharing);
    engine_agent_command_->setObjectName(QStringLiteral("bench-settings-engine-agent-command"));
    engine_agent_command_->setWordWrap(true);
    engine_agent_command_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sharing_form->addRow(engine_agent_command_);
    syncs_.push_back([this] {
        const bool on = engine_share_->isChecked();
        engine_listen_->setEnabled(on);
        engine_stream_port_->setEnabled(on);
        engine_agent_command_->setText(session_->agentCommand());
    });
    engine_layout->addWidget(sharing);

    auto* remote = new QGroupBox(QStringLiteral("Engines elsewhere"), engine);
    auto* remote_layout = new QVBoxLayout(remote);
    // ADR-0234: engines on other machines, each connected at once beside
    // this computer's, their libraries in tabs of their own. The list; below
    // it, the one chosen.
    engines_view_ = new QListWidget(remote);
    engines_view_->setObjectName(QStringLiteral("bench-settings-engines"));
    engines_view_->setAccessibleName(QStringLiteral("Engines elsewhere"));
    engines_view_->setMaximumHeight(110);
    remote_layout->addWidget(engines_view_);
    auto* engine_buttons = new QHBoxLayout;
    auto* add_engine = new QPushButton(QStringLiteral("Add"), remote);
    add_engine->setObjectName(QStringLiteral("bench-settings-engine-add"));
    auto* remove_engine = new QPushButton(QStringLiteral("Remove"), remote);
    remove_engine->setObjectName(QStringLiteral("bench-settings-engine-remove"));
    engine_buttons->addWidget(add_engine);
    engine_buttons->addWidget(remove_engine);
    auto* engine_form = new QFormLayout;
    engine_socket_ = new QLineEdit(remote);
    engine_socket_->setObjectName(QStringLiteral("bench-settings-engine-socket"));
    engine_socket_->setPlaceholderText(QStringLiteral("host:port or socket path"));
    // Engines that announce themselves on the network, by name: chosen
    // rather than typed. The list fills as they answer.
    auto* found_engines = new QToolButton(remote);
    found_engines->setObjectName(QStringLiteral("bench-settings-found-engines"));
    found_engines->setText(QStringLiteral("On the network"));
    found_engines->setToolTip(QStringLiteral("Engines that announce themselves on this network"));
    found_engines->setPopupMode(QToolButton::InstantPopup);
    found_menu_ = new QMenu(found_engines);
    found_menu_->setObjectName(QStringLiteral("bench-settings-found-engines-menu"));
    found_engines->setMenu(found_menu_);
    if (!session_->discoveryError().isEmpty()) {
        found_engines->setEnabled(false);
        found_engines->setToolTip(session_->discoveryError());
    }
    connect(session_, &SettingsSession::foundChanged, this, &SettingsDialog::refreshFound);
    refreshFound();
    engine_buttons->addStretch(1);
    engine_buttons->addWidget(found_engines);
    remote_layout->addLayout(engine_buttons);
    engine_form->addRow(QStringLiteral("Address:"), engine_socket_);
    engine_token_ = new QLineEdit(remote);
    engine_token_->setObjectName(QStringLiteral("bench-settings-engine-token"));
    engine_token_->setEchoMode(QLineEdit::Password);
    engine_token_->setPlaceholderText(QStringLiteral("the password above"));
    engine_token_->setToolTip(
        QStringLiteral("Only when that engine's password differs from yours"));
    engine_form->addRow(QStringLiteral("Password:"), engine_token_);
    // Where its music is, and where the same folder is reachable from this
    // computer -- if it is, by a mount made outside Trackknife. Only moving
    // its tracks to this computer's lists and the tag and file tools need it.
    remote_folder_ = new QLineEdit(remote);
    remote_folder_->setObjectName(QStringLiteral("bench-settings-remote-folder"));
    remote_folder_->setPlaceholderText(
        QStringLiteral("e.g. /mnt/nas/Music, as that engine sees it"));
    engine_form->addRow(QStringLiteral("Its music folder:"), remote_folder_);
    remote_mount_ = new QLineEdit(remote);
    remote_mount_->setObjectName(QStringLiteral("bench-settings-remote-mount"));
    remote_mount_->setPlaceholderText(
        QStringLiteral("optional; empty: the same path, or not reachable here"));
    remote_mount_->setToolTip(QStringLiteral(
        "Where that folder is on this computer, if you have mounted it (NFS, SMB, …). "
        "Trackknife mounts nothing itself."));
    engine_form->addRow(QStringLiteral("Also reachable here at:"), remote_mount_);
    remote_stream_ = new QComboBox(remote);
    remote_stream_->setObjectName(QStringLiteral("bench-settings-remote-stream"));

    remote_stream_->setToolTip(QStringLiteral(
        "What it streams to this computer's speakers when its music is not reachable here. "
        "Automatic: the rates below, by whether it is on this network or reached through a "
        "VPN or a router."));
    engine_form->addRow(QStringLiteral("Streamed here:"), remote_stream_);
    // The form edits the engine chosen in the list.
    const auto edited = [this] {
        if (syncing_) {
            return;
        }
        auto chosen = session_->engine();
        chosen.address = engine_socket_->text();
        chosen.password = engine_token_->text();
        chosen.music_folder = remote_folder_->text();
        chosen.reachable_at = remote_mount_->text();
        chosen.stream_kbps = remote_stream_->currentData().toInt();
        session_->editEngine(chosen);
        if (auto* item = engines_view_->item(session_->currentEngine())) {
            item->setText(session_->engineLabels().value(session_->currentEngine()));
        }
    };
    for (auto* field : {engine_socket_, engine_token_, remote_folder_, remote_mount_}) {
        connect(field, &QLineEdit::textChanged, this, edited);
    }
    connect(remote_stream_, &QComboBox::currentIndexChanged, this, edited);
    connect(engines_view_, &QListWidget::currentRowChanged, session_,
            &SettingsSession::selectEngine);
    connect(add_engine, &QPushButton::clicked, this, [this] {
        session_->addEngine();
        engine_socket_->setFocus();
    });
    connect(remove_engine, &QPushButton::clicked, session_, &SettingsSession::removeEngine);
    connect(session_, &SettingsSession::enginesChanged, this, &SettingsDialog::refreshEngines);
    refreshEngines();
    play_for_remote_ =
        new QCheckBox(QStringLiteral("Let other engines play on this computer's speakers"), remote);
    play_for_remote_->setObjectName(QStringLiteral("bench-settings-play-for-remote"));
    play_for_remote_->setToolTip(QStringLiteral(
        "This computer's engine appears among the outputs of the remote engine and of any "
        "engine found on the network, so no melody-agent is needed here. Whichever starts "
        "playing last has the speakers."));
    bind(play_for_remote_, engine_play_for_remote_key);
    engine_form->addRow(QString{}, play_for_remote_);
    // ADR-0239: streamed, when the music is not reachable here, by route.
    stream_nearby_ = new QComboBox(remote);
    stream_nearby_->setObjectName(QStringLiteral("bench-settings-stream-nearby"));
    bind(stream_nearby_, session_->nearbyRates(), engine_stream_nearby_key);
    stream_nearby_->setToolTip(
        QStringLiteral("From an engine on this computer's own network"));
    engine_form->addRow(QStringLiteral("Streamed on this network:"), stream_nearby_);
    stream_away_ = new QComboBox(remote);
    stream_away_->setObjectName(QStringLiteral("bench-settings-stream-away"));
    bind(stream_away_, session_->awayRates(), engine_stream_away_key);
    stream_away_->setToolTip(QStringLiteral(
        "From an engine reached through WireGuard or another VPN, or through a router"));
    engine_form->addRow(QStringLiteral("Through a VPN or router:"), stream_away_);
    syncs_.push_back([this] {
        const auto on = play_for_remote_->isChecked();
        stream_nearby_->setEnabled(on);
        stream_away_->setEnabled(on);
        remote_stream_->setEnabled(on);
    });
    remote_layout->addLayout(engine_form);
    auto* engine_note = new QLabel(
        QStringLiteral("A melodyd on a NAS or server (started with --listen), beside this "
                       "computer's: its library gets a tab and its tracks play there, and this "
                       "computer's speakers are offered to each. Unencrypted: for a home network "
                       "or WireGuard."),
        remote);
    engine_note->setWordWrap(true);
    engine_note->setForegroundRole(QPalette::PlaceholderText);
    remote_layout->addWidget(engine_note);

    engine_layout->addWidget(remote);
    engine_layout->addStretch(1);
    add_page(QStringLiteral("Engine"), engine);

    // --- Naming ------------------------------------------------------------
    auto* naming = new QWidget(this);
    auto* naming_layout = new QVBoxLayout(naming);
    if (profile_store.load) {
        auto* manager = new OutputProfilesManagerWidget(std::move(profile_store), naming);
        output_profiles_ = manager;
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
    bind(replaygain_sidecar_only_, replaygain_sidecar_only_key);
    replaygain_layout->addWidget(replaygain_sidecar_only_);
    replaygain_true_peak_ =
        new QCheckBox(QStringLiteral("True peak as ReplayGain peak"), replaygain);
    replaygain_true_peak_->setObjectName(QStringLiteral("bench-replaygain-true-peak"));
    replaygain_true_peak_->setToolTip(
        QStringLiteral("Scan oversampled true peak instead of the plain sample peak"));
    bind(replaygain_true_peak_, replaygain_true_peak_key);
    replaygain_layout->addWidget(replaygain_true_peak_);
    replaygain_layout->addStretch(1);
    add_page(QStringLiteral("ReplayGain"), replaygain);

    // --- Covers ------------------------------------------------------------
    auto* covers = new QWidget(this);
    auto* covers_layout = new QVBoxLayout(covers);
    artwork_embed_ = new QCheckBox(QStringLiteral("Embed covers into the files"), covers);
    artwork_embed_->setObjectName(QStringLiteral("bench-artwork-embed"));
    bind(artwork_embed_, artwork_embed_key);
    covers_layout->addWidget(artwork_embed_);
    artwork_folder_image_ =
        new QCheckBox(QStringLiteral("Write a front-cover image next to the tracks"), covers);
    artwork_folder_image_->setObjectName(QStringLiteral("bench-artwork-folder-image"));
    bind(artwork_folder_image_, artwork_folder_image_key);
    covers_layout->addWidget(artwork_folder_image_);
    auto* covers_form = new QFormLayout;
    artwork_folder_image_name_ = new QComboBox(covers);
    artwork_folder_image_name_->setObjectName(QStringLiteral("bench-artwork-folder-image-name"));
    artwork_folder_image_name_->setEditable(true);
    artwork_folder_image_name_->addItems(SettingsSession::folderImageNames());
    bind(artwork_folder_image_name_->lineEdit(), artwork_folder_image_name_key);
    covers_form->addRow(QStringLiteral("Folder image name:"), artwork_folder_image_name_);
    artwork_fetch_source_ = new QComboBox(covers);
    artwork_fetch_source_->setObjectName(QStringLiteral("bench-artwork-fetch-source"));
    bind(artwork_fetch_source_, SettingsSession::fetchSources(), artwork_fetch_source_key);
    covers_form->addRow(QStringLiteral("Fetch covers from:"), artwork_fetch_source_);
    const auto edge_box = [&](const char* key, const QString& name) {
        auto* box = new QSpinBox(covers);
        box->setObjectName(name);
        box->setRange(0, 10'000);
        box->setSingleStep(100);
        box->setSuffix(QStringLiteral(" px"));
        box->setSpecialValueText(QStringLiteral("No limit"));
        bind(box, key);
        return box;
    };
    artwork_max_embedded_edge_ =
        edge_box(artwork_max_embedded_edge_key, QStringLiteral("bench-artwork-max-embedded-edge"));
    covers_form->addRow(QStringLiteral("Largest embedded cover:"), artwork_max_embedded_edge_);
    artwork_max_folder_edge_ =
        edge_box(artwork_max_folder_edge_key, QStringLiteral("bench-artwork-max-folder-edge"));
    covers_form->addRow(QStringLiteral("Largest folder image:"), artwork_max_folder_edge_);
    covers_layout->addLayout(covers_form);
    auto* covers_note = new QLabel(
        QStringLiteral(
            "Front covers use this storage policy on Apply. The filename extension "
            "follows the image format (.jpg or .png). Folder replacements are reviewed "
            "and retain recovery backups. A cover wider or taller than its limit is scaled "
            "down and saved as JPEG (PNG if it has transparency) when it is written; "
            "covers already in your files are left alone."),
        covers);
    covers_note->setWordWrap(true);
    covers_note->setForegroundRole(QPalette::PlaceholderText);
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
    musicbrainz_note->setForegroundRole(QPalette::PlaceholderText);
    metadata_layout->addWidget(musicbrainz_note);
    auto* key_form = new QFormLayout;
    acoustid_key_ = new QLineEdit(metadata_services);
    acoustid_key_->setObjectName(QStringLiteral("bench-settings-acoustid-key"));
    acoustid_key_->setEchoMode(QLineEdit::Password);
    acoustid_key_->setPlaceholderText(
        QStringLiteral("Client/application key for fingerprint lookup"));
    bind(acoustid_key_, acoustid_client_key);
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
    acoustid_note->setForegroundRole(QPalette::PlaceholderText);
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
    bind(lastfm_key_, "lastfm/api-key");
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
    lastfm_note->setForegroundRole(QPalette::PlaceholderText);
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
    save_note->setForegroundRole(QPalette::PlaceholderText);
    root->addWidget(save_note);
    connect(pages_, &QListWidget::currentRowChanged, save_note, [save_note](int row) {
        save_note->setText(SettingsSession::saveNote(static_cast<SettingsSession::Page>(row)));
        // Said only where a page does not wait for Save.
        save_note->setVisible(!save_note->text().isEmpty());
    });
    save_note->hide();
    pages_->setCurrentRow(0);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("bench-settings-buttons"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (shortcuts_ && !shortcuts_->apply()) {
            showPage(Page::shortcuts);
            return;
        }
        // ADR-0223: sharing without a password is not a thing to save.
        if (const auto refused = session_->save()) {
            showPage(static_cast<Page>(*refused));
            engine_password_->setFocus();
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
    connect(session_, &SettingsSession::changed, this, &SettingsDialog::sync);
    sync();
}

void SettingsDialog::bind(QCheckBox* box, const char* key) {
    const auto name = QLatin1String(key);
    connect(box, &QCheckBox::toggled, this, [this, name](const bool on) {
        if (!syncing_) {
            session_->setValue(name, on);
        }
    });
    syncs_.push_back([this, box, name] { box->setChecked(session_->value(name).toBool()); });
}

void SettingsDialog::bind(QLineEdit* field, const char* key) {
    const auto name = QLatin1String(key);
    connect(field, &QLineEdit::textChanged, this, [this, name](const QString& text) {
        if (!syncing_) {
            session_->setValue(name, text);
        }
    });
    syncs_.push_back([this, field, name] {
        if (const auto text = session_->value(name).toString(); field->text() != text) {
            field->setText(text);
        }
    });
}

void SettingsDialog::bind(QSpinBox* box, const char* key) {
    const auto name = QLatin1String(key);
    connect(box, &QSpinBox::valueChanged, this, [this, name](const int value) {
        if (!syncing_) {
            session_->setValue(name, value);
        }
    });
    syncs_.push_back([this, box, name] { box->setValue(session_->value(name).toInt()); });
}

void SettingsDialog::bind(QDoubleSpinBox* box, const char* key) {
    const auto name = QLatin1String(key);
    connect(box, &QDoubleSpinBox::valueChanged, this, [this, name](const double value) {
        if (!syncing_) {
            session_->setValue(name, value);
        }
    });
    syncs_.push_back([this, box, name] { box->setValue(session_->value(name).toDouble()); });
}

void SettingsDialog::bind(QComboBox* box, const std::vector<SettingsSession::Choice>& choices,
                          const char* key) {
    for (const auto& choice : choices) {
        box->addItem(choice.label, choice.value);
    }
    const auto name = QLatin1String(key);
    connect(box, &QComboBox::currentIndexChanged, this, [this, box, name] {
        if (!syncing_) {
            session_->setValue(name, box->currentData());
        }
    });
    syncs_.push_back([this, box, name] {
        box->setCurrentIndex(std::max(0, box->findData(session_->value(name))));
    });
}

void SettingsDialog::sync() {
    syncing_ = true;
    for (const auto& sync : syncs_) {
        sync();
    }
    syncing_ = false;
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

void SettingsDialog::showNamingLayouts() {
    showPage(Page::naming);
    if (output_profiles_ != nullptr) {
        output_profiles_->showNamingLayouts();
    }
}

void SettingsDialog::showDestinationsOf(const QString& key) {
    showPage(Page::naming);
    if (output_profiles_ != nullptr) {
        output_profiles_->showDestinationsOf(key);
    }
}

void SettingsDialog::showPage(const Page page) { pages_->setCurrentRow(static_cast<int>(page)); }

void SettingsDialog::refreshEngines() {
    syncing_ = true;
    {
        const QSignalBlocker blocker{engines_view_};
        engines_view_->clear();
        engines_view_->addItems(session_->engineLabels());
        engines_view_->setCurrentRow(session_->currentEngine());
    }
    const auto engine = session_->engine();
    engine_socket_->setText(engine.address);
    engine_token_->setText(engine.password);
    remote_folder_->setText(engine.music_folder);
    remote_mount_->setText(engine.reachable_at);
    remote_stream_->clear();
    for (const auto& choice : session_->engineRates()) {
        remote_stream_->addItem(choice.label, choice.value);
    }
    remote_stream_->setCurrentIndex(
        std::max(0, remote_stream_->findData(engine.stream_kbps < 0 ? -1 : engine.stream_kbps)));
    syncing_ = false;
}

void SettingsDialog::refreshFound() {
    found_menu_->clear();
    if (session_->found().empty()) {
        found_menu_->addAction(QStringLiteral("None found yet"))->setEnabled(false);
        return;
    }
    for (const auto& announced : session_->found()) {
        auto* choice = found_menu_->addAction(announced.label);
        connect(choice, &QAction::triggered, this,
                [this, where = announced.address] { session_->chooseFoundEngine(where); });
    }
}

} // namespace trackknife::bench
