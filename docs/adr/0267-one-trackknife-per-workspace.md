# ADR-0267: One Trackknife per workspace

## Status

Accepted, 2026-10-04.

## Context

Trackknife keeps its workspace -- open lists, layout, settings -- in its
settings and data folders, and saves them as it goes. Started twice, two
windows each held their own idea of it and saved over the other's.

## Decision

- **The first start claims the workspace; a later one hands over.** A
  lock file in the runtime folder says one is running, and a local socket
  beside it takes what a later start was given -- files to open, raw paths
  as bytes -- after which that start exits. The running window comes
  forward and opens the files.
- **Claimed before anything is read.** The claim comes right after the
  application's name is set, before a pending workspace restore is applied
  or the settings are touched.
- **Per settings location.** The key is the settings folder: a sandbox
  with its own XDG folders, or a screenshot run on test data, runs apart.
- A lock whose process has died is taken over, and the socket it left is
  removed. A lock held by one that does not answer within three seconds
  does not stop a start.

## Consequences

- Opening files from a file manager while Trackknife runs opens them in
  the running window.
- On Wayland, a window can ask to come forward but the compositor decides.
