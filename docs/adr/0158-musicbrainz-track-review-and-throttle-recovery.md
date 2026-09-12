# ADR-0158: MusicBrainz track review and throttle recovery

- Status: accepted
- Date: 2026-09-13
- Supersedes: ADR-0090's immediate automatic staging after release selection
- Extends: ADR-0088, ADR-0089, ADR-0096

## Context

The user reports unreliable identification of untagged albums, wants to
assign local files to release tracks explicitly, and asks about API limits.
The previous UI required an exact selected-file count in the search query and
could reject a found release when automatic matching had too little tag
evidence. Properties supplied no durations to the matcher. Finding a release
and accepting its track alignment are distinct stages.

## Decision

**Trackknife decision:** Artist/album search uses the entered text without
requiring existing tags or an exact track count. Count agreement affects
ranking only. Candidate rows describe count agreement instead of presenting
an unexplained score above 100; the search and ranking scores remain in the
tooltip. Search remains an explicit network action with bounded results.

Selecting a release opens a review page inside Identify, showing local files
and release tracks side by side, with filenames, known lengths, disc/track
positions, and current assignments. Users may:

- assign a selected pair, swapping assignments when the target is occupied;
- unmatch a file;
- sort local filenames naturally and assign by displayed file order;
- return to the release candidates without staging anything;
- explicitly stage the displayed assignments into the normal tag draft.

Automatic suggestions below the existing confidence threshold begin
unmatched. Untagged files can therefore be assigned without manufacturing
automatic confidence. Stage confirms the displayed mapping as a user
decision, with explicit user-confirmed rationale in the proposals. Unmatched
files receive no proposals. The Qt-free validator rejects duplicate release
track assignments, incorrect mapping sizes, and out-of-range targets. Local
selection identities survive sorting and reassignment. Apply remains the
separate file-writing action, and staging remains one undoable transaction.

The similarity matcher runs on a background worker and accepts cancellation;
closing or leaving its page cancels pending computation. The review page
supports at most 2,000 local files and 2,000 release tracks, showing a reason
when that bound is exceeded. Unknown durations remain visibly unknown.

The HTTP client retains serialized 1.1-second dispatch pacing and its cache.
HTTP 429/503 responses retry at most twice with increasing waits, honoring
Retry-After in seconds or HTTP-date form. The cooldown also applies to queued
requests. Server delays above 60 seconds return an actionable error instead
of holding the current operation for an automatic retry; queued network work
still respects the cooldown. Requests have a 30-second transfer timeout.
Only successful responses enter the cache. The identifying User-Agent names
Trackknife and its actual project repository; it does not impersonate Picard.

MusicBrainz's public policy normally limits source IPs to one request per
second unless otherwise agreed. Paid support is not treated as an automatic
client-side exemption. No subscription, key purchase, or account change is
part of this implementation.

## Verification

- UI regressions exercise tagged and fully untagged inputs, manually entered
  artist/album search, explicit reversed assignments, unmatching, staging,
  and the existing AcoustID route.
- Core fixtures cover partial multi-disc mapping, stable selection IDs,
  user-confirmed rationale, duplicate/out-of-range rejection, and cancellation.
- Scripted transport tests cover 429/503 retry limits, Retry-After cooldown,
  queue order, success-only caching, and long-delay failure feedback.
- A read-only live query on 2026-09-13 returned both releases of the user's
  `No Joy / Sonic Boom` example using either album title alone or artist plus
  album title. This verifies that example's searchability, not general live
  availability or a reproduction of the user's intermittent failure.

## Sources

- [MusicBrainz rate limiting](https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting)
- [MusicBrainz API](https://musicbrainz.org/doc/MusicBrainz_API)
- [User's release group](https://musicbrainz.org/release-group/00d817d1-fe4e-493b-bc8f-996b07e57b1b)
