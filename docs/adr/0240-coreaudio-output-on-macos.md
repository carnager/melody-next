# ADR-0240: CoreAudio output on macOS

- Status: accepted and implemented
- Date: 2026-09-29
- Builds on: ADR-0016 (bounded local playback), ADR-0220 (engine-owned playback)

## Context

The engine's local output contract was implemented only by PipeWire. macOS
could build the engine and play through agents or UPnP renderers, but its local
output adapter was an unavailable stub. That also meant `melodyd --agent` on a
Mac could register with another engine but could not make sound.

The playback worker, decoder, PCM ring, ReplayGain stage, queue, protocol and
device-selection UI are already platform-independent. Reimplementing any of
them for macOS would create a second playback path and violate ADR-0220.

## Decision

macOS implements the existing local-output contract with the system CoreAudio
HAL output Audio Unit.

- The real-time Audio Unit callback reads the existing bounded `LocalPlayback`
  ring. It performs no allocation, locking, decoding, file I/O or protocol
  work.
- The client format is interleaved 32-bit float PCM at the source rate, mono or
  stereo. CoreAudio performs device-rate conversion.
- Device selection persists the CoreAudio device UID, not the process-local
  numeric `AudioDeviceID`. Names shown to the user come from the device object.
- The system default output and the available output-device inventory are
  polled through the existing worker-owned monitor. The engine's established
  pause-on-loss and explicit-resume-on-return behavior is unchanged.
- Volume remains per Trackknife stream. Because CoreAudio has no PipeWire-style
  session mixer, the callback multiplies the already-decoded float samples by
  the same cubic gain chosen by the playback service. It never changes the
  system or device volume.
- Drain waits for the Audio Unit latency plus two device buffers before the
  existing worker quiesces the unit. The wait is bounded by the configured
  output transition timeout.
- Exclusive device access is unsupported on macOS. The current UI does not
  request it; a future exclusive-mode design needs its own device-hogging and
  format-restoration decision.

`TRACKKNIFE_ENABLE_LOCAL_AUDIO` now means the platform local-output backend:
PipeWire on Linux and CoreAudio on macOS. It defaults on for both platforms.
The `macos` preset enables it. The generic headless `server` preset keeps it
disabled, while `macos-server` builds a headless engine/agent with CoreAudio;
the two-machine build helper uses that preset for its Mac server.

The public C++ types retain their historical `PipeWire*` names for this change.
They are internal implementation vocabulary, while protocol and product
surfaces already say output/device. Renaming them is mechanical cleanup and is
not coupled to making macOS playback work.

## Consequences

Trackknife's local engine and `melodyd --agent` can play through built-in,
USB, HDMI and other CoreAudio outputs on macOS. The existing output menu lists
those devices and follows the system default when no explicit target is saved.
ReplayGain, gapless handoff, seeks, pause/resume, buffer profiles and engine
ownership use the same implementation above the adapter on Linux and macOS.

The macOS backend depends only on the system AudioToolbox, AudioUnit, CoreAudio
and CoreFoundation frameworks. A build with local audio disabled links none of
them and retains the unavailable adapter.

The silent output integration test, local-audition lifecycle test and engine
playback test run against CoreAudio on macOS. They cover connection, device
enumeration, per-stream volume, rendering, drain, quiescence, retargeting and
engine commands without requiring audible test material.
