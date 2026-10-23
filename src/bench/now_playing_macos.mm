// SPDX-License-Identifier: GPL-3.0-only

#include "bench/now_playing_macos.hpp"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>

#include <QPointer>

#include <cstdlib>
#include <utility>
#include <vector>

namespace trackknife::bench {

namespace {

[[nodiscard]] NSString* ns(const QString& text) { return text.toNSString(); }

[[nodiscard]] MPNowPlayingPlaybackState playback_state(const QString& status) {
    if (status == QStringLiteral("Playing")) {
        return MPNowPlayingPlaybackStatePlaying;
    }
    if (status == QStringLiteral("Paused")) {
        return MPNowPlayingPlaybackStatePaused;
    }
    return MPNowPlayingPlaybackStateStopped;
}

[[nodiscard]] MPRepeatType repeat_type(const QString& loop_status) {
    if (loop_status == QStringLiteral("Track")) {
        return MPRepeatTypeOne;
    }
    if (loop_status == QStringLiteral("Playlist")) {
        return MPRepeatTypeAll;
    }
    return MPRepeatTypeOff;
}

[[nodiscard]] QString loop_status(const MPRepeatType type) {
    switch (type) {
    case MPRepeatTypeOne:
        return QStringLiteral("Track");
    case MPRepeatTypeAll:
        return QStringLiteral("Playlist");
    case MPRepeatTypeOff:
        break;
    }
    return QStringLiteral("None");
}

} // namespace

// The handler tokens, so the commands stop reaching a session that is gone.
struct NowPlayingSession::Commands {
    std::vector<std::pair<MPRemoteCommand*, id>> targets;

    void add(MPRemoteCommand* command, MPRemoteCommandHandlerStatus (^handler)(
                                           MPRemoteCommandEvent*)) {
        targets.emplace_back(command, [command addTargetWithHandler:handler]);
    }

    void addSimple(MPRemoteCommand* command, QPointer<NowPlayingSession> self,
                   void (NowPlayingSession::*signal)()) {
        // A block nested in a C++ lambda captures that lambda's `this` when
        // it reads a lambda capture, not the captured QPointer itself. That
        // lambda died at the end of session setup. Function parameters here
        // are copied into the escaping block, including the lifetime guard.
        add(command, ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
          (void)event;
          if (self.isNull()) {
              return MPRemoteCommandHandlerStatusCommandFailed;
          }
          emit(self.data()->*signal)();
          return MPRemoteCommandHandlerStatusSuccess;
        });
    }

    ~Commands() {
        for (auto& [command, target] : targets) {
            [command removeTarget:target];
        }
    }
};

NowPlayingSession::NowPlayingSession(QObject* parent)
    : DesktopMediaSession(parent), commands_(std::make_unique<Commands>()) {
    // Handlers run on the main queue, which is the Qt event loop's thread;
    // the guard drops a command that arrives as the window closes.
    QPointer<NowPlayingSession> self{this};
    auto* center = [MPRemoteCommandCenter sharedCommandCenter];
    commands_->addSimple(center.togglePlayPauseCommand, self,
                         &NowPlayingSession::playPauseRequested);
    commands_->addSimple(center.playCommand, self, &NowPlayingSession::playRequested);
    commands_->addSimple(center.pauseCommand, self, &NowPlayingSession::pauseRequested);
    commands_->addSimple(center.stopCommand, self, &NowPlayingSession::stopRequested);
    commands_->addSimple(center.nextTrackCommand, self, &NowPlayingSession::nextRequested);
    commands_->addSimple(center.previousTrackCommand, self, &NowPlayingSession::previousRequested);
    commands_->add(center.changePlaybackPositionCommand,
                   ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
                     if (self.isNull()) {
                         return MPRemoteCommandHandlerStatusCommandFailed;
                     }
                     auto* change = static_cast<MPChangePlaybackPositionCommandEvent*>(event);
                     emit self->positionRequested(
                         static_cast<qlonglong>(change.positionTime * 1'000.0));
                     return MPRemoteCommandHandlerStatusSuccess;
                   });
    commands_->add(center.changeRepeatModeCommand,
                   ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
                     if (self.isNull()) {
                         return MPRemoteCommandHandlerStatusCommandFailed;
                     }
                     auto* change = static_cast<MPChangeRepeatModeCommandEvent*>(event);
                     emit self->loopStatusRequested(loop_status(change.repeatType));
                     return MPRemoteCommandHandlerStatusSuccess;
                   });
    commands_->add(center.changeShuffleModeCommand,
                   ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
                     if (self.isNull()) {
                         return MPRemoteCommandHandlerStatusCommandFailed;
                     }
                     auto* change = static_cast<MPChangeShuffleModeCommandEvent*>(event);
                     emit self->shuffleRequested(change.shuffleType != MPShuffleTypeOff);
                     return MPRemoteCommandHandlerStatusSuccess;
                   });
}

