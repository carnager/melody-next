# ADR-0302: Listening together

## Status

Accepted, 2026-10-20. Phase 1 -- everyone plays their own copy -- is built
with this record; phase 2 -- the host's copy for those without one -- and
phase 3 -- a web page for friends without Melody, with the room's chat --
after it. Phase 4 -- party mode, suggestions and votes -- and the phone are
its direction.

## Context

- **Friends want to hear the same album at the same moment**, each at home,
  talking about it as it plays. It is for a few friends, never a public
  service: a room is reached only by its invite.
- **Most often only one of them has the album.** Sending it from the host to
  each listener costs the host's upload once for every listener, which
  breaks down past one or two over a home connection.
- **The engine plays; the window may be closed** (ADR-0220). Following
  someone else's playback is playback, so it is the engine's to do, through
  whichever speakers and agents it has.
- **Nothing in the engine starts at a wall-clock time** or knows another
  machine's clock, and its Opus transcodes (ADR-0231) are whole files, one a
  track, cached -- not a live stream.

## Decision

1. **A small server, `melody-together`, in this repository** -- in Go, the
   one Go program here, because it is copied to a server as a single
   static binary and nothing else -- run by one of the friends -- behind the web proxy that already gives the engine its TLS
   (ADR-0271); it speaks plain HTTP and has no TLS of its own. It holds
   rooms: each the host's playback state, the listeners present, and an
   invite. Making a room takes the server's password; joining takes the
   room's invite; the host alone changes what plays.
2. **One timeline, on the server's clock.** Each engine measures its clock
   against the server's (`GET /time`, the best of several round trips). The
   host's state says which track, from which moment of the server's clock it
   counts, and whether it is paused. A listener's engine plays the track at
   the position that moment implies, and seeks back to it when it drifts by
   more than a quarter of a second.
3. **The session is the engine's**: `together.host`, `together.join`,
   `together.leave` and `together.state`, and an event, `together.changed`.
   The host's engine says every change of what plays -- a track, a seek, a
   pause -- to the server; a listener's engine waits for the next (a long
   poll, plain HTTP through any proxy) and follows it. While listening, the
   listener's queue is the session's.
4. **Phase 1: each plays their own copy.** A listener's engine finds the
   track in its library by MusicBrainz track id, else by artist, title and
   album; one not found is said, and the listener waits silently for the
   next.
5. **Phase 2: the host's copy, sent once.** While anyone listens, the
   host's engine uploads its Opus transcode (160 kbit/s) of what plays and of
   what follows to the room -- once, however many listen; the state names
   what follows, and the room's answers list the copies it has. A listener
   without the track downloads it -- what follows ahead -- and plays it where
   the room is; its own copy, when it has one, still comes first. The server
   keeps a track an hour on disk, within a size for all, and none past its
   room. The host's upload is one copy of
   each track: a hundred kilobits a second, for any number of friends.
6. **Phase 3: a web page, for friends without Melody.** The invite opens
   it, served by melody-together itself: asked a name, it listens in the
   browser -- the server's clock measured as an engine does, the host's
   Opus copy played where the room is, what follows fetched ahead, drift
   seeked back past 300 ms -- or opens Melody. Melody's blackbird is its
   centre, bobbing while music plays, the track's progress a ring around
   it; who listens, and the room's chat, beside it. The chat is the
   server's: the last hundred lines, carried by the same long poll. It is
   the page's alone: Melody listeners chat there too, and Melody shows no
   chat of its own (2026-10-20).
7. **Phase 4: party mode.** A room the host opens as a party lets anyone
   suggest a track -- from their own library, or the host's -- and vote for
   the suggestions; the host's engine takes the most wanted into its Up Next
   as each track ends. A suggestion only its friend has is sent as phase 2
   sends the host's: once, by whoever has the file. A room not a party
   stays the host's alone.
8. **Driven by melody-cli and an official plugin.** `melody-cli together
   host|join|leave|status`; the "Listen together" plugin starts, joins and
   leaves from Workspace › Run and shows the room -- who listens, the invite
   -- in a panel.
9. **Few steps.** Hosting puts the invite on the clipboard (a trigger's
   `result` may now copy). The invite's page opens Melody by a
   `melody://together/join?invite=…` link, registered by a desktop entry of
   its own, and Melody asks its engine to join -- as the name set in the
   plugin, else the login's. A friend arriving or leaving is said. An engine
   keeps its session in its state folder and, restarted, comes back into the
   room; only leaving, or the room gone, forgets it.

## Consequences

- Sharing music with friends is redistribution; rooms are private, reached
  by invite, and no server is run for anyone else.
- A listener hears the host's track a moment after the host, the time the
  change takes to arrive and the track to start; the drift correction keeps
  them together after that.

## Verification

- `together` (`go test` in `together/`): rooms, passwords and invites;
  state kept and long-polled; listeners gone and rooms idle swept; the chat;
  the page and the blackbird served.
- By hand, in headless Chromium against a local server: the page joins,
  plays within 60 ms of the room, follows a seek and a pause, and chats
  both ways.
- `engine-together`: a host and a listener engine, each with the album: the
  listener plays the host's track at the host's position; a seek, a drift
  and a pause followed; a track it lacks played from the host's copy, where
  the host is.
- `together`: the host's copies kept, fetched by invite -- a header or the
  address -- in ranges, listed with the state, gone after an hour, over the
  size and with their room.
