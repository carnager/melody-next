# ADR-0230: Opus streams and download tickets

- Status: accepted
- Date: 2026-09-24
- Extends: ADR-0228 (streams for agents without files)

## Context

The stream port served one thing: the original file, to an agent that
presented the engine's token, and only while the player held that track.
Three things didn't fit:

- A phone on mobile data should not pull a 30 MB FLAC per track.
- A phone can't decode everything the engine plays: WavPack, APE, tracker
  files, single tracks from a CUE sheet.
- Offline albums need files the engine isn't playing.

## Decision

**The stream port is HTTP and nothing else.** Which file a request gets, if
any, is the engine's decision (`MediaStreams`). It accepts two keys:

- **The agents' token**, as before: only for what the player holds.
- **A ticket**: an HMAC-SHA256 signature over the exact request and an
  expiry, with a key made when the engine starts. A client that has given
  the password asks for one with `streams.ticket` and gets a URL for one
  library track, as it is or as Opus at a bit rate, good for an hour. Paths
  outside the library get no ticket.

**Converted once, then served like a file.** A request that names a format
is answered from a transcode cache in the state directory. The whole track
is converted to Opus with the existing converter, keyed by the file's
revision, the part of it, and the bit rate. Two requests for the same track
share one conversion, and the least recently used files go when the cache
is over its size (`--transcode-cache MB`, 2 GB by default).

This rather than encoding live, because a finished file has a length:
ranges, seeking, the duration and gapless playback all behave as they do
with the original, and the same file is the offline download. Conversion
runs at about 190 times real time, so a track waits a second or two, and a
gapless next track is converted while the current one plays.

**Agents say what they can play.** At registration an agent lists the codecs
it decodes and whether it wants Opus (`stream: {format, bitrate}`). It
repeats the wish in its reports, so a phone leaving Wi-Fi changes what it
gets from the next track. The engine converts when the agent asks, when it
can't decode the codec, and always for a part of a file. A part is only
ever sent as a track of its own, so an agent never needs to know about CUE
sheets or subsongs.

Output is stereo with no gain applied. The player applies ReplayGain from
the values the engine sends with the stream, as it does for originals.

## Consequences

- The first play of a track over mobile data waits a moment for the
  conversion.
- Tickets die with the engine, and a download in progress asks for new ones.
- A ticket names a track, not a person: anyone who has the URL can fetch
  that track for an hour. It opens music, not the disk.
- Cover images went the same way: `catalogue.artwork` takes a size, and the
  engine scales covers before sending them.
