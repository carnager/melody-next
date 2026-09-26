# Workspace, panels, views, and performance

## Application scope

ADR-0115 adds a local source switch with **Folders** and **Library**. Folders
keeps direct navigation and bookmarks; Library provides optional indexed
artist/album browsing and album/track search. Per ADR-0130 each authority's
sidebar carries a flat one-click tab bar: **Folders | Library** in local
context, **Library | Playlists** in MPD context, replacing the earlier
dropdown. See [local-library.md](local-library.md).

Per ADR-0058 these surfaces share one Trackbench shell while retaining distinct
authorities. Selecting **MPD Queue** binds the sidebar, transport, and output
selector to MPD; selecting **Local Queue** or another local list binds them to
local folders, local playback, and PipeWire. Local preparation commands are
unavailable in MPD context. Both queue types use the exact same declarative
track-view layout schema, renderer, editor, and preset path.

## Product promise

Trackknife opens as a complete, satisfying MPD client. Users do not need to
design an interface, import the server library, install essential panels, or
learn title formatting before they can connect, browse, queue, and play music.

Power remains available in layers:

1. excellent shipped layout and view presets;
2. choose another shipped presentation;
3. adjust columns, grouping, sorting, and artwork;
4. rearrange/tab/float panels;
5. write Trackknife format expressions;
6. install plugins that provide additional panel types, data, or actions.

“Customizable” must never become an excuse for weak defaults.

## Default workspace

The initial workspace is intentionally conventional:

```text
+---------------------------------------------------------------------+
| controls | cover | title / artist — album                          |
| elapsed   ================= seek =================   volume --      |
+----------------------+----------------------------------------------+
| Library search       | Queue / working-list tabs                    |
| Browse / results     |                                              |
|                      | live MPD queue, scratch lists, and           |
|                      | search results / server playlists           |
|                      |                                              |
+----------------------+----------------------------------------------+
| queue count / duration      list actions | modes | RG | outputs   |
+---------------------------------------------------------------------+
```

- Library navigation starts on the left with one default domain: **MPD**
  browses server folders, artists, albums, playlists, and advertised
  dimensions. Local-file navigation and preparation now live in Trackbench
  (ADR-0025). Per ADR-0120, MPD search lives above the library content in
  Sources, matching the local library. A nonempty query shows results in place
  of the browse tree; clearing it restores that tree. Search never replaces the
  active queue/list tab. Committed `Shift+Enter` search-result tabs existed only
  in the retired standalone shell (ADR-0071) and remain follow-up work.
- Per ADR-0122, MPD and local live search share an expandable **Albums / Tracks**
  tree, library row renderer, spacing, artwork size, and extended selection.
  Album titles have an artist subtitle; track titles include their track number.
  Queue actions appear on the hovered or focused row and act on the selection.
  Right-click exposes append, insert-next, replace-and-play, and expansion actions.
  Enter/click expands a branch; Enter/double-click on a track appends the selection.
  Left/Right use normal tree navigation. Return in the query field does not enqueue.
  Both searches accept any nonempty query with a 200 ms debounce, and clearing
  the field restores browsing. MPD continuation appears as a Show more… tree row.
  Clicking elsewhere leaves results visible. Switching authority preserves each
  library's query. `Ctrl+L` focuses the search of the library showing, or,
  from Folders, of the library the source switch opens to.
- ADR-0125 adds a hidden-by-default native **Find in current list** toolbar for
  local lists and the MPD queue. `Ctrl+F` opens it; Next/Previous,
  Enter/Shift+Enter, and F3/Shift+F3
  select matching occurrences with wraparound. Escape closes it. Search uses
  cached metadata and escaped local paths or exact server URIs, with bounded
  background matching and progress. Tab changes close find; Properties disables
  these track-list commands. Finding never filters rows, changes playback, or
  sends server commands.