NowPlayingSession::~NowPlayingSession() {
    commands_.reset();
    if (published_) {
        auto* info = [MPNowPlayingInfoCenter defaultCenter];
        info.nowPlayingInfo = nil;
        info.playbackState = MPNowPlayingPlaybackStateStopped;
    }
}

void NowPlayingSession::publish(const MprisPlaybackState& state) {
    const auto previous = state_;
    state_ = state;

    // The system moves the position on by itself from the elapsed time and
    // the rate, so the 30 Hz refresh writes nothing unless something other
    // than the position changed, or the position left where the last write
    // would have carried it: a seek.
    const auto carried = written_position_us_ + (previous.status == QStringLiteral("Playing")
                                                     ? written_clock_.elapsed() * 1'000
                                                     : 0);
    const bool seeked = std::llabs(state.position_us - carried) > 1'500'000;
    if (published_ && state.sameExceptPosition(previous) && !seeked) {
        return;
    }
    written_position_us_ = state.position_us;
    written_clock_.start();
    published_ = true;

    auto* center = [MPRemoteCommandCenter sharedCommandCenter];
    center.togglePlayPauseCommand.enabled = state.can_play || state.can_pause;
    center.playCommand.enabled = state.can_play;
    center.pauseCommand.enabled = state.can_pause;
    center.stopCommand.enabled = state.can_play;
    center.nextTrackCommand.enabled = state.can_next;
    center.previousTrackCommand.enabled = state.can_previous;
    center.changePlaybackPositionCommand.enabled = state.can_seek;
    center.changeRepeatModeCommand.currentRepeatType = repeat_type(state.loop_status);
    center.changeShuffleModeCommand.currentShuffleType =
        state.shuffle ? MPShuffleTypeItems : MPShuffleTypeOff;

    auto* info = [MPNowPlayingInfoCenter defaultCenter];
    if (state.track_key.isEmpty()) {
        info.nowPlayingInfo = nil;
        info.playbackState = MPNowPlayingPlaybackStateStopped;
        return;
    }
    NSMutableDictionary* now = [NSMutableDictionary dictionary];
    now[MPMediaItemPropertyTitle] = ns(state.title);
    if (!state.artist.isEmpty()) {
        now[MPMediaItemPropertyArtist] = ns(state.artist);
    }
    if (!state.album.isEmpty()) {
        now[MPMediaItemPropertyAlbumTitle] = ns(state.album);
    }
    if (state.length_us >= 0) {
        now[MPMediaItemPropertyPlaybackDuration] = @(static_cast<double>(state.length_us) / 1e6);
    }
    now[MPNowPlayingInfoPropertyElapsedPlaybackTime] =
        @(static_cast<double>(state.position_us) / 1e6);
    now[MPNowPlayingInfoPropertyPlaybackRate] = @(state.status == QStringLiteral("Playing") ? 1.0
                                                                                           : 0.0);
    now[MPNowPlayingInfoPropertyMediaType] = @(MPNowPlayingInfoMediaTypeAudio);
    if (!state.art_path.isEmpty()) {
        NSImage* image = [[NSImage alloc] initWithContentsOfFile:ns(state.art_path)];
        if (image != nil) {
            now[MPMediaItemPropertyArtwork] =
                [[MPMediaItemArtwork alloc] initWithBoundsSize:image.size
                                                requestHandler:^NSImage*(CGSize size) {
                                                  (void)size;
                                                  return image;
                                                }];
        }
    }
    info.nowPlayingInfo = now;
    info.playbackState = playback_state(state.status);
}

} // namespace trackknife::bench
