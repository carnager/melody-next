# Trackbench: agent task list

This file once carried the M5-era validation and decomposition backlog. All of
its tasks are complete and M0-M10 are closed (see `MILESTONES.md` and
ADR-0169 through ADR-0175):

- Sanitizer and static-analysis validation: asan, tsan, and tidy presets pass
  the full suite; findings and the tsan pthread_clockjoin_np shim are recorded
  in the git history (2026-09-11).
- The `metadata_properties_dialog.cpp` and `bench_main_window.cpp` monoliths
  were decomposed into per-concern translation units under `src/bench/`.
- The UX follow-ups (dialog geometry persistence, empty-state placeholders,
  live naming-layout examples, plain-language copy) landed with their tests.

There is no standing agent backlog here anymore. Read `AGENTS.md` first, then
take work from [docs/roadmap.md](docs/roadmap.md); record consequential
decisions as ADRs under `docs/adr/`.