- Album search results sort chronologically by release year, with undated
  releases last and deterministic date/artist/title fallbacks. Track results
  use a stable release-friendly order: album-artist sort name (falling back
  through album artist to track artist), album, numeric disc, numeric track,
  then title. Credited names remain unchanged for display. Expanding or queueing an album
  resolves its complete release identity with an exact asynchronous MPD lookup;
  the partial set of tracks that happened to match the text query is never used
  as the album contents.
- Album discovery is derived from the complete asynchronous MPD song-search
  response before track rows are reduced to the bounded 200-result display page.
  Candidates are then filtered so every text term matches the displayed album
  artist or album title; matches found only in track-level tags do not create
  false album results.
- The main/right workspace is tabbed and always exposes the live MPD queue plus
  scratch, stored-playlist, and named working-list tabs. Opening a server
  playlist creates or refreshes a separately closable tab keyed by its server
  name instead of replacing the folder browser. When the server advertises the
  relevant commands, the tab reuses the compact add/remove/reorder controls and
  exposes queue-load, clear, rename, and delete through its list menu; accepted
  edits reload authoritative server contents. ADR-0129 implements this through
  a sidebar Playlists page (a full-height list behind the ADR-0130 tab bar)
  and server-round-trip edits with `stored_playlist`
  idle refresh; multi-row reorder and restoring open playlist tabs across
  restart remain follow-ups.
- The default track presentation is grouped albums with cover art and readable
  track rows.
- A plain, information-dense columns preset is one action away.
- The compact Cantata-informed top player keeps transport and two-line
  artist/title and album/date information above a wide seek slider and small
  volume slider. In the unified Trackbench shell, tab selection binds this row
  to MPD or local playback. The bottom status bar keeps selection/queue context
  left and exposes Repeat, Random, Single, Consume, and advertised ReplayGain
  controls at right only in MPD context.
- Trackbench keeps this player chrome visually quiet: the metadata block is
  centered over its seek rail, and one compact application-menu button replaces
  a permanently visible menu bar without removing or duplicating its actions.
- Trackbench's status bar summarizes the current local selection. One selected
  track shows artist/title, release/date, and duration; a multi-track selection
  shows its count and combined known duration.
- The live queue groups consecutive tracks by album using restrained header
  rows with a compact asynchronously loaded cover, album artist, album, date,
  and aggregate duration. A group header is omitted when the group contains
  only one track; its cover remains as a small inline image in the ordinary
  track row. Dense track rows reserve that distinct artwork/status
  gutter for playback state, followed by one indented text block containing the
  two-digit track-number prefix and title, plus duration, instead of repeating
  album-level metadata.
  The playing row uses a restrained playback tint and outline, while selection
  retains the stronger system highlight. Clicking an album header selects every
  contiguous track represented by that header. Unmodified `Home` and `End`
  select and reveal the first and last queue occurrence respectively.
  `Shift+Home` and `Shift+End` extend the selection from its existing anchor
  to that boundary. Reversing direction or continuing with Shift+arrow keys
  retains the anchor; plain navigation starts a new selection. This behavior
  is shared by MPD and local track lists.
- In MPD context the row menu repeats play, append, add-next, remove, crop, and
  advertised priority choices for the current selection. Priority labels expose
  their numeric MPD values and mark a uniform selected value.
- Common playback modes remain visible as compact one-click controls. Boolean
  modes toggle directly; MPD multi-state playback modes cycle with their current
  state visible and the next state explained by the tooltip. ReplayGain keeps a
  compact `RG: current mode` indicator and an output-style choice popup because
  it is changed less often.
- The job center is unobtrusive when idle and obvious during long-running
  work — in Trackbench, scans, conversion, tagging, and file operations.
- Transient failures use non-modal, self-dismissed toasts. They do not replace
  title, selection, result-count, or other persistent workspace information.

