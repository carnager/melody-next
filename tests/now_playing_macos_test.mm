// SPDX-License-Identifier: GPL-3.0-only

#include "bench/now_playing_macos.hpp"

#import <MediaPlayer/MediaPlayer.h>
#import <objc/runtime.h>

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

using Handler = MPRemoteCommandHandlerStatus (^)(MPRemoteCommandEvent*);
NSMutableArray* registrations;
unsigned removed = 0;

// Capture the actual escaping blocks handed to Apple, without sending media
// commands to another running player or depending on private dispatch APIs.
id add_target(id command, SEL, Handler handler) {
    auto* token = [NSObject new];
    [registrations
        addObject:@{@"command" : command, @"handler" : [handler copy], @"token" : token}];
    return token;
}

void remove_target(id command, SEL, id token) {
    for (NSDictionary* entry in registrations) {
        if (entry[@"command"] == command && entry[@"token"] == token) {
            ++removed;
            return;
        }
    }
    std::abort();
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

Handler handler_for(MPRemoteCommand* command) {
    for (NSDictionary* entry in registrations) {
        if (entry[@"command"] == command) {
            return entry[@"handler"];
        }
    }
    std::abort();
}

// Keep construction in a separate frame: invoking after this returns exposed
// the dangling C++ lambda capture in the six transport command blocks.
[[gnu::noinline]] std::unique_ptr<trackknife::bench::NowPlayingSession> make_session() {
    return std::make_unique<trackknife::bench::NowPlayingSession>();
}

} // namespace

// The public event subclasses have no public payload initializers. Supply
// their selectors without relying on MediaPlayer's private initializers.
@interface MelodyTestRemoteEvent : NSObject
@property(nonatomic) NSTimeInterval positionTime;
@property(nonatomic) MPRepeatType repeatType;
@property(nonatomic) MPShuffleType shuffleType;
@end
@implementation MelodyTestRemoteEvent
@end

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    @autoreleasepool {
        registrations = [NSMutableArray new];
        auto add =
            class_getInstanceMethod([MPRemoteCommand class], @selector(addTargetWithHandler:));
        auto remove = class_getInstanceMethod([MPRemoteCommand class], @selector(removeTarget:));
        auto old_add = method_setImplementation(add, reinterpret_cast<IMP>(add_target));
        auto old_remove = method_setImplementation(remove, reinterpret_cast<IMP>(remove_target));

        auto session = make_session();
        auto* center = [MPRemoteCommandCenter sharedCommandCenter];
        using Session = trackknife::bench::NowPlayingSession;
        struct Command {
            MPRemoteCommand* command;
            void (Session::*signal)();
        };
        const Command commands[]{
            {center.togglePlayPauseCommand, &Session::playPauseRequested},
            {center.playCommand, &Session::playRequested},
            {center.pauseCommand, &Session::pauseRequested},
            {center.stopCommand, &Session::stopRequested},
            {center.nextTrackCommand, &Session::nextRequested},
            {center.previousTrackCommand, &Session::previousRequested},
        };
        require(registrations.count == 9, "all remote commands registered");
        for (const auto& command : commands) {
            QSignalSpy received(session.get(), command.signal);
            auto handler = handler_for(command.command);
            for (int press = 0; press < 20; ++press) {
                require(handler(nil) == MPRemoteCommandHandlerStatusSuccess, "command accepted");
            }
            require(received.count() == 20, "each media key emits its matching signal once");
        }

        QSignalSpy pause(session.get(), &Session::pauseRequested);
        QSignalSpy toggle(session.get(), &Session::playPauseRequested);
        auto pause_handler = handler_for(center.pauseCommand);
        pause_handler(nil);
        pause_handler(nil);
        require(pause.count() == 2 && toggle.count() == 0,
                "headset pause stays a pause, including repeated removal events");

        auto* payload = [MelodyTestRemoteEvent new];
        payload.positionTime = 12.5;
        payload.repeatType = MPRepeatTypeOne;
        payload.shuffleType = MPShuffleTypeItems;
        auto* event = reinterpret_cast<MPRemoteCommandEvent*>(payload);
        QSignalSpy position(session.get(), &Session::positionRequested);
        QSignalSpy repeat(session.get(), &Session::loopStatusRequested);
        QSignalSpy shuffle(session.get(), &Session::shuffleRequested);
        handler_for(center.changePlaybackPositionCommand)(event);
        handler_for(center.changeRepeatModeCommand)(event);
        handler_for(center.changeShuffleModeCommand)(event);
        require(position.count() == 1 && position.front().front().toLongLong() == 12'500,
                "seek payload preserved");
        require(repeat.count() == 1 && repeat.front().front().toString() == QStringLiteral("Track"),
                "repeat payload preserved");
        require(shuffle.count() == 1 && shuffle.front().front().toBool(),
                "shuffle payload preserved");

        session.reset();
        require(removed == 9, "destruction unregisters every handler token");
        for (NSDictionary* entry in registrations) {
            Handler handler = entry[@"handler"];
            require(handler(event) == MPRemoteCommandHandlerStatusCommandFailed,
                    "a late callback safely rejects a destroyed session");
        }
        method_setImplementation(add, old_add);
        method_setImplementation(remove, old_remove);
        registrations = nil;
    }
    return 0;
}
