# ADR-0169: Portable output-path sanitization

## Status

Accepted.

## Context

Saved naming layouts previously required `linux-v1`. That policy is deliberately
loss-minimizing on Linux, but users preparing a collection for Windows, a NAS,
or removable media need predictable names before publication. Changing
`linux-v1` would silently reinterpret persisted layouts.

## Decision

Add a separately versioned `portable-v1` policy. Profiles store the exact
choice and the editor offers **Linux filenames** and **Portable filenames**.

In addition to `linux-v1`, `portable-v1` replaces ASCII controls and
`< > : " \\ | ? *`, replaces trailing spaces/dots, and prefixes Windows device
stems `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, and `LPT1`–`LPT9`. It preserves
Unicode code points exactly: no normalization, transliteration, case folding,
or whitespace trimming occurs.

Directory `/` characters remain structural. All collision, length,
containment, preflight, and no-replace publication checks still run afterwards.

## Consequences

Existing `linux-v1` profiles retain identical behavior. A future normalization
or user-defined policy needs a new name/version and cannot alter either v1.

## Evidence

Planner tests cover forbidden characters, trailing dot/space, reserved names,
target paths, and unknown versions. Offscreen UI tests cover the naming editor.
