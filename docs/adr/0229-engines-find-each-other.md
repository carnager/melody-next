# ADR-0229: Engines find each other and play for each other

- Status: accepted
- Date: 2026-09-24
- Extends: ADR-0223 (TCP), ADR-0228 (output agents)

## Context

After ADR-0228, playing on another machine meant installing `melody-agent`
there and giving it the engine's address. A machine that already ran an
engine, a desktop with Trackknife for example, needed a second process just
to lend out its speakers. Every client and agent had to be told every
address, and none of it survived a DHCP lease changing.

## Decision

**Engines are on the network by default.** `melodyd` listens on
0.0.0.0:6603 and serves streams on 6604 unless told `--local-only`.
Trackknife passes `--local-only` for its own engine unless **Share on the
network** is ticked. If the default ports are taken the engine falls back
to local only and says so, rather than failing to start.

**They announce themselves.** Every engine listening on the network
announces `_melody._tcp` over multicast DNS, with its id, protocol version,
whether it wants a password and its stream port in the TXT record. The
responder and browser are built in (`src/discovery`) rather than going
through Avahi, so a server or a container needs nothing extra. Each
interface is told only the address it can reach (RFC 6762 §15): announcing
every address everywhere once sent a phone to gemenon's Docker bridge.

**An engine can be a speaker for the others.** `melodyd --agent` plays for
every other engine it finds, through the same agent code as `melody-agent`,
built in. `--play-for HOST:PORT` does it for one engine by address, for
when discovery can't reach. `melody-agent` without `--server` behaves like
`--agent`. A machine with an engine never needs a separate agent.

**An engine is known by its id, not its address.** An engine on the same
machine is heard once per interface, from a different address each time.
Keyed by address, the built-in agent took every announcement for a new
engine, registered again and dropped the previous registration, and removing
guests that fast crashed it. Guests are keyed by the announced id, and the
browser keeps the first address it heard while the engine keeps announcing.

**One set of speakers, newest wins.** When several engines play on the same
machine's speakers, whether the machine's own engine or guests, the one that
started playing last gets them and the others pause. The paused engine
reports who took them (`output.taken_by`), and clients say so.

Tests use a private service name (`TRACKKNIFE_DISCOVERY_SERVICE`), so they
never see or disturb real engines on the network.

## Consequences

- The common setup is one flag: `melodyd --name gemenon --agent`.
- An engine on a shared network is open to anyone on it until it has a
  password. That was already true of `--listen`; now it's the default, and
  the help says so.
- Multicast DNS doesn't cross a Docker bridge, so the container runs with
  host networking. Connecting by address still works everywhere.
- A second engine started on a machine for testing is found by every agent
  on the network. That's how the address-keying bug was noticed; tests
  should use a private service name.
