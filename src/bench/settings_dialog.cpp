// SPDX-License-Identifier: GPL-3.0-only

#include "settings_dialog.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/shortcut_settings.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/audio/local_playback.hpp"
#include "trackknife/discovery/mdns.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QSysInfo>
#include <QGroupBox>
#include <QLabel>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QToolButton>
#include <QMenu>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
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
    buffer_note->setForegroundRole(QPalette::PlaceholderText);
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
    preamp_note->setForegroundRole(QPalette::PlaceholderText);
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
        note->setForegroundRole(QPalette::PlaceholderText);
        library_layout->addWidget(note);
        library_layout->addStretch(1);
    }
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

    // ADR-0226/0228: this computer's engine, shared on the network so output
    // agents -- the bedside speaker -- can play what it plays.
    auto* sharing = new QGroupBox(QStringLiteral("This computer's engine"), engine);
    auto* sharing_form = new QFormLayout(sharing);
    show_local_library_ = new QCheckBox(QStringLiteral("Show this computer's library"), sharing);
    show_local_library_->setObjectName(QStringLiteral("bench-settings-show-local-library"));
    show_local_library_->setToolTip(QStringLiteral(
        "Hide it when all your music is in the remote engine's library. Files on this computer "
        "still open and play here."));
    show_local_library_->setChecked(
        settings.value(QLatin1String(library_show_local_key), true).toBool());
    sharing_form->addRow(show_local_library_);
    engine_share_ = new QCheckBox(QStringLiteral("Share on the network"), sharing);
    engine_share_->setObjectName(QStringLiteral("bench-settings-engine-share"));
    engine_share_->setToolTip(QStringLiteral(
        "Lets output agents play this computer's music, and other Trackknife windows control it"));
    engine_share_->setChecked(
        settings.value(QLatin1String(engine_share_key), false).toBool());
    sharing_form->addRow(engine_share_);
    engine_listen_ = new QLineEdit(sharing);
    engine_listen_->setObjectName(QStringLiteral("bench-settings-engine-listen"));
    engine_listen_->setText(settings
                                .value(QLatin1String(engine_listen_key),
                                       QString::fromLatin1(engine_listen_default))
                                .toString());
    engine_listen_->setToolTip(
        QStringLiteral("host:port; 0.0.0.0 listens on every network this computer is on"));
    sharing_form->addRow(QStringLiteral("Address:"), engine_listen_);
    engine_stream_port_ = new QSpinBox(sharing);
    engine_stream_port_->setObjectName(QStringLiteral("bench-settings-engine-stream-port"));
    engine_stream_port_->setRange(1, 65535);
    engine_stream_port_->setValue(
        settings.value(QLatin1String(engine_stream_port_key), engine_stream_port_default).toInt());
    engine_stream_port_->setToolTip(QStringLiteral(
        "Where agents without a copy of the music fetch it, on the same address"));
    sharing_form->addRow(QStringLiteral("Stream port:"), engine_stream_port_);
    engine_password_ = new QLineEdit(sharing);
    engine_password_->setObjectName(QStringLiteral("bench-settings-engine-password"));
    engine_password_->setEchoMode(QLineEdit::Password);
    engine_password_->setPlaceholderText(QStringLiteral("required to share"));
    engine_password_->setToolTip(QStringLiteral(
        "Every agent and client on the network gives this. It travels unencrypted: across an "
        "untrusted network, use WireGuard or a TLS proxy."));
    engine_password_->setText(
        settings.value(QLatin1String(engine_password_key), QString{}).toString());
    sharing_form->addRow(QStringLiteral("Password:"), engine_password_);
    engine_music_root_ = new QLineEdit(sharing);
    engine_music_root_->setObjectName(QStringLiteral("bench-settings-engine-music-root"));
    engine_music_root_->setPlaceholderText(QStringLiteral("optional"));
    engine_music_root_->setToolTip(QStringLiteral(
        "Agents started with their own --music-root are sent paths relative to this folder; "
        "agents without one stream"));
    engine_music_root_->setText(
        settings.value(QLatin1String(engine_music_root_key), QString{}).toString());
    auto* music_root_row = new QHBoxLayout;
    music_root_row->addWidget(engine_music_root_, 1);
    auto* music_root_browse = new QPushButton(QStringLiteral("Browse…"), sharing);
    connect(music_root_browse, &QPushButton::clicked, this, [this] {
        const auto chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Music root"), engine_music_root_->text());
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
    const auto refresh_sharing = [this] {
        const bool on = engine_share_->isChecked();
        for (QWidget* field : std::initializer_list<QWidget*>{
                 engine_listen_, engine_stream_port_, engine_password_}) {
            field->setEnabled(on);
        }
        if (!on) {
            engine_agent_command_->setText(QStringLiteral(
                "Not shared: only this computer plays, and Trackknife here controls it."));
            return;
        }
        const auto listen = engine_listen_->text().trimmed();
        const auto colon = listen.lastIndexOf(QLatin1Char(':'));
        auto host = colon > 0 ? listen.left(colon) : listen;
        if (host.isEmpty() || host == QStringLiteral("0.0.0.0") || host == QStringLiteral("::")) {
            host = QSysInfo::machineHostName();
        }
        const auto port = colon > 0 ? listen.mid(colon + 1) : QString{};
        if (engine_password_->text().isEmpty()) {
            engine_agent_command_->setText(
                QStringLiteral("Set a password to share: every connection from the network "
                               "must give it."));
            return;
        }
        const auto command =
            QStringLiteral("melody-agent --server %1:%2 --password …").arg(host, port);
        engine_agent_command_->setText(
            QStringLiteral("On a machine with speakers, run:\n%1\nAdd --music-root DIR where "
                           "it has the music itself; without it, it streams. Changing these "
                           "restarts this computer's engine; playback comes back paused.")
                .arg(command));
    };
    connect(engine_share_, &QCheckBox::toggled, this, refresh_sharing);
    connect(engine_listen_, &QLineEdit::textChanged, this, refresh_sharing);
    connect(engine_password_, &QLineEdit::textChanged, this, refresh_sharing);
    refresh_sharing();
    engine_layout->addWidget(sharing);

    auto* remote = new QGroupBox(QStringLiteral("Remote engine"), engine);
    auto* remote_layout = new QVBoxLayout(remote);
    // ADR-0220: which engine serves the catalogue and owns playback. Empty
    // means this computer's own, started when needed (ADR-0226) -- stated
    // here rather than left as a hand-edited setting, because "which engine
    // is in use" is otherwise unanswerable from the UI.
    auto* engine_form = new QFormLayout;
    engine_socket_ = new QLineEdit(remote);
    engine_socket_->setObjectName(QStringLiteral("bench-settings-engine-socket"));
    engine_socket_->setPlaceholderText(
        QStringLiteral("host:port or socket path; empty: no remote engine"));
    engine_socket_->setText(
        settings.value(QLatin1String(library_engine_socket_key), QString{}).toString());
    // Engines that announce themselves on the network, by name: chosen
    // rather than typed. The list fills as they answer.
    auto* found_engines = new QToolButton(remote);
    found_engines->setObjectName(QStringLiteral("bench-settings-found-engines"));
    found_engines->setText(QStringLiteral("On the network"));
    found_engines->setToolTip(QStringLiteral("Engines that announce themselves on this network"));
    found_engines->setPopupMode(QToolButton::InstantPopup);
    auto* found_menu = new QMenu(found_engines);
    found_menu->setObjectName(QStringLiteral("bench-settings-found-engines-menu"));
    found_engines->setMenu(found_menu);
    const auto fill_found = [this, found_menu](const std::vector<discovery::Found>& engines) {
        found_menu->clear();
        if (engines.empty()) {
            found_menu->addAction(QStringLiteral("None found yet"))->setEnabled(false);
            return;
        }
        for (const auto& announced : engines) {
            const auto where = QStringLiteral("%1:%2")
                                   .arg(QString::fromStdString(announced.address))
                                   .arg(announced.port);
            const bool locked = announced.txt.contains("auth") && announced.txt.at("auth") == "1";
            auto* choice = found_menu->addAction(
                QStringLiteral("%1 — %2%3")
                    .arg(QString::fromStdString(announced.instance), where,
                         locked ? QStringLiteral(" · password") : QString{}));
            connect(choice, &QAction::triggered, this, [this, where] { engine_socket_->setText(where); });
        }
    };
    fill_found({});
    if (auto browser = discovery::Browser::start([this, fill_found](
                                                     const std::vector<discovery::Found>& engines) {
            QMetaObject::invokeMethod(this, [fill_found, engines] { fill_found(engines); },
                                      Qt::QueuedConnection);
        })) {
        engine_browser_ = std::move(*browser);
    } else {
        found_engines->setEnabled(false);
        found_engines->setToolTip(QString::fromStdString(browser.error().message));
    }
    auto* engine_row = new QHBoxLayout;
    engine_row->addWidget(engine_socket_, 1);
    engine_row->addWidget(found_engines);
    engine_form->addRow(QStringLiteral("Remote engine:"), engine_row);
    engine_token_ = new QLineEdit(remote);
    engine_token_->setObjectName(QStringLiteral("bench-settings-engine-token"));
    engine_token_->setEchoMode(QLineEdit::Password);
    engine_token_->setPlaceholderText(QStringLiteral("only if the engine has one"));
    engine_token_->setText(
        settings.value(QLatin1String(library_engine_token_key), QString{}).toString());
    engine_form->addRow(QStringLiteral("Remote password:"), engine_token_);
    // Where the remote's music is on this computer, for moving its tracks
    // into local lists (and back): played here, and tagged here.
    remote_folder_ = new QLineEdit(remote);
    remote_folder_->setObjectName(QStringLiteral("bench-settings-remote-folder"));
    remote_folder_->setPlaceholderText(QStringLiteral("e.g. /mnt/nas/Music, as the remote sees it"));
    remote_folder_->setText(
        settings.value(QLatin1String(library_remote_folder_key), QString{}).toString());
    engine_form->addRow(QStringLiteral("Remote music folder:"), remote_folder_);
    remote_mount_ = new QLineEdit(remote);
    remote_mount_->setObjectName(QStringLiteral("bench-settings-remote-mount"));
    remote_mount_->setPlaceholderText(QStringLiteral("the same folder here; empty: same path"));
    remote_mount_->setText(
        settings.value(QLatin1String(library_remote_mount_key), QString{}).toString());
    engine_form->addRow(QStringLiteral("Mounted here at:"), remote_mount_);
    play_for_remote_ =
        new QCheckBox(QStringLiteral("Let other engines play on this computer's speakers"), remote);
    play_for_remote_->setObjectName(QStringLiteral("bench-settings-play-for-remote"));
    play_for_remote_->setToolTip(QStringLiteral(
        "This computer's engine appears among the outputs of the remote engine and of any "
        "engine found on the network, so no melody-agent is needed here. Whichever starts "
        "playing last has the speakers."));
    play_for_remote_->setChecked(
        settings.value(QLatin1String(engine_play_for_remote_key), true).toBool());
    engine_form->addRow(QString{}, play_for_remote_);
    remote_layout->addLayout(engine_form);
    auto* engine_note = new QLabel(
        QStringLiteral("A melodyd on a NAS or server (started with --listen), beside this "
                       "computer's: its library gets a tab and its tracks play there. Unencrypted: "
                       "for a home network or WireGuard. Applies after restarting Trackknife."),
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
    const auto edge_box = [&](const char* key, const QString& name) {
        auto* box = new QSpinBox(covers);
        box->setObjectName(name);
        box->setRange(0, 10'000);
        box->setSingleStep(100);
        box->setSuffix(QStringLiteral(" px"));
        box->setSpecialValueText(QStringLiteral("No limit"));
        box->setValue(settings.value(QLatin1String(key), 0).toInt());
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
        const auto page = static_cast<Page>(row);
        if (page == Page::library)
            save_note->setText(
                QStringLiteral("Folder changes save immediately. Cancel does not undo them."));
        else if (page == Page::naming)
            save_note->setText(QStringLiteral("Save layout, Save destination, and Remove take "
                                              "effect immediately. Cancel does not undo them."));
        else
            save_note->clear();
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
        if (engine_share_->isChecked() && engine_password_->text().isEmpty()) {
            showPage(Page::engine);
            engine_password_->setFocus();
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
                                .toStdString(),
            .max_embedded_edge = static_cast<std::uint32_t>(std::max(
                0, settings.value(QLatin1String(artwork_max_embedded_edge_key), 0).toInt())),
            .max_folder_edge = static_cast<std::uint32_t>(std::max(
                0, settings.value(QLatin1String(artwork_max_folder_edge_key), 0).toInt()))};
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
    settings.setValue(QStringLiteral("playback/buffer-capacity-ms"), buffer_capacity_->value());
    settings.setValue(QStringLiteral("playback/buffer-start-threshold-ms"),
                      buffer_threshold_->value());
    settings.setValue(QStringLiteral("playback/rg-preamp-with"), preamp_with_->value());
    settings.setValue(QStringLiteral("playback/rg-preamp-without"), preamp_without_->value());
    settings.setValue(QLatin1String(library_engine_socket_key), engine_socket_->text().trimmed());
    settings.setValue(QLatin1String(library_engine_token_key), engine_token_->text().trimmed());
    settings.setValue(QLatin1String(library_remote_folder_key), remote_folder_->text().trimmed());
    settings.setValue(QLatin1String(library_remote_mount_key), remote_mount_->text().trimmed());
    settings.setValue(QLatin1String(library_show_local_key), show_local_library_->isChecked());
    settings.setValue(QLatin1String(engine_share_key), engine_share_->isChecked());
    settings.setValue(QLatin1String(engine_listen_key), engine_listen_->text().trimmed());
    settings.setValue(QLatin1String(engine_stream_port_key), engine_stream_port_->value());
    settings.setValue(QLatin1String(engine_password_key), engine_password_->text());
    settings.setValue(QLatin1String(engine_music_root_key), engine_music_root_->text().trimmed());
    settings.setValue(QLatin1String(engine_play_for_remote_key), play_for_remote_->isChecked());
    settings.setValue(QLatin1String(replaygain_sidecar_only_key),
                      replaygain_sidecar_only_->isChecked());
    settings.setValue(QLatin1String(replaygain_true_peak_key), replaygain_true_peak_->isChecked());
    settings.setValue(QLatin1String(artwork_embed_key), artwork_embed_->isChecked());
    settings.setValue(QLatin1String(artwork_folder_image_key), artwork_folder_image_->isChecked());
    settings.setValue(QLatin1String(artwork_folder_image_name_key),
                      artwork_folder_image_name_->currentText().trimmed());
    settings.setValue(QLatin1String(artwork_fetch_source_key),
                      artwork_fetch_source_->currentData().toString());
    settings.setValue(QLatin1String(artwork_max_embedded_edge_key),
                      artwork_max_embedded_edge_->value());
    settings.setValue(QLatin1String(artwork_max_folder_edge_key),
                      artwork_max_folder_edge_->value());
}

} // namespace trackknife::bench
