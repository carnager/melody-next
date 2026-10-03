# ADR-0258: Dynamic rules pick groups, the engine selects, and some ship

Status: Accepted (2026-10-03)

Extends the dynamic playlists of ADR-0195 (library rules) and the
continuation of ADR-0253.

## Context

A library rule is a `tkq-1` query, a maximum of 1--500 tracks and an
optional shuffle. Three things are missing or wrong:

- **No way to pick by group.** "A random album", "ten random artists, three
  songs each" cannot be said: a rule finds tracks, and a shuffle picks tracks,
  so an artist with 400 tracks comes up 40 times as often as one with 10.
- **The window selects.** It reads every match from the engine and only then
  shuffles and trims; a rule matching more than 20,000 tracks is refused for
  that reason, so `ALL` -- the obvious rule for "random tracks" -- cannot run
  on a 66,000-track library. The engine's own continuation selects
  separately, its own way.
- **Nothing ships.** A new user has no rule to continue a list with until
  they write one.

## Decision

**A rule may pick groups.** Besides its query, maximum and shuffle, a rule may
name a `tkfmt-1` expression to group its matches by, how many groups to pick
and how many tracks to take from each (0: all of them):

1. The query's matches, in the library's order (artist, year, album, disc,
   track).
2. With groups: the matches grouped by the expression's text; that many groups
   chosen at random, each with the same odds whatever its size; from each,
   that many tracks at random, kept in library order -- or all of them.
   Groups follow each other in the order chosen.
3. With shuffle, the selection is shuffled; without, a rule with no groups
   keeps the library's (or its `SORT`'s) order.
4. At most the maximum.

**The engine selects.** `catalogue.select {query, limit, shuffle, group_by?,
groups?, per_group?, exclude?}` answers the chosen paths and how many tracks
the query matched. The window asks it for a rule's results and reads only
those tracks; the 20,000-match refusal goes. A list continuing with a rule is
continued from the same selection: a grouped rule appends its selection --
the next album, whole -- and an ungrouped one ten tracks, as before, not what
is queued and preferably not what played in the last seven days.

**Some rules ship.** They are always there, under fixed identities
(`shipped:…`), so a list continuing with one keeps doing so across updates.
They can be used and copied, not changed or removed:

| Rule | Query | Selection |
| --- | --- | --- |
| Random tracks | `ALL` | 50, shuffled |
| Random album | `ALL` | 1 group by `%albumartist% — %album% %date%`, all its tracks, in order |
| Random artists | `ALL` | 10 groups by `%albumartist%`, 3 each, shuffled |
| Rated 8 and higher | `rating GREATER 7` | 100, shuffled |
| Unrated | `rating MISSING` | 100, shuffled |
| Recently added | `dayssinceadded LESS 31` | 100, newest first |
| Not heard in a year | `HISTORY(dayssinceplayed) GREATER 365 OR HISTORY(dayssinceplayed) MISSING` | 100, shuffled |

**Definitions saved by the retired MPD backend** -- kept under a profile
of their own the window no longer reads -- are moved to the one set of
definitions when the window starts, once.

## Consequences

- A rule's results never cross the network whole; at most 500 tracks do.
- A rule saved before this has no groups and selects as before, by the
  engine now.
- An engine older than `catalogue.select` cannot run rules for a newer
  window; the window says so. Continuation rules carry the group fields;
  an older engine ignores them and continues ungrouped.

## Verification

- Selection: equal odds per group whatever its size; per-group counts; whole
  groups in library order; the maximum; shuffle off keeping order; exclusion.
- An engine test: `catalogue.select` over the wire, and a list continuing
  with "Random album" appending one whole album.
- A window test: the shipped rules listed, read-only, copyable, offered by
  "Continue with"; a rule matching everything runs on a large library.