Sharing one tab container does not merge list semantics. The live MPD queue
is server-authoritative. MPD stored playlists are server collections. Scratch
and named Trackknife lists are working memory of server references — persisted
lists drop or migrate local-only rows (ADR-0025) — and they never become a
second playback queue. Each tab identifies its kind and dirty/offline state.

Local audition inside the client is superseded by ADR-0025: local playback is
now first-class in Trackbench. The tested engine contracts behind it — the
serialized audition worker and list progression of ADR-0023, the cubic
PipeWire stream volume and in-place output-device retargeting of ADR-0024 —
transfer intact into Trackbench's own single-domain transport, which reuses
the shared transport-row widgets, tooltips, and diagnostics described here.

## Queue/list tabs as work surfaces

Tabs are a core workflow inherited from the best foobar2000-style file-working
experience, not merely a way to switch screens.

- Create, rename, pin, duplicate, reorder, and close tabs from mouse or keyboard.
- Preserve duplicate items, order, tab position, and view state across restart.
- Copy or move selections between tabs by command, clipboard, or drag-and-drop.
- Offer sort, reverse, randomize, deduplicate, crop-to-selection, and total
  duration/size where the source can provide it.
- In Trackbench, let tag, ReplayGain, conversion, export, and file actions
  consume the selected rows or the entire scratch/named tab.
- Protect a dirty unsaved tab on close without forcing a save dialog for every
  ordinary live-queue change.
- Make the live queue unmistakable and keep its pending server mutations visible
  until confirmed.

The default tab strip contains the live queue and one scratch list. Live search
stays in the library panel and does not occupy a tab. Committed search tabs
remain follow-up work in the unified workspace. Opening local files creates or
reuses a local tab without destroying the current work surface.

Per ADR-0123, local lists offer **Undo/Redo** for removal and rearrangement in
Edit and the row menu. With a local track view focused, use **Ctrl+Z** to undo
and **Ctrl+Shift+Z** or **Ctrl+Y** to redo. Restored tracks are selected; edits
persist normally and do not restart playback. Search text and tag drafts retain
their own undo. History is per tab and session, capped at 100 edits / 64 MiB.
Adding/replacing contents, cross-tab moves, automatic Consume, and logical-track
expansion clear affected history in this first slice.

## Panel and layout system

Use `QMainWindow`, toolbars, tab containers, and model/view track tables. The
Trackknife client retains native dock panels and versioned window/dock/toolbar
state. Trackbench uses ADR-0026's versioned declarative composition tree below
its player chrome: registered panel instances may be nested in
horizontal/vertical splitters or tab stacks, with bounded validation before
rendering. Its shipped default remains Folders left and Track Lists right.
Layout editing is explicit customization layered over that default, not
required setup.

Trackbench schema version 1 persists stable panel instance IDs, split
orientation/weights, child order, and active panel tabs. A malformed current
layout falls back visibly to the shipped default. A newer layout also falls
back, but its stored bytes are not replaced until the user explicitly edits or
resets the layout. New functional panels arrive with their owning milestones;
do not ship empty placeholders merely to make the layout editor look powerful.

Every panel has:

```text
PanelInstance
  stable instance ID
  panel type ID + provider/plugin ID
  title and icon
  data-source binding
  presentation/view preset
  panel-local configuration
  allowed docking areas and size constraints
  schema/config version
```

Users can resize, show/hide, split, and tab supported panels and create multiple
instances where the declared layout permits it. Layouts persist by stable panel
instance IDs and can be named, duplicated, exported, imported, and reset safely.

### Shipped panels

- MPD browser: connection-aware folders/artists/albums/search.
- Track view: reusable list/group renderer bound to server results, stored
  playlists, scratch/named lists, local selections (in Trackbench), or the
  live queue.
- Queue/working-list tabs.
- Now playing and transport.
- Artwork.
- Selection properties/technical details.
- Playback queue inspector.
- Search/filter editor.
- Jobs and errors.
- Console/diagnostics, hidden by default.

