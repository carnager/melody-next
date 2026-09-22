// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/mpris_service.hpp"
#include <QDockWidget>

#include "bench/bench_main_window_helpers.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "trackknife/audio/listen_observation.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/audio/melody_agent.hpp"
#include "uicommon/line_slider.hpp"
#include "uicommon/list_persistence_service.hpp"

#include "uicommon/track_row_roles.hpp"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr int transport_refresh_ms = 33;
constexpr int minimum_custom_buffer_ms = 10;
constexpr int maximum_custom_buffer_ms = 10'000;
constexpr auto buffer_profile_settings_key = "playback/buffer-profile";
constexpr auto buffer_capacity_settings_key = "playback/buffer-capacity-ms";
constexpr auto buffer_threshold_settings_key = "playback/buffer-start-threshold-ms";

struct PlaybackBufferPreference {
    QString profile;
    audio::PlaybackBufferDurationConfig config;
};

[[nodiscard]] QString bufferProfileLabel(const QString& profile) {
    if (profile == QStringLiteral("responsive")) {
        return QStringLiteral("Responsive");
    }
    if (profile == QStringLiteral("resilient")) {
        return QStringLiteral("Resilient");
    }
    if (profile == QStringLiteral("custom")) {
        return QStringLiteral("Custom");
    }
    return QStringLiteral("Balanced");
}

[[nodiscard]] PlaybackBufferPreference loadPlaybackBufferPreference() {
    QSettings settings;
    const auto profile =
        settings.value(QString::fromLatin1(buffer_profile_settings_key), QStringLiteral("balanced"))
            .toString();
    const auto profile_bytes = utf8Bytes(profile);
    if (const auto preset = audio::playback_buffer_preset_from_id(profile_bytes)) {
        return {.profile = profile, .config = audio::playback_buffer_preset_config(*preset)};
    }
    if (profile == QStringLiteral("custom")) {
        bool capacity_ok = false;
        bool threshold_ok = false;
        const auto capacity =
            settings.value(QString::fromLatin1(buffer_capacity_settings_key)).toInt(&capacity_ok);
        const auto threshold =
            settings.value(QString::fromLatin1(buffer_threshold_settings_key)).toInt(&threshold_ok);
        const audio::PlaybackBufferDurationConfig config{
            .capacity = std::chrono::milliseconds{capacity},
            .start_threshold = std::chrono::milliseconds{threshold},
        };
        if (capacity_ok && threshold_ok && capacity >= minimum_custom_buffer_ms &&
            capacity <= maximum_custom_buffer_ms &&
            audio::valid_local_audition_buffer_config(config)) {
            return {.profile = profile, .config = config};
        }
    }
    return {.profile = QStringLiteral("balanced"),
            .config = audio::playback_buffer_preset_config(audio::PlaybackBufferPreset::balanced)};
}

[[nodiscard]] bool playerActive(const audio::LocalAuditionState state) {
    return state == audio::LocalAuditionState::buffering ||
           state == audio::LocalAuditionState::playing ||
           state == audio::LocalAuditionState::draining;
}

// Reads the row's projected ReplayGain values at one provenance layer:
// the last value wins, mirroring the CUE remark policy.
[[nodiscard]] std::optional<formats::ReplayGainInfo>
projected_replay_gain(const LocalTrackRow& row, const metadata::FieldProvenance provenance) {
    const auto last_value =
        [&row, provenance](const std::string_view name) -> std::optional<std::string> {
        const auto canonical = metadata::canonicalize_field_name(name);
        std::optional<std::string> value;
        for (const auto& field : row.metadata.fields) {
            if (field.provenance == provenance && field.canonical_name == canonical &&
                !field.values.empty()) {
                value = field.values.back();
            }
        }
        return value;
    };
    formats::ReplayGainInfo info;
    if (const auto text = last_value("REPLAYGAIN_TRACK_GAIN")) {
        info.track_gain_db = formats::parse_replay_gain_decibels(*text);
    }
    if (const auto text = last_value("REPLAYGAIN_TRACK_PEAK")) {
        info.track_peak = formats::parse_replay_gain_peak(*text);
    }
    if (const auto text = last_value("REPLAYGAIN_ALBUM_GAIN")) {
        info.album_gain_db = formats::parse_replay_gain_decibels(*text);
    }
    if (const auto text = last_value("REPLAYGAIN_ALBUM_PEAK")) {
        info.album_peak = formats::parse_replay_gain_peak(*text);
    }
    if (!info.track_gain_db && !info.album_gain_db) {
        return std::nullopt;
    }
    return info;
}

// The explicit playback override, in persistence-precedence order
// (ADR-0139/0141): fresh sidecar values first, then a CUE track's
// sheet-carried REM values; otherwise the decoder's own tags apply.
[[nodiscard]] std::optional<formats::ReplayGainInfo>
local_replay_gain_override(const LocalTrackRow& row) {
    if (auto sidecar = projected_replay_gain(row, metadata::FieldProvenance::sidecar)) {
        return sidecar;
    }
    if (row.logical_reference && row.logical_reference->starts_with("cue-v1")) {
        return projected_replay_gain(row, metadata::FieldProvenance::segment);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<formats::ReplayGainInfo>
local_replay_gain_override(const LocalListModel& model, const int row) {
    const auto& rows = model.rows();
    if (row < 0 || row >= static_cast<int>(rows.size())) {
        return std::nullopt;
    }
    return local_replay_gain_override(rows[static_cast<std::size_t>(row)]);
}

[[nodiscard]] core::Result<void>
load_and_play(audio::LocalAuditionService& player, const LocalTrackSource& source,
              std::optional<formats::ReplayGainInfo> replay_gain_override = {}) {
    return source.segment
               ? player.load_selected_segment_and_play(source.raw_path, source.selection,
                                                       *source.segment, replay_gain_override)
               : player.load_selected_and_play(source.raw_path, source.selection,
                                               replay_gain_override);
}

[[nodiscard]] core::Result<void>
queue_gapless(audio::LocalAuditionService& player, const LocalTrackSource& source,
              std::optional<formats::ReplayGainInfo> replay_gain_override = {},
              std::uint64_t token = 0U) {
    return source.segment
               ? player.queue_gapless_next_selected_segment(source.raw_path, source.selection,
                                                            *source.segment, replay_gain_override,
                                                            token)
               : player.queue_gapless_next_selected(source.raw_path, source.selection,
                                                    replay_gain_override, token);
}

[[nodiscard]] LocalTrackSource source_from_snapshot(const audio::LocalAuditionSnapshot& snapshot) {
    return LocalTrackSource{.raw_path = snapshot.raw_path,
                            .selection = snapshot.selection,
                            .segment = snapshot.segment};
}

[[nodiscard]] std::optional<LocalTrackSource>
queued_source_from_snapshot(const audio::LocalAuditionSnapshot& snapshot) {
    if (snapshot.next_raw_path.empty()) {
        return std::nullopt;
    }
    return LocalTrackSource{.raw_path = snapshot.next_raw_path,
                            .selection = snapshot.next_selection,
                            .segment = snapshot.next_segment};
}

} // namespace

BenchMainWindow::BenchMainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Trackknife"));
    resize(1100, 720);
    setAcceptDrops(true);

    const auto buffer_preference = loadPlaybackBufferPreference();
    selected_buffer_profile_ = buffer_preference.profile;
    audio::LocalAuditionConfig player_config;
    player_config.buffer = buffer_preference.config;
    if (auto player = audio::LocalAuditionService::create(std::move(player_config)); player) {
        player_storage_ = std::move(*player);
        player_ = player_storage_.get();
    }

    buildWorkspace();
    buildTransport();
    buildLastFm();
    buildShortcuts();
    connect(&metadata_operation_watcher_, &QFutureWatcherBase::finished, this,
            &BenchMainWindow::finishMetadataOperationJob);
    initializePersistence();

    if (player_ != nullptr) {
        static_cast<void>(player_->refresh_output_devices());
    } else {
        statusBar()->showMessage(
            QStringLiteral("Local playback unavailable: the audio worker failed to start"));
    }
    transport_timer_ = new QTimer(this);
    transport_timer_->setInterval(transport_refresh_ms);
    connect(transport_timer_, &QTimer::timeout, this, &BenchMainWindow::refreshTransport);
    transport_timer_->start();
    buildMprisService();
    refreshActiveContext();
    refreshTransport();
}

BenchMainWindow::~BenchMainWindow() { stopBackgroundWork(); }

void BenchMainWindow::refreshMuteButton() {
    if (!mute_button_ || !volume_)
        return;
    const bool muted = volume_->value() == 0;
    if (!muted && volume_->isEnabled()) {
        const auto key = isMpdContext() ? QStringLiteral("server/") + mpd_controller_->profileId() +
                                              "/" + mpd_controller_->activeOutputName()
                                        : QStringLiteral("local");
        unmuted_volumes_.insert(key, volume_->value());
    }
    mute_button_->setEnabled(volume_->isEnabled());
    mute_button_->setChecked(muted);
    mute_button_->setIcon(QIcon::fromTheme(
        muted ? QStringLiteral("audio-volume-muted") : QStringLiteral("audio-volume-high"),
        style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume)));
    mute_button_->setToolTip(muted ? tr("Unmute") : tr("Mute"));
    mute_button_->setAccessibleName(mute_button_->toolTip());
}

