# Listening together

Friends hear the same album at the same moment, each at home on their own
speakers ([ADR-0302](adr/0302-listening-together.md)). One of them hosts a
room; the others join it with an invite. The host's engine says what it
plays -- each track, seek and pause -- and every listener's engine plays the
same track from its own library, where the host is.

A friend who has the track plays their **own copy**. One who does not gets
the **host's**: while anyone listens, the host's engine sends its Opus copy
(160 kbit/s) of what plays, and of what follows, to the server -- once,
however many listen -- and a listener lacking it fetches it before it is
due.

A friend **without Melody** listens in the browser: the invite opens the
room's page, which asks their name and plays the host's copy, in step, with
who listens and the room's chat beside Melody's blackbird. A party mode
with suggestions and votes comes next.

## The server

`melody-together` is a small program one of the friends runs, on a machine
everyone can reach, behind a web proxy that gives it a name and TLS. It is Go,
one static binary with nothing else to install:

```sh
cd together && CGO_ENABLED=0 go build -o melody-together .
scp melody-together server:/usr/local/bin/
```

On the server, the program -- and, if only some may start rooms, a
password for that; each room is kept among friends by its own password
either way:

```sh
melody-together
# or, rooms started only with a password:
openssl rand -hex 16 > /etc/melody-together/password
melody-together -password-file /etc/melody-together/password
# listens on 127.0.0.1:6099; -listen HOST:PORT for elsewhere
# keeps its rooms, and the copies of their tracks -- an hour each, 2 GB at
# most -- in ~/.local/state/melody-together: -data DIR and -files-mb MB for
# elsewhere and more; a restart ends no room
```

A systemd unit:

```ini
[Unit]
Description=melody-together
After=network.target

[Service]
ExecStart=/usr/local/bin/melody-together -password-file /etc/melody-together/password
DynamicUser=yes
LoadCredential=password:/etc/melody-together/password
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

(With `LoadCredential`, point `-password-file` at
`${CREDENTIALS_DIRECTORY}/password` instead.)

And Caddy in front:

```
together.example.org {
    reverse_proxy 127.0.0.1:6099
}
```

A room no one has touched for twelve hours is closed. Its name is the
host's choice -- the plugin's **Room name**, or `melody-cli together host
SERVER friday-doom` -- else two words the server makes, `quiet-otter`; the
invite's key after `#` is what keeps it among friends.

## Hosting and joining

In Melody, enable **Listen together** under Settings → Plugins and, once, in
its **Settings…**, set the server (`https://together.example.org`) and your
name. Everything else is the **Listening room** panel:

- **To start a room:** its name and password -- yours to choose, or left
  empty to have them made -- then **Start room**. Send your friends the
  room's name and password, or the room's page: `…/rooms/NAME#PASSWORD`,
  which opens the room in their browser or their Melody.
- **To join:** the room's name and password, then **Join room** -- or open
  the page a friend sent.

In a room the panel is the room itself: **Stop room** or **Leave room**, the
host's **Party** toggle, the queue -- what plays and whose it is, what is
next, at a party the suggestions with **▲** to vote and, for the host,
**✕** to remove -- what played and who listens. Your tabs stay yours. An
engine that restarts comes back into its room by itself.

From a terminal, the same:

```sh
MELODY_TOGETHER_PASSWORD=… melody-cli together host https://together.example.org
melody-cli together join 'https://together.example.org/rooms/ROOM#KEY' Ana
melody-cli together status
melody-cli together leave
```

The session is the engine's: it goes on with Melody's window closed, and
plays through whatever speakers the engine plays on.

## A party

A room is a radio its host programs -- or a party, a jukebox the room
votes: the host's **Party** toggle. Then everyone in Melody, the host too,
suggests tracks of their own -- right-click them, **Suggest to the listening
room**, or `melody-cli together suggest FILE…` -- three waiting at most each,
and everyone votes: **▲** in the panel, on the page, or `melody-cli together
vote N`. As each track ends, the most wanted suggestion plays next; the
host's own queue plays on only when nothing is suggested. A suggestion only
its friend has is played from their copy, sent once.

## How a listener finds the track

By its MusicBrainz track id, when both copies are tagged with one; else by
title and artist, and album where it is there. Not found, the host's copy is
fetched; the panel says so while it comes.