The default layout uses only the panels needed for ordinary listening. Advanced
panels are discoverable through a searchable “Add panel” command/menu.

### Panel plugins

A plugin may register:

- panel factories and configuration schema;
- data-source providers;
- title-format fields/functions in a namespace;
- columns/group presets;
- commands and context actions;
- optional inspectors or visualizations.

Plugins do not receive raw ownership of another panel's widgets or internal
models. The first plugin API should be designed only after several built-in
panels prove the boundary. Missing/disabled plugins leave a visible placeholder
that preserves configuration and offers removal/reinstallation rather than
silently destroying the layout.

## One reusable track-view engine

MPD results, stored playlists, Trackknife working lists, Trackbench local
selections, and the live queue use the same presentation engine but different
data-source adapters:

```text
TrackDataSource
  -> ordered/paged TrackRows + context roles
  -> group plan
  -> shared TrackView presentation
  -> selection/action adapter appropriate to the source
```

This is the UI superpower retained from foobar2000: tabs are useful listening
and file-working surfaces, and formatting logic is not limited to one column.
The same language can describe display values, group labels, sort keys,
filenames, derived tags, and album grouping, with fields appropriate to each
source context.

### View definition

```text
TrackViewDefinition
  source binding
  presentation: grouped_rows | columns | compact | artwork_grid
  filter/query
  group expressions and hierarchy
  sort expressions/directions
  columns[]
  row primary/secondary expressions
  artwork source/type/size policy
  group-summary expressions
  visible actions and interaction policy
  empty/loading/error presentation
```

Each column includes name, title-format display expression, optional independent
sort expression, alignment, width policy, visibility, and context capability.
Group definitions contain compatible expressions, artwork role, collapsing
policy, and summary values. Preserve original script source and dialect version.

Definitions are declarative data, not arbitrary imperative UI code. This keeps
them portable, inspectable, sandboxable, migratable, and fast. A future trusted
plugin can build truly custom widgets through the panel API; ordinary users
should not need executable scripts merely to arrange metadata.

ADR-0027 implements the first bounded Trackbench definition slice. Its v1
state records one of four shipped presentations plus stable column order,
visibility, and width per list. The default album presentation has a
full-width group label/duration and a side-artwork column whose cover extends
through the visible member rows; Artist, track number, Title, Album, Date, and
Length remain normal independent columns. Compact semantic columns keep their
preferred widths while Artist, Title, and Album proportionally fill the rest
of the viewport on every panel/window resize. Cells and group labels remain
single-line and elide instead of wrapping. Header and Workspace menus expose
the same controls. Custom `tkfmt-1` column, sort, group, label, and summary
expressions remain required extensions to this definition rather than being
represented by ad-hoc Qt state.

## Shipped presentation presets

Presets apply equally where meaningful to library results, playlists, and queue.
Source-specific columns such as queue index appear only when available.

### Albums with covers — default

- Group by album identity using album artist, album, release date, edition, and
  disc information with sane missing-field fallbacks.
- Show one asynchronously loaded cover thumbnail per group.
- Track rows show disc/track number, title, track artist when different, and
  duration; optional codec/loudness details remain subtle.
- Multi-disc albums remain one album with visible disc subdivisions.
- Compilation albums use album artist consistently and expose track artists.
- Groups can collapse without unloading the source result or disturbing play.

### Plain columns

- No album decoration; compact row height.
- Useful defaults: playing/queue state, artist, title, album, date, track, codec,
  ReplayGain state, duration.
- Columns can be reordered, resized, hidden, and defined using title formatting.
- Sorting is stable and executed by the data source/database where possible.

### Compact queue

- Queue position, playing state, artist/title, album, duration.
- Duplicate queued occurrences remain separately visible.
- Drag to reorder; delete consumes only the chosen occurrence.
- Optional grouped-album presentation uses the same view engine.

### Folder/library tree

