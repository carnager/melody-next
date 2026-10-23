# ADR-0275: Now Playing on macOS

## Status

Accepted, 2026-10-10. Extends ADR-0135 (MPRIS) and ADR-0144 (notifications)
to macOS; builds on ADR-0255 (CoreAudio on macOS).

## Context

On Linux the desktop sees and steers playback through MPRIS: media keys,
the panel's player and the lock screen. On macOS Trackknife had none of
that. The keyboard's play key, AirPods and other Bluetooth headsets, Control
Center and the lock screen all speak to one thing there -- the system's
*Now Playing* -- and an app takes part by publishing what plays to
`MPNowPlayingInfoCenter` and taking commands from `MPRemoteCommandCenter`.
Notifications are a separate matter on macOS, posted through
`UNUserNotificationCenter`, and do not give any control.

## Decision

- **One session, two platforms.** A `DesktopMediaSession` holds the
  now-playing snapshot and the desktop's commands as signals. `MprisService`
  is the Linux one, unchanged; `NowPlayingSession` is the macOS one. The
  window binds the signals once, to the transport actions the visible
  controls use, so neither can steer past them.
- **Now Playing** shows the title, artist, album, length, position and cover,
  and whether it plays. The system carries the position on by itself, so the
  session writes only when something besides the position changes, or the
  position jumps: a seek. Play, pause, play/pause, stop, next, previous,
  seeking from Control Center, repeat and shuffle come back as commands, each
  enabled as the transport allows it.
- **Notifications** on macOS go through the notification center with the
  same rules as on Linux (ADR-0144): opt-in, track changes only, optionally
  only in the background, one notification replacing the last. The text is
  plain, not markup. The cover is attached as a copy, because the
  notification center takes the file it is given.
- **Only for the app bundle.** The notification center serves an app with a
  bundle identifier and ends a process without one that asks it. Run as a
  bare binary from the build tree, Trackknife reports that macOS shows
  notifications only for `Trackknife.app`, and Now Playing still works.

## Consequences

- Media keys, AirPods and Control Center play, pause and skip on whichever
  engine plays, local or remote, like MPRIS on Linux.
- The macOS build gains Objective-C++ (two `.mm` files, ARC) and links
  AppKit, MediaPlayer and UserNotifications.
- Notifications need the build to produce `Trackknife.app`. **Unknown**:
  whether the build should make the bundle itself or packaging should; until
  one does, notifications on macOS report why they are not shown.
- The macOS code cannot be built on the Linux machine that runs the tests;
  it is checked by building and using it on a Mac.

## Callback lifetime correction, 2026-10-23

The six transport callbacks were blocks created inside a temporary C++
lambda. Reading its captured session guard captured the lambda's `this`,
which was gone when a media key or headset command arrived. Four macOS
crash reports show the fault in that block's `QPointer::isNull()` call.
Transport blocks now copy function parameters directly, keeping the guard
and signal alive until the system releases the block.

The macOS-only `now-playing-macos` test intercepts registration at the public
`MPRemoteCommand` methods and invokes the actual escaping blocks after
construction returns. It checks repeated play/pause, play, pause, stop,
next and previous, the seek/repeat/shuffle payloads, unregistration, and
callbacks retained past session destruction. It failed before the correction
and passes with it.

**Melody decision:** headset pause commands remain explicit pauses, separate
from play/pause toggles. AirPods removal uses the system's automatic ear/head
detection and Now Playing command delivery; Melody does not poll private
Bluetooth sensor properties. Automatic Ear Detection (AirPods) or Automatic
Head Detection (AirPods Max) must be enabled in the Mac's AirPods settings.
See [Apple's playback guide](https://support.apple.com/guide/airpods/dev216d7c746/web).
**Unknown:** physical removal and replacement on each headset/Mac combination
still need a hardware check; the callback test verifies handling of delivered
pause commands, not sensor detection or the system's choice of active player.
Disconnection is separate: the engine already pauses on output-device loss
(ADR-0255).
