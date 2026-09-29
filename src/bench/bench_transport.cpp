// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/mpris_service.hpp"
#include <QDockWidget>

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "uicommon/eliding_label.hpp"
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
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QWidgetAction>
#include <QMenuBar>
#include <QPushButton>
#include <QSet>
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

#include <cmath>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr int transport_refresh_ms = 33;
} // namespace

BenchMainWindow::BenchMainWindow(QWidget* parent) : QMainWindow(parent) {
    workspace_.setView(this);
    layout_pushes_.setMaxThreadCount(1);
    // ADR-0234: this computer's engine is always the first link, connected
    // or not; remotes follow when configured.
    engines_.push_back(std::make_unique<EngineLink>());
    engines_.front()->key = EngineKey::local();
    setWindowTitle(QStringLiteral("Trackknife"));
    resize(1100, 720);
    setAcceptDrops(true);

    // ADR-0226: this window plays nothing itself. The engine owns playback,
    // and the buffer shown here is the one it reports.
    selected_buffer_profile_ = Workspace::loadPlaybackBufferPreference().profile;

    buildWorkspace();
    buildTransport();
    buildLastFm();
    buildShortcuts();
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

void BenchMainWindow::showMessage(const QString& text, const int timeout_ms) {
    statusBar()->showMessage(text, timeout_ms);
}

void BenchMainWindow::artworkLoaded(const QString& key) {
    if (key == header_cover_wanted_) {
        refreshHeaderCover(header_cover_entry_);
    }
}

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

namespace {
constexpr int header_cover_size = 44;
} // namespace

void BenchMainWindow::setUpNextCount(const int count) {
    if (up_next_button_ == nullptr || up_next_badge_ == nullptr) {
        return;
    }
    up_next_badge_->setText(QString::number(count));
    up_next_badge_->setVisible(count > 0);
    up_next_button_->setAccessibleName(
        count > 0 ? QStringLiteral("Up Next, %1 waiting").arg(count) : QStringLiteral("Up Next"));
    up_next_button_->setToolTip(up_next_button_->accessibleName());
    up_next_button_->layout()->activate();
    up_next_button_->setFixedWidth(up_next_button_->layout()->sizeHint().width());
}

void BenchMainWindow::refreshHeaderCover(const QString& entry) {
    // By album, not by the entry: a track playing from Up Next is in no tab
    // under its own id, but its album's cover is the same wherever it is.
    header_cover_entry_ = entry;
    QImage cover;
    QString key;
    if (const auto* row = entry.isEmpty() ? nullptr : playingRow(entry)) {
        key = LocalListModel::groupKeyOf(*row);
        const auto* playing = linkOf(transport_);
        cover = coverFor(*row, playing != nullptr ? playing->key : EngineKey::local());
    }
    header_cover_wanted_ = key;
    if (cover.isNull()) {
        key.clear();
    }
    if (key == header_cover_key_ && !now_playing_cover_->pixmap().isNull()) {
        return;
    }
    header_cover_key_ = key;
    if (cover.isNull()) {
        // A quiet tile rather than a hole, so the title does not jump.
        QPixmap tile{QSize{header_cover_size, header_cover_size} * devicePixelRatioF()};
        tile.setDevicePixelRatio(devicePixelRatioF());
        tile.fill(Qt::transparent);
        QPainter painter{&tile};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const auto ground = palette().color(QPalette::Window);
        const auto ink = palette().color(QPalette::Text);
        const auto tint = [](const int a, const int b) { return (a * 90 + b * 10) / 100; };
        painter.setBrush(QColor::fromRgb(tint(ground.red(), ink.red()),
                                         tint(ground.green(), ink.green()),
                                         tint(ground.blue(), ink.blue())));
        painter.drawRoundedRect(QRectF{0, 0, header_cover_size, header_cover_size}, 3, 3);
        // A quiet note in the theme's own muted colour, not a file icon.
        auto note_font = font();
        note_font.setPointSizeF(note_font.pointSizeF() * 1.5);
        painter.setFont(note_font);
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(QRectF{0, 0, header_cover_size, header_cover_size}, Qt::AlignCenter,
                         QStringLiteral("\u266A"));
        painter.end();
        now_playing_cover_->setPixmap(tile);
        return;
    }
    now_playing_cover_->setPixmap(QPixmap::fromImage(
        cover.scaled(QSize{header_cover_size, header_cover_size} * devicePixelRatioF(),
                     Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    auto pixmap = now_playing_cover_->pixmap();
    pixmap.setDevicePixelRatio(devicePixelRatioF());
    now_playing_cover_->setPixmap(pixmap);
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
    connect(previous_action_, &QAction::triggered, &workspace_, &Workspace::previous);
    play_pause_action_ =
        new QAction(style()->standardIcon(QStyle::SP_MediaPlay), QStringLiteral("Play"), this);
    play_pause_action_->setShortcut(Qt::Key_Space);
    play_pause_action_->setShortcutContext(Qt::ApplicationShortcut);
    connect(play_pause_action_, &QAction::triggered, this, &BenchMainWindow::togglePlayPause);
    stop_action_ =
        new QAction(style()->standardIcon(QStyle::SP_MediaStop), QStringLiteral("Stop"), this);
    connect(stop_action_, &QAction::triggered, &workspace_, &Workspace::stop);
    next_action_ = new QAction(style()->standardIcon(QStyle::SP_MediaSkipForward),
                               QStringLiteral("Next"), this);
    connect(next_action_, &QAction::triggered, &workspace_, &Workspace::next);

    // One row, as players read: what is playing on the left, the controls
    // and the position in the middle, where the sound goes on the right.
    auto* header = new QWidget(bar);
    header->setObjectName(QStringLiteral("bench-player-header"));
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(10, 6, 10, 6);
    header_layout->setSpacing(10);

    now_playing_cover_ = new QLabel(header);
    now_playing_cover_->setObjectName(QStringLiteral("bench-now-playing-cover"));
    now_playing_cover_->setFixedSize(header_cover_size, header_cover_size);
    now_playing_cover_->setAlignment(Qt::AlignCenter);
    now_playing_cover_->setAccessibleName(QStringLiteral("Cover of the current album"));
    header_layout->addWidget(now_playing_cover_);

    auto* track_display = new QWidget(header);
    track_display->setObjectName(QStringLiteral("bench-track-display"));
    track_display->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    track_display->setMinimumWidth(140);
    track_display->setMaximumWidth(420);
    auto* track_display_layout = new QVBoxLayout(track_display);
    track_display_layout->setContentsMargins(0, 0, 0, 0);
    track_display_layout->setSpacing(1);
    track_display_layout->addStretch();

    now_playing_ = new ui::ElidingLabel(track_display);
    now_playing_->setObjectName(QStringLiteral("bench-now-playing"));
    now_playing_->setAccessibleName(QStringLiteral("Current artist and title"));
    now_playing_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto title_font = now_playing_->font();
    title_font.setWeight(QFont::DemiBold);
    title_font.setPointSizeF(title_font.pointSizeF() * 1.08);
    now_playing_->setFont(title_font);
    track_display_layout->addWidget(now_playing_);

    now_playing_context_ = new ui::ElidingLabel(track_display);
    now_playing_context_->setObjectName(QStringLiteral("bench-now-playing-context"));
    now_playing_context_->setAccessibleName(QStringLiteral("Current album and date"));
    now_playing_context_->setForegroundRole(QPalette::PlaceholderText);
    now_playing_context_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    track_display_layout->addWidget(now_playing_context_);
    track_display_layout->addStretch();
    header_layout->addWidget(track_display, 3);
    header_layout->addSpacing(6);

    // Stop is in the Playback menu: pause does what it did, and the row has
    // room for what is used.
    auto* transport = new QWidget(header);
    transport->setObjectName(QStringLiteral("bench-transport-buttons"));
    auto* transport_layout = new QHBoxLayout(transport);
    transport_layout->setContentsMargins(0, 0, 0, 0);
    transport_layout->setSpacing(4);
    const auto add_transport_button = [transport, transport_layout](QAction* action,
                                                                    const int size) {
        auto* button = new QToolButton(transport);
        button->setDefaultAction(action);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(size, size);
        button->setIconSize(QSize{18, 18});
        transport_layout->addWidget(button);
        return button;
    };
    add_transport_button(previous_action_, 30);
    auto* play = add_transport_button(play_pause_action_, 36);
    play->setObjectName(QStringLiteral("bench-play"));
    play->setAutoRaise(false);
    // The one control that is always the next thing to do.
    // With nothing to play, a shade off the header rather than a dark hole.
    const auto idle_play = [this] {
        const auto ground = palette().color(QPalette::Window);
        const auto ink = palette().color(QPalette::Text);
        const auto mix = [](const int a, const int b) { return (a * 88 + b * 12) / 100; };
        return QColor::fromRgb(mix(ground.red(), ink.red()), mix(ground.green(), ink.green()),
                               mix(ground.blue(), ink.blue()))
            .name();
    }();
    play->setStyleSheet(QStringLiteral("QToolButton#bench-play { border: none; border-radius: 18px;"
                                       " background: palette(highlight); }"
                                       "QToolButton#bench-play:hover { background: "
                                       "palette(highlight); border: 1px solid palette(light); }"
                                       "QToolButton#bench-play:disabled { background: %1; }")
                            .arg(idle_play));
    add_transport_button(next_action_, 30);
    header_layout->addWidget(transport);

    elapsed_ = new QLabel(QStringLiteral("0:00"), header);
    elapsed_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    elapsed_->setForegroundRole(QPalette::PlaceholderText);
    elapsed_->setFixedWidth(elapsed_->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00")));
    header_layout->addWidget(elapsed_);
    seek_ = new ui::LineSlider(header);
    seek_->setObjectName(QStringLiteral("bench-seek"));
    seek_->setAccessibleName(QStringLiteral("Playback position"));
    seek_->setMinimumWidth(120);
    seek_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(seek_, &QSlider::sliderPressed, this, [this] { seeking_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, [this] {
        seeking_ = false;
        seekToMs(seek_->value());
    });
    header_layout->addWidget(seek_, 4);
    duration_ = new QLabel(QStringLiteral("0:00"), header);
    duration_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    duration_->setForegroundRole(QPalette::PlaceholderText);
    duration_->setFixedWidth(
        duration_->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00")));
    header_layout->addWidget(duration_);

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
    volume_->setFixedWidth(90);
    volume_->setToolTip(QStringLiteral("Volume"));
    connect(volume_, &QSlider::sliderPressed, this, [this] { changing_volume_ = true; });
    connect(volume_, &QSlider::sliderReleased, this, [this] { changing_volume_ = false; });
    connect(volume_, &QSlider::valueChanged, this, [this](const int value) {
        workspace_.setVolume(value);
        refreshMuteButton();
    });
    volumeLayout->addWidget(volume_);
    header_layout->addSpacing(6);
    header_layout->addWidget(volumeBox);
    refreshMuteButton();

    // Where the sound goes and what is waiting: pills, so they read as
    // places to click rather than as more labels.
    // A shade off the header, with a soft edge -- from the palette, so a
    // light theme gets the same relationship.
    const auto shade = [this](const double amount) {
        const auto from = palette().color(QPalette::Window);
        const auto to = palette().color(QPalette::Text);
        const auto mix = [amount](const int a, const int b) {
            return static_cast<int>(std::lround(a + (b - a) * amount));
        };
        return QColor::fromRgb(mix(from.red(), to.red()), mix(from.green(), to.green()),
                               mix(from.blue(), to.blue()))
            .name();
    };
    const auto pill =
        QStringLiteral(
            "QToolButton { background: %1; border: 1px solid %2; border-radius: 13px;"
            " padding: 0 10px; }"
            "QToolButton[chevron=\"true\"] { padding-right: 24px; }"
            "QToolButton:hover { border-color: palette(highlight); }"
            "QToolButton:pressed, QToolButton:checked { background: %2; }"
            "QToolButton::menu-indicator { image: none; width: 0; }")
            .arg(shade(0.06), shade(0.16));
    auto pill_font = font();
    pill_font.setPointSizeF(pill_font.pointSizeF() * 0.92);
    device_button_ = new QToolButton(header);
    device_button_->setObjectName(QStringLiteral("bench-device"));
    // The outline symbol, as the other header icons are drawn.
    device_button_->setIcon(QIcon::fromTheme(
        QStringLiteral("audio-speakers-symbolic"),
        QIcon::fromTheme(QStringLiteral("audio-speakers"),
                         style()->standardIcon(QStyle::SP_ComputerIcon))));
    device_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    device_button_->setFixedSize(34, 26);
    device_button_->setIconSize(QSize{14, 14});
    device_button_->setFont(pill_font);
    device_button_->setPopupMode(QToolButton::InstantPopup);
    device_button_->setAccessibleName(QStringLiteral("Audio output device"));
    device_button_->setStyleSheet(pill);
    // A chevron at the end says the pill opens a choice.
    auto* device_layout = new QHBoxLayout(device_button_);
    device_layout->setContentsMargins(0, 0, 9, 0);
    device_layout->addStretch();
    device_chevron_ = new QLabel(device_button_);
    device_chevron_->setObjectName(QStringLiteral("bench-device-chevron"));
    device_chevron_->setAttribute(Qt::WA_TransparentForMouseEvents);
    device_chevron_->setPixmap(
        QIcon::fromTheme(QStringLiteral("pan-down-symbolic"),
                         QIcon::fromTheme(QStringLiteral("arrow-down"),
                                          style()->standardIcon(QStyle::SP_ArrowDown)))
            .pixmap(QSize{10, 10}, devicePixelRatioF()));
    device_chevron_->hide();
    device_layout->addWidget(device_chevron_);
    device_menu_ = new QMenu(device_button_);
    device_menu_->setObjectName(QStringLiteral("bench-device-menu"));
    device_group_ = new QActionGroup(device_menu_);
    device_group_->setExclusive(true);
    device_button_->setMenu(device_menu_);
    rebuildDeviceMenu();
    header_layout->addWidget(device_button_);

    up_next_button_ = new QToolButton(header);
    up_next_button_->setObjectName(QStringLiteral("action-up-next"));
    up_next_button_->setFixedHeight(26);
    up_next_button_->setAcceptDrops(true);
    up_next_button_->installEventFilter(this);
    up_next_button_->setStyleSheet(pill);
    up_next_button_->setFont(pill_font);
    // The count as a badge beside the words, shown only when something waits.
    auto* up_next_layout = new QHBoxLayout(up_next_button_);
    up_next_layout->setContentsMargins(11, 0, 7, 0);
    up_next_layout->setSpacing(6);
    auto* up_next_text = new QLabel(QStringLiteral("Up Next"), up_next_button_);
    up_next_text->setFont(pill_font);
    up_next_text->setAttribute(Qt::WA_TransparentForMouseEvents);
    up_next_layout->addWidget(up_next_text);
    up_next_badge_ = new QLabel(up_next_button_);
    up_next_badge_->setObjectName(QStringLiteral("bench-up-next-count"));
    up_next_badge_->setAttribute(Qt::WA_TransparentForMouseEvents);
    up_next_badge_->setAlignment(Qt::AlignCenter);
    up_next_badge_->setMinimumWidth(18);
    up_next_badge_->setFixedHeight(18);
    auto badge_font = up_next_badge_->font();
    badge_font.setPointSizeF(badge_font.pointSizeF() * 0.85);
    up_next_badge_->setFont(badge_font);
    up_next_badge_->setStyleSheet(
        QStringLiteral("QLabel { background: palette(highlight); color: palette(highlighted-text);"
                       " border-radius: 9px; padding: 0 5px; }"));
    up_next_layout->addWidget(up_next_badge_);
    connect(up_next_button_, &QToolButton::clicked, this, [this] {
        findChild<QAction*>(QStringLiteral("action-show-up-next"))->trigger();
        refreshUpNext();
    });
    setUpNextCount(0);
    header_layout->addWidget(up_next_button_);
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

void BenchMainWindow::reloadPlaybackPreferences() {
    const QSettings settings;
    workspace_.reloadPlaybackPreferences();
    if (notifier_)
        notifier_->setBackgroundOnly(
            settings.value(QStringLiteral("desktop/notifications-background-only"), false)
                .toBool());
    if (notifications_action_)
        notifications_action_->setChecked(
            settings.value(QStringLiteral("desktop/notifications"), false).toBool());
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
    // The previous rebuild's output group, whose actions clear() just took.
    for (auto* group : device_menu_->findChildren<QActionGroup*>(Qt::FindDirectChildrenOnly)) {
        if (group != device_group_) {
            group->deleteLater();
        }
    }
    device_menu_->setToolTipsVisible(true);

    device_group_->setExclusive(true);
    device_button_->setAccessibleName(QStringLiteral("Audio output device"));

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

    const auto menu = workspace_.outputMenu();
    if (menu.speakers_shown) {
        add_heading(QStringLiteral("Speakers"));
        auto* outputs = new QActionGroup(device_menu_);
        outputs->setExclusive(true);
        for (const auto& speaker : menu.speakers) {
            auto* action = device_menu_->addAction(speaker.label);
            action->setObjectName(
                QStringLiteral("action-output-%1").arg(QString::fromStdString(speaker.id)));
            action->setCheckable(true);
            action->setChecked(speaker.checked);
            if (!speaker.tooltip.isEmpty()) {
                action->setToolTip(speaker.tooltip);
            }
            outputs->addAction(action);
            connect(action, &QAction::triggered, this,
                    [this, id = speaker.id] { workspace_.selectOutput(id); });
        }
        device_menu_->addSeparator();
        add_heading(menu.devices_heading);
    }
    for (const auto& device : menu.devices) {
        auto* action = device_menu_->addAction(device.label);
        action->setCheckable(true);
        action->setChecked(device.checked);
        action->setEnabled(device.enabled);
        device_group_->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, target = device.target] { workspace_.setOutputDevice(target); });
    }
    device_menu_->addSeparator();
    auto* refresh = device_menu_->addAction(QStringLiteral("Refresh audio devices"));
    refresh->setObjectName(QStringLiteral("action-refresh-audio-devices"));
    connect(refresh, &QAction::triggered, &workspace_, &Workspace::refreshOutputs);
}

QIcon BenchMainWindow::oneShotIcon(const QIcon& plain) const {
    QIcon marked;
    for (const int extent : {16, 22, 32}) {
        QPixmap pixmap = plain.pixmap(extent, extent);
        if (pixmap.isNull()) {
            continue;
        }
        QPainter painter{&pixmap};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Highlight));
        const auto dot = static_cast<qreal>(extent) * 0.38;
        painter.drawEllipse(QRectF{extent - dot, extent - dot, dot, dot});
        marked.addPixmap(pixmap);
    }
    return marked.isNull() ? plain : marked;
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
    local_single_action_ = add_mode(QStringLiteral("single"), QStringLiteral("Single"),
                                    QStringLiteral("media-playlist-repeat-song"));
    local_album_random_action_ = add_mode(QStringLiteral("album-random"), tr("Album shuffle"),
                                          QStringLiteral("media-playlist-shuffle"));
    local_album_random_action_->setIcon(albumShuffleIcon(palette()));
    connect(local_album_random_action_, &QAction::triggered, &workspace_,
            &Workspace::setAlbumRandom);
    local_consume_action_ = add_mode(QStringLiteral("consume"), QStringLiteral("Consume"),
                                     QStringLiteral("edit-clear-list"));
    // Their plain icons, so one-shot can be drawn as a mark on them.
    for (auto* action : {local_single_action_, local_consume_action_}) {
        action->setProperty("bench-plain-icon", QVariant::fromValue(action->icon()));
    }
    connect(local_repeat_action_, &QAction::triggered, &workspace_, &Workspace::setRepeat);
    connect(local_random_action_, &QAction::triggered, &workspace_, &Workspace::setRandom);
    connect(local_single_action_, &QAction::triggered, &workspace_, &Workspace::cycleSingle);
    connect(local_consume_action_, &QAction::triggered, &workspace_, &Workspace::cycleConsume);
    local_replaygain_button_ = new QToolButton(statusBar());
    local_replaygain_button_->setObjectName(QStringLiteral("bench-local-replaygain"));
    local_replaygain_button_->setAccessibleName(QStringLiteral("ReplayGain mode"));
    local_replaygain_button_->setIcon(QIcon::fromTheme(
        QStringLiteral("view-media-equalizer"), style()->standardIcon(QStyle::SP_MediaVolume)));
    local_replaygain_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    local_replaygain_button_->setAutoRaise(true);
    local_replaygain_button_->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(QStringLiteral("ReplayGain"), local_replaygain_button_);
    menu->setObjectName(QStringLiteral("bench-local-replaygain-menu"));
    local_replaygain_group_ = new QActionGroup(menu);
    local_replaygain_group_->setExclusive(true);
    for (const auto& [label, value] : Workspace::replayGainModes()) {
        auto* action = menu->addAction(label);
        action->setObjectName(QStringLiteral("action-local-replaygain-%1").arg(value));
        action->setCheckable(true);
        action->setData(value);
        local_replaygain_group_->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, value] { workspace_.setReplayGain(value); });
    }
    menu->addSeparator();
    auto* preamp_action = menu->addAction(QStringLiteral("Preamp…"));
    preamp_action->setObjectName(QStringLiteral("action-local-replaygain-preamp"));
    connect(preamp_action, &QAction::triggered, this, &BenchMainWindow::showReplayGainPreampDialog);
    local_replaygain_button_->setMenu(menu);
    styleStatusBar();
    statusBar()->addPermanentWidget(local_replaygain_button_);
    playback_menu->addMenu(menu);
    playback_menu->addSeparator();
    applyLocalPlaybackModes();
}

void BenchMainWindow::styleStatusBar() {
    // The status bar as the header's counterpart: the same ground, a hairline
    // above, no frames around its parts, and the modes that are on tinted in
    // the accent -- so which are on can be seen, not hovered for.
    const auto ground = palette().color(QPalette::Window);
    const auto ink = palette().color(QPalette::Text);
    const auto accent = palette().color(QPalette::Highlight);
    const auto mix = [](const QColor& from, const QColor& to, const int percent) {
        const auto channel = [percent](const int a, const int b) {
            return (a * (100 - percent) + b * percent) / 100;
        };
        return QColor::fromRgb(channel(from.red(), to.red()), channel(from.green(), to.green()),
                               channel(from.blue(), to.blue()))
            .name();
    };
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setStyleSheet(
        QStringLiteral("QStatusBar { border-top: 1px solid %1; }"
                       "QStatusBar::item { border: none; }"
                       "QStatusBar QLabel { color: palette(placeholder-text); }")
            .arg(mix(ground, ink, 12)));
    const auto mode_style =
        QStringLiteral("QToolButton { border: none; border-radius: 4px; padding: 3px; }"
                       "QToolButton:hover { background: %1; }"
                       "QToolButton:checked { background: %2; }")
            .arg(mix(ground, ink, 8), mix(ground, accent, 35));
    for (auto* button : local_mode_buttons_) {
        button->setAutoRaise(false);
        button->setStyleSheet(mode_style);
    }
    auto* divider = new QFrame(statusBar());
    divider->setObjectName(QStringLiteral("bench-status-divider"));
    divider->setFixedSize(1, 16);
    divider->setStyleSheet(QStringLiteral("background: %1;").arg(mix(ground, ink, 18)));
    statusBar()->addPermanentWidget(divider);
    // ReplayGain as quiet as the modes beside it: text, no frame, a gain in
    // use told by the text's colour rather than a filled pill.
    local_replaygain_button_->setAutoRaise(false);
    local_replaygain_button_->setStyleSheet(
        QStringLiteral("QToolButton { border: none; border-radius: 4px; padding: 3px 6px;"
                       " color: palette(placeholder-text); }"
                       "QToolButton[active=\"true\"] { color: palette(text); }"
                       "QToolButton:hover { background: %1; }"
                       "QToolButton::menu-indicator { image: none; width: 0; }")
            .arg(mix(ground, ink, 8)));
    local_replaygain_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
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
    const auto texts = workspace_.modeTexts();
    local_repeat_action_->setChecked(texts.repeat.checked);
    local_random_action_->setChecked(texts.random.checked);
    local_album_random_action_->setChecked(texts.album_random.checked);
    local_album_random_action_->setToolTip(texts.album_random.tooltip);
    local_repeat_action_->setToolTip(texts.repeat.tooltip);
    local_random_action_->setToolTip(texts.random.tooltip);
    const auto cycle = [this](QAction* action, const ModeText& mode, const QString& symbol) {
        action->setChecked(mode.checked);
        action->setIconText(mode.oneshot ? symbol + QStringLiteral("×") : symbol);
        // One-shot is marked on the icon: a dot, where the letters once had
        // an "×" -- an icon cannot carry a letter.
        const auto plain = action->property("bench-plain-icon").value<QIcon>();
        action->setIcon(mode.oneshot ? oneShotIcon(plain) : plain);
        action->setText(mode.text);
        action->setToolTip(mode.tooltip);
    };
    cycle(local_single_action_, texts.single, QStringLiteral("1"));
    cycle(local_consume_action_, texts.consume, QStringLiteral("C"));
    const bool can_play = playingOnEngine();
    local_replaygain_button_->setEnabled(can_play);
    for (auto* action : local_replaygain_group_->actions()) {
        action->setEnabled(can_play);
        action->setChecked(action->data().toString() == local_replaygain_);
    }
    local_replaygain_button_->setText(texts.replaygain);
    // Off reads as quiet; a gain in use is marked like a mode that is on.
    local_replaygain_button_->setProperty("active", texts.replaygain_active);
    local_replaygain_button_->style()->unpolish(local_replaygain_button_);
    local_replaygain_button_->style()->polish(local_replaygain_button_);
    local_replaygain_button_->setToolTip(texts.replaygain_tooltip);
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
    auto* focus_search = new QAction(tr("Search the library"), this);
    bind(focus_search, QStringLiteral("action-focus-library-search"), QStringLiteral("Ctrl+L"));
    connect(focus_search, &QAction::triggered, this, &BenchMainWindow::focusLibrarySearch);

    // Every action with a key by default, and every workspace command
    // whether it has one or not: Convert, ReplayGain and the rest can be
    // given one too.
    QSet<QAction*> commands;
    for (const auto* id : workspace_command_ids) {
        if (auto* action = findChild<QAction*>(QString::fromLatin1(id))) {
            commands.insert(action);
        }
    }
    for (auto* action : findChildren<QAction*>()) {
        if (!action->objectName().startsWith(QStringLiteral("action-")) ||
            (action->shortcut().isEmpty() && !commands.contains(action)))
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
    // including a change another client made. Not while this window's own
    // commands are on their way: a report from before them would put back
    // the volume just changed -- a mute undone for a moment.
    volume_->setEnabled(true);
    if (!changing_volume_ && !transport_->settling()) {
        const QSignalBlocker blocker{volume_};
        volume_->setValue(state.volume_percent);
    }
    refreshMuteButton();

    const auto shown = workspace_.nowPlaying(state);
    now_playing_->setText(shown.title);
    setWindowTitle(shown.window_title);
    now_playing_context_->setText(shown.context);
    refreshHeaderCover(shown.cover_entry);
    now_playing_->setToolTip(shown.tooltip);
    now_playing_context_->setToolTip(shown.tooltip);
    workspace_.followEngineState(state);

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
    setWindowTitle(QStringLiteral("Trackknife"));
    now_playing_context_->clear();
    setProperty("trackknife-engine-playback", QStringLiteral("unavailable"));
    publishMprisState();
}

// The engine's sink and buffer (ADR-0226). The devices are the engine
// machine's: for an engine on a NAS they are the NAS's, which is the point.
void BenchMainWindow::refreshOutputControls(const EnginePlayback::State& state) {
    const auto outputs = workspace_.takeOutputs(state);
    if (outputs.menu_changed) {
        rebuildDeviceMenu();
    }
    device_button_->setEnabled(true);
    const auto& shown = outputs.shown;
    const bool named = !shown.isEmpty();
    if (device_button_->property("chevron").toBool() != named) {
        device_button_->setProperty("chevron", named);
        device_button_->style()->unpolish(device_button_);
        device_button_->style()->polish(device_button_);
    }
    device_chevron_->setVisible(named);
    if (!named) {
        device_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        device_button_->setText({});
        device_button_->setFixedSize(34, 26);
    } else {
        device_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        device_button_->setText(
            device_button_->fontMetrics().elidedText(shown, Qt::ElideMiddle, 280));
        device_button_->setMinimumSize(26, 26);
        device_button_->setMaximumSize(QWIDGETSIZE_MAX, 26);
    }
    setProperty("trackknife-player-output", outputs.selected_output);
    device_button_->setToolTip(outputs.tooltip);
    device_button_->setAccessibleDescription(outputs.description);

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

} // namespace trackknife::bench