- The left navigation surface is the library tree, not a generic auxiliary
  sidebar. It is a native dock in the client, whose single default tab is
  **MPD**; the **Local Folders** tree described below is a registered
  Trackbench panel in ADR-0026's composed layout.
- The MPD tab provides folder structure and configurable title-format
  hierarchy while MPD remains authoritative for server membership. Its shipped
  definition is album artist → chronologically sorted album → disc when the
  release has multiple discs → tracks. Expanding an artist performs one bounded
  exact MPD query; opening the pane never mirrors the complete server database.
  A native editor changes the explicit grouping, label, stable-sort, and
  singleton-elision expression at each structural level and restores the
  shipped default.
- The server tree never commandeers the center workspace on selection. Compact
  inline actions append a node to the live queue, insert it next, or replace the
  queue and play; the context menu repeats those actions and can add the node to
  a chosen working-list tab. Dragging selected library rows into the live queue
  uses the queue's exact painted insertion target. Actions and drops on an
  unloaded root wait for its exact
  branch query. Inline actions appear on the current or hovered row instead of
  consuming permanent columns; the current row reveals them only while the tree
  has keyboard focus, so Qt's implicit first current index does not leave a
  permanent action strip behind. A single click anywhere on a branch row toggles
  expansion; the native disclosure arrow and the rest of the row each toggle it
  exactly once, while clicking an inline action performs only that action. A
  filter field remains visible above the tree, and unloaded matching roots
  retain their disclosure indicator while filtering.
- Rows have hierarchy-specific visual weight rather than a spreadsheet layout:
  artist rows pair a silhouette placeholder with album count; album rows pair
  a compact cover thumbnail with dated title plus track count and duration;
  track rows pair a media icon with numbered title and duration. Album rows are
  denser than artist roots and retain a visible record placeholder while covers
  load asynchronously and serially; expanding one artist must not start an
  unbounded image-request burst or decode artwork on the UI thread.
- **Local Folders** (Trackbench) lazily enumerates explicitly chosen/recent
  raw filesystem roots without scanning them into a library database. It
  preserves raw path identity, displays non-UTF-8 names losslessly, does not
  follow directory symlinks, and probes file capability asynchronously. It is
  also the home for local readiness and actions: identify/edit metadata and
  artwork, ReplayGain, organize, convert, play, and import into an accessible
  MPD music root.
- Opening or activating a local folder/file feeds a Trackbench working list
  with explicit local-source badges; nothing Trackbench does makes that item
  an MPD-library member. Resolving a mapped MPD item to its local counterpart
  is a deferred cross-tool convenience (ADR-0025), not a client feature.
- Multi-value facet expansion uses the explicit bounded behavior described in
  `title-formatting.md`.
- Selecting an MPD tree node only selects it; disclosure and explicit actions
  expand or send its contents without replacing the active center tab.

### Artwork grid

- Album-oriented grid with cover, album, artist, date, and optional summary.
- Loaded lazily with bounded thumbnail cache; never decode original-size art on
  the UI thread.

Every shipped preset is editable by copying it. Built-in originals remain
recoverable so experimentation cannot destroy the good defaults.

## Script experience

The same `tkfmt-1` compiler/evaluator powers columns, rows, groups, sorts,
relative names, and ReplayGain grouping. Context capability controls which
fields exist while grammar and scalar semantics remain uniform.

Panel/view editors provide:

- live preview against several real rows and groups;
- syntax highlighting, field/function completion, and diagnostics;
- raw output plus field and function dependency inspection;
- explicit display, sort, and grouping expressions;
- dependency/cost warnings;
- one-click restore of shipped defaults;
- import/export of declarative definitions.

Panel scripting must not run filesystem/network/process operations. Those are
commands with explicit permissions and previews, not formatting functions.

## Performance is a feature

The interface must feel immediate while the application performs serious work.
No amount of customization compensates for dropped frames, frozen windows,
selection lag, or delayed playback controls.

### User-visible performance contract

