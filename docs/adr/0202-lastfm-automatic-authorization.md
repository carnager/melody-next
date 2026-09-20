# ADR-0202: Automatic Last.fm authorization completion

## Status

Accepted, 2026-09-20. Post-M10 refinement of ADR-0197.

## Decision

Keep user-supplied API credentials. The Connect action explains that connecting
first-time setup enables scrobbling. Open Last.fm in the browser and check for
approval automatically; remove the manual Finish button. Check three seconds
after each pending response, with no overlapping requests, for up to five
minutes. Provider error 14 means approval is pending, not a failed connection.
Other errors end waiting and allow another Connect attempt. The settings page
owns its timers: closing it stops checks. Cancel stops further checks and does
not revoke an authorization already processed by the provider. Late replies
must not restart a stopped wait. Authority selection is locked during waiting.

Each player's service enables scrobbling on first successful authorization and
persists it with the session. Reconnecting the same user preserves the enabled
setting. Changing username retains ADR-0197's clearing/disabled behavior;
explicit Disconnect removes the account and resets setup. No new database or
state-file version is needed. Clients with older Melody versions can recognize
the pending error but need an updated daemon for the first-connection default.

## Validation

Fake HTTP tests cover pending approval, automatic scrobbling enable, disabled
preference preservation on reconnect, and persistence. UI tests cover polling
scheduling, completion, cancellation, timeout, late responses, and the absence
of a Finish button. Live browser authorization remains a manual check.

Provider contract: [auth.getSession](https://www.last.fm/api/show/auth.getSession).
