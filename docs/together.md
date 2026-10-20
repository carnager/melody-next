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
due. A web page for friends without Melody, and a party mode with
suggestions and votes, come next.

## The server

`melody-together` is a small program one of the friends runs, on a machine
everyone can reach, behind a web proxy that gives it a name and TLS. It is Go,
one static binary with nothing else to install:

```sh
cd together && CGO_ENABLED=0 go build -o melody-together .
scp melody-together server:/usr/local/bin/
```

On the server, a password -- whoever hosts needs it; listeners need only an
invite -- and the program:

```sh
openssl rand -hex 16 > /etc/melody-together/password
melody-together -password-file /etc/melody-together/password
# listens on 127.0.0.1:6099; -listen HOST:PORT for elsewhere
# keeps the hosts' copies in $TMPDIR/melody-together, an hour each, 2 GB at
# most: -files DIR and -files-mb MB for elsewhere and more
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

A room no one has touched for twelve hours is closed. The server keeps no
rooms on disk -- a restart ends them -- and the copies of tracks only an
hour, and none past their room.

## Hosting and joining

In Melody, enable **Listen together** under Settings → Plugins, and once, in
its **Settings…**, set the server (`https://together.example.org`) and its
password.

- **To host:** Workspace › Run › **Host a listening room**. The invite is on
  your clipboard -- paste it to your friends -- and in the **Listening
  together** panel, with who listens. A friend arriving or leaving is said.
- **To join:** open the invite you were sent. Its page has **Open in
  Melody**: Melody joins the room, as your login name or the name set in
  the plugin's settings. (Or paste the invite into the plugin's settings and
  Workspace › Run › **Join the listening room**.)

**Leave the listening room** leaves -- and, as its host, closes the room.
An engine that restarts -- an update -- comes back into its room by itself.

From a terminal, the same:

```sh
MELODY_TOGETHER_PASSWORD=… melody-cli together host https://together.example.org
melody-cli together join 'https://together.example.org/rooms/ROOM#KEY' Ana
melody-cli together status
melody-cli together leave
```

The session is the engine's: it goes on with Melody's window closed, and
plays through whatever speakers the engine plays on.

## How a listener finds the track

By its MusicBrainz track id, when both copies are tagged with one; else by
title and artist, and album where it is there. Not found, the host's copy is
fetched; the panel says so while it comes.