- Playback controls, tab switches, and queue actions acknowledge input
  immediately; pending server changes remain visibly pending until confirmed.
- Scrolling cached rows stays fluid while scans/conversions run.
- Opening a server view does not download the whole MPD database.
- Search shows useful server-backed results as the user types and cancels stale
  requests.
- Changing a view preset, sort, or group never blocks the UI thread.
- Cover placeholders appear instantly and fill progressively without reflow.
- Cancelling a job changes visible state immediately even if a backend needs
  time to reach a cancellation point.
- Split resizing, tab switching, and layout restoration do not interrupt
  audio or trigger full-library recomputation.

### Provisional measurable budgets

Finalize these against a documented baseline machine, but design to them now:

| Interaction | Provisional target |
| --- | --- |
| Warm launch to first usable disconnected/cached window | <= 1 second |
| Playback/queue command UI acknowledgement | <= 50 ms |
| Cached panel/tab switch | <= 50 ms |
| Saved workspace restore and first repaint | <= 75 ms |
| Local command-to-search-loading acknowledgement | <= 50 ms |
| Cached/server-page search first visible update | <= 250 ms plus server latency |
| Cached list scroll/frame work at 60 Hz | <= 16.7 ms p95 |
| Selection change to visible lightweight details | <= 50 ms |
| Long-operation cancellation acknowledgement | <= 100 ms |

Backend completion can take longer; responsiveness means acknowledging work and
showing truthful progress, not faking completion. Performance tests report p50,
p95, and worst-case, not only an average.

### Mandatory implementation rules

- No disk, network, decoder, tag parser, artwork decode, expensive title-format
  batch, or unbounded SQL runs on the UI thread.
- Use custom C++ `QAbstractItemModel` implementations with `QTableView` and
  `QTreeView`.
  Keep delegates lean and reusable; never materialize one persistent object for
  every cell/row.
- Page/fetch rows on demand and paint only visible cells with delegates.
- Perform remote filtering/search through bounded server requests and local
  sort/group work in core services; proxy models may handle small cached pages
  but never pull an entire remote database onto the UI thread.
- Compile title-format scripts once. Cache results by program ID, TrackId,
  track revision, context revision, and relevant dynamic state.
- Extract field dependencies from scripts so an artwork or unrelated tag change
  does not invalidate every displayed cell.
- Recompute and emit only changed rows/roles; avoid full model resets.
- Coalesce scan/job/library event bursts to at most the useful display cadence.
- Load artwork asynchronously into bounded, resolution-aware memory/disk caches;
  cancel requests for content scrolled out of view.
- Panels share immutable/query snapshots and caches instead of independently
  querying and duplicating the library.
- Grouping is lazy/paged. Do not eagerly create heavyweight objects for every
  album and row merely because a tree could be expanded.
- Keep audio real-time work isolated from Qt's UI thread and general worker pool.
- Cap background CPU and I/O so ReplayGain/conversion cannot starve playback or
  interaction; expose a foreground/background priority policy.
- Plugin panels obey the same pagination, cancellation, event-coalescing, and
  measurement contracts.

Avoid `ResizeToContents` over an entire huge model during ordinary interaction;
sample visible/bounded rows or persist user widths. Expensive “perfect” sizing
is not worth freezing the interface.

### Performance corpus

Maintain generated and real anonymized fixtures for:

- 10k, 100k, and 1m logical tracks;
- high-cardinality and multi-value tags;
- many cue subsongs sharing fewer physical files;
- missing/unmounted/network sources;
- long Unicode metadata and lyrics;
- albums with missing art, huge art, and many distinct covers;
- frequent metadata/statistics updates during scrolling;
- simultaneous playback, ReplayGain scan, and library search;
- plugin panels that are slow, failing, or producing event bursts.

Benchmarks cover warm/cold launch, memory, query latency, formatting throughput,
scroll frame times, artwork cache hit/miss, scan scaling, tag-plan preview, and
shutdown/persistence. Store baseline hardware/software details and detect
regressions in CI where results are stable enough.

