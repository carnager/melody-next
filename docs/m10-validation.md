# M10 hardening and release validation

This record closes the engineering gate for the first public Trackknife
workspace release. Distribution maintainers must still complete the
artifact-specific and hardware-specific items in the release checklist for
each published build.

## Automated acceptance

- The development and optimized release configurations build, and all 67
  tests pass with `QT_QPA_PLATFORM=offscreen`.
- The ASan/UBSan configuration passes the same suite. CI also retains the
  clang static-analysis and parser/evaluator fuzz jobs.
- MPD session/reconnect, Melody endpoint, local playback, conversion, and
  online workspace-backup tests pass 25 consecutive iterations as a focused
  fault/stress check. The application retains the `TRACKKNIFE_SOAK_LOG` hook
  for release-candidate listening so a publisher can record long-run memory,
  QObject, underrun, and event-loop measurements.
- The checked UI suite covers keyboard shortcuts, focus-driven operations,
  mouse actions, authority switching, accessible control names, and non-color
  state text. The benchmark smoke covers the million-row workspace budgets.
- `format-check`, SPDX validation, `git diff --check`, the desktop-file
  validator, and Arch package metadata validation pass. `ldd` confirms that
  Qt 6 and the media/database dependencies are dynamically linked.

## Release surfaces added for M10

- File > Back up workspace creates a consistent SQLite online snapshot plus a
  versioned settings companion without overwriting an existing file.
- File > Restore workspace validates the snapshot, restores it before services
  start on the next launch, and retains the previous database as a timestamped
  rollback. Corrupt and non-Trackknife databases are rejected.
- File > MPD capability diagnostics exposes the negotiated protocol version,
  advertised commands and tag types, queue state, and outputs without requiring
  a terminal probe.
- The converter can export the selected built-in or saved encoder preset as
  versioned `trackknife-encoder-preset-1` JSON. Metadata transformation chains
  retain their existing versioned import/export surface.
- `THIRD_PARTY.md`, the Arch package, and the release checklist now carry the
  dependency, license, Qt LGPL replacement, installation, backup, accessibility,
  protocol, and end-to-end workflow obligations.

## Compatibility evidence and release boundary

The stock MPD and Melody hands-on workflows are recorded in
`docs/m3-validation.md`, while the live Melody endpoint/volume/ReplayGain pass
completed during M9. Local real-file format, metadata preservation,
ReplayGain, and conversion coverage is recorded by the feature-specific docs
and real-file tests.

This closes M10 as a product engineering milestone. It does not certify an
arbitrary future package artifact: before publishing one, its maintainer must
run `docs/release-checklist.md` against that exact checksum and record the
distribution, server, audio device, filesystem, and soak evidence. Flatpak is
not claimed; the supported native release surface is the Arch package.