void BenchMainWindow::buildTransport() {
    buildUpNext();
    auto* bar = addToolBar(QStringLiteral("Transport"));
    bar->setObjectName(QStringLiteral("bench-transport"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize{18, 18});
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    previous_action_ = new QAction(style()->standardIcon(QStyle::SP_MediaSkipBackward),
                                   QStringLiteral("Previous"), this);
    connect(previous_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            mpd_controller_->previous();
        } else if (playingOnEngine()) {
            engine_playback_->previous();
        } else {
            playAdjacent(-1);
        }
    });
    play_pause_action_ =
        new QAction(style()->standardIcon(QStyle::SP_MediaPlay), QStringLiteral("Play"), this);
    play_pause_action_->setShortcut(Qt::Key_Space);
    play_pause_action_->setShortcutContext(Qt::ApplicationShortcut);
    connect(play_pause_action_, &QAction::triggered, this, &BenchMainWindow::togglePlayPause);
    stop_action_ =
        new QAction(style()->standardIcon(QStyle::SP_MediaStop), QStringLiteral("Stop"), this);
    connect(stop_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            mpd_controller_->stop();
        } else if (playingOnEngine()) {
            engine_playback_->stop();
        } else if (player_ != nullptr) {
            ++resume_intent_generation_;
            static_cast<void>(player_->stop());
        }
    });
    next_action_ = new QAction(style()->standardIcon(QStyle::SP_MediaSkipForward),
                               QStringLiteral("Next"), this);
    connect(next_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            mpd_controller_->next();
        } else if (playingOnEngine()) {
            engine_playback_->next();
        } else {
            playAdjacent(1);
        }
    });

    auto* header = new QWidget(bar);
    header->setObjectName(QStringLiteral("bench-player-header"));
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* header_layout = new QGridLayout(header);
    header_layout->setContentsMargins(6, 3, 6, 3);
    header_layout->setHorizontalSpacing(6);
    header_layout->setVerticalSpacing(0);
    header_layout->setColumnStretch(2, 1);

    auto* transport = new QWidget(header);
    transport->setObjectName(QStringLiteral("bench-transport-buttons"));
    auto* transport_layout = new QHBoxLayout(transport);
    transport_layout->setContentsMargins(0, 0, 2, 0);
    transport_layout->setSpacing(1);
    const auto add_transport_button = [transport, transport_layout](QAction* action) {
        auto* button = new QToolButton(transport);
        button->setDefaultAction(action);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(26, 26);
        button->setIconSize(QSize{18, 18});
        transport_layout->addWidget(button);
    };
    add_transport_button(previous_action_);
    add_transport_button(play_pause_action_);
    add_transport_button(stop_action_);
    add_transport_button(next_action_);
    up_next_button_ = new QToolButton(transport);
    up_next_button_->setObjectName(QStringLiteral("action-up-next"));
    up_next_button_->setText(QStringLiteral("Up Next · 0"));
    up_next_button_->setAutoRaise(true);
    up_next_button_->setFixedHeight(26);
    up_next_button_->setAcceptDrops(true);
    up_next_button_->installEventFilter(this);
    connect(up_next_button_, &QToolButton::clicked, this, [this] {
        findChild<QAction*>(QStringLiteral("action-show-up-next"))->trigger();
        refreshUpNext();
    });
    transport_layout->addWidget(up_next_button_);
    header_layout->addWidget(transport, 1, 0, Qt::AlignVCenter);

    auto* track_display = new QWidget(header);
    track_display->setObjectName(QStringLiteral("bench-track-display"));
    track_display->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* track_display_layout = new QVBoxLayout(track_display);
    track_display_layout->setContentsMargins(0, 0, 0, 1);
    track_display_layout->setSpacing(0);

    now_playing_ = new QLabel(track_display);
    now_playing_->setObjectName(QStringLiteral("bench-now-playing"));
    now_playing_->setTextFormat(Qt::PlainText);
    now_playing_->setAccessibleName(QStringLiteral("Current artist and title"));
    now_playing_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    now_playing_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    track_display_layout->addWidget(now_playing_);

    now_playing_context_ = new QLabel(track_display);
    now_playing_context_->setObjectName(QStringLiteral("bench-now-playing-context"));
    now_playing_context_->setTextFormat(Qt::PlainText);
    now_playing_context_->setAccessibleName(QStringLiteral("Current album and date"));
    now_playing_context_->setForegroundRole(QPalette::PlaceholderText);
    now_playing_context_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    now_playing_context_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    track_display_layout->addWidget(now_playing_context_);
    header_layout->addWidget(track_display, 0, 2);

    elapsed_ = new QLabel(QStringLiteral("0:00"), header);
    elapsed_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    elapsed_->setFixedWidth(elapsed_->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00")));
    header_layout->addWidget(elapsed_, 1, 1, Qt::AlignVCenter);
    seek_ = new ui::LineSlider(header);
    seek_->setObjectName(QStringLiteral("bench-seek"));
    seek_->setAccessibleName(QStringLiteral("Playback position"));
    seek_->setMinimumWidth(200);
    seek_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(seek_, &QSlider::sliderPressed, this, [this] { seeking_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, [this] {
        seeking_ = false;
        seekToMs(seek_->value());
    });
    header_layout->addWidget(seek_, 1, 2, Qt::AlignVCenter);
    duration_ = new QLabel(QStringLiteral("0:00"), header);
    duration_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    duration_->setFixedWidth(
        duration_->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00")));
    header_layout->addWidget(duration_, 1, 3, Qt::AlignVCenter);

    auto* volumeBox = new QWidget(header);
    auto* volumeLayout = new QHBoxLayout(volumeBox);
    volumeLayout->setContentsMargins(0, 0, 0, 0);
    volumeLayout->setSpacing(4);
    mute_button_ = new QToolButton(volumeBox);
    mute_button_->setObjectName(QStringLiteral("bench-mute"));
    mute_button_->setAutoRaise(true);
    mute_button_->setCheckable(true);
    mute_button_->setIconSize(QSize(18, 18));
    mute_button_->setFixedSize(26, 26);
    volumeLayout->addWidget(mute_button_);
    connect(mute_button_, &QToolButton::clicked, this, [this] {
        if (!volume_->isEnabled())
            return;
        const auto key = isMpdContext() ? QStringLiteral("server/") + mpd_controller_->profileId() +
                                              "/" + mpd_controller_->activeOutputName()
                                        : QStringLiteral("local");
        if (volume_->value() > 0) {
            unmuted_volumes_.insert(key, volume_->value());
            volume_->setValue(0);
        } else {
            volume_->setValue(unmuted_volumes_.value(key, 100));
        }
        refreshMuteButton();
    });
    volume_ = new ui::LineSlider(volumeBox);
    volume_->setObjectName(QStringLiteral("bench-volume"));
    volume_->setAccessibleName(QStringLiteral("Volume"));
    volume_->setRange(0, 100);
    volume_->setValue(100);
    volume_->setFixedWidth(104);
    volume_->setToolTip(QStringLiteral("Volume"));
    connect(volume_, &QSlider::sliderPressed, this, [this] { changing_volume_ = true; });
    connect(volume_, &QSlider::sliderReleased, this, [this] { changing_volume_ = false; });
    connect(volume_, &QSlider::valueChanged, this, [this](const int value) {
        if (isMpdContext()) {
            mpd_controller_->setVolume(value);
        } else if (playingOnEngine()) {
            // The engine owns the output, so the volume lives there: another
            // client watching the same engine sees the same number, and it
            // survives this window closing.
            engine_playback_->setVolume(value);
        } else if (player_ != nullptr) {
            static_cast<void>(player_->set_volume_percent(value));
        }
        refreshMuteButton();
    });
    volumeLayout->addWidget(volume_);
    header_layout->addWidget(volumeBox, 1, 4, Qt::AlignVCenter);
    refreshMuteButton();

    device_button_ = new QToolButton(header);
    device_button_->setObjectName(QStringLiteral("bench-device"));
    device_button_->setIcon(QIcon::fromTheme(QStringLiteral("audio-speakers"),
                                             style()->standardIcon(QStyle::SP_ComputerIcon)));
    device_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    device_button_->setAutoRaise(true);
    device_button_->setFixedSize(26, 26);
    device_button_->setIconSize(QSize{18, 18});
    device_button_->setPopupMode(QToolButton::InstantPopup);
    device_button_->setAccessibleName(QStringLiteral("Audio output device"));
    device_menu_ = new QMenu(device_button_);
    device_menu_->setObjectName(QStringLiteral("bench-device-menu"));
    device_group_ = new QActionGroup(device_menu_);
    device_group_->setExclusive(true);
    device_button_->setMenu(device_menu_);
    rebuildDeviceMenu();
    header_layout->addWidget(device_button_, 1, 5, Qt::AlignVCenter);
    bar->addWidget(header);

    auto* playback_menu = menuBar()->addMenu(QStringLiteral("&Playback"));
    playback_menu->addAction(play_pause_action_);
    playback_menu->addAction(stop_action_);
    playback_menu->addAction(previous_action_);
    playback_menu->addAction(next_action_);
    playback_menu->addSeparator();

    buildLocalPlaybackControls(playback_menu);
    if (mpd_album_random_action_)
        playback_menu->addAction(mpd_album_random_action_);

    // ADR-0144: quiet, opt-in track-change notifications while the
    // window is in the background.
    notifications_action_ = playback_menu->addAction(QStringLiteral("Desktop notifications"));
    notifications_action_->setObjectName(QStringLiteral("action-desktop-notifications"));
    notifications_action_->setCheckable(true);
    notifications_action_->setChecked(
        QSettings{}.value(QStringLiteral("desktop/notifications"), false).toBool());
    notifications_action_->setToolTip(
        QStringLiteral("Show a notification when playback changes to another track."));
    connect(notifications_action_, &QAction::toggled, this, [this](const bool enabled) {
        QSettings{}.setValue(QStringLiteral("desktop/notifications"), enabled);
        if (notifier_ != nullptr) {
            notifier_->setEnabled(enabled);
        }
    });

    buffer_menu_ = playback_menu->addMenu(QStringLiteral("Playback buffer"));
    buffer_menu_->setObjectName(QStringLiteral("bench-buffer-menu"));
    buffer_group_ = new QActionGroup(buffer_menu_);
    buffer_group_->setExclusive(true);
    const auto add_buffer_preset = [this](const QString& label,
                                          const audio::PlaybackBufferPreset preset) {
        auto* action = buffer_menu_->addAction(label);
        const auto id = audio::playback_buffer_preset_id(preset);
        const auto profile = QString::fromLatin1(id.data(), static_cast<qsizetype>(id.size()));
        const auto config = audio::playback_buffer_preset_config(preset);
        action->setObjectName(QStringLiteral("action-buffer-%1").arg(profile));
        action->setData(profile);
        action->setCheckable(true);
        action->setToolTip(QStringLiteral("%1 ms capacity; playback starts at %2 ms")
                               .arg(config.capacity.count())
                               .arg(config.start_threshold.count()));
        buffer_group_->addAction(action);
        connect(action, &QAction::triggered, this, [this, profile, config] {
            configurePlaybackBuffer(profile, static_cast<int>(config.capacity.count()),
                                    static_cast<int>(config.start_threshold.count()));
        });
    };
    add_buffer_preset(QStringLiteral("Responsive"), audio::PlaybackBufferPreset::responsive);
    add_buffer_preset(QStringLiteral("Balanced"), audio::PlaybackBufferPreset::balanced);
    add_buffer_preset(QStringLiteral("Resilient"), audio::PlaybackBufferPreset::resilient);
    buffer_menu_->addSeparator();
    auto* custom_buffer = buffer_menu_->addAction(QStringLiteral("Custom…"));
    custom_buffer->setObjectName(QStringLiteral("action-buffer-custom"));
    custom_buffer->setData(QStringLiteral("custom"));
    custom_buffer->setCheckable(true);
    buffer_group_->addAction(custom_buffer);
    connect(custom_buffer, &QAction::triggered, this,
            &BenchMainWindow::showCustomPlaybackBufferDialog);
    refreshPlaybackBufferChecks();

    auto* refresh_devices = playback_menu->addAction(QStringLiteral("Refresh audio devices"));
    connect(refresh_devices, &QAction::triggered, this, [this] {
        if (player_ != nullptr) {
            static_cast<void>(player_->refresh_output_devices());
        }
    });
}

void BenchMainWindow::configurePlaybackBuffer(const QString& profile, const int capacity_ms,
                                              const int start_threshold_ms) {
    const audio::PlaybackBufferDurationConfig config{
        .capacity = std::chrono::milliseconds{capacity_ms},
        .start_threshold = std::chrono::milliseconds{start_threshold_ms},
    };
    if (!audio::valid_local_audition_buffer_config(config)) {
        statusBar()->showMessage(QStringLiteral("Invalid playback buffer values"), 5'000);
        refreshPlaybackBufferChecks();
        return;
    }

    auto active_buffer = std::optional<audio::PlaybackBufferDurationConfig>{};
    if (player_ != nullptr) {
        active_buffer = player_->snapshot().active_buffer;
        if (auto changed = player_->set_buffer_config(config); !changed) {
            statusBar()->showMessage(QStringLiteral("Playback buffer unchanged: %1")
                                         .arg(displayText(changed.error().message)),
                                     5'000);
            refreshPlaybackBufferChecks();
            return;
        }
    }

    selected_buffer_profile_ = profile;
    QSettings settings;
    settings.setValue(QString::fromLatin1(buffer_profile_settings_key), profile);
    settings.setValue(QString::fromLatin1(buffer_capacity_settings_key), capacity_ms);
    settings.setValue(QString::fromLatin1(buffer_threshold_settings_key), start_threshold_ms);
    settings.sync();
    refreshPlaybackBufferChecks();

    const bool pending = active_buffer && *active_buffer != config;
    statusBar()->showMessage(
        QStringLiteral("%1 buffer · %2 ms capacity · %3 ms start%4")
            .arg(bufferProfileLabel(profile))
            .arg(capacity_ms)
            .arg(start_threshold_ms)
            .arg(pending ? QStringLiteral(" · applies next track") : QString{}),
        5'000);
}

void BenchMainWindow::reloadPlaybackPreferences() {
    const QSettings settings;
    const auto preference = loadPlaybackBufferPreference();
    if (selected_buffer_profile_ != preference.profile ||
        (player_ && player_->snapshot().configured_buffer != preference.config)) {
        configurePlaybackBuffer(preference.profile,
                                static_cast<int>(preference.config.capacity.count()),
                                static_cast<int>(preference.config.start_threshold.count()));
    }
    if (notifier_)
        notifier_->setBackgroundOnly(
            settings.value(QStringLiteral("desktop/notifications-background-only"), false)
                .toBool());
    if (notifications_action_)
        notifications_action_->setChecked(
            settings.value(QStringLiteral("desktop/notifications"), false).toBool());
    const auto with_gain =
        settings.value(QStringLiteral("playback/rg-preamp-with"), 0.0).toDouble();
    const auto without_gain =
        settings.value(QStringLiteral("playback/rg-preamp-without"), 0.0).toDouble();
    if (local_rg_preamp_with_ != with_gain || local_rg_preamp_without_ != without_gain) {
        local_rg_preamp_with_ = with_gain;
        local_rg_preamp_without_ = without_gain;
        applyLocalPlaybackModes();
    }
}

void BenchMainWindow::showCustomPlaybackBufferDialog() {
    refreshPlaybackBufferChecks();
    showSettingsDialog(SettingsDialog::Page::playback)->editCustomBuffer();
}

void BenchMainWindow::refreshPlaybackBufferChecks() {
    if (buffer_group_ == nullptr) {
        return;
    }
    for (auto* action : buffer_group_->actions()) {
        action->setChecked(action->data().toString() == selected_buffer_profile_);
    }
}

void BenchMainWindow::rebuildDeviceMenu() {
    device_menu_->clear();
    device_menu_->setToolTipsVisible(true);

    if (isMpdContext()) {
        device_group_->setExclusive(false);
        auto* output_model = mpd_controller_->outputModel();
        for (int row = 0; row < output_model->rowCount(); ++row) {
            const auto index = output_model->index(row, 0);
            const auto id = output_model->data(index, quick::MpdOutputModel::OutputIdRole).toUInt();
            const auto name = output_model->data(index, quick::MpdOutputModel::NameRole).toString();
            const auto enabled =
                output_model->data(index, quick::MpdOutputModel::EnabledRole).toBool();
            auto label = name;
            QString endpoint_detail;
            if (melody_endpoint_ != nullptr && name == displayText(melody_endpoint_->name())) {
                const auto endpoint = melody_endpoint_->snapshot();
                const auto mode = endpoint.replay_gain_mode == audio::ReplayGainMode::track
                                      ? QStringLiteral("Track")
                                  : endpoint.replay_gain_mode == audio::ReplayGainMode::album
                                      ? QStringLiteral("Album")
                                      : QStringLiteral("Off");
                const auto selected_gain =
                    endpoint.replay_gain_mode == audio::ReplayGainMode::album &&
                            endpoint.album_gain_db
                        ? endpoint.album_gain_db
                        : endpoint.track_gain_db;
                endpoint_detail = QStringLiteral("ReplayGain: %1").arg(mode);
                if (selected_gain) {
                    endpoint_detail += QStringLiteral(" · %1 dB · %2×")
                                           .arg(*selected_gain, 0, 'f', 2)
                                           .arg(endpoint.effective_gain_multiplier, 0, 'f', 3);
                } else if (endpoint.replay_gain_mode != audio::ReplayGainMode::off) {
                    const auto received =
                        endpoint.replay_gain_mode == audio::ReplayGainMode::album &&
                                endpoint.received_album_gain_db
                            ? endpoint.received_album_gain_db
                            : endpoint.received_track_gain_db;
                    endpoint_detail +=
                        received ? QStringLiteral(" · queue %1 dB, player missing")
                                       .arg(*received, 0, 'f', 2)
                        : std::abs(endpoint.effective_gain_multiplier - 1.0F) > 0.0001F
                            ? QStringLiteral(" · decoder metadata · %1×")
                                  .arg(endpoint.effective_gain_multiplier, 0, 'f', 3)
                            : QStringLiteral(" · no gain metadata");
                }
                label += QStringLiteral(" — %1").arg(endpoint_detail);
            }
            auto* action = device_menu_->addAction(label);
            action->setObjectName(QStringLiteral("action-mpd-output-%1").arg(id));
            action->setCheckable(true);
            // MPD outputs are independent toggles; clicking one must never
            // silently disable the others.
            action->setChecked(enabled);
            auto detail = output_model->data(index, quick::MpdOutputModel::DetailRole).toString();
            if (!endpoint_detail.isEmpty()) {
                detail += QStringLiteral("\n") + endpoint_detail;
            }
            action->setToolTip(detail);
            device_group_->addAction(action);
            connect(action, &QAction::triggered, this,
                    [this, id, enabled] { mpd_controller_->setOutputEnabled(id, !enabled); });
        }
        if (device_menu_->isEmpty()) {
            auto* none = device_menu_->addAction(mpd_controller_->connected()
                                                     ? QStringLiteral("No MPD outputs")
                                                     : QStringLiteral("Connect to MPD"));
            none->setEnabled(false);
        }
        device_button_->setAccessibleName(QStringLiteral("MPD output"));
        return;
    }

    device_group_->setExclusive(true);
    device_button_->setAccessibleName(QStringLiteral("Audio output device"));

    const auto add_choice = [this](const QString& label, std::optional<std::string> target,
                                   const bool enabled = true) {
        auto* action = device_menu_->addAction(label);
        action->setCheckable(true);
        action->setChecked(target == selected_device_);
        action->setEnabled(enabled);
        device_group_->addAction(action);
        connect(action, &QAction::triggered, this, [this, target = std::move(target)] {
            if (player_ != nullptr) {
                static_cast<void>(player_->set_output_target(target));
            }
        });
        return action;
    };

    add_choice(QStringLiteral("System default"), std::nullopt);
    for (const auto& [name, description] : device_choices_) {
        add_choice(displayText(description.empty() ? name : description), name);
    }
    if (selected_device_ && std::ranges::none_of(device_choices_, [this](const auto& choice) {
            return choice.first == *selected_device_;
        })) {
        add_choice(QStringLiteral("%1 (unavailable)").arg(displayText(*selected_device_)),
                   selected_device_, false);
    }
    device_menu_->addSeparator();
    auto* refresh = device_menu_->addAction(QStringLiteral("Refresh audio devices"));
    refresh->setObjectName(QStringLiteral("action-refresh-audio-devices"));
    connect(refresh, &QAction::triggered, this, [this] {
        if (player_ != nullptr) {
            static_cast<void>(player_->refresh_output_devices());
        }
    });
}

void BenchMainWindow::buildLocalPlaybackControls(QMenu* playback_menu) {
    const QSettings settings;
    playback_.modes.repeat =
        settings.value(QStringLiteral("playback/local-repeat"), false).toBool();
    playback_.modes.random =
        settings.value(QStringLiteral("playback/local-random"), false).toBool();
    playback_.modes.album_random =
        settings.value(QStringLiteral("playback/local-album-random"), false).toBool();
    if (playback_.modes.album_random)
        playback_.modes.random = false;
    playback_.modes.single = audio::mode_state_from_int(
        settings.value(QStringLiteral("playback/local-single"), 0).toInt());
    playback_.modes.consume = audio::mode_state_from_int(
        settings.value(QStringLiteral("playback/local-consume"), 0).toInt());
    local_replaygain_ =
        settings.value(QStringLiteral("playback/local-replaygain"), QStringLiteral("off"))
            .toString();
    if (local_replaygain_ != QStringLiteral("track") &&
        local_replaygain_ != QStringLiteral("album") &&
        local_replaygain_ != QStringLiteral("auto")) {
        local_replaygain_ = QStringLiteral("off");
    }
    const auto preamp_limit = static_cast<double>(audio::maximum_replay_gain_preamp_db);
    local_rg_preamp_with_ =
        std::clamp(settings.value(QStringLiteral("playback/rg-preamp-with"), 0.0).toDouble(),
                   -preamp_limit, preamp_limit);
    local_rg_preamp_without_ =
        std::clamp(settings.value(QStringLiteral("playback/rg-preamp-without"), 0.0).toDouble(),
                   -preamp_limit, preamp_limit);
    const auto add_mode = [&](const QString& id, const QString& label, const QString& icon) {
        auto* action = new QAction(label, this);
        action->setObjectName(QStringLiteral("action-local-%1").arg(id));
        action->setCheckable(true);
        auto* button = new QToolButton(statusBar());
        button->setObjectName(QStringLiteral("bench-local-%1").arg(id));
        button->setAutoRaise(true);
        if (!icon.isEmpty()) {
            action->setIcon(
                QIcon::fromTheme(icon, style()->standardIcon(QStyle::SP_BrowserReload)));
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        } else {
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        }
        button->setDefaultAction(action);
        statusBar()->addPermanentWidget(button);
        local_mode_buttons_.push_back(button);
        playback_menu->addAction(action);
        return action;
    };
    local_repeat_action_ = add_mode(QStringLiteral("repeat"), QStringLiteral("Repeat"),
                                    QStringLiteral("media-playlist-repeat"));
    local_random_action_ = add_mode(QStringLiteral("random"), QStringLiteral("Random"),
                                    QStringLiteral("media-playlist-shuffle"));
    local_single_action_ = add_mode(QStringLiteral("single"), QStringLiteral("Single"), {});
    local_album_random_action_ = add_mode(QStringLiteral("album-random"), tr("Album shuffle"),
                                          QStringLiteral("media-playlist-shuffle"));
    local_album_random_action_->setIcon(albumShuffleIcon(palette()));
    connect(local_album_random_action_, &QAction::triggered, this, [this](bool on) {
        if (isMpdContext())
            return;
        playback_.modes.album_random = on;
        if (on)
            playback_.modes.random = false;
        resetPlaybackOrder();
        applyLocalPlaybackModes();
    });
    local_consume_action_ = add_mode(QStringLiteral("consume"), QStringLiteral("Consume"), {});
    connect(local_repeat_action_, &QAction::triggered, this, [this](bool on) {
        if (isMpdContext()) {
            return;
        }
        playback_.modes.repeat = on;
        applyLocalPlaybackModes();
    });
    connect(local_random_action_, &QAction::triggered, this, [this](bool on) {
        if (isMpdContext()) {
            return;
        }
        playback_.modes.random = on;
        if (on)
            playback_.modes.album_random = false;
        resetPlaybackOrder();
        applyLocalPlaybackModes();
    });
    connect(local_single_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            return;
        }
        playback_.modes.single = audio::next_mode_state(playback_.modes.single);
        applyLocalPlaybackModes();
    });
    connect(local_consume_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            return;
        }
        playback_.modes.consume = audio::next_mode_state(playback_.modes.consume);
        applyLocalPlaybackModes();
    });
    local_replaygain_button_ = new QToolButton(statusBar());
    local_replaygain_button_->setObjectName(QStringLiteral("bench-local-replaygain"));
    local_replaygain_button_->setAccessibleName(QStringLiteral("Local ReplayGain mode"));
    local_replaygain_button_->setIcon(QIcon::fromTheme(
        QStringLiteral("view-media-equalizer"), style()->standardIcon(QStyle::SP_MediaVolume)));
    local_replaygain_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    local_replaygain_button_->setAutoRaise(true);
    local_replaygain_button_->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(QStringLiteral("Local ReplayGain"), local_replaygain_button_);
    menu->setObjectName(QStringLiteral("bench-local-replaygain-menu"));
    local_replaygain_group_ = new QActionGroup(menu);
    local_replaygain_group_->setExclusive(true);
    const std::array modes{
        std::pair{QStringLiteral("Off"), QStringLiteral("off")},
        std::pair{QStringLiteral("Track"), QStringLiteral("track")},
        std::pair{QStringLiteral("Album"), QStringLiteral("album")},
        std::pair{QStringLiteral("Automatic"), QStringLiteral("auto")},
    };
    for (const auto& [label, value] : modes) {
        auto* action = menu->addAction(label);
        action->setObjectName(QStringLiteral("action-local-replaygain-%1").arg(value));
        action->setCheckable(true);
        action->setData(value);
        local_replaygain_group_->addAction(action);
        connect(action, &QAction::triggered, this, [this, value] {
            if (isMpdContext()) {
                return;
            }
            local_replaygain_ = value;
            applyLocalPlaybackModes();
        });
    }
    menu->addSeparator();
    auto* preamp_action = menu->addAction(QStringLiteral("Preamp…"));
    preamp_action->setObjectName(QStringLiteral("action-local-replaygain-preamp"));
    connect(preamp_action, &QAction::triggered, this, &BenchMainWindow::showReplayGainPreampDialog);
    local_replaygain_button_->setMenu(menu);
    statusBar()->addPermanentWidget(local_replaygain_button_);
    playback_menu->addMenu(menu);
    playback_menu->addSeparator();
    applyLocalPlaybackModes();
}

void BenchMainWindow::saveLocalPlaybackModes() {
    QSettings settings;
    settings.setValue(QStringLiteral("playback/local-repeat"), playback_.modes.repeat);
    settings.setValue(QStringLiteral("playback/local-random"), playback_.modes.random);
    settings.setValue(QStringLiteral("playback/local-album-random"), playback_.modes.album_random);
    settings.setValue(QStringLiteral("playback/local-single"),
                      static_cast<int>(playback_.modes.single));
    settings.setValue(QStringLiteral("playback/local-consume"),
                      static_cast<int>(playback_.modes.consume));
    settings.setValue(QStringLiteral("playback/local-replaygain"), local_replaygain_);
    settings.setValue(QStringLiteral("playback/rg-preamp-with"), local_rg_preamp_with_);
    settings.setValue(QStringLiteral("playback/rg-preamp-without"), local_rg_preamp_without_);
}

void BenchMainWindow::applyLocalPlaybackModes() {
    saveLocalPlaybackModes();
    playback_.last_requested_next.reset();
    // "Auto" is this window's policy about its own shuffle, so it resolves
    // here whichever player is listening.
    auto mode = audio::ReplayGainMode::off;
    if (local_replaygain_ == QStringLiteral("track") ||
        (local_replaygain_ == QStringLiteral("auto") && playback_.modes.random)) {
        mode = audio::ReplayGainMode::track;
    } else if (local_replaygain_ == QStringLiteral("album") ||
               local_replaygain_ == QStringLiteral("auto")) {
        mode = audio::ReplayGainMode::album;
    }
    const audio::ReplayGainPreamps preamps{
        .with_gain_db = static_cast<float>(local_rg_preamp_with_),
        .without_gain_db = static_cast<float>(local_rg_preamp_without_),
    };
    if (playingOnEngine()) {
        // The engine decides what plays next and how loud it is, so every one
        // of these is its business. Sending them to the idle local player
        // instead is why the buttons appeared to do nothing.
        engine_playback_->setModes(playback_.modes);
        engine_playback_->setReplayGain(mode, preamps);
    } else if (player_ != nullptr) {
        static_cast<void>(player_->clear_gapless_next());
        static_cast<void>(player_->set_replay_gain_mode(mode));
        static_cast<void>(player_->set_replay_gain_preamps(preamps));
    }
    refreshLocalPlaybackControls();
}

// Separate preamps for tracks with and without loudness data (ADR-0138);
// both apply only while local ReplayGain is active.
void BenchMainWindow::showReplayGainPreampDialog() {
    showSettingsDialog(SettingsDialog::Page::playback)->focusReplayGainPreamp();
}

void BenchMainWindow::refreshLocalPlaybackControls() {
    if (local_repeat_action_ == nullptr) {
        return;
    }
    const auto visible = !isMpdContext();
    for (auto* button : local_mode_buttons_) {
        button->setVisible(visible);
        button->defaultAction()->setVisible(visible);
        // An engine can act on these even when this process has no audio
        // device of its own, which is the whole point of it owning playback.
        button->defaultAction()->setEnabled(visible && (player_ != nullptr || playingOnEngine()));
    }
    local_repeat_action_->setChecked(playback_.modes.repeat);
    local_random_action_->setChecked(playback_.modes.random);
    local_album_random_action_->setChecked(playback_.modes.album_random);
    local_album_random_action_->setToolTip(
        tr("Shuffle albums during playback without rearranging the list. Tracks within each album "
           "keep their list order."));
    local_repeat_action_->setToolTip(
        QStringLiteral("Repeat: %1")
            .arg(playback_.modes.repeat ? QStringLiteral("On") : QStringLiteral("Off")));
    local_random_action_->setToolTip(
        QStringLiteral("Random: %1")
            .arg(playback_.modes.random ? QStringLiteral("On") : QStringLiteral("Off")));
    const auto cycle = [](QAction* action, const audio::ModeState mode, const QString& name,
                          const QString& symbol, const QString& help) {
        const auto state = mode == audio::ModeState::off  ? QStringLiteral("Off")
                           : mode == audio::ModeState::on ? QStringLiteral("On")
                                                          : QStringLiteral("One-shot");
        action->setChecked(mode != audio::ModeState::off);
        action->setIconText(mode == audio::ModeState::oneshot ? symbol + QStringLiteral("×")
                                                              : symbol);
        action->setText(QStringLiteral("%1: %2").arg(name, state));
        action->setToolTip(QStringLiteral("%1: %2\n%3").arg(name, state, help));
    };
    cycle(local_single_action_, playback_.modes.single, QStringLiteral("Single"),
          QStringLiteral("1"),
          QStringLiteral("Stop after this track; with Repeat, repeat this track. Click to cycle "
                         "Off / On / One-shot."));
    cycle(local_consume_action_, playback_.modes.consume, QStringLiteral("Consume"),
          QStringLiteral("C"),
          QStringLiteral("Remove finished or skipped entries from the local list. Files stay on "
                         "disk. Click to cycle Off / On / One-shot."));
    local_replaygain_button_->setVisible(visible);
    local_replaygain_button_->setEnabled(visible && player_ != nullptr);
    local_replaygain_button_->menu()->menuAction()->setVisible(visible);
    for (auto* action : local_replaygain_group_->actions()) {
        action->setEnabled(visible && player_ != nullptr);
        action->setChecked(action->data().toString() == local_replaygain_);
        if (action->isChecked()) {
            local_replaygain_button_->setText(QStringLiteral("RG: %1").arg(action->text()));
        }
    }
    local_replaygain_button_->setToolTip(
        QStringLiteral(
            "Local ReplayGain: %1\nAutomatic: track gain with Random, album gain otherwise.\nUses "
            "embedded gain with peak-based clipping prevention when a matching peak is "
            "present.\nChanges apply as buffered audio drains; missing gain plays unchanged.")
            .arg(local_replaygain_button_->text().mid(4)));
}

bool BenchMainWindow::playbackIsMpd() const {
    const auto mpd_playing =
        mpd_controller_ != nullptr && mpd_controller_->connected() && mpd_controller_->playing();
    // Whichever authority is actually making sound wins. One engine plays at
    // a time, so this is a choice between two and never a merge.
    if (playingOnEngine()) {
        if (engine_playback_->state().status != QStringLiteral("stopped")) {
            return false;
        }
    } else if (player_ != nullptr &&
               player_->snapshot().state != audio::LocalAuditionState::empty &&
               player_->snapshot().state != audio::LocalAuditionState::ended) {
        return false;
    }
    if (mpd_playing) {
        return true;
    }
    // Nothing is playing anywhere. Prefer the local anchors when there are
    // any, so jumping after a stop still lands on the track that was playing.
    return playback_.anchors.current.is_nil();
}

bool BenchMainWindow::playingOnEngine() const {
    // Ownership, not visibility. This deliberately does not ask which tab is
    // on screen: looking at the MPD queue does not hand the local player back
    // its queue, and when it did, the local refresh saw an idle player and
    // wiped the anchors the engine was playing from -- which is what broke
    // jumping to the playing track.
    return engine_playback_ != nullptr && engine_playback_->active();
}

int BenchMainWindow::resolvePlaybackRow(const ListTab* tab) const {
    if (tab == nullptr) {
        return -1;
    }
    const LocalListPlaybackView list{*tab->model};
    return playback_.resolveRow(list);
}

void BenchMainWindow::resetPlaybackOrder() {
    ++album_order_generation_;
    album_order_preparing_ = false;
    album_grouper_ = audio::AlbumGrouper{};
    auto* tab = tabForDocument(playback_.anchors.document);
    playback_.row = resolvePlaybackRow(tab);
    playback_.order.reset(tab != nullptr ? tab->model->rowCount() : 0, playback_.row,
                          playback_.modes.random);
    if (playback_.modes.album_random && tab && tab->model->rowCount() > 0) {
        album_order_preparing_ = true;
        album_order_build_row_ = 0;
        playback_.order.reset(0, -1, false);
        QTimer::singleShot(0, this, [this, generation = album_order_generation_] {
            prepareAlbumPlaybackOrder(generation);
        });
    }
    playback_.last_requested_next.reset();
    if (player_ != nullptr) {
        static_cast<void>(player_->clear_gapless_next());
    }
}

void BenchMainWindow::prepareAlbumPlaybackOrder(const std::uint64_t generation) {
    if (generation != album_order_generation_ || !album_order_preparing_)
        return;
    auto* tab = tabForDocument(playback_.anchors.document);
    if (!tab) {
        album_order_preparing_ = false;
        return;
    }
    const auto& rows = tab->model->rows();
    const auto abandon = [this] {
        playback_.modes.album_random = false;
        resetPlaybackOrder();
        applyLocalPlaybackModes();
        statusBar()->showMessage(tr("Album shuffle exceeds the grouping limits."), 8000);
    };
    if (!album_grouper_.admits(rows.size())) {
        abandon();
        return;
    }
    const auto end = std::min(static_cast<int>(rows.size()), album_order_build_row_ + 128);
    for (; album_order_build_row_ < end; ++album_order_build_row_) {
        const auto& row = rows[static_cast<std::size_t>(album_order_build_row_)];
        if (!album_grouper_.add({.album_artist = row.album_artist,
                                 .artist = row.artist,
                                 .album = row.album,
                                 .date = row.date},
                                album_order_build_row_)) {
            abandon();
            return;
        }
    }
    if (album_order_build_row_ < static_cast<int>(rows.size())) {
        QTimer::singleShot(0, this, [this, generation] { prepareAlbumPlaybackOrder(generation); });
        return;
    }
    auto* watcher = new QFutureWatcher<audio::PlaybackOrder>(this);
    connect(watcher, &QFutureWatcher<audio::PlaybackOrder>::finished, this,
            [this, watcher, generation] {
                if (generation == album_order_generation_) {
                    playback_.order = watcher->future().takeResult();
                    album_order_preparing_ = false;
                }
                watcher->deleteLater();
            });
    watcher->setFuture(
        QtConcurrent::run([groups = album_grouper_.take(), current = playback_.row]() mutable {
            audio::PlaybackOrder order;
            order.resetAlbums(std::move(groups), current);
            return order;
        }));
}

void BenchMainWindow::consumePlaybackRow(ListTab& tab, const core::StableId& entry,
                                         const int hint_row) {
    const auto row = tab.model->rowOfEntry(entry, hint_row);
    if (!playback_.modes.consume_active() || row < 0) {
        return;
    }
    consuming_row_ = true;
    tab.model->removeRowIndexes({row}, false);
    consuming_row_ = false;
    // A QPersistentModelIndex used to shuffle itself down after the removal.
    // Re-resolving the identity does the same thing explicitly, and reports -1
    // when the consumed row was the playing one.
    playback_.row = resolvePlaybackRow(&tab);
    resetPlaybackOrder();
    if (playback_.row < 0) {
        tab.model->setCurrentSource({}, -1);
    }
    markTabDirty(tab);
    if (playback_.modes.expire_consume()) {
        saveLocalPlaybackModes();
        refreshLocalPlaybackControls();
    }
}

void BenchMainWindow::adoptPlaybackRow(ListTab& tab, const int row, const LocalTrackSource& source,
                                       const bool consume, const int direction) {
    const auto previous = playback_.anchors.current;
    const auto previous_row = playback_.row;
    playback_.adopt(tab.model->rows().at(static_cast<std::size_t>(row)).entry_id, row, source);
    playback_.order.advance(row, direction);
    if (consume && previous != playback_.anchors.current) {
        consumePlaybackRow(tab, previous, previous_row);
    }
    tab.model->setCurrentSource(source, playback_.row);
}

std::optional<std::pair<int, LocalTrackSource>> BenchMainWindow::automaticPlaybackRow() {
    auto* tab = tabForDocument(playback_.anchors.document);
    if (tab == nullptr) {
        return std::nullopt;
    }
    const LocalListPlaybackView list{*tab->model};
    const auto choice = playback_.automaticRow(list);
    if (!choice) {
        return std::nullopt;
    }
    return std::make_pair(choice->row, choice->source);
}

void BenchMainWindow::playRow(ListTab& tab, const int row,
                              std::optional<std::int64_t> restore_position_ms) {
    ++resume_intent_generation_;
    if (playingOnEngine()) {
        // The engine owns the queue, so it is given the whole list rather
        // than one track: skipping, shuffling and gapless are its decisions
        // now, and it cannot make them from a single entry.
        const auto& rows = tab.model->rows();
        if (row < 0 || static_cast<std::size_t>(row) >= rows.size()) {
            return;
        }
        std::vector<std::optional<formats::ReplayGainInfo>> overrides;
        overrides.reserve(rows.size());
        for (const auto& source_row : rows) {
            overrides.push_back(local_replay_gain_override(source_row));
        }
        engine_playback_->play(rows, overrides, rows[static_cast<std::size_t>(row)].entry_id);
        playback_.anchors.document = tab.document.id;
        playback_.anchors.current = rows[static_cast<std::size_t>(row)].entry_id;
        playback_.row = row;
        setActiveLocalList(QString::fromStdString(tab.document.id.to_string()));
        tab.model->setCurrentSource(tab.model->source(row), row);
        return;
    }
    if (player_ == nullptr) {
        return;
    }
    const auto source = tab.model->source(row);
    if (source.empty()) {
        return;
    }
    if (restore_position_ms && !tab.model->rows().at(static_cast<std::size_t>(row)).source_revision)
        return;
    const auto result =
        restore_position_ms
            ? player_->restore_paused(
                  source.raw_path,
                  *tab.model->rows().at(static_cast<std::size_t>(row)).source_revision,
                  source.selection, source.segment, *restore_position_ms,
                  local_replay_gain_override(*tab.model, row))
            : load_and_play(*player_, source, local_replay_gain_override(*tab.model, row));
    if (!result) {
        statusBar()->showMessage(
            QStringLiteral("Playback failed: %1").arg(displayText(result.error().message)), 5'000);
        return;
    }
    if (detached_playback_ && detached_playback_->model != tab.model) {
        detached_playback_->model->deleteLater();
        detached_playback_.reset();
    }
    if (!restore_position_ms)
        playback_.requests.abandon();
    requested_request_.reset();
    queued_request_.reset();
    persistUpNext();
    refreshUpNext();
    const auto id = QString::fromStdString(tab.document.id.to_string());
    if (playback_.anchors.document != tab.document.id) {
        if (auto* previous = tabForDocument(playback_.anchors.document); previous != nullptr) {
            previous->model->setCurrentSource({}, -1);
        }
    }
    playback_.anchors.document = tab.document.id;
    setActiveLocalList(id);
    playback_.anchors.current = tab.model->rows().at(static_cast<std::size_t>(row)).entry_id;
    playback_.row = row;
    playback_.anchors.source = source;
    resetPlaybackOrder();
    playback_.anchors.forget_transition();
    // A load was just dispatched; block auto-advance until the player state
    // leaves "ended" so the previous track's end cannot skip this one.
    advance_pending_ = true;
    playback_.last_requested_next.reset();
    tab.model->setCurrentSource(source, row);
}

std::optional<std::pair<int, LocalTrackSource>>
BenchMainWindow::adjacentPlaybackRow(const int direction) {
    auto* tab = tabForDocument(playback_.anchors.document);
    if (tab == nullptr) {
        return std::nullopt;
    }
    const LocalListPlaybackView list{*tab->model};
    const auto choice = playback_.adjacentRow(list, direction);
    if (!choice) {
        return std::nullopt;
    }
    return std::make_pair(choice->row, choice->source);
}

void BenchMainWindow::adoptLocalRequest(audio::RequestQueue<LocalTrackRow>::Entry entry,
                                        bool restoring) {
    if (std::ranges::none_of(playback_.requests.pending(),
                             [&](const auto& pending) { return pending.id == entry.id; }))
        statusBar()->showMessage(
            QStringLiteral(
                "This request was already handed to the player; your queue edits apply next."),
            5000);
    if (!playback_.requests.active() && !restoring) {
        playback_.anchors.request_return = core::StableId{};
        const auto next = adjacentPlaybackRow(1);
        if (auto* tab = tabForDocument(playback_.anchors.document); tab && next)
            playback_.anchors.request_return =
                tab->model->rows().at(static_cast<std::size_t>(next->first)).entry_id;
        if (auto* tab = tabForDocument(playback_.anchors.document))
            consumePlaybackRow(*tab, playback_.anchors.current, playback_.row);
    }
    playback_.requests.started(std::move(entry));
    if (auto* tab = tabForDocument(playback_.anchors.document))
        tab->model->setCurrentSource({}, -1);
    requested_request_.reset();
    queued_request_.reset();
    playback_.anchors.forget_transition();
    playback_.last_requested_next.reset();
    persistUpNext();
    refreshUpNext();
}

bool BenchMainWindow::playLocalRequest(std::optional<std::int64_t> restore_position_ms) {
    if (album_order_preparing_)
        return false;
    if (!player_ || playback_.requests.pending().empty())
        return false;
    const auto entry = playback_.requests.pending().front();
    ++resume_intent_generation_;
    if (restore_position_ms && !entry.source.source_revision)
        return false;
    LocalTrackSource source{entry.source.raw_path, entry.source.selection, entry.source.segment};
    auto result =
        restore_position_ms
            ? player_->restore_paused(source.raw_path, *entry.source.source_revision,
                                      source.selection, source.segment, *restore_position_ms,
                                      local_replay_gain_override(*up_next_local_model_, 0))
            : load_and_play(*player_, source, local_replay_gain_override(*up_next_local_model_, 0));
    if (result) {
        adoptLocalRequest(entry, restore_position_ms.has_value());
        advance_pending_ = true;
    } else
        statusBar()->showMessage(
            QStringLiteral("Playback failed: %1").arg(displayText(result.error().message)), 5000);
    return true;
}

void BenchMainWindow::playAdjacent(const int direction) {
    if (direction > 0 && playLocalRequest())
        return;
    if (direction < 0 && playback_.requests.active() && player_) {
        const auto& row = playback_.requests.active()->source;
        static_cast<void>(load_and_play(
            *player_, LocalTrackSource{row.raw_path, row.selection, row.segment}, std::nullopt));
        advance_pending_ = true;
        return;
    }
    auto* tab = tabForDocument(playback_.anchors.document);
    const auto next = adjacentPlaybackRow(direction);
    if (tab == nullptr || !next || player_ == nullptr) {
        if (direction > 0 && playback_.requests.active() && player_) {
            static_cast<void>(player_->stop());
            playback_.requests.finished();
            persistUpNext();
            refreshUpNext();
        }
        return;
    }
    if (auto result = load_and_play(*player_, next->second,
                                    local_replay_gain_override(*tab->model, next->first));
        result) {
        playback_.requests.finished();
        persistUpNext();
        refreshUpNext();
        adoptPlaybackRow(*tab, next->first, next->second, true, direction);
        advance_pending_ = true;
        playback_.last_requested_next.reset();
        playback_.anchors.forget_transition();
    } else {
        statusBar()->showMessage(
            QStringLiteral("Playback failed: %1").arg(displayText(result.error().message)), 5'000);
    }
}

void BenchMainWindow::togglePlayPause() {
    if (isMpdContext()) {
        mpd_controller_->playPause();
        return;
    }
    if (playingOnEngine()) {
        // What the engine last reported, rather than a local snapshot: the
        // local player is idle here and would always answer "not playing".
        if (engine_playback_->state().status == QStringLiteral("playing")) {
            engine_playback_->pause();
        } else {
            engine_playback_->resume();
        }
        return;
    }
    if (player_ == nullptr) {
        return;
    }
    ++resume_intent_generation_;
    const auto snapshot = player_->snapshot();
    if (playerActive(snapshot.state)) {
        static_cast<void>(player_->pause());
    } else {
        if ((snapshot.state == audio::LocalAuditionState::empty ||
             snapshot.state == audio::LocalAuditionState::ended) &&
            playLocalRequest())
            return;
        static_cast<void>(player_->play());
    }
}

void BenchMainWindow::seekToMs(const qint64 position_ms) {
    if (isMpdContext()) {
        mpd_controller_->seekTo(position_ms);
        return;
    }
    if (playingOnEngine()) {
        engine_playback_->seek(position_ms);
        return;
    }
    if (player_ == nullptr) {
        return;
    }
    const auto snapshot = player_->snapshot();
    if (!snapshot.format || snapshot.format->sample_rate <= 0) {
        return;
    }
    static_cast<void>(player_->seek_to_sample(position_ms * snapshot.format->sample_rate / 1'000));
}

void BenchMainWindow::buildShortcuts() {
    const auto bind = [this](QAction* action, const QString& name, const QString& keys) {
        action->setObjectName(name);
        action->setShortcut(QKeySequence(keys));
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
    };
    bind(play_pause_action_, QStringLiteral("action-play-pause"), QStringLiteral("Space"));
    bind(stop_action_, QStringLiteral("action-stop"), QStringLiteral("Ctrl+."));
    bind(previous_action_, QStringLiteral("action-previous-track"), QStringLiteral("Alt+Left"));
    bind(next_action_, QStringLiteral("action-next-track"), QStringLiteral("Alt+Right"));
    for (const bool prepend : {true, false}) {
        auto* action = new QAction(prepend ? tr("Queue next") : tr("Queue at end"), this);
        bind(action,
             prepend ? QStringLiteral("action-queue-next") : QStringLiteral("action-queue-end"),
             prepend ? QStringLiteral("Ctrl+Return") : QStringLiteral("Ctrl+Shift+Return"));
        connect(action, &QAction::triggered, this, [this, prepend] {
            auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
            if (view && view->selectionModel() &&
                (qobject_cast<LocalListModel*>(view->model()) || isMpdContext()))
                enqueueUpNext(view, prepend);
        });
    }
    for (auto* action : findChildren<QAction*>()) {
        if (!action->objectName().startsWith(QStringLiteral("action-")) ||
            action->shortcut().isEmpty())
            continue;
        action->setProperty("shortcut-default",
                            action->shortcut().toString(QKeySequence::PortableText));
        const auto key = QStringLiteral("shortcuts/") + action->objectName();
        if (QSettings{}.contains(key))
            action->setShortcut(
                QKeySequence(QSettings{}.value(key).toString(), QKeySequence::PortableText));
        configurable_shortcuts_.append(action);
    }
    std::sort(configurable_shortcuts_.begin(), configurable_shortcuts_.end(),
              [](const QAction* a, const QAction* b) {
                  return a->text().localeAwareCompare(b->text()) < 0;
              });
}

void BenchMainWindow::refreshPlaybackCursor(const bool jump) {
    if (!tabs_ || (!jump && (!follow_playback_action_ || !follow_playback_action_->isChecked())))
        return;
    QTableView* view = nullptr;
    int row = -1;
    if (playbackIsMpd()) {
        if (!mpd_controller_->connected())
            return;
        if (const auto& requests = mpd_controller_->requestQueue();
            requests && requests->active_id != 0) {
            if (jump && up_next_dock_) {
                up_next_dock_->setVisible(true);
                up_next_dock_->raise();
            }
            return;
        }
        const auto name = mpd_controller_->activeContextName();
        row = mpd_controller_->songPosition();
        if (row < 0)
            return;
        if (name.isEmpty()) {
            if (mpd_controller_->queueStashed())
                return;
            view = mpd_queue_view_;
        } else if (auto* tab = mpdPlaylistTabNamed(name)) {
            view = tab->view;
        } else if (jump) {
            pending_playing_list_ = name;
            openMpdPlaylistTab(name, true);
            return;
        }
        // Context positions can temporarily precede the list refresh. Never
        // select a different track just because it occupies the same row.
        if (view && (row >= view->model()->rowCount() ||
                     view->model()->index(row, 0).data(ui::track_source_role).toString() !=
                         mpd_controller_->nowPlayingUri()))
            return;
    } else {
        if (playback_.requests.active()) {
            if (jump && up_next_dock_) {
                up_next_dock_->setVisible(true);
                up_next_dock_->raise();
            }
            return;
        }
        if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
            if (const auto playing_row = resolvePlaybackRow(tab); playing_row >= 0) {
                view = tab->view;
                row = playing_row;
            }
        }
    }
    if (!view || !view->model() || row < 0 || row >= view->model()->rowCount())
        return;
    const auto index = view->model()->index(row, ui::track_title_column);
    if (!jump && (tabs_->currentWidget() != view ||
                  (followed_playback_view_ == view && followed_playback_index_ == index)))
        return;
    followed_playback_view_ = view;
    followed_playback_index_ = index;
    if (jump)
        tabs_->setCurrentWidget(view);
    view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
    view->scrollTo(index, QAbstractItemView::PositionAtCenter);
    if (jump)
        view->setFocus(Qt::ShortcutFocusReason);
}

void BenchMainWindow::sampleListeningHistory(const audio::LocalAuditionSnapshot& snapshot,
                                             const qint64 monotonic_ms, const qint64 wall_ms) {
    const auto observation = audio::listen_observation(snapshot);
    if (!local_listen_accounting_.observe(observation.identity, observation.duration_seconds,
                                          observation.position_seconds, observation.playing,
                                          monotonic_ms) ||
        !persistence_)
        return;
    persistence::ListItem source;
    source.source = persistence::ListSource::local;
    source.source_reference = snapshot.raw_path;
    source.source_revision = snapshot.source_revision;
    source.source_selection = persistence::ListItemSourceSelection{
        snapshot.selection.stream_index, snapshot.selection.subsong_index};
    if (snapshot.segment)
        source.segment = persistence::ListItemSegment{snapshot.segment->start_sample,
                                                      snapshot.segment->end_sample};
    persistence_->recordLocalListen(
        std::move(source), core::StableId::random(), wall_ms, [this](const QString& error) {
            if (!error.isEmpty())
                statusBar()->showMessage(
                    QStringLiteral("Could not save listening history: %1").arg(error), 10000);
        });
}

void BenchMainWindow::refreshEngineTransport() {
    const auto state = engine_playback_->state();
    const auto playing = state.status == QStringLiteral("playing");
    const auto stopped = state.status == QStringLiteral("stopped");

    // Anything the engine could act on enables the control. The local path
    // gates on its own player's snapshot; here the authority is a process
    // away, and this client's picture of it is a moment old.
    play_pause_action_->setEnabled(!stopped || state.queue_size > 0U);
    stop_action_->setEnabled(!stopped || state.queue_size > 0U);
    next_action_->setEnabled(state.queue_size > 1U);
    previous_action_->setEnabled(state.queue_size > 1U);
    play_pause_action_->setText(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
    if (transport_icon_playing_ != std::optional{playing}) {
        transport_icon_playing_ = playing;
        play_pause_action_->setIcon(
            style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    }

    const auto duration_ms =
        std::clamp<qint64>(state.duration_ms, 0, std::numeric_limits<int>::max());
    seek_->setEnabled(duration_ms > 0);
    seek_->setRange(0, static_cast<int>(duration_ms));
    if (!seeking_) {
        const QSignalBlocker blocker{seek_};
        seek_->setValue(static_cast<int>(std::clamp<qint64>(state.position_ms, 0, duration_ms)));
    }
    elapsed_->setText(formatTime(state.position_ms));
    duration_->setText(formatTime(duration_ms));

    // The engine owns the output, so the slider shows what the engine has --
    // including a change another client made.
    volume_->setEnabled(true);
    if (!changing_volume_) {
        const QSignalBlocker blocker{volume_};
        volume_->setValue(state.volume_percent);
    }
    refreshMuteButton();

    if (stopped || state.path.isEmpty()) {
        now_playing_->setText(QStringLiteral("Nothing playing"));
        now_playing_context_->clear();
        now_playing_->setToolTip({});
        now_playing_context_->setToolTip({});
    } else {
        // Named from the list the entry came from when it is still open, so
        // the header reads the same as the row; the file name is the fallback
        // for an entry whose tab has been closed.
        auto label = QFileInfo{state.path}.fileName();
        QString context;
        if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
            const auto& rows = tab->model->rows();
            const auto match = std::find_if(rows.begin(), rows.end(), [&state](const auto& row) {
                return QString::fromStdString(row.entry_id.to_string()) == state.entry;
            });
            if (match != rows.end() && !match->title.empty()) {
                label = QString::fromStdString(match->title);
                if (!match->artist.empty()) {
                    context = QString::fromStdString(match->artist);
                }
            }
            if (context.isEmpty()) {
                context = QString::fromStdString(tab->document.name);
            }
        }
        now_playing_->setText(label);
        now_playing_context_->setText(context);
        now_playing_->setToolTip(state.path);
        now_playing_context_->setToolTip(state.path);
    }
    if (state.entry != engine_entry_) {
        engine_entry_ = state.entry;
        if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
            const auto& rows = tab->model->rows();
            const auto match = std::find_if(rows.begin(), rows.end(), [&state](const auto& row) {
                return QString::fromStdString(row.entry_id.to_string()) == state.entry;
            });
            if (match != rows.end()) {
                const auto row = static_cast<int>(std::distance(rows.begin(), match));
                playback_.anchors.current = match->entry_id;
                playback_.row = row;
                tab->model->setCurrentSource(tab->model->source(row), row);
            }
        }
    }

    // Observable for offscreen tests and diagnostics, the same way the local
    // path publishes its state.
    setProperty("trackknife-engine-playback", state.status);
    refreshPlaybackCursor();
}

void BenchMainWindow::refreshTransport() {
    if (playingOnEngine()) {
        // The workspace's own up-next, resume, listening and gapless belong
        // to the engine now, so none of the 400 lines below run: doing both
        // would double-count listening and fight over the queue.
        if (isMpdContext()) {
            // An MPD tab is still the MPD server's, and the header follows the
            // tab. The local path is skipped either way, because the local
            // player is idle and its state means nothing here.
            refreshMpdTransport();
            refreshPlaybackCursor();
            return;
        }
        refreshEngineTransport();
        return;
    }
    refreshPlaybackCursor();
    if (player_ == nullptr) {
        if (isMpdContext()) {
            refreshMpdTransport();
            return;
        }
        for (auto* action : {previous_action_, play_pause_action_, stop_action_, next_action_}) {
            action->setEnabled(false);
        }
        seek_->setEnabled(false);
        volume_->setEnabled(false);
        refreshMuteButton();
        device_button_->setEnabled(false);
        return;
    }
    const auto snapshot = player_->snapshot();
    sampleLastFm(snapshot);
    checkpointLocalResume(snapshot);
    if (!local_history_clock_.isValid())
        local_history_clock_.start();
    sampleListeningHistory(snapshot, local_history_clock_.elapsed(),
                           QDateTime::currentMSecsSinceEpoch());
    // Observable for offscreen tests and diagnostics.
    setProperty("trackknife-player-state", static_cast<int>(snapshot.state));

    refreshUpNext();
    const auto queued_source = queued_source_from_snapshot(snapshot);
    if (requested_request_ && snapshot.next_occurrence_token == requested_request_->id)
        queued_request_ = requested_request_;
    if (!playback_.anchors.requested.is_nil() && queued_source) {
        if (const auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
            if (const auto row = tab->model->rowOfEntry(playback_.anchors.requested, -1);
                row >= 0 && tab->model->source(row) == *queued_source) {
                playback_.anchors.queued = playback_.anchors.requested;
            }
        }
    }
    // A consumed gapless takeover moves the anchors and highlight without any
    // load; the engine already plays the next row.
    if (snapshot.chain_transitions != last_chain_transitions_) {
        last_chain_transitions_ = snapshot.chain_transitions;
        playback_.last_requested_next.reset();
        auto transitioned_request =
            queued_request_ && queued_request_->id == snapshot.occurrence_token
                ? queued_request_
                : requested_request_;
        if (transitioned_request && snapshot.occurrence_token != 0U &&
            transitioned_request->id == snapshot.occurrence_token) {
            adoptLocalRequest(*transitioned_request);
        } else if (auto* tab = tabForDocument(playback_.anchors.document);
                   tab != nullptr && !snapshot.raw_path.empty()) {
            playback_.requests.finished();
            persistUpNext();
            const auto transitioned_source = source_from_snapshot(snapshot);
            // The queued anchor wins over the requested one when both still
            // resolve and match what the engine actually transitioned to.
            const auto anchored_row = [&](const core::StableId& entry) {
                const auto candidate = tab->model->rowOfEntry(entry, -1);
                return candidate >= 0 && tab->model->source(candidate) == transitioned_source
                           ? candidate
                           : -1;
            };
            auto row = anchored_row(playback_.anchors.queued);
            if (row < 0) {
                row = anchored_row(playback_.anchors.requested);
            }
            if (row >= 0) {
                adoptPlaybackRow(*tab, row, transitioned_source, true);
            } else {
                playback_.anchors.current = core::StableId{};
                playback_.anchors.source = transitioned_source;
                resetPlaybackOrder();
                tab->model->setCurrentSource({}, -1);
            }
            playback_.anchors.forget_transition();
            if (playback_.modes.expire_single()) {
                saveLocalPlaybackModes();
                refreshLocalPlaybackControls();
            }
        }
    }
    setProperty("trackknife-player-replaygain", static_cast<int>(snapshot.replay_gain_mode));
    setProperty("trackknife-player-rg-preamp-with",
                static_cast<double>(snapshot.replay_gain_preamps.with_gain_db));
    setProperty("trackknife-player-rg-preamp-without",
                static_cast<double>(snapshot.replay_gain_preamps.without_gain_db));
    setProperty("trackknife-player-row", playback_.row);
    setProperty("trackknife-player-position", static_cast<qlonglong>(snapshot.position_sample));
    setProperty("trackknife-player-buffered", static_cast<qlonglong>(snapshot.buffered_frames));
    setProperty("trackknife-player-buffer-capacity-ms",
                static_cast<qlonglong>(snapshot.configured_buffer.capacity.count()));
    setProperty("trackknife-player-active-buffer-capacity-ms",
                snapshot.active_buffer
                    ? static_cast<qlonglong>(snapshot.active_buffer->capacity.count())
                    : static_cast<qlonglong>(-1));
    setProperty("trackknife-player-buffer-pending",
                snapshot.active_buffer && *snapshot.active_buffer != snapshot.configured_buffer);
    setProperty("trackknife-player-underruns", static_cast<qulonglong>(snapshot.underrun_count));
    setProperty("trackknife-player-callbacks",
                static_cast<qlonglong>(snapshot.output.callback_count));
    setProperty("trackknife-player-outputstate", static_cast<int>(snapshot.output.state));
    setProperty("trackknife-player-output-available", snapshot.output_target_available);
    setProperty("trackknife-player-output-suspended", snapshot.output_suspended);
    setProperty("trackknife-player-device-generation",
                static_cast<qulonglong>(snapshot.device_generation));
    setProperty("trackknife-player-default-output",
                snapshot.default_output_target ? displayText(*snapshot.default_output_target)
                                               : QString{});

    // Local progression (ADR-0119) applies once per finished occurrence,
    // independently of which authority is currently visible.
    if (snapshot.state == audio::LocalAuditionState::ended) {
        // The guard stays set until the worker actually leaves "ended";
        // resetting it on dispatch would re-fire every timer tick while the
        // next source is still loading and race through the list.
        if (!advance_pending_ && !album_order_preparing_) {
            advance_pending_ = true;
            const bool request_started = !playback_.modes.single_active() && playLocalRequest();
            const auto next = request_started ? std::nullopt : automaticPlaybackRow();
            if (!request_started) {
                if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
                    if (next) {
                        if (auto result =
                                load_and_play(*player_, next->second,
                                              local_replay_gain_override(*tab->model, next->first));
                            result) {
                            playback_.requests.finished();
                            persistUpNext();
                            adoptPlaybackRow(*tab, next->first, next->second, true);
                            playback_.last_requested_next.reset();
                            playback_.anchors.forget_transition();
                        } else {
                            statusBar()->showMessage(QStringLiteral("Playback failed: %1")
                                                         .arg(displayText(result.error().message)),
                                                     5'000);
                        }
                    } else {
                        if (!playback_.requests.active())
                            consumePlaybackRow(*tab, playback_.anchors.current, playback_.row);
                    }
                }
                if (!playback_.modes.single_active()) {
                    playback_.requests.finished();
                    persistUpNext();
                }
            }
            if (playback_.modes.expire_single()) {
                saveLocalPlaybackModes();
                refreshLocalPlaybackControls();
            }
        }
    } else if (snapshot.state == audio::LocalAuditionState::empty) {
        if (!playback_.anchors.document.is_nil() && playback_.requests.pending().empty()) {
            if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
                tab->model->setCurrentSource({}, -1);
            }
            playback_.stop();
        }
        advance_pending_ = false;
    } else if (snapshot.state != audio::LocalAuditionState::loading) {
        advance_pending_ = false;
    }

    const auto error = snapshot.error ? displayText(snapshot.error->message) : QString{};
    if (!error.isEmpty() && error != last_player_error_) {
        last_player_error_ = error;
        statusBar()->showMessage(QStringLiteral("Playback failed: %1").arg(error), 5'000);
    } else if (error.isEmpty()) {
        last_player_error_.clear();
    }

    const auto monitor_error = snapshot.device_monitor_error
                                   ? displayText(snapshot.device_monitor_error->message)
                                   : QString{};
    if (!monitor_error.isEmpty() && monitor_error != last_device_monitor_error_) {
        last_device_monitor_error_ = monitor_error;
        statusBar()->showMessage(
            QStringLiteral("Audio device monitoring failed: %1").arg(monitor_error), 5'000);
    } else if (monitor_error.isEmpty()) {
        last_device_monitor_error_.clear();
    }
    const auto recovery_error = snapshot.output_recovery_error
                                    ? displayText(snapshot.output_recovery_error->message)
                                    : QString{};
    if (!recovery_error.isEmpty() && recovery_error != last_output_recovery_error_) {
        last_output_recovery_error_ = recovery_error;
        statusBar()->showMessage(
            QStringLiteral("Audio output recovery failed: %1").arg(recovery_error), 5'000);
    } else if (recovery_error.isEmpty()) {
        last_output_recovery_error_.clear();
    }
    if (last_device_generation_ != 0U) {
        if (selected_device_available_ && !snapshot.output_target_available) {
            statusBar()->showMessage(QStringLiteral("Audio output unavailable · playback paused"),
                                     5'000);
        } else if (!selected_device_available_ && snapshot.output_target_available &&
                   !snapshot.output_suspended) {
            statusBar()->showMessage(
                QStringLiteral("Audio output available again · press Play to resume"), 5'000);
        } else if (!snapshot.output_target && default_device_ &&
                   snapshot.default_output_target != default_device_) {
            statusBar()->showMessage(QStringLiteral("System audio output changed"), 5'000);
        }
    }

    // Keep the engine's queued continuation in sync with the next list row so
    // transitions are gapless. Re-requests are throttled: the engine drops
    // the queue on seeks and silently rejects format changes, and the drain
    // fallback below covers rejected continuations.
    if (snapshot.format.has_value() && snapshot.state != audio::LocalAuditionState::failed &&
        snapshot.state != audio::LocalAuditionState::empty &&
        snapshot.state != audio::LocalAuditionState::loading &&
        snapshot.state != audio::LocalAuditionState::ended && !advance_pending_) {
        const auto next = automaticPlaybackRow();
        const auto next_request =
            !playback_.modes.single_active() && !playback_.requests.pending().empty()
                ? std::optional{playback_.requests.pending().front()}
                : std::nullopt;
        const auto desired = next_request
                                 ? std::optional{LocalTrackSource{next_request->source.raw_path,
                                                                  next_request->source.selection,
                                                                  next_request->source.segment}}
                                 : (next ? std::optional{next->second} : std::nullopt);
        const auto desired_marker = desired.value_or(LocalTrackSource{});
        const auto queued = queued_source_from_snapshot(snapshot);
        const auto desired_token = next_request ? next_request->id : 0U;
        const bool changed = !playback_.last_requested_next ||
                             *playback_.last_requested_next != desired_marker ||
                             desired_token != last_requested_token_;
        const bool stale =
            (desired != queued || desired_token != snapshot.next_occurrence_token) &&
            (!next_request_timer_.isValid() || next_request_timer_.elapsed() > 1'000);
        const bool token_changed = desired_token != snapshot.next_occurrence_token;
        if ((desired != queued || token_changed) && (changed || stale)) {
            if (!desired) {
                static_cast<void>(player_->clear_gapless_next());
                requested_request_.reset();
            } else {
                auto* tab = tabForDocument(playback_.anchors.document);
                auto override_info =
                    next_request ? local_replay_gain_override(*up_next_local_model_, 0)
                                 : (tab != nullptr && next
                                        ? local_replay_gain_override(*tab->model, next->first)
                                        : std::nullopt);
                if (auto result = queue_gapless(*player_, *desired, override_info, desired_token);
                    result) {
                    requested_request_ = next_request;
                    if (tab != nullptr && next && !next_request) {
                        playback_.anchors.requested =
                            tab->model->rows().at(static_cast<std::size_t>(next->first)).entry_id;
                    } else
                        playback_.anchors.requested = core::StableId{};
                }
            }
            last_requested_token_ = desired_token;
            playback_.last_requested_next = desired_marker;
            next_request_timer_.start();
        }
    }

    if (isMpdContext()) {
        refreshMpdTransport();
        return;
    }
    const bool active = playerActive(snapshot.state);
    const bool source_ready = snapshot.format.has_value() &&
                              snapshot.state != audio::LocalAuditionState::loading &&
                              snapshot.state != audio::LocalAuditionState::failed;
    previous_action_->setEnabled(playback_.requests.active().has_value() ||
                                 adjacentPlaybackRow(-1).has_value());
    next_action_->setEnabled(!playback_.requests.pending().empty() ||
                             adjacentPlaybackRow(1).has_value());
    play_pause_action_->setEnabled((source_ready || !playback_.requests.pending().empty()) &&
                                   snapshot.output_target_available && !snapshot.output_suspended);
    play_pause_action_->setText(active ? QStringLiteral("Pause") : QStringLiteral("Play"));
    if (transport_icon_playing_ != std::optional{active}) {
        transport_icon_playing_ = active;
        play_pause_action_->setIcon(
            style()->standardIcon(active ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    }
    stop_action_->setEnabled(source_ready);

    if (snapshot.state == audio::LocalAuditionState::empty) {
        now_playing_->clear();
        now_playing_context_->clear();
        now_playing_->setToolTip({});
        now_playing_context_->setToolTip({});
    } else {
        const auto slash = snapshot.raw_path.find_last_of('/');
        const auto name = slash == std::string::npos || slash + 1U >= snapshot.raw_path.size()
                              ? snapshot.raw_path
                              : snapshot.raw_path.substr(slash + 1U);
        const auto fallback = QString::fromStdString(core::escape_raw_path(name));
        auto label = fallback;
        auto context = QString{};
        if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
            const auto row = tab->model->rowOfSource(source_from_snapshot(snapshot), playback_.row);
            if (row >= 0) {
                const auto& track = tab->model->rows()[static_cast<std::size_t>(row)];
                const auto title = track.title.empty() ? fallback : displayText(track.title);
                if (!track.artist.empty()) {
                    label = QStringLiteral("%1 — %2").arg(displayText(track.artist), title);
                } else {
                    label = title;
                }
                QStringList details;
                if (!track.album.empty()) {
                    details.push_back(displayText(track.album));
                }
                if (!track.date.empty()) {
                    details.push_back(displayText(track.date));
                }
                context = details.join(QStringLiteral(" · "));
            }
        }
        if (playback_.requests.active()) {
            const auto& request = playback_.requests.active()->source;
            label =
                displayText(request.artist) + QStringLiteral(" — ") + displayText(request.title);
            context = QStringLiteral("Up Next");
        }
        now_playing_->setText(label);
        now_playing_context_->setText(context);
        const auto path_tooltip = QString::fromStdString(core::escape_raw_path(snapshot.raw_path));
        now_playing_->setToolTip(path_tooltip);
        now_playing_context_->setToolTip(context.isEmpty() ? path_tooltip : context);
    }

    qint64 position_ms = 0;
    qint64 duration_ms = 0;
    if (snapshot.format && snapshot.format->sample_rate > 0) {
        position_ms = snapshot.position_sample * 1'000 / snapshot.format->sample_rate;
        if (snapshot.end_sample) {
            duration_ms = *snapshot.end_sample * 1'000 / snapshot.format->sample_rate;
        }
    }
    elapsed_->setText(formatTime(position_ms));
    duration_->setText(formatTime(duration_ms));
    const auto bounded = std::clamp<qint64>(duration_ms, 0, std::numeric_limits<int>::max());
    seek_->setEnabled(source_ready && bounded > 0);
    seek_->setRange(0, static_cast<int>(bounded));
    if (!seeking_) {
        const QSignalBlocker blocker{seek_};
        seek_->setValue(
            static_cast<int>(std::clamp<qint64>(position_ms, 0, std::numeric_limits<int>::max())));
    }
    volume_->setEnabled(true);
    if (!changing_volume_) {
        const QSignalBlocker blocker{volume_};
        volume_->setValue(snapshot.volume_percent);
    }
    refreshMuteButton();

    std::vector<std::pair<std::string, std::string>> choices;
    choices.reserve(snapshot.devices.size());
    for (const auto& device : snapshot.devices) {
        choices.emplace_back(device.name, device.description);
    }
    const bool device_menu_changed = choices != device_choices_;
    const bool selection_changed = snapshot.output_target != selected_device_;
    const bool availability_changed =
        snapshot.output_target_available != selected_device_available_;
    const bool default_changed = snapshot.default_output_target != default_device_;
    device_choices_ = std::move(choices);
    selected_device_ = snapshot.output_target;
    default_device_ = snapshot.default_output_target;
    selected_device_available_ = snapshot.output_target_available;
    last_device_generation_ = snapshot.device_generation;
    if (device_menu_changed || selection_changed || availability_changed || default_changed) {
        rebuildDeviceMenu();
    }
    QString device_label = QStringLiteral("System default");
    if (selected_device_) {
        const auto found = std::ranges::find(device_choices_, *selected_device_,
                                             &std::pair<std::string, std::string>::first);
        device_label = found == device_choices_.end()
                           ? displayText(*selected_device_)
                           : displayText(found->second.empty() ? found->first : found->second);
    } else if (default_device_) {
        const auto found = std::ranges::find(device_choices_, *default_device_,
                                             &std::pair<std::string, std::string>::first);
        const auto default_label =
            found == device_choices_.end()
                ? displayText(*default_device_)
                : displayText(found->second.empty() ? found->first : found->second);
        device_label += QStringLiteral(" — %1").arg(default_label);
    }
    if (!snapshot.output_target_available) {
        device_label += QStringLiteral(" (unavailable)");
    }
    device_button_->setEnabled(true);
    const bool buffer_pending =
        snapshot.active_buffer && *snapshot.active_buffer != snapshot.configured_buffer;
    auto audio_tooltip =
        QStringLiteral("Audio output: %1\nBuffer: %2 · %3 ms capacity · %4 ms start%5\n"
                       "Underruns: %6")
            .arg(device_label)
            .arg(bufferProfileLabel(selected_buffer_profile_))
            .arg(snapshot.configured_buffer.capacity.count())
            .arg(snapshot.configured_buffer.start_threshold.count())
            .arg(buffer_pending ? QStringLiteral(" · applies next track") : QString{})
            .arg(snapshot.underrun_count);
    if (!snapshot.output_target_available) {
        audio_tooltip += QStringLiteral("\nPlayback is paused until an output is available");
    } else if (snapshot.output_suspended) {
        audio_tooltip += QStringLiteral("\nReconnecting the audio output");
    }
    if (snapshot.output.node_id) {
        audio_tooltip += QStringLiteral("\nPipeWire node: %1").arg(*snapshot.output.node_id);
    }
    device_button_->setToolTip(audio_tooltip);
    device_button_->setAccessibleDescription(device_label);
    publishMprisState();
}

// Desktop commands land on the exact transport actions the visible controls
// use, so MPRIS can never steer past the active authority (ADR-0135).
void BenchMainWindow::buildMprisService() {
    mpris_ = new MprisService(this);
    const auto trigger = [](QAction* action) {
        if (action != nullptr && action->isEnabled()) {
            action->trigger();
        }
    };
    connect(mpris_, &MprisService::playPauseRequested, this,
            [this, trigger] { trigger(play_pause_action_); });
    connect(mpris_, &MprisService::playRequested, this, [this, trigger] {
        if (mpris_->currentState().status != QStringLiteral("Playing")) {
            trigger(play_pause_action_);
        }
    });
    connect(mpris_, &MprisService::pauseRequested, this, [this, trigger] {
        if (mpris_->currentState().status == QStringLiteral("Playing")) {
            trigger(play_pause_action_);
        }
    });
    connect(mpris_, &MprisService::stopRequested, this, [this, trigger] { trigger(stop_action_); });
    connect(mpris_, &MprisService::nextRequested, this, [this, trigger] { trigger(next_action_); });
    connect(mpris_, &MprisService::previousRequested, this,
            [this, trigger] { trigger(previous_action_); });
    connect(mpris_, &MprisService::positionRequested, this, [this](const qlonglong position_ms) {
        if (seek_ != nullptr && seek_->isEnabled()) {
            seekToMs(position_ms);
        }
    });
    connect(mpris_, &MprisService::volumeRequested, this, [this](const int volume_percent) {
        if (volume_ != nullptr && volume_->isEnabled()) {
            volume_->setValue(volume_percent);
        }
    });
    connect(mpris_, &MprisService::raiseRequested, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    // ADR-0144: quiet, opt-in track-change notifications share the MPRIS
    // now-playing snapshot.
    notifier_ = new DesktopNotifier(this);
    notifier_->setBackgroundOnly(
        QSettings{}.value(QStringLiteral("desktop/notifications-background-only"), false).toBool());
    connect(notifier_, &DesktopNotifier::deliveryFinished, this, [this](const QString& error) {
        if (!error.isEmpty())
            statusBar()->showMessage(QStringLiteral("Notification failed: %1").arg(error), 8000);
    });
    notifier_->setEnabled(
        QSettings{}.value(QStringLiteral("desktop/notifications"), false).toBool());
    if (notifications_action_ != nullptr) {
        const QSignalBlocker blocker{notifications_action_};
        notifications_action_->setChecked(notifier_->isEnabled());
    }
}

void BenchMainWindow::publishMprisState() {
    if (mpris_ == nullptr && notifier_ == nullptr) {
        return;
    }
    MprisPlaybackState state;
    if (isMpdContext()) {
        const auto connected = mpd_controller_->connected();
        const auto command_ready = connected && !mpd_controller_->commandBusy();
        const auto has_queue = mpd_controller_->queueCount() > 0;
        state.status = mpd_controller_->playing()  ? QStringLiteral("Playing")
                       : mpd_controller_->paused() ? QStringLiteral("Paused")
                                                   : QStringLiteral("Stopped");
        state.track_key = mpd_controller_->nowPlayingUri();
        if (!state.track_key.isEmpty()) {
            state.title = mpd_controller_->nowPlayingTitle();
            state.artist = mpd_controller_->nowPlayingArtist();
            state.album = mpd_controller_->nowPlayingAlbum();
        }
        const auto duration_ms = mpd_controller_->durationMs();
        state.length_us = duration_ms > 0 ? duration_ms * 1'000 : -1;
        state.position_us = mpd_controller_->elapsedMs() * 1'000;
        state.volume_percent = mpd_controller_->volume();
        state.can_next = command_ready && has_queue;
        state.can_previous = command_ready && has_queue;
        state.can_play = command_ready;
        state.can_pause = command_ready;
        state.can_seek = command_ready && duration_ms > 0;
    } else if (player_ != nullptr) {
        const auto snapshot = player_->snapshot();
        const auto active = playerActive(snapshot.state);
        state.status = active ? QStringLiteral("Playing")
                       : snapshot.state == audio::LocalAuditionState::paused
                           ? QStringLiteral("Paused")
                           : QStringLiteral("Stopped");
        if (!snapshot.raw_path.empty()) {
            state.track_key = QString::fromStdString(core::escape_raw_path(snapshot.raw_path));
            const auto slash = snapshot.raw_path.find_last_of('/');
            const auto name = slash == std::string::npos || slash + 1U >= snapshot.raw_path.size()
                                  ? snapshot.raw_path
                                  : snapshot.raw_path.substr(slash + 1U);
            state.title = QString::fromStdString(core::escape_raw_path(name));
            if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
                const auto row =
                    tab->model->rowOfSource(source_from_snapshot(snapshot), playback_.row);
                if (row >= 0) {
                    const auto& track = tab->model->rows()[static_cast<std::size_t>(row)];
                    if (!track.title.empty()) {
                        state.title = displayText(track.title);
                    }
                    state.artist = displayText(track.artist);
                    state.album = displayText(track.album);
                }
            }
        }
        if (playback_.requests.active()) {
            const auto& request = *playback_.requests.active();
            state.track_key += QStringLiteral("#up-next-%1").arg(request.id);
            state.title = displayText(request.source.title);
            state.artist = displayText(request.source.artist);
            state.album = displayText(request.source.album);
        }
        if (snapshot.format && snapshot.format->sample_rate > 0) {
            state.position_us = snapshot.position_sample * 1'000'000 / snapshot.format->sample_rate;
            if (snapshot.end_sample) {
                state.length_us = *snapshot.end_sample * 1'000'000 / snapshot.format->sample_rate;
            }
        }
        state.volume_percent = snapshot.volume_percent;
        const bool source_ready = snapshot.format.has_value() &&
                                  snapshot.state != audio::LocalAuditionState::loading &&
                                  snapshot.state != audio::LocalAuditionState::failed;
        state.can_next =
            !playback_.requests.pending().empty() || adjacentPlaybackRow(1).has_value();
        state.can_previous =
            playback_.requests.active().has_value() || adjacentPlaybackRow(-1).has_value();
        state.can_play = (source_ready || !playback_.requests.pending().empty()) &&
                         snapshot.output_target_available;
        state.can_pause = state.can_play;
        state.can_seek = source_ready && state.length_us > 0;
    }
    if (mpris_ != nullptr) {
        mpris_->publish(state);
    }
    if (notifier_ != nullptr &&
        notifier_->publish(state, QApplication::activeWindow() != nullptr)) {
        setProperty("trackknife-notifications-sent",
                    static_cast<qulonglong>(notifier_->sentCount()));
        setProperty("trackknife-notification-summary", notifier_->lastSummary());
        setProperty("trackknife-notification-body", notifier_->lastBody());
    }
}

} // namespace trackknife::bench