### Instrumentation

Include an opt-in local diagnostics overlay/log with frame/update timing, model
fetches, SQL timings, title-format cache hit rate, artwork cache behavior, job
worker utilization, audio underruns, and event queue depth. No telemetry is sent
anywhere.

## Accessibility and keyboard use

Fast also means low interaction cost:

- every command has a discoverable name and optional shortcut;
- tables, tabs, docks, menus, dialogs, and custom delegates expose correct
  accessible roles/names;
- focus location remains predictable when panels update;
- users can navigate, select, queue, edit, preview, and commit without a mouse;
- scripts and presets do not remove essential actions from the command palette;
- animations are restrained and respect reduced-motion preferences.

## Failure and reset behavior

- A corrupt layout restores the shipped layout without touching library data.
- “Reset workspace” previews what changes and leaves custom view definitions
  available unless explicitly removed.
- A failed panel/plugin shows a contained error; it does not take down playback
  or the entire main window.
- Unknown future panel configuration is preserved where possible and clearly
  marked rather than rewritten destructively.

## Creating tabs from selected tracks

**Trackknife decision (ADR-0167):** Local track menus offer **Copy to list →
New tab…** and **Move to list → New tab…**, with a name prompt. Dropping local
tracks on empty tab-strip space creates and selects a persistent **Selection**
tab immediately. Drop on an existing local tab to append there. Dragging moves
by default; hold **Ctrl** to copy. These actions transfer list occurrences,
not files on disk; MPD and metadata tabs reject local track drops.

Local and MPD library trees share row metrics, indentation, icon sizing, and
animated expansion (ADR-0168). Artist subtitles display **1 album** / **N albums**.
Local counts come from the background indexed query; MPD counts arrive through
a background grouped tag query, without fetching every track or opening branches.

## Playback navigation

**Workspace → Cursor follows playback** is off by default and remembered.
When enabled, a new playing row becomes the selected cursor row and scrolls
into view if its list is currently visible. It does not switch tabs or keep
resetting a manual selection while the same track plays.

**Workspace → Jump to playing** (`Ctrl+J`) opens the playback list and centers
its current row, using the selected tab's local/server authority. A closed
server playlist is reopened and loaded asynchronously. During an Up Next
request it opens the request panel instead of selecting an unrelated list row.
Neither navigation action changes playback or the sticky Active marker.

## Keyboard shortcuts

Settings → Shortcuts lists every action with a default key, and every command
the command palette offers, with or without one, so Convert, ReplayGain and
the rest can be given a key. Save applies and persists them; Cancel leaves
current bindings alone. Restore defaults edits the form until Save. Empty
bindings disable a shortcut. Duplicate and chord-prefix conflicts are
rejected; `Ctrl+L` is an ordinary binding like the others.
These are application shortcuts, not desktop-wide hotkeys.

| Action | Default |
| --- | --- |
| Play / Pause | Space |
| Stop | Ctrl+. |
| Previous / Next track | Alt+Left / Alt+Right |
| Queue next / Queue at end | Ctrl+Return / Ctrl+Shift+Return |
| Show Up Next | Ctrl+Shift+U |
| Jump to playing | Ctrl+J |
| Cursor follows playback | Ctrl+Shift+J |
| Find in list / Search | Ctrl+F / Ctrl+Shift+F |
| Open files / Open folder | Ctrl+O / Ctrl+Shift+O |
| Open list (any engine's lists, ADR-0233) | Ctrl+Alt+O |
| New / Duplicate / Close tab | Ctrl+N / Ctrl+Shift+D / Ctrl+W |
| Rename / Save list | F2 / Ctrl+S |
| Edit tags / Settings | Alt+Return / Ctrl+, |

Playback and queue shortcuts use the selected tab's authority. Queue shortcuts
use the current track selection and the same Up Next actions as the track menu.
