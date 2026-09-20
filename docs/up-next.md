# Up Next

Up Next is a temporary request queue over normal playback. If A is playing in
`A → B → C`, queueing X and Y gives `A → X → Y → B → C` without adding X or Y to
the original playlist.

Right-click selected tracks and choose **Queue next** to put them first, or
**Queue at end** to append them. Album/library entries and dynamic-playlist
results offer the same actions. Selected tracks retain their order, and
intentional duplicates are separate requests. The old track-selection append /
insert-next actions are removed; use **Send to tab** for ordinary list edits.
MPD Queue and named server-list tabs share the same track menu and action order.

Click **Up Next · N** in the transport, or press **Ctrl+Shift+U**, to open its
side panel. The panel shows the current authority, pending tracks, and normal
playback destination. Its rows stay single-line, with artist, title, and length.
Drag to reorder or insert tracks; dropping onto the transport button appends.
Delete/Remove, the move buttons, Clear, and Undo edit pending requests. Enter or
double-click starts a selected request now. **Return to playlist now** skips the
active request, clears pending requests, and returns immediately.

Enqueueing does not start stopped playback or unpause it. Clear leaves the
currently playing request alone. Next serves pending requests before normal
playback. During a request, Previous restarts that request. Explicitly playing a
normal list ends the detour. Merely browsing another tab does not affect it.
Requests play once in FIFO order; normal shuffle/repeat traversal is suspended.
Single/stop-after-current still stops at a boundary. Requests cannot be kept
repeating by repeat-current. Missing local files surface a playback error;
Next explicitly skips a failed request.

Local playback and Melody use the same panel and actions but retain separate
queues. Melody owns the remote request queue, including progression after the
client disconnects. Rebuild/restart Melody as well as Trackknife to expose the
new `melody_upnext` capability. Stock MPD retains ordinary positional insertion;
the panel explains that it does not support this separate request queue.

Pending local requests are stored on the workspace persistence worker, including
raw path bytes, logical source ranges, and metadata. Restore never starts audio
by itself. An interrupted request is restored for replay from its beginning.
A saved normal-list anchor is used only if its list and source still match;
otherwise no unrelated list is chosen. Closing a local source tab during a
request retains its playback model for the running session, without a hidden
view. That deliberately closed list is not reopened on restart.

Melody persists occurrence IDs and its return candidates with its queue state.
Concurrent edits use revision checks; an uncertain network mutation is never
silently replayed. Undo is one pending edit, becomes unavailable after playback
advances or another server queue edit invalidates its revision, and does not
survive restart.

The request queue is bounded to 500 pending tracks. Auto-DJ remains a separate
TODO: it will replenish normal playback while manual requests take precedence.
See [ADR-0196](adr/0196-up-next-request-queue.md) for the design and implementation
boundaries.
