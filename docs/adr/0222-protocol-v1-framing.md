# ADR-0222: Protocol v1 framing and envelope

Status: Accepted

## Decision

One JSON object per line over a stream socket. UTF-8, `\n` terminated, no
embedded newlines. `nc` and a shell script stay debugging tools, which is a
requirement rather than a nicety: a protocol you cannot read by hand is one you
debug by guessing.

Four message shapes, distinguished by which keys are present:

```json
{"id": 7, "method": "catalogue.filter", "params": {"query": "genre HAS jazz"}}
{"id": 7, "result": {"paths": ["..."]}}
{"id": 7, "error": {"code": "not_found", "message": "…", "context": {}}}
{"event": "playback.changed", "data": {"state": "playing", "entry": "…"}}
```

- **Request** — has `id` and `method`. `params` optional.
- **Response** — has `id` and exactly one of `result` or `error`.
- **Event** — has `event`, never `id`. Server to client only, unsolicited.
- **Notification** — a request with no `id`: fire and forget, no response ever.

`id` is a client-chosen positive integer, unique among that client's
outstanding requests. The engine echoes it and never invents one.

## Why line-delimited rather than length-prefixed

Length prefixing is the usual answer and it is rejected here. The control path
carries no binary, so framing on `\n` costs nothing, and it is the difference
between `nc` showing you the conversation and showing you a hexdump. Binary
lives on a separate channel (below), which is what makes this affordable.

The constraint it imposes: no message may contain a raw newline. JSON escapes
them in strings, so this only binds encoders, which must not pretty-print.

## Multiplexing and ordering

Ids make responses independent of arrival order, so **nothing may be inferred
from ordering** except within a single `id`. Events in particular are not
ordered against responses: a `playback.changed` may arrive before the response
to the request that caused it. Clients reconcile on state, not sequence.

**What the protocol permits and what the first server does differ, and the
difference is deliberate.** The envelope allows an engine to answer a cheap
request while an expensive one is still running. The server here does not: it
dispatches on the connection's own thread, so requests on one connection are
served in order and a slow handler delays the next one behind it.

That is a constraint on *handlers*, not a gap in the framing. Long work is a
job, which answers immediately and reports through events, so a correct handler
is always fast — and ADR-0219's starvation cannot happen because the scan never
occupied a request slot to begin with. A handler slow enough to be noticed is a
handler that should have been a job.

A client that genuinely needs concurrent calls opens a second connection, which
is served by its own thread. Both behaviours are covered by
`tests/protocol-roundtrip`, including the timing that distinguishes them.

If a future engine wants true per-request concurrency it can dispatch onto a
pool without any protocol change, because the ids are already there. Nothing a
client can observe today would become wrong.

## Jobs

Long operations — scan, ReplayGain, convert, metadata apply — are submitted like
any request and answer immediately with a job identity:

```json
{"id": 12, "method": "job.submit", "params": {"job": "catalogue.scan"}}
{"id": 12, "result": {"job_id": "…"}}
{"event": "job.progress", "data": {"job_id": "…", "visited": 120, "indexed": 118}}
{"event": "job.finished", "data": {"job_id": "…", "outcome": {…}}}
```

Progress is an event stream, not a response, so a job occupies no request slot
while it runs. `job.finished` carries the result document; there is no separate
"collect" step to forget.

Cancellation is a request naming the job, not a transport-level abort:

```json
{"id": 13, "method": "job.cancel", "params": {"job_id": "…"}}
```

It answers immediately with acknowledgement. The job's own `job.finished`
reports whether it stopped early — cancelling is a request, not a guarantee, and
a job that completes before the cancel arrives completes.

## Binary side-channel

Artwork, waveform peaks and audio never travel as base64 in the control path.
A request returns a handle; the bytes are fetched on a separate connection
naming that handle. This keeps the control path readable, keeps a 10 MB cover
from delaying a transport command, and lets byte transfers be range-requested
and cancelled independently.

The side-channel's own framing is out of scope here and belongs with Phase 3,
where remote byte access is actually built.

## Errors

`error.code` is a stable machine-readable string from a closed set, mapped from
`core::ErrorCode`. `error.message` is human text and may change freely —
clients must not parse it. `error.context` is an optional string map carrying
what the code alone cannot say, such as which path failed.

A malformed line is answered with an error carrying the `id` when one could be
parsed, and closed with an error event when it could not. An unparseable line
never silently disappears.

## What this does not carry

**Connection qualifiers on entry references.** An earlier note in
`docs/unified-engine.md` called for entry references to be "addressable with a
connection". That was wrong about where the requirement lives. The connection
*is* the socket: `entry 47` on the socket to engine A is unambiguous, and stays
so even if an engine is ever a client of another engine, because that too is a
socket with its own reference space. The qualifier is a **client-side**
bookkeeping concern — a client holding several connections must track which
engine a tab's entries belong to — and nothing on the wire needs it.

**A version negotiation handshake.** Protocol v1 is v1. A version field on
every message pays a cost on every message for something that changes once; the
first incompatible change gets a handshake, designed when there is a second
version to negotiate against and the shape of the incompatibility is known.

## Verification

One schema artifact in the repository, plus a golden corpus of encoded messages
with their expected parse, in the established shape of
`tests/titleformat/tkfmt-corpus/` — each case recording its input, expected
output and rationale. Both the C++ and Go implementations run the same corpus,
which is what stops them drifting.

Specific cases the corpus must carry:

- Every message shape, including a notification and an error with context.
- A response arriving for a request other than the most recent, proving out of
  order answering.
- An event interleaved between a request and its response.
- A rejected message containing a raw newline.
- Unicode and invalid-UTF-8 raw paths, which are bytes and not text.
- A cancel answered after its job already finished.

Plus a fault-injection test, from ADR-0220: submit a long job and a burst of
control calls concurrently, and assert the control calls are neither delayed nor
dropped. That is the ADR-0219 scenario, and it is the one this framing exists to
make impossible.
