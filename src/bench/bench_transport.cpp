// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/mpris_service.hpp"
#include <QDockWidget>

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/audio/local_audition.hpp"
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
#include <QWidgetAction>
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

} // namespace

BenchMainWindow::BenchMainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Trackknife"));
    resize(1100, 720);
    setAcceptDrops(true);

    // ADR-0226: this window plays nothing itself. The engine owns playback,
    // and the buffer shown here is the one it reports.
    selected_buffer_profile_ = loadPlaybackBufferPreference().profile;

    buildWorkspace();
    buildTransport();
    buildLastFm();
    buildShortcuts();
    connect(&metadata_operation_watcher_, &QFutureWatcherBase::finished, this,
            &BenchMainWindow::finishMetadataOperationJob);
    initializePersistence();

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
        const auto key = QStringLiteral("local");
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
        if (playingOnEngine()) {
            transport_->previous();
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
        if (playingOnEngine()) {
            transport_->stop();
        }
    });
    next_action_ = new QAction(style()->standardIcon(QStyle::SP_MediaSkipForward),
                               QStringLiteral("Next"), this);
    connect(next_action_, &QAction::triggered, this, [this] {
        if (playingOnEngine()) {
            transport_->next();
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
        const auto key = QStringLiteral("local");
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
        if (playingOnEngine()) {
            // The engine owns the output, so the volume lives there: another
            // client watching the same engine sees the same number, and it
            // survives this window closing.
            transport_->setVolume(value);
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
        if (playingOnEngine()) {
            transport_->refreshOutputs();
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

    if (!playingOnEngine()) {
        statusBar()->showMessage(QStringLiteral("Playback buffer unchanged: no engine"), 5'000);
        refreshPlaybackBufferChecks();
        return;
    }
    // ADR-0226: the engine's buffer, which it keeps. Settings mirror it so
    // the dialog shows the engine's value.
    transport_->setBuffer(capacity_ms, start_threshold_ms);

    selected_buffer_profile_ = profile;
    QSettings settings;
    settings.setValue(QString::fromLatin1(buffer_profile_settings_key), profile);
    settings.setValue(QString::fromLatin1(buffer_capacity_settings_key), capacity_ms);
    settings.setValue(QString::fromLatin1(buffer_threshold_settings_key), start_threshold_ms);
    settings.sync();
    refreshPlaybackBufferChecks();

    const bool pending = transport_->state().status != QStringLiteral("stopped");
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
    const auto engine = playingOnEngine() ? std::optional{transport_->state()} : std::nullopt;
    if (selected_buffer_profile_ != preference.profile ||
        (engine &&
         (engine->buffer_capacity_ms != preference.config.capacity.count() ||
          engine->buffer_start_threshold_ms != preference.config.start_threshold.count()))) {
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

QString BenchMainWindow::outputLabel(const EnginePlayback::State::Output& output) const {
    if (output.local) {
        // The engine's own audio: this computer's, or the server's -- by the
        // name the server gives it, where it gives one.
        if (!output_choices_remote_) {
            return QStringLiteral("This computer");
        }
        return output.name.empty() || output.name == "this machine" ? QStringLiteral("The server")
                                                                    : displayText(output.name);
    }
    return displayText(output.name);
}

void BenchMainWindow::rebuildDeviceMenu() {
    device_menu_->clear();
    // The previous rebuild's output group, whose actions clear() just took.
    for (auto* group : device_menu_->findChildren<QActionGroup*>(Qt::FindDirectChildrenOnly)) {
        if (group != device_group_) {
            group->deleteLater();
        }
    }
    device_menu_->setToolTipsVisible(true);

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
            if (playingOnEngine()) {
                transport_->setOutput(target);
            }
        });
        return action;
    };

    // Headings as labels: QMenu::addSection draws its text only in some
    // styles, and in others the two groups ran together unlabelled -- the
    // speakers read as sound cards.
    const auto add_heading = [this](const QString& text) {
        auto* label = new QLabel(text, device_menu_);
        label->setObjectName(QStringLiteral("bench-device-menu-heading"));
        auto font = label->font();
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() * 0.9);
        label->setFont(font);
        label->setContentsMargins(10, 6, 10, 2);
        label->setForegroundRole(QPalette::PlaceholderText);
        auto* heading = new QWidgetAction(device_menu_);
        heading->setDefaultWidget(label);
        heading->setEnabled(false);
        device_menu_->addAction(heading);
    };

    // ADR-0228: which of the engine's outputs plays -- shown once there is a
    // choice, which is when an agent has ever registered.
    const bool agents = std::ranges::any_of(output_choices_, [](const auto& output) {
        return !output.local;
    });
    if (agents) {
        add_heading(QStringLiteral("Speakers"));
        auto* outputs = new QActionGroup(device_menu_);
        outputs->setExclusive(true);
        for (const auto& output : output_choices_) {
            auto label = outputLabel(output);
            if (!output.online) {
                label += QStringLiteral(" (offline)");
            }
            auto* action = device_menu_->addAction(label);
            action->setObjectName(
                QStringLiteral("action-output-%1").arg(QString::fromStdString(output.id)));
            action->setCheckable(true);
            action->setChecked(output.selected);
            // An offline agent can still be chosen: the music waits there
            // and starts when it is back.
            if (!output.online) {
                action->setToolTip(
                    QStringLiteral("Not connected. Chosen, it plays as soon as it is back."));
            } else if (!output.local && !output.files) {
                action->setToolTip(QStringLiteral("Streams the music from the engine"));
            }
            outputs->addAction(action);
            connect(action, &QAction::triggered, this, [this, id = output.id] {
                if (playingOnEngine()) {
                    transport_->selectOutput(id);
                }
            });
        }
        const auto chosen = std::ranges::find_if(output_choices_, &EnginePlayback::State::Output::selected);
        device_menu_->addSeparator();
        add_heading(chosen != output_choices_.end()
                        ? QStringLiteral("Sound device on %1").arg(outputLabel(*chosen))
                        : QStringLiteral("Sound device"));
    }

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
        if (playingOnEngine()) {
            transport_->refreshOutputs();
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
        playback_.modes.album_random = on;
        if (on)
            playback_.modes.random = false;
        applyLocalPlaybackModes();
    });
    local_consume_action_ = add_mode(QStringLiteral("consume"), QStringLiteral("Consume"), {});
    connect(local_repeat_action_, &QAction::triggered, this, [this](bool on) {
        playback_.modes.repeat = on;
        applyLocalPlaybackModes();
    });
    connect(local_random_action_, &QAction::triggered, this, [this](bool on) {
        playback_.modes.random = on;
        if (on)
            playback_.modes.album_random = false;
        applyLocalPlaybackModes();
    });
    connect(local_single_action_, &QAction::triggered, this, [this] {
        playback_.modes.single = audio::next_mode_state(playback_.modes.single);
        applyLocalPlaybackModes();
    });
    connect(local_consume_action_, &QAction::triggered, this, [this] {
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
        // of these is its business.
        transport_->setModes(playback_.modes);
        transport_->setReplayGain(mode, preamps);
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
    for (auto* button : local_mode_buttons_) {
        // An engine can act on these even when this process has no audio
        // device of its own, which is the whole point of it owning playback.
        button->defaultAction()->setEnabled(playingOnEngine());
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
    const bool can_play = playingOnEngine();
    local_replaygain_button_->setEnabled(can_play);
    for (auto* action : local_replaygain_group_->actions()) {
        action->setEnabled(can_play);
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

void BenchMainWindow::syncEngineRequests() {
    // Asks go to the engine whose files they are, and only while it is the
    // one playing: the other would be asked for paths it does not have.
    if (!playingOnEngine() || (transport_ == remote_playback_) != up_next_remote_) {
        return;
    }
    std::vector<LocalTrackRow> rows;
    std::vector<std::optional<formats::ReplayGainInfo>> gains;
    QString stated;
    for (const auto& entry : playback_.requests.pending()) {
        stated += QString::fromStdString(entry.source.entry_id.to_string());
        gains.push_back(local_replay_gain_override(entry.source));
        rows.push_back(entry.source);
    }
    if (stated == engine_requests_) {
        return;
    }
    engine_requests_ = stated;
    transport_->setRequests(rows, gains);
}

void BenchMainWindow::syncEngineQueue() {
    if (!playingOnEngine()) {
        return;
    }
    auto* tab = tabForDocument(playback_.anchors.document);
    if (tab == nullptr) {
        return;
    }
    const auto& rows = tab->model->rows();
    QString stated;
    for (const auto& row : rows) {
        stated += QString::fromStdString(row.entry_id.to_string());
    }
    if (stated == engine_queue_) {
        return;
    }
    engine_queue_ = stated;
    std::vector<std::optional<formats::ReplayGainInfo>> overrides;
    overrides.reserve(rows.size());
    for (const auto& row : rows) {
        overrides.push_back(local_replay_gain_override(row));
    }
    // The engine follows identity, so the playing entry survives being handed
    // a queue that no longer holds it in the same row -- or at all.
    transport_->replaceQueue(rows, overrides);
}

void BenchMainWindow::adoptEngineQueue() {
    auto* tab = tabForDocument(playback_.anchors.document);
    if (tab == nullptr) {
        return;
    }
    const auto held = transport_->queueEntries();
    if (held.empty()) {
        return;
    }
    // Merged rather than replaced: the engine's entries are paths and tags it
    // was given, while these rows carry everything this window has read from
    // the files. Replacing them would throw that away and show a list of
    // filenames.
    std::vector<LocalTrackRow> merged;
    merged.reserve(held.size());
    const auto& existing = tab->model->rows();
    for (const auto& entry : held) {
        const auto row = tab->model->rowOfEntry(entry.entry_id, -1);
        if (row >= 0) {
            merged.push_back(existing[static_cast<std::size_t>(row)]);
            continue;
        }
        auto fresh = entry;
        fresh.title =
            core::escape_raw_path(fresh.raw_path.substr(fresh.raw_path.find_last_of('/') + 1));
        merged.push_back(std::move(fresh));
    }
    QString stated;
    for (const auto& row : merged) {
        stated += QString::fromStdString(row.entry_id.to_string());
    }
    if (stated == engine_queue_) {
        return;
    }
    engine_queue_ = stated;
    tab->model->replaceRows(std::move(merged), true);
    enqueueUnprobedRows(*tab);
    playback_.row = resolvePlaybackRow(tab);
    if (playback_.row >= 0) {
        tab->model->setCurrentSource(tab->model->source(playback_.row), playback_.row);
    }
    markTabDirty(*tab);
}

void BenchMainWindow::reattachToEngine() {
    if (!playingOnEngine()) {
        return;
    }
    const auto state = transport_->state();
    if (state.entry.isEmpty()) {
        return; // The engine holds nothing; there is nothing to attach to.
    }
    const auto playing = core::StableId::parse(state.entry.toStdString());
    if (!playing) {
        return;
    }

    // The list it came from is usually still open: identities are persisted
    // with the document (ADR-0221), so the entry the engine names is findable
    // without inventing a tab.
    for (const auto& tab : list_tabs_) {
        const auto row = tab->model->rowOfEntry(*playing, -1);
        if (row < 0) {
            continue;
        }
        adoptEngineRow(*tab, row, *playing);
        return;
    }

    // Otherwise the queue is the only record of what is playing, so it becomes
    // a list. The rows carry the engine's identities rather than fresh ones,
    // or the anchor below would name an entry this list does not contain.
    auto rows = transport_->queueEntries();
    if (rows.empty()) {
        return;
    }
    for (auto& row : rows) {
        row.title = core::escape_raw_path(row.raw_path.substr(row.raw_path.find_last_of('/') + 1));
    }
    const bool remote = transport_ != nullptr && transport_ == remote_playback_;
    // A remote engine's queue goes into its tab, which it always has.
    auto* tab = remote
                    ? remoteQueueTab()
                    : addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                           .kind = persistence::ListKind::scratch,
                                                           .name = "Playing on the engine",
                                                           .pinned = false,
                                                           .dirty = false,
                                                           .items = {}},
                                 true);
    if (tab == nullptr) {
        return;
    }
    tab->model->replaceRows(std::move(rows), true);
    // Titles are filenames until the files have been read; the ordinary probe
    // queue fills them in rather than a second path for this case.
    enqueueUnprobedRows(*tab);
    const auto row = tab->model->rowOfEntry(*playing, -1);
    if (row >= 0) {
        adoptEngineRow(*tab, row, *playing);
    }
    markTabDirty(*tab);
    schedulePersist();
}

void BenchMainWindow::adoptEngineRow(ListTab& tab, const int row, const core::StableId& entry) {
    playback_.anchors.document = tab.document.id;
    playback_.anchors.current = entry;
    playback_.row = row;
    engine_entry_ = QString::fromStdString(entry.to_string());
    tab.model->setCurrentSource(tab.model->source(row), row);
    setActiveLocalList(QString::fromStdString(tab.document.id.to_string()));
    refreshTransport();
    refreshPlaybackCursor(true);
}

EnginePlayback* BenchMainWindow::playbackFor(const bool remote) const {
    return remote ? remote_playback_ : local_playback_;
}

void BenchMainWindow::followPlayback(EnginePlayback* playback) {
    if (playback == nullptr || playback == transport_) {
        return;
    }
    // ADR-0227: one engine plays at a time. Starting on one stops the other,
    // in that order, so an output agent the two share is released first.
    if (transport_ != nullptr && transport_->active() &&
        transport_->state().status != QStringLiteral("stopped")) {
        transport_->stop();
    }
    if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
        tab->model->setCurrentSource({}, -1);
    }
    transport_ = playback;
    // What the window knew about the other engine says nothing about this one.
    playback_.anchors = {};
    playback_.row = -1;
    engine_entry_.clear();
    engine_queue_.clear();
    engine_requests_.clear();
    engine_consumed_.clear();
    engine_queue_revision_ = 0;
    refreshTransport();
}

bool BenchMainWindow::playingOnEngine() const {
    // Ownership, not visibility: whether an engine is connected, never which
    // tab is on screen. Deciding it from the visible tab once let the local
    // refresh see an idle player and wipe the anchors the engine was playing
    // from.
    return transport_ != nullptr && transport_->active();
}

int BenchMainWindow::resolvePlaybackRow(const ListTab* tab) const {
    if (tab == nullptr) {
        return -1;
    }
    const LocalListPlaybackView list{*tab->model};
    return playback_.resolveRow(list);
}

void BenchMainWindow::playRow(ListTab& tab, const int row) {
    // ADR-0227: a tab plays on the engine whose files it lists.
    auto* target = playbackFor(tab.document.remote);
    if (target == nullptr || !target->active()) {
        statusBar()->showMessage(tab.document.remote
                                     ? QStringLiteral("Nothing can play: the remote engine is "
                                                      "not connected")
                                     : QStringLiteral("Nothing can play: this computer's engine "
                                                      "is not running"),
                                 5'000);
        return;
    }
    followPlayback(target);
    // The engine owns the queue, so it is given the whole list rather than
    // one track: skipping, shuffling and gapless are its decisions, and it
    // cannot make them from a single entry.
    const auto& rows = tab.model->rows();
    if (row < 0 || static_cast<std::size_t>(row) >= rows.size()) {
        return;
    }
    std::vector<std::optional<formats::ReplayGainInfo>> overrides;
    overrides.reserve(rows.size());
    for (const auto& source_row : rows) {
        overrides.push_back(local_replay_gain_override(source_row));
    }
    transport_->play(rows, overrides, rows[static_cast<std::size_t>(row)].entry_id);
    if (playback_.anchors.document != tab.document.id) {
        if (auto* previous = tabForDocument(playback_.anchors.document); previous != nullptr) {
            previous->model->setCurrentSource({}, -1);
        }
    }
    playback_.anchors.document = tab.document.id;
    playback_.anchors.current = rows[static_cast<std::size_t>(row)].entry_id;
    playback_.row = row;
    setActiveLocalList(QString::fromStdString(tab.document.id.to_string()));
    tab.model->setCurrentSource(tab.model->source(row), row);
}

void BenchMainWindow::togglePlayPause() {
    if (!playingOnEngine()) {
        return;
    }
    if (transport_->state().status == QStringLiteral("playing")) {
        transport_->pause();
    } else {
        transport_->resume();
    }
}

void BenchMainWindow::seekToMs(const qint64 position_ms) {
    if (playingOnEngine()) {
        transport_->seek(position_ms);
    }
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
                qobject_cast<LocalListModel*>(view->model()) != nullptr)
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

const LocalTrackRow* BenchMainWindow::playingRow(const QString& entry) {
    const auto identity = core::StableId::parse(entry.toStdString());
    if (!identity) {
        return nullptr;
    }
    const auto in = [&identity](const std::vector<LocalTrackRow>& rows) -> const LocalTrackRow* {
        const auto found = std::ranges::find(rows, *identity, &LocalTrackRow::entry_id);
        return found != rows.end() ? &*found : nullptr;
    };
    // The list it was played from, first: the header reads as the row does.
    if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
        if (const auto* row = in(tab->model->rows())) {
            return row;
        }
    }
    // Up Next: an ask is often from another list, the library or a search,
    // and it leaves Up Next as it starts -- so the one playing is looked for
    // there as well as among those waiting.
    if (const auto& active = playback_.requests.active();
        active && active->source.entry_id == *identity) {
        return &active->source;
    }
    for (const auto& waiting : playback_.requests.pending()) {
        if (waiting.source.entry_id == *identity) {
            return &waiting.source;
        }
    }
    for (const auto& tab : list_tabs_) {
        if (const auto* row = in(tab->model->rows())) {
            return row;
        }
    }
    return nullptr;
}

void BenchMainWindow::refreshEngineTransport() {
    const auto state = transport_->state();
    sampleLastFmFromEngine(state);
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
        const auto* row = playingRow(state.entry);
        if (row != nullptr && !row->title.empty()) {
            label = QString::fromStdString(row->title);
            if (!row->artist.empty()) {
                context = QString::fromStdString(row->artist);
            }
        }
        if (auto* tab = tabForDocument(playback_.anchors.document);
            tab != nullptr && context.isEmpty()) {
            context = QString::fromStdString(tab->document.name);
        }
        now_playing_->setText(label);
        now_playing_context_->setText(context);
        now_playing_->setToolTip(state.path);
        now_playing_context_->setToolTip(state.path);
    }
    if (state.modes != playback_.modes) {
        // The engine owns the modes while it owns playback: a one-shot
        // expires where the track actually ended. Adopted rather than pushed
        // back, or the two would argue.
        playback_.modes = state.modes;
        saveLocalPlaybackModes();
        refreshLocalPlaybackControls();
    }
    if (!state.consumed.isEmpty() && state.consumed != engine_consumed_) {
        engine_consumed_ = state.consumed;
        // The engine dropped it from its queue; the list it came from drops it
        // too. Told rather than deduced, so the two cannot disagree.
        if (const auto dropped = core::StableId::parse(state.consumed.toStdString())) {
            for (const auto& tab : list_tabs_) {
                const auto row = tab->model->rowOfEntry(*dropped, -1);
                if (row < 0) {
                    continue;
                }
                consuming_row_ = true;
                tab->model->removeRowIndexes({row}, false);
                consuming_row_ = false;
                markTabDirty(*tab);
                schedulePersist();
                break;
            }
        }
    }
    if (state.entry != engine_entry_) {
        engine_entry_ = state.entry;
        // The engine consumes a request by playing it, so the panel has to let
        // go of it too or it would be re-stated on the next sync and play
        // twice. The return-point the local path keeps is the engine's
        // business now: it continues from the row it played, which is a
        // difference worth knowing rather than papering over.
        const auto started = core::StableId::parse(state.entry.toStdString());
        if (started) {
            const auto& pending = playback_.requests.pending();
            const auto match = std::ranges::find_if(pending, [&started](const auto& entry) {
                return entry.source.entry_id == *started;
            });
            if (match != pending.end()) {
                playback_.requests.started(*match);
                engine_requests_.clear();
                persistUpNext();
                refreshUpNext();
            } else if (playback_.requests.active()) {
                playback_.requests.finished();
                persistUpNext();
                refreshUpNext();
            }
        }
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

    // Has the engine's queue drifted from what this window is showing? Asked
    // by size, which is in the state document already, rather than by fetching
    // the queue: fetching blocks on a round trip, and this runs on every
    // sample. Two queues of the same size with different contents slip
    // through, which is why attaching re-reads it properly; what this catches
    // is the case that actually happens -- something added or removed an entry
    // behind this window's back.
    // Not while this window's own commands are on their way: until they are
    // answered, the engine may report a queue from before them, and adopting
    // it would trade the rows just added for the engine's older list -- and
    // then, once the engine caught up, bring them back as bare paths. The
    // revision is left unread so the check runs once they are answered.
    if (state.queue_revision != engine_queue_revision_ && !transport_->settling()) {
        engine_queue_revision_ = state.queue_revision;
        if (auto* tab = tabForDocument(playback_.anchors.document);
            tab != nullptr &&
            state.queue_size != static_cast<std::size_t>(tab->model->rowCount())) {
            adoptEngineQueue();
        }
    }

    refreshOutputControls(state);
    // Observable for offscreen tests and diagnostics.
    setProperty("trackknife-engine-playback", state.status);
    setProperty("trackknife-player-replaygain", static_cast<int>(state.replay_gain_mode));
    setProperty("trackknife-player-rg-preamp-with",
                static_cast<double>(state.replay_gain_preamps.with_gain_db));
    setProperty("trackknife-player-rg-preamp-without",
                static_cast<double>(state.replay_gain_preamps.without_gain_db));
    publishMprisState();
    refreshPlaybackCursor();
}

void BenchMainWindow::refreshTransport() {
    if (playingOnEngine()) {
        refreshEngineTransport();
        return;
    }
    // ADR-0226: without an engine nothing plays; this window has no player
    // of its own to fall back on.
    refreshPlaybackCursor();
    for (auto* action : {previous_action_, play_pause_action_, stop_action_, next_action_}) {
        action->setEnabled(false);
    }
    seek_->setEnabled(false);
    volume_->setEnabled(false);
    refreshMuteButton();
    device_button_->setEnabled(false);
    device_button_->setToolTip(QStringLiteral("No engine is connected"));
    now_playing_->setText(QStringLiteral("No engine"));
    now_playing_context_->clear();
    setProperty("trackknife-engine-playback", QStringLiteral("unavailable"));
    publishMprisState();
}

// The engine's sink and buffer (ADR-0226). The devices are the engine
// machine's: for an engine on a NAS they are the NAS's, which is the point.
void BenchMainWindow::refreshOutputControls(const EnginePlayback::State& state) {
    std::vector<std::pair<std::string, std::string>> choices;
    choices.reserve(state.devices.size());
    for (const auto& device : state.devices) {
        choices.emplace_back(device.name, device.description);
    }
    if (engine_output_seen_) {
        if (selected_device_available_ && !state.output_available) {
            statusBar()->showMessage(QStringLiteral("Audio output unavailable · playback paused"),
                                     5'000);
        } else if (!selected_device_available_ && state.output_available &&
                   !state.output_suspended) {
            statusBar()->showMessage(
                QStringLiteral("Audio output available again · press Play to resume"), 5'000);
        } else if (!state.output_target && default_device_ &&
                   state.default_output != default_device_) {
            statusBar()->showMessage(QStringLiteral("System audio output changed"), 5'000);
        }
    }
    // ADR-0228: the chosen agent going away and coming back is worth saying;
    // the music waits for it either way.
    const auto chosen = [](const std::vector<EnginePlayback::State::Output>& outputs) {
        const auto found = std::ranges::find_if(outputs, &EnginePlayback::State::Output::selected);
        return found == outputs.end() ? std::optional<EnginePlayback::State::Output>{}
                                      : std::optional{*found};
    };
    const auto was = chosen(output_choices_);
    const auto now = chosen(state.outputs);
    if (engine_output_seen_ && was && now && was->id == now->id && !now->local &&
        was->online != now->online) {
        statusBar()->showMessage(now->online
                                     ? QStringLiteral("%1 is back").arg(outputLabel(*now))
                                     : QStringLiteral("%1 went away · playback waits for it")
                                           .arg(outputLabel(*now)),
                                 5'000);
    }
    const bool remote = transport_ != nullptr && transport_ == remote_playback_;
    const bool outputs_changed =
        state.outputs != output_choices_ || remote != output_choices_remote_;
    output_choices_ = state.outputs;
    output_choices_remote_ = remote;
    engine_output_seen_ = true;
    const bool menu_changed = outputs_changed || choices != device_choices_ ||
                              state.output_target != selected_device_ ||
                              state.output_available != selected_device_available_ ||
                              state.default_output != default_device_;
    device_choices_ = std::move(choices);
    selected_device_ = state.output_target;
    default_device_ = state.default_output;
    selected_device_available_ = state.output_available;
    if (menu_changed) {
        rebuildDeviceMenu();
    }

    const auto label_of = [this](const std::string& name) {
        const auto found =
            std::ranges::find(device_choices_, name, &std::pair<std::string, std::string>::first);
        return found == device_choices_.end()
                   ? displayText(name)
                   : displayText(found->second.empty() ? found->first : found->second);
    };
    QString device_label = QStringLiteral("System default");
    if (selected_device_) {
        device_label = label_of(*selected_device_);
    } else if (default_device_) {
        device_label += QStringLiteral(" — %1").arg(label_of(*default_device_));
    }
    if (!state.output_available) {
        device_label += QStringLiteral(" (unavailable)");
    }

    // The profile is a name for a pair of numbers, and the engine keeps only
    // the numbers; a match against the presets recovers the name.
    auto profile = QStringLiteral("custom");
    for (const auto preset :
         {audio::PlaybackBufferPreset::responsive, audio::PlaybackBufferPreset::balanced,
          audio::PlaybackBufferPreset::resilient}) {
        const auto config = audio::playback_buffer_preset_config(preset);
        if (config.capacity.count() == state.buffer_capacity_ms &&
            config.start_threshold.count() == state.buffer_start_threshold_ms) {
            const auto id = audio::playback_buffer_preset_id(preset);
            profile = QString::fromLatin1(id.data(), static_cast<qsizetype>(id.size()));
        }
    }
    if (state.buffer_capacity_ms > 0 && profile != selected_buffer_profile_) {
        selected_buffer_profile_ = profile;
        QSettings settings;
        settings.setValue(QString::fromLatin1(buffer_profile_settings_key), profile);
        settings.setValue(QString::fromLatin1(buffer_capacity_settings_key),
                          static_cast<int>(state.buffer_capacity_ms));
        settings.setValue(QString::fromLatin1(buffer_threshold_settings_key),
                          static_cast<int>(state.buffer_start_threshold_ms));
        refreshPlaybackBufferChecks();
    }

    device_button_->setEnabled(true);
    // Where the sound goes, on the button itself: music coming out of another
    // room is not something to have to hover to find out. Named when there is
    // a choice to have made -- an agent, or a device other than the default.
    const bool agents = std::ranges::any_of(output_choices_, [](const auto& output) {
        return !output.local;
    });
    const auto engine_name = remote && remote_catalogue_source_ ? remote_catalogue_source_->name()
                                                                : QStringLiteral("This computer");
    const auto speaker = now ? outputLabel(*now) : engine_name;
    const auto device = selected_device_ ? label_of(*selected_device_) : QString{};
    QString shown;
    if (agents) {
        shown = device.isEmpty() ? speaker : QStringLiteral("%1 · %2").arg(speaker, device);
    } else if (!device.isEmpty()) {
        shown = device;
    }
    if (now && !now->online) {
        shown += QStringLiteral(" (offline)");
    }
    if (shown.isEmpty()) {
        device_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        device_button_->setText({});
        device_button_->setFixedSize(26, 26);
    } else {
        device_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        device_button_->setText(
            device_button_->fontMetrics().elidedText(shown, Qt::ElideMiddle, 280));
        device_button_->setMinimumSize(26, 26);
        device_button_->setMaximumSize(QWIDGETSIZE_MAX, 26);
    }
    device_label = QStringLiteral("%1 → %2 → %3%4")
                       .arg(engine_name, speaker, device_label,
                            now && !now->online ? QStringLiteral(" (offline)") : QString{});
    setProperty("trackknife-player-output",
                now ? QString::fromStdString(now->id) : QString{});
    auto tooltip =
        QStringLiteral("Audio output: %1\nBuffer: %2 · %3 ms capacity · %4 ms start%5\n"
                       "Underruns: %6")
            .arg(device_label)
            .arg(bufferProfileLabel(selected_buffer_profile_))
            .arg(state.buffer_capacity_ms)
            .arg(state.buffer_start_threshold_ms)
            .arg(state.buffer_pending ? QStringLiteral(" · applies next track") : QString{})
            .arg(state.underruns);
    if (!state.output_available) {
        tooltip += QStringLiteral("\nPlayback is paused until an output is available");
    } else if (state.output_suspended) {
        tooltip += QStringLiteral("\nReconnecting the audio output");
    }
    device_button_->setToolTip(tooltip);
    device_button_->setAccessibleDescription(device_label);

    // Observable for offscreen tests and diagnostics.
    setProperty("trackknife-player-output-available", state.output_available);
    setProperty("trackknife-player-output-suspended", state.output_suspended);
    setProperty("trackknife-player-default-output",
                state.default_output ? displayText(*state.default_output) : QString{});
    setProperty("trackknife-player-buffer-capacity-ms",
                static_cast<qlonglong>(state.buffer_capacity_ms));
    setProperty("trackknife-player-buffer-pending", state.buffer_pending);
    setProperty("trackknife-player-underruns", static_cast<qulonglong>(state.underruns));
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
    if (playingOnEngine()) {
        // What the desktop sees is what the engine is doing. Reading the
        // local player here would publish an idle player while music plays,
        // so media keys and the notification would describe nothing.
        const auto engine = transport_->state();
        state.status = engine.status == QStringLiteral("playing")  ? QStringLiteral("Playing")
                       : engine.status == QStringLiteral("paused") ? QStringLiteral("Paused")
                                                                   : QStringLiteral("Stopped");
        if (!engine.entry.isEmpty()) {
            // The entry, not the path: the same file queued twice is two
            // tracks to the desktop, and a notification per occurrence.
            state.track_key = engine.entry;
            state.title = QFileInfo{engine.path}.fileName();
            if (const auto entry = core::StableId::parse(engine.entry.toStdString())) {
                if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
                    if (const auto row = tab->model->rowOfEntry(*entry, playback_.row); row >= 0) {
                        const auto& track = tab->model->rows()[static_cast<std::size_t>(row)];
                        if (!track.title.empty()) {
                            state.title = displayText(track.title);
                        }
                        state.artist = displayText(track.artist);
                        state.album = displayText(track.album);
                    }
                }
            }
        }
        state.position_us = engine.position_ms * 1'000;
        state.length_us = engine.duration_ms > 0 ? engine.duration_ms * 1'000 : -1;
        state.volume_percent = engine.volume_percent;
        const bool has_queue = engine.queue_size > 0U;
        state.can_next = engine.queue_size > 1U || engine.requests > 0U;
        state.can_previous = engine.queue_size > 1U;
        state.can_play = has_queue;
        state.can_pause = has_queue;
        state.can_seek = !engine.entry.isEmpty() && engine.duration_ms > 0;
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
