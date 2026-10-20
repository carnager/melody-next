# ADR-0302: Listening together

## Status

Accepted, 2026-10-20. Phase 1 -- everyone plays their own copy -- is built
with this record; phase 2 -- the host's copy for those without one --
phase 3 -- a web page for friends without Melody, with the room's chat --
and phase 4 -- a party: suggestions and votes -- after it. The phone is
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
   invite. A room has a name -- the host's choice, or two words made by the
   server, `quiet-otter` -- in its address, `/rooms/NAME`; the invite's key,
   after `#`, is the secret. Rooms are kept on disk with what they played,
   said and suggested, and their copies of tracks, so a restart of the
   server ends none. Making a room takes the server's password; joining takes the
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
   seeked back past 300 ms -- or opens Melody. It fills the window in three
   columns -- stacked on a phone: Melody's blackbird, bobbing while music
   plays, over what plays and its progress; the room's tracks, large --
   what plays and whose it is, what is next, at a party the suggestions
   with a vote each, what played; and who listens over the room's chat,
   the height of the window. The chat is the
   server's: the last hundred lines, carried by the same long poll. It is
   the page's alone: Melody listeners chat there too, and Melody shows no
   chat of its own (2026-10-20).
7. **Phase 4: party mode.** A room is a radio the host programs, or -- the
   host's to choose -- a party: a jukebox the room votes. At a party anyone
   suggests tracks of their library -- the host as one of the guests, under
   a name of its own -- three waiting at most each, every one's copy sent
   once by whoever suggested it, the one upload a listener may make. Anyone
   votes, in Melody or on the page; a vote given again is taken back; the
   most wanted come first, the oldest among as wanted. As each track ends --
   decided then, so the votes cast while it played count -- the host's
   engine takes the most wanted whose copy has come into Up Next: its own
   copy when it has the track, its own file when it suggested it, else its
   friend's, fetched ahead while it leads. The host's own queue plays on
   only when nothing is suggested. A suggestion's copy goes by its own name
   in the room; its friend plays the file they suggested. The host removes
   suggestions, and ending the party clears what waits.
8. **Driven by melody-cli and an official plugin.** `melody-cli together
   host|join|leave|status`; the "Listen together" plugin starts, joins and
   leaves from Workspace › Run. Its panel is the room's own queue -- what
   plays and whose it is, what is next, the suggestions with their votes,
   who listens, what played -- apart from Melody's lists, which stay this
   Melody's: a listener's or host's tabs never pretend to be the room. It is
   asked again as the room changes (a panel's `"refresh":
   ["listening-room"]`).
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
- `together`: a party -- suggestions refused elsewhere, three each, the
  listener's one upload, votes ordering them, the host taking and removing.
- `engine-together`: at a party, two tracks only the friend has, the second
  voted for: nothing taken while the host's track plays, the second taken as
  it ends, the host playing her copy and she her file; the host's own pick
  waiting for votes among hers; the next taken as a suggestion ends.
- By hand, in headless Chromium against a local server: the page joins,
  plays within 60 ms of the room, follows a seek and a pause, chats both
  ways, and votes at a party.
- `engine-together`: a host and a listener engine, each with the album: the
  listener plays the host's track at the host's position; a seek, a drift
  and a pause followed; a track it lacks played from the host's copy, where
  the host is.
- `together`: the host's copies kept, fetched by invite -- a header or the
  address -- in ranges, listed with the state, gone after an hour, over the
  size and with their room.
