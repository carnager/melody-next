# Qt Quick port: parity inventory of the widgets UI

The widgets window (src/bench, src/uicommon) as the Qt Quick window must reproduce it, 1:1. Gathered 2026-09-29 from the code on branch qml-mockup; each section cites file:line. It is the checklist the port is measured against.

---

## Main window, menus and shortcuts

## Trackknife main window shell inventory (Qt Widgets, for the Qt Quick port)

Paths are relative to `/home/carnager/Code/melody-next-engines/src/`. BMW means `BenchMainWindow`. `[tr]` marks a string wrapped in `tr()`. Every other user-facing string is a `QStringLiteral` and is not translatable.

**Construction order** (`bench/bench_transport.cpp:165-192`):
- Title "Trackknife", `resize(1100, 720)`, `setAcceptDrops(true)`.
- Build calls: `buildWorkspace()` → `buildTransport()` → `buildLastFm()` → `buildShortcuts()` → `initializePersistence()`.
- A 33 ms `transport_timer_` calls `refreshTransport` (line 59/185).
- Then `buildMprisService()`.
- The menu bar is a real `QMainWindow::menuBar()`. Menus in order: **&File, &Edit, &Workspace** (built in `buildWorkspace`), then **&Playback** (built in `buildTransport`). There is no Help menu and no hamburger/menu button.

---

### 1. Widget layout tree

#### 1.1 Top: "Transport" QToolBar (top toolbar area)
Source: `bench/bench_transport.cpp:283-570`
- `addToolBar("Transport")`, objectName `bench-transport`. Not movable, not floatable, iconSize 18, IconOnly (285-290).
- It holds one widget, `bench-player-header`. That widget is Expanding and uses a QHBoxLayout with margins 10,6,10,6 and spacing 10 (321-326).

Left to right:
1. **Cover** `bench-now-playing-cover` (QLabel), fixed 44×44 (`header_cover_size`, 214), centered (328-333).
   - `refreshHeaderCover` (230-281) picks the cover by album group key and scales it with KeepAspectRatio.
   - Placeholder: a rounded rect with radius 3, filled with 90% Window + 10% Text. It draws "♪" (U+266A) at 1.5× font size in PlaceholderText color.
2. **Track display** `bench-track-display`: a QVBoxLayout with spacing 1 and stretches above and below. SizePolicy Ignored, min width 140, max width 420, stretch factor 3 (335-362).
   - `bench-now-playing`: `ui::ElidingLabel`, DemiBold, 1.08× point size. Shows the title.
   - `bench-now-playing-context`: ElidingLabel in PlaceholderText color. Shows "Artist — Album (Year)".
   - Text states (`refreshEngineTransport`, 1688-1752):
     - "Nothing playing"
     - "Could not play" [tr], with the error text on the context line
     - "No engine" (1866)
     - "Paused · %1 is playing on these speakers" [tr]
     - If the row is unknown, the file name is shown, with the tab name as context.
   - Tooltips show the path.
   - Window title becomes "Artist – Title — Trackknife" (1746).
3. `addSpacing(6)`.
4. **Transport buttons** `bench-transport-buttons`: HBox with spacing 4 (367-404). Each is a QToolButton with a default action and autoRaise.
   - Previous: 30×30, icon `SP_MediaSkipBackward`.
   - Play/Pause `bench-play`: 36×36, not autoRaise. Stylesheet: `border:none; border-radius:18px; background: palette(highlight)`; hover adds `1px solid palette(light)`; disabled background is 88% Window + 12% Text (389-402).
     - Icons are `SP_MediaPlay` / `SP_MediaPause`; text "Play" / "Pause" (1659-1664).
   - Next: 30×30, icon `SP_MediaSkipForward`.
   - Stop is **not** in the header. It is only in the Playback menu (comment at 365).
5. **Elapsed** (QLabel "0:00"): right-aligned, PlaceholderText color, fixed width = advance of "00:00:00" (406-410).
6. **Seek** `bench-seek` (`ui::LineSlider`): min width 120, Expanding/Fixed, stretch 4 (411-421).
   - Seeks on release only (`seeking_` flag).
   - Disabled when duration is 0.
7. **Duration** (QLabel "0:00"): left-aligned, placeholder color, same fixed width (422-427).
8. `addSpacing(6)`, then the **volume box** (HBox, spacing 4) (429-472):
   - `bench-mute` QToolButton: checkable, autoRaise, 26×26, icon 18.
     - Icon is theme `audio-volume-muted` / `audio-volume-high`, falling back to `SP_MediaVolumeMuted` / `SP_MediaVolume`.
     - Tooltip "Unmute" / "Mute" [tr] (196-211).
     - Clicking toggles between 0 and the last remembered volume (default 100).
   - `bench-volume` LineSlider: range 0..100, fixed width 90, tooltip "Volume". Its value is sent to `transport_->setVolume`.
9. **Device / output pill** `bench-device` (QToolButton, InstantPopup menu) (475-534, 1971-2034).
   - Shared "pill" stylesheet: background = shade 6%, `1px solid` shade 16%, `border-radius:13px`, `padding:0 10px`. `[chevron="true"]` gets `padding-right:24px`; hover border is highlight; pressed/checked fill is shade 16%; no menu-indicator. Font is 0.92×.
   - Icon: theme `audio-speakers-symbolic` → `audio-speakers` → `SP_ComputerIcon`, at 14×14.
   - Unnamed state: IconOnly, fixed 34×26.
   - Named state: TextBesideIcon, height 26, text ElideMiddle to 280px, plus the chevron label `bench-device-chevron`. The chevron uses theme `pan-down-symbolic` → `arrow-down` → `SP_ArrowDown` at 10×10, with right margin 9.
   - It is named when agents exist or a non-default device is chosen. Text is "Speaker · Device", plus " (offline)" when offline.
   - Multi-line tooltip:
     - "Audio output: engine → speaker → device"
     - "Buffer: <profile> · N ms capacity · N ms start"
     - "Underruns: N"
     - optionally "Playback is paused until an output is available" or "Reconnecting the audio output".
   - With no engine: disabled, tooltip "No engine is connected".
10. **Up Next pill** `action-up-next` (QToolButton): fixed height 26, pill style, accepts drops, event-filtered (536-569).
    - Inner HBox with margins 11,0,7,0 and spacing 6: QLabel "Up Next", then badge `bench-up-next-count`.
    - Badge: min width 18, height 18, font 0.85×. Style: `background: palette(highlight); color: highlighted-text; border-radius:9px; padding:0 5px`. Visible only when count > 0.
    - Accessible name / tooltip: "Up Next, %1 waiting". Width is fixed to the layout's sizeHint (217-228).
    - Click triggers `action-show-up-next`, which toggles the dock.
    - Drop target: library entries or LocalListModel rows are queued at the end (`bench/bench_main_window.cpp:94-119`).

#### 1.2 Central widget
Built in `bench/bench_workspace_layout.cpp:80-240`.
- `layout_host_` (`bench-panel-layout-host`, VBox with 0 margins) is `setCentralWidget`.
- It hosts a panel-layout tree rendered from `ui::PanelLayout` (`uicommon/panel_layout.hpp`).
  - Node kinds: `panel`, `split` (QSplitter with orientation and weights), `tabs` (QTabWidget, documentMode, movable).
  - Registered panels: `"folders"` titled "Sources" and `"track-lists"` titled "Track Lists" (72-73, 111, 120, 231-232).
  - Default layout: horizontal split `[folders, track-lists]` with weights **1:3** (636-644).
  - Splitters use `setChildrenCollapsible(false)`, stretch factors = weights, and are rescaled after the first show (700-737).
  - A tab stack with an empty title falls back to "Panel group".

**Sources panel** `bench-panel-folders`: min width 160, VBox with 0 margins and 0 spacing (116-229).
- Heading row (margins 6,4,6,0; spacing 2) holds QTabBar `bench-local-source-tabs`.
  - Accessible name "Local music source"; not expanding, no base, documentMode.
  - Stylesheet: tabs are transparent with `padding 5px 12px` and top radius 5; text is `palette(placeholder-text)`; hover fill is Base at alpha 110; the selected tab is `palette(base)` with `palette(text)` (131-152).
  - Tabs: "Folders", "Library". One more tab per connected remote engine, named by `catalogue->name()`, with tabData = engine key and tooltip = `describe()` (`bench/bench_remote.cpp:363-366`).
  - The Library tab is hidden when the "show local library" setting is off (`bench/bench_list_tabs.cpp:1508-1518`).
- "Bookmarks" heading label `bench-folder-bookmarks-heading`: 0.85× bold, margins 8,4,8,2.
- `bench-folder-bookmarks` QListWidget: no frame, max height 150, AdjustToContents, no horizontal scrollbar.
  - Items use theme icon `folder`, show the folder basename, and have the full path as tooltip.
  - Activating an item reveals it in the tree.
  - Heading and list are visible only when non-empty and the Folders tab is current (`bench/bench_list_tabs.cpp:1283-1288`).
- `bench-source-stack` QStackedWidget, stretch 1. Pages:
  - `bench-folder-tree` QTreeView: header hidden, drag-only, extended selection; activating a file opens it. Root "/" is added and home is revealed.
  - Local `LocalLibraryPanel`, added after persistence initializes (`bench/bench_list_tabs.cpp:185-189`).
  - One remote `LocalLibraryPanel` per engine (`bench/bench_remote.cpp:354-358`).
  - The page shown follows the source tab (`refreshActiveContext`, `bench/bench_list_tabs.cpp:1269-1282`).

**Track Lists area** `bench-track-area`: horizontal QSplitter, not collapsible (107-114).
- `tabs_` (`PlaybackTabWidget`, `bench-tabs`): documentMode, movable, accepts drops, event-filtered.
  - Tab-bar tooltip: "Drop local tracks on a tab to transfer, or on empty tab-bar space to create a tab. Hold Ctrl to copy." [tr] (81-91).
  - Each tab is a `ui::QueueTableView`. Setup in `bench/bench_list_tabs.cpp:907-1153`:
    - Row height 22 (min 18); vertical header hidden; alternating colors; no grid; SelectRows; Extended selection; per-pixel scrolling.
    - Movable, interactive header sections (min 24).
    - Custom context menus on both the header and the body.
    - Drag and drop with Move as the default action.
  - Tab text is the name plus " *" when dirty. Remote tabs get theme icon `network-server` (1086-1090, 1737-1739).
  - Tab tooltip: "Persistent scratch list" or "Named Trackknife working list", plus " · pinned", " · modified", and " · Active playback queue" [tr] (1740-1748).
  - Pinned tabs hide their close button.
  - Tabs are kept grouped by engine rank (`keepTabGroupsTogether`, 688).
  - Empty-list text [tr] (1488-1498):
    - Local title "This list is empty"; hint "Drop files or folders here, or add albums from the library on the left."
    - Remote title "Nothing from %1 here yet"; hint "Add albums from %1's library on the left, or drag them here."
- `bench-lists-pane` (`bench/bench_lists_panel.cpp:32-76`): the second splitter child, min width 160, stretch 0.
  - Width comes from `lists-panel/width` (default 220) and is saved on splitter move.
  - Heading (margins 8,4,4,2): bold "Lists" label, plus `bench-lists-new` QToolButton (theme icon `list-add`, tooltip "New list") that calls `createList`.
  - Body is `ListsPanel` (QTreeWidget `bench-lists-panel`, `bench/lists_panel.cpp:20-127`): 2 columns (name stretch, count right-aligned in the disabled-text color), header hidden, no root decoration, indentation 10, no frame, drop-only.
    - Groups are bold 0.85× headings per engine: "This computer", the catalogue name, or "Remote".
    - Saved lists sort first by name. Working (unsaved) lists are *italic*.
    - The playing list gets icon `media-playback-start`.
    - Dirty lists get " *".
    - Item tooltip: "Saved list" or "Working list, not saved", plus " · open here" and " · modified".
    - Empty group note: "No lists" or "No lists: %1".
    - An "Other tabs" group lists non-list tabs.
- **Tabs vs side panel** (`bench/bench_lists_panel.cpp:114-133`): setting `appearance/lists-display`, either `"panel"` or `"tabs"` (key at `bench/bench_main_window.hpp:214`).
  - Panel mode hides `tabs_->tabBar()` and shows `lists_pane_`.
  - Panel mode fetches `list.all` from every engine through a 150 ms single-shot timer; presentation is debounced at 0 ms.

#### 1.3 Bottom toolbars (bottom toolbar area, hidden until used)
Added in this order:
1. `PlaylistTransferBar` — "Portable playlist" (`bench/bench_playlists.cpp:49-51`). Contains a status label and a Cancel/Close action.
2. `TrackListFindBar` `bench-list-find-bar` (`bench/bench_workspace_layout.cpp:320-322`; `bench/track_list_find_bar.cpp:45-91`).
   - Query QLineEdit: placeholder "Find in current list", min width 180, max length 1024, clear button.
   - Actions: Previous, Next, a status label, and Close (Esc).
3. `LocalListEditBar` "Edit local list" (354-356; `bench/local_list_edit_bar.cpp:60-90`).
   - Expression QLineEdit (default `%title%`), direction combo (Ascending/Descending), a "Sort list" action, a status label, and Close/Cancel.

#### 1.4 Right dock: Up Next
Source: `bench/bench_up_next.cpp:43-241`; `bench/animated_panel_dock.hpp`.
- `AnimatedPanelDock("Up Next", "up-next")`, objectName `bench-up-next`.
  - Allowed areas Left and Right; added to **RightDockWidgetArea**.
  - `NoDockWidgetFeatures`, and a 0-height empty title-bar widget, so it has no native title.
- Content is a VBox with 0 margins:
  - Heading (margins 12,10,6,8):
    - Title "Up Next", DemiBold 1.08×.
    - Status `up-next-status`: "%1 · %2 waiting", where %1 is "This computer" or the engine name. Placeholder color, word-wrapped. Tooltip "Playing: artist — title".
    - Close QToolButton `up-next-close` (theme icon `window-close`, tooltip "Close Up Next").
  - `up-next-tracks` QueueTableView:
    - Only the title column is visible; `UpNextDelegate` draws a two-line row with cover, title, artist and length, at `UpNextDelegate::row_height`.
    - No headers, no grid, no horizontal scrollbar.
    - Drag and drop reorders; accepts rows from lists and entries from the library.
    - Empty message: "Nothing waiting" / "Drag tracks here from a list or the library." [tr].
    - Activating a row moves it to the front and calls engine `next()`.
  - Footer QFrame `up-next-footer`: border-top 1px of 88% Window + 12% Text; margins 4,4,8,4.
    - QToolBar `up-next-toolbar`, icons 16, IconOnly:
      - Remove from Up Next (`list-remove`, Delete, widget-with-children context)
      - Move up (`go-up`)
      - Move down (`go-down`)
      - separator
      - Clear pending tracks (`edit-clear`)
      - Undo (`edit-undo`)
    - Stretch, then `up-next-return` QToolButton: text "Back to the list" or "Back to %1" [tr], elided; icon `go-next`; RightToLeft layout; autoRaise; tooltip "Skip what is waiting and return to the list now".

#### 1.5 Status bar
Styled in `styleStatusBar()` (`bench/bench_transport.cpp:988-1034`).
- Size grip is off.
- Stylesheet: `QStatusBar { border-top:1px solid mix(Window,Text,12%) }`, `::item { border:none }`, and `QStatusBar QLabel { color: palette(placeholder-text) }`.

Left to right:
1. **Selection summary** `bench-selection-status`: QLabel, stretch 1, margins 6,0,6,0 (`bench/bench_workspace_layout.cpp:241-247`).
   - This is a normal (non-permanent) widget, so a temporary `showMessage()` replaces it while shown.
   - Texts (`bench/bench_track_views.cpp:317-412`):
     - "No tracks selected"
     - One track: "Artist — Title · Album (Date) · m:ss", with the path as tooltip.
     - Several: "N tracks selected · <total> total", plus " · N duration unknown" when needed.
2. Permanent **mode toggles**, one QToolButton each with a checkable default action (`bench/bench_transport.cpp:888-946`). Style: `border:none; radius 4; padding 3`; hover is mix 8% Text; checked is mix 35% Highlight.
   - `bench-local-repeat`: "Repeat", icon `media-playlist-repeat`.
   - `bench-local-random`: "Random", icon `media-playlist-shuffle`.
   - `bench-local-single`: text "Single: Off|On|One-shot", icon `media-playlist-repeat-song`. One-shot adds a highlight dot overlay (`oneShotIcon`, 841-857). Clicking cycles through the states.
   - `bench-local-album-random`: "Album shuffle" [tr]. Its icon is custom-painted by `albumShuffleIcon(palette)` (`bench/bench_main_window_helpers.cpp:16`).
   - `bench-local-consume`: "Consume: …", icon `edit-clear-list`, also cycles.
   - The fallback icon for all modes is `SP_BrowserReload`.
   - Tooltips: "Repeat: On/Off"; Single/Consume include help text (1096-1124).
   - Random and Album shuffle are mutually exclusive.
3. **Divider** `bench-status-divider`: 1×16 QFrame in mix 18% Text (1018-1022).
4. **ReplayGain** `bench-local-replaygain`: QToolButton, InstantPopup, TextOnly.
   - Text "ReplayGain: Off|Track|Album|Automatic".
   - Colored `placeholder-text`, or `palette(text)` when `[active="true"]` (1025-1033, 1126-1144).
   - Long multi-line tooltip.

There is **no** device button, job/progress indicator, or engine indicator in the status bar. The device control is the header pill.

Temporary messages go through `statusBar()->showMessage(msg, ms)` (57 call sites in `bench/*.cpp`). These are the de-facto toasts; there is no separate toast widget.

---

### 2. Menus (in order)

#### &File (`bench/bench_workspace_layout.cpp:275-317`, plus `bench/bench_playlists.cpp:52-61`)

| # | Text | Shortcut | Notes / action |
|---|---|---|---|
| 1 | New list… | Ctrl+N (`QKeySequence::New`) | `createList` (asks for name, then adds a saved-kind tab) |
| 2 | Open files… | Ctrl+O | `openFilesDialog` |
| 3 | Open folder… | Ctrl+Shift+O | `openFolderDialog` |
| 4 | Import M3U8 playlist… [tr] | — | `importPlaylistDialog` |
| 5 | Export list as M3U8… [tr] | — | `exportPlaylistDialog`; has a tooltip; enabled only with a list tab |
| 6 | Open list… | Ctrl+Alt+O | `showOpenListDialog` |
| 7 | Dynamic playlists… | — | `showDynamicPlaylists` |
| 8 | Back up workspace database… | — | `backupWorkspace` |
| 9 | Restore workspace database… | — | `scheduleWorkspaceRestore` (applied on next start, `bench/main.cpp:127-175`) |
| 10 | Bookmark folder… | — | `addFolderRoot` (no objectName) |
| — | separator | | |
| 11 | Close window | — | `close()`; statusTip "The music keeps playing" |
| 12 | Quit and stop playback | Ctrl+Q (`QKeySequence::Quit`) | `quitAndStopEngine`; statusTip "Close Trackknife and stop this computer's engine" |

#### &Edit (319-434)

| # | Text | Shortcut | Notes |
|---|---|---|---|
| 1 | Find in current list… [tr] | Ctrl+F | Opens the find bar; enabled only with a list tab |
| 2 | Find next in list [tr] | F3 | |
| 3 | Find previous in list [tr] | Shift+F3 | |
| — | sep | | |
| 4 | Undo list edit (dynamic: "Undo %1" / "Undo Move tracks between tabs") | Ctrl+Z, WidgetWithChildren context | `replayListEdit(true)` |
| 5 | Redo list edit (dynamic) | Ctrl+Shift+Z **and** Ctrl+Y | |
| — | sep | | |
| 6 | Sort list ▸ [tr] | | Presets: Title `%title%`; Artist / album / track; Album / track; Track number; Path `$info(path)`; sep; Custom expression… (opens the edit bar). All [tr]. Enabled when rows > 1 |
| 7 | Reverse list [tr] | | Runs through the edit bar |
| 8 | Shuffle albums [tr] | | Has a tooltip; random seed |
| 9 | Remove duplicate entries [tr] | | Has a tooltip |
| — | sep | | |
| 10 | Edit tags… | Alt+Return | `showMetadataProperties`; enabled with a selection |
| 11 | ReplayGain… | — | `showReplayGainDialog` |
| 12 | Convert files… | — | `showConvertDialog`; enabled with a selection |
| — | sep | | |
| 13 | Settings… | Ctrl+, | `showSettingsDialog()` |
| — | sep | | |
| 14 | Remove selected | Delete | `removeSelectedRows` |

A "Play" action (`action-play-selected-track`) exists but only appears in the context menu (408-410).

#### &Workspace (454-614)

| # | Text | Shortcut | Checkable | Action |
|---|---|---|---|---|
| 1 | Commands… [tr] | Ctrl+Shift+P | | Command palette |
| — | sep | | | |
| 2 | Jump to playing [tr] | Ctrl+J | | `refreshPlaybackCursor(true)` |
| 3 | Cursor follows playback [tr] | Ctrl+Shift+J | yes (`workspace/follow-playback`) | |
| — | sep | | | |
| 4 | Search… | Ctrl+Shift+F | | `openSearchDialog` |
| 5 | Quick album… [tr] | Ctrl+Shift+A (Window context) | | `openQuickPick(album)` |
| 6 | Quick track… [tr] | Ctrl+Shift+T (Window context) | | `openQuickPick(track)` |
| 7 | Lists in a side panel [tr] | — | yes (`appearance/lists-display`) | `applyListsDisplay` |
| — | sep | | | |
| 8 | Duplicate tab | Ctrl+Shift+D | | |
| 9 | Pin tab | Ctrl+Alt+P | yes | |
| 10 | Save list | Ctrl+S | | Scratch lists ask for a name ("Save working list") |
| 11 | Rename tab… | F2 | | |
| — | sep | | | |
| 12 | Close tab | Ctrl+W | | |
| — | sep | | | |
| 13 | Track list layout ▸ | | | Exclusive group: Albums with side artwork / Albums with header artwork / Plain columns / Compact queue; sep; **Columns ▸** (checkable: Artwork, Artist, Track number, Title, Album, Date, Length, Rating, Play count, Last played — `bench/bench_main_window_helpers.hpp:88-99`); Reset current list layout; Apply current layout to all queues and lists |
| — | sep | | | |
| 14 | Edit panel layout | Ctrl+Alt+L | yes | Adds a dashed highlight border on panels; status message "Panel layout editing: choose an arrangement or swap panels" (817-830) |
| 15 | Panel arrangement ▸ | | | Exclusive: Side by side / Top and bottom / Tabbed stack. Enabled only in edit mode |
| 16 | Swap panels | | | Edit mode only |
| 17 | Reset panel layout | | | |

#### &Playback (`bench/bench_transport.cpp:572-637`, 888-984)

| # | Text | Shortcut | Notes |
|---|---|---|---|
| 1 | Play / Pause (dynamic) | Space | `togglePlayPause` |
| 2 | Stop | Ctrl+. | |
| 3 | Previous | Alt+Left | |
| 4 | Next | Alt+Right | |
| — | sep | | |
| 5 | Repeat | | checkable |
| 6 | Random | | checkable |
| 7 | Single: Off/On/One-shot | | checkable, cycles |
| 8 | Album shuffle [tr] | | checkable |
| 9 | Consume: Off/On/One-shot | | checkable, cycles |
| 10 | ReplayGain ▸ | | Same menu object as the status-bar button: Off / Track / Album / Automatic (exclusive checkable); sep; Preamp… (opens Settings → Playback, focus on preamp) |
| — | sep | | |
| 11 | Desktop notifications | | checkable (`desktop/notifications`); has a tooltip |
| 12 | Playback buffer ▸ | | Exclusive: Responsive / Balanced / Resilient (tooltip "%1 ms capacity; playback starts at %2 ms"); sep; Custom… (opens Settings → Playback custom buffer) |
| 13 | Refresh audio devices | | |

#### Context and popup menus
- **Tab bar context** (`bench/bench_workspace_layout.cpp:616-626`, `bench/bench_list_tabs.cpp:1985-1993`): Rename tab…, Save list, Pin tab ✓, Duplicate tab, sep, Close tab. Right-clicking selects the tab first.
- **Track list context** (`bench/bench_list_tabs.cpp:1995-2134`). Right-click on an album header selects the whole album.
  1. Play
  2. Queue next [tr] (Ctrl+Return)
  3. Queue at end [tr] (Ctrl+Shift+Return)
  4. sep
  5. **Tools ▸** [tr]: Edit tags…, ReplayGain…, Convert files…
  6. sep
  7. Locate artist, Locate album (switch the sidebar to the engine's library)
  8. [sep, **Rate ▸**, **Rate album ▸**] — items are "Unrate" (checkable) plus star `RatingMenuAction`s for 2, 4, 6, 8, 10 (`uicommon/rating_stars.cpp:166`)
  9. sep
  10. **Copy to list ▸** / **Move to list ▸**: New tab… [tr] (asks for a name, default "Selection"), sep, every other tab's name
  11. Remove selected
  12. sep
  13. Sort list ▸, Reverse list, Shuffle albums, Remove duplicate entries
  14. sep
  15. Undo…, Redo…
  16. sep
  17. **Last.fm ▸**: disabled info item "Checking loved state…" (becomes "♥ Loved on Last.fm" / "Not loved on Last.fm"), Love track, Unlove track (`bench/bench_lastfm.cpp:491-530`)
- **Track header context** (`bench/bench_track_views.cpp:274-292`): section "Presentation", the 4 presentations, Columns ▸, sep, Reset current list layout.
- **Up Next context** (`bench/bench_up_next.cpp:215-230`): Play [tr] (`media-playback-start`), sep, Remove from Up Next, Move up, Move down, sep, Clear pending tracks, Undo.
- **Lists panel context** (`bench/bench_lists_panel.cpp:230-321`):
  - List not open: Open. List open: Close (disabled if pinned), and Save (only when scratch or dirty).
  - Then Rename…, Delete… (confirm dialog "Delete “%1”? Its files are not touched.", Delete/Cancel), sep, New list (`list-add`).
- **Folder tree context** (`bench/bench_list_tabs.cpp:2311-2334`): "Add folder to current list" / "Add file to current list"; for directories also "Expand"/"Collapse" and "Bookmark folder".
- **Folder bookmark context** (`bench/bench_workspace_layout.cpp:265-267`): Remove bookmark.
- **Device menu** (`bench/bench_transport.cpp:734-839`), tooltips visible:
  - Only when agents exist: non-selectable bold 0.9× placeholder heading "Speakers"; outputs (exclusive checkable, "(offline)", tooltips "Not connected. Chosen, it plays as soon as it is back." / "Streams the music from the engine"); sep; heading "Sound device on %1" (or "Sound device").
  - Then: System default, each device (description or name), "%1 (unavailable)" (disabled), sep, Refresh audio devices.
- **ReplayGain status menu**: listed under Playback above.
- **Dynamic playlist results context** (`bench/bench_dynamic_playlists.cpp:158-213`): Play, Queue next/end, Go to artist, Go to album, Tools ▸, Rate menus, Copy to list ▸, sep, Open editable snapshot…, sep, Last.fm ▸.
- The library panel has its own menus (`bench/local_library_panel.cpp:938-990`). They are outside this shell.

---

### 3. Keyboard shortcuts (all of `src/bench`)

User-configurable: every `action-*` that has a default key, plus every `workspace_command_ids` entry, is stored at `shortcuts/<objectName>` (`bench/bench_transport.cpp:1549-1573`). Editing UI is `bench/shortcut_settings.cpp`, with conflict check and "Restore defaults".

| Key | Action (objectName) | Where |
|---|---|---|
| Ctrl+N | New list (`action-new-list`) | `bench/bench_workspace_layout.cpp:278` |
| Ctrl+O | Open files | 282 |
| Ctrl+Shift+O | Open folder | 286 |
| Ctrl+Alt+O | Open list | 292 |
| Ctrl+Q | Quit and stop playback | 315 |
| Ctrl+F | Find in current list | 325 |
| F3 / Shift+F3 | Find next / previous | 330 / 335 |
| Ctrl+Z | Undo list edit (widget-with-children; also added to each view) | 342, `bench/bench_list_tabs.cpp:930` |
| Ctrl+Shift+Z, Ctrl+Y | Redo list edit | 348-349 |
| Alt+Return | Edit tags | 416 |
| Ctrl+, | Settings | 427 |
| Delete | Remove selected | 432 |
| Ctrl+Shift+P | Commands… | 457 |
| Ctrl+J | Jump to playing | 462 |
| Ctrl+Shift+J | Cursor follows playback | 467 |
| Ctrl+Shift+F | Search… | 478 |
| Ctrl+Shift+A | Quick album | 482 |
| Ctrl+Shift+T | Quick track | 488 |
| Ctrl+Shift+D | Duplicate tab | 505 |
| Ctrl+Alt+P | Pin tab | 511 |
| Ctrl+S | Save list | 515 |
| F2 | Rename tab | 519 |
| Ctrl+W | Close tab | 524 |
| Ctrl+Alt+L | Edit panel layout | 585 |
| Space | Play/Pause (`action-play-pause`, Window context) | `bench/bench_transport.cpp:1529` (set to Application context at 301, then overridden) |
| Ctrl+. | Stop | 1530 |
| Alt+Left / Alt+Right | Previous / Next track | 1531-1532 |
| Ctrl+Return / Ctrl+Shift+Return | Queue next / Queue at end (window-level hidden actions) | 1533-1544 |
| Ctrl+L | Search the library (`action-focus-library-search`) | 1545-1547 |
| Ctrl+Shift+U | Show Up Next (dock toggle, `action-show-up-next`) | `bench/bench_up_next.cpp:237-240` |
| Delete (Up Next view only) | Remove from Up Next | `bench/bench_up_next.cpp:211-213` |
| Escape | Close find bar | `bench/track_list_find_bar.cpp:75` |
| Enter / Shift+Enter (find bar) | Next / previous match | find bar tooltips |

- **Quick pick keys** (`bench/quick_pick_popup.cpp:286-326`):
  - Up/Down move by 1; PgUp/PgDn move by 8; Esc closes.
  - Enter = append; Shift+Enter = replace and play; Ctrl+Enter = Up Next front; Ctrl+Shift+Enter = Up Next end; Alt+Enter = new tab.
- **Command palette**: Up/Down in the filter moves the selection; Enter runs.
- Dialog-local shortcuts exist in `metadata_properties_dialog.cpp:460,468`, `metadata_artwork_section.cpp:380,407` and `musicbrainz_track_match_widget.cpp:250`. They are outside the shell.

---

### 4. Window-level behaviours

- **Geometry / window state:** `saveGeometry`, `saveState` and `restoreState` are used nowhere. The window always opens at 1100×720. What is persisted (QSettings):
  - `workspace/panel-layout-v1`: panel-layout tree with splitter weights and active tab (`bench/bench_workspace_layout.cpp:646-668, 806-815`).
    - Saved on splitter move and on tab change/move.
    - If deserializing fails, the default layout is used, the saved value is left untouched, and the status bar shows "Panel layout was not loaded (%1); the saved value was preserved" for 7 s.
  - `lists-panel/width` (default 220) and `appearance/lists-display`.
  - `up-next/visible` and `up-next/width` (default 300, clamped 260..1200).
  - `local-library/view` (`folders` / `library` / `remote`, saved only on user click), `library/bookmarks`, `library/roots`.
  - `workspace/follow-playback`, `desktop/notifications`, `desktop/notifications-background-only`.
  - `playback/local-{repeat,random,album-random,single,consume,replaygain}`, `playback/rg-preamp-{with,without}`, `playback/buffer-{profile,capacity-ms,start-threshold-ms}`.
  - `shortcuts/*`.
  - Per-tab track-view layouts are stored in the workspace SQLite database (binding `local:<id>`); Up Next is stored in the UI state `playback/up-next/v1`.
- **Panel animation** (`bench/animated_panel_dock.hpp`):
  - A `QVariantAnimation` animates the dock's fixed width over 180 ms with OutCubic easing, from 1 to the saved width (or the reverse).
  - Content sits in a clipping `PanelViewport` at its full reveal width, so it does not reflow during the animation.
  - The animation is skipped when `appearance/panel-animations` is false (default true) or the parent is hidden.
  - Target width is clamped to [260, parent width / 2]. Min width 260 afterwards.
  - Resizing saves the width via a 0 ms timer.
- **Lists display:** tabs vs side panel, as described in §1.2.
- **Follow playback** (`bench/bench_transport.cpp:1576-1609`):
  - When on, the playing row in the current tab is selected and scrolled to center.
  - "Jump to playing" also switches tab and focuses the view. If an Up Next request is active, it opens the Up Next dock instead.
- **Tab behaviours:**
  - Double-clicking empty tab-bar space runs `createList` (`bench/bench_main_window.cpp:120-135`).
  - Closing a tab returns to the previously visited tab (history of 32) (`bench/bench_list_tabs.cpp:1790-1830`).
  - Closing a pinned tab shows "Unpin this list before closing it". Closing a dirty tab asks "Discard the unsaved contents of “%1”?" (Yes/No, default No).
  - Closing the last tab creates "Untitled".
  - The playing tab shows an accent dot.
- **Command palette** (`uicommon/command_palette.cpp`):
  - QDialog "Commands", 620×480.
  - Filter line edit (placeholder "Find a command by name or shortcut…"), then a QListWidget with a custom delegate that draws the shortcut right-aligned, then a status label, then the hint "Change shortcuts in Settings → Shortcuts.", then Close/Run buttons.
  - Shows only visible, enabled actions from `workspace_command_ids` (`bench/bench_main_window_helpers.hpp:53-86`), sorted case-folded. Checked actions get " · On".
  - Matching: every typed word must appear in name + shortcut + objectName.
  - Status texts: "No matching command", "Unavailable for the current selection or connection.", statusTip, or "Press Enter to run this command."
  - Single instance (`bench/bench_workspace_layout.cpp:1089-1105`).
- **Quick pick** (`bench/quick_pick_popup.cpp`):
  - `Qt::Popup` QFrame (StyledPanel), width clamp(420, parent width − 80, 640) × 420, placed centered 90 px below the central widget's top.
  - Input font 1.1×, with placeholder text, and a scope label (library name) beside it.
  - Results list uses `AlbumRowDelegate`: 30 px rows, DemiBold name at 60% width, details in placeholder color; the selection fill is 68% Base + 32% Highlight.
  - Status label and key-hint label at 0.88×.
  - 120 ms debounce, 60-result limit. An empty query shows newest first with "added N days ago".
  - The library used is the current tab's engine, falling back to local (`bench/bench_workspace_layout.cpp:1066-1087`).
- **Notifications:** `DesktopNotifier` (opt-in, background-only option) and `MprisService` (`bench/bench_transport.cpp:2049-2155`). Transient feedback otherwise goes through `statusBar()->showMessage` (3–8 s). Startup restore uses a `QMessageBox` (`bench/main.cpp:205-208`).
- **Drag and drop targets:**
  - Main window: file URLs → `openLocalPaths` (`bench/bench_main_window.cpp:368-390`).
  - Tab bar / tab widget strip (`bench/bench_main_window.cpp:136-275`):
    - Rows dropped on a tab move; with Ctrl they copy.
    - Rows dropped on empty bar space make a new tab named "Selection".
    - Files from a library or file manager go into that tab, or into a new tab named "Dropped files", the folder name, or "Library selection".
  - Each track view: reorder, cross-view transfer, library files, URLs (`bench/bench_list_tabs.cpp:989-1074`).
  - Lists panel items: open lists behave like tabs; unopened lists are opened first and receive a copy (`bench/bench_main_window.cpp:297-366`).
  - Up Next pill and Up Next view.
  - Drag sources: folder tree, track views, Up Next view.
- **Close:** `closeEvent` closes tag-editor windows first (any of them can cancel), then stops background work and persists synchronously (`bench/bench_main_window.cpp:78-91`). "Close window" keeps the engine playing; "Quit" retires engines and calls `QCoreApplication::quit` (`bench/bench_list_tabs.cpp:1568-1589`).

---

### 5. Styling approach

- **No application-wide QStyle, palette or stylesheet** is set (`bench/main.cpp` sets only org/app names). The desktop style and palette are used; the comments say the design was done under Kvantum.
- All custom colors are derived from the palette through mixes:
  - Window/Text at 6, 8, 12, 16 and 18%
  - Highlight at 35%
  - Base at alpha 110
  - `PlaceholderText` is used as the "quiet" text role everywhere.
- **Local stylesheets:**
  - Play button, header pills, badge: `bench/bench_transport.cpp:397-402, 489-497, 560-562`
  - Status bar, mode buttons, ReplayGain button: 1004-1032
  - Source tab bar: `bench/bench_workspace_layout.cpp:143-150`
  - Layout-edit dashed border: 819-822
  - Up Next footer: `bench/bench_up_next.cpp:151-155`
- **Proxy styles and custom painting:**
  - `PlaybackTabBar` + `ButtonPlacement` QProxyStyle (`bench/playback_tab_widget.hpp:90-248`), fully custom paint:
    - Tab width = 12 + content + 12 + 13 + 10; height = font height + 16.
    - The current tab is filled with Base in a rounded rect (radius 5); a hovered tab uses Base at alpha 110.
    - Text is centered: Text color when current, PlaceholderText otherwise.
    - The playing tab gets a 3.5 px Highlight dot. Icon 14 px.
    - Custom `TabCloseButton` (16×16 painted cross), always on the right.
    - `playbackSpeakerIcon` helper (21-42).
  - `FlatHeaderStyle` (`uicommon/flat_header_style.hpp`) for track-view headers:
    - Flat on Base; bold labels at 58.6% Text; 18 px separators at 15.7%; no bottom rule; height = font height + 14.
    - Installed at `uicommon/queue_table_view.cpp:331`.
  - `DelegateSelectionStyle` (`uicommon/delegate_selection_style.hpp`) suppresses the style's selected-row panel. Installed at `uicommon/queue_table_view.cpp:330` and `uicommon/library_tree_view.cpp:32`.
  - `ui::LineSlider` (`uicommon/line_slider.cpp`): a flat 4 px bar. Track is Text at alpha 0.28 (0.16 when disabled); progress is Highlight (alpha 0.35 when disabled); no handle; clicking jumps to the position; sizeHint height 18.
  - `ui::ElidingLabel` for the header text lines. `UpNextDelegate`, `QueueItemDelegate` (album headers, badges) and `CommandDelegate` paint their own rows.
- **Fonts:** only relative scaling of the default font:
  - Title: DemiBold ×1.08
  - Pills: ×0.92
  - Badge: ×0.85
  - Headings: bold ×0.85–0.9
  - Placeholder "♪": ×1.5
  - Quick pick input: ×1.1; quick pick hints: ×0.88
- **Icons:**
  - Qt standard icons: `SP_MediaSkipBackward`, `SP_MediaPlay`, `SP_MediaPause`, `SP_MediaStop`, `SP_MediaSkipForward`, `SP_MediaVolume`, `SP_MediaVolumeMuted`, `SP_ComputerIcon`, `SP_ArrowDown`, `SP_BrowserReload`.
  - Freedesktop theme icons used in the shell: `audio-volume-muted`, `audio-volume-high`, `audio-speakers-symbolic`, `audio-speakers`, `pan-down-symbolic`, `arrow-down`, `media-playlist-repeat`, `media-playlist-shuffle`, `media-playlist-repeat-song`, `edit-clear-list`, `view-media-equalizer`, `folder`, `network-server`, `list-add`, `list-remove`, `go-up`, `go-down`, `go-next`, `edit-clear`, `edit-undo`, `window-close`, `media-playback-start`.
  - Custom-painted icons: album shuffle, one-shot dot overlay, playing speaker, tab close cross.

---

## Track lists, tabs, Up Next and dynamic playlists

## Trackknife track list, tabs and view engine: parity inventory (Qt Widgets to Qt Quick)

Repo: `/home/carnager/Code/melody-next-engines`. All paths are relative to `src/`.

**Four things that differ from what the request assumed:**
1. **Columns are fixed.** There are no tkfmt column expressions. The 10 columns are hard-coded and can only be reordered, resized and shown or hidden. tkfmt is used in only one place in this UI: sorting the list (§3).
2. **There is no track-layout editor dialog.** `Ctrl+Alt+L` is **"Edit panel layout"**, a checkable toggle for the workspace panel arrangement (`bench/bench_workspace_layout.cpp:582-586`). Track layout is edited only through menus (§1.6).
3. **Clicking a column header does not sort.** `setSortingEnabled` is never called.
4. **There are no hover actions on rows.** The delegate strips hover state (`queue_item_delegate.cpp:177`). The `trackknife-hover-row` property is set to -1 at `queue_table_view.cpp:336` and `bench_list_tabs.cpp:945` and never updated.

---

### 1. Track-view layout engine

#### 1.1 Layout model: `uicommon/track_view_layout.hpp/.cpp`
- Schema version is 2 (`hpp:16`). Version 1 album-side layouts are migrated by hiding artist, album and date, but only if some other column stays visible (`cpp:202-219`).
- **`TrackViewPresentation`** (`hpp:18-23`) has four values, serialized as:
  - `albums-side-artwork`
  - `albums-header-artwork`
  - `plain-columns`
  - `compact-queue`
- **Column entry:** `{id, width (default 100), visible}`. The order of the vector is the visual order.
- **Serialized JSON:** `{"schema":N,"presentation":"…","columns":[{"id","width","visible"}]}` (`cpp:105-119`).
- **Validation** (`cpp:121-223`):
  - width must be 24–4096
  - total size at most 64 KiB
  - no unknown or duplicate ids
  - at least one column visible
  - columns registered later are appended hidden with width 100
- **Validation error strings** (`cpp:131-193`):
  - "Track-view layout size is invalid"
  - "Track-view column registry is invalid"
  - "Invalid track-view layout JSON"
  - "Unsupported track-view layout version"
  - "Track-view presentation is unsupported"
  - "Track-view columns are malformed"
  - "Track-view layout does not place every registered column"
  - "Track-view column entry is malformed"
  - "Track-view layout references an unavailable or duplicate column"
  - "Track-view column attributes are malformed"
  - "Track-view column width is outside the supported range"
  - "Track-view layout must place every column and show at least one"
- **Persistence:**
  - Each tab saves a `TrackViewPreset` with binding `"local:<documentId>"` (`bench/bench_list_tabs.cpp:446-460`), restored at `:162-168` and `:1098-1113`.
  - If a saved layout fails to decode, the default is shown and the original bytes are kept (`view_layout_persistence_protected`). The status bar shows "Track layout was not loaded (%1); the saved value was preserved" for 7 s (`bench_list_tabs.cpp:1108-1111`).

#### 1.2 Column registry
Specs are in `bench/bench_main_window_helpers.hpp:88-99` as {id, label, default width, minimum width}. Header text comes from `uicommon/track_row_roles.hpp:294-295`.

| # | id | Menu label | Header text | Default width | Minimum width |
|---|---|---|---|---|---|
| 0 | artwork | Artwork | "" | 110 | 72 |
| 1 | artist | Artist | Artist | 150 | 72 |
| 2 | track-number | Track number | # | 46 | 36 |
| 3 | title | Title | Title | 220 | 96 |
| 4 | album | Album | Album | 160 | 72 |
| 5 | date | Date | Date | 64 | 52 |
| 6 | length | Length | Length | 68 | 56 |
| 7 | rating | Rating | Rating | 84 | 56 |
| 8 | play-count | Play count | Play count | 88 | 60 |
| 9 | last-played | Last played | Last played | 170 | 100 |

**Model roles** (`track_row_roles.hpp:254-273`):
- source bytes
- id
- position
- duration_ms
- current (playing)
- album_artist (falls back to artist)
- album artwork QImage
- artwork key
- album_group_start
- rating (0–10)
- album_rating
- disc_start ("Disc 2" or "Disc 2 · subtitle")

**Cell content** (`bench/local_list_model.cpp:940-1011`):
- Title falls back to the escaped file name.
- Length is formatted `m:ss`, or `h:mm:ss` when there are hours.
- Rating is star text: ★ per full star plus ½ (`track_row_roles.hpp:299-311`), coloured by `ForegroundRole` = `ratingStarColor()` rgb(245,197,24).
- Tooltip on every cell is the escaped raw path.
- Play count is right-aligned; last played is left-aligned.
- **Play count / Last played** (`bench/local_list_history.cpp:42-83`) are loaded lazily in batches of 64.
  - Placeholder "…" while loading; tooltip "Loading listening history…".
  - "—" when unavailable. Tooltips: "Listening history service is unavailable." / "Listening history is unavailable until a source revision is known."
  - "Never" when there is no listen; tooltip "No qualified local listens recorded."
  - Otherwise tooltip "Local play count: %1. Last qualified listen: %2" (ISO date). The cell uses a locale short date.
  - These two columns are forced hidden when the model is not a `LocalListModel` (`bench_track_views.cpp:118-122`).

#### 1.3 Presets: the defaults for each presentation
Defined in `bench/bench_track_views.cpp:27-60`. In every preset, Rating, Play count and Last played are hidden by default (`visible = logical < rating`).

- **Albums with side artwork** (the default for new tabs): shows Artwork (110), #, Title, Length. Artist, Album and Date are hidden.
- **Albums with header artwork:** shows Artwork (width **42**), Artist, #, Title, Album, Date, Length.
- **Plain columns:** shows Artist, #, Title, Album, Date, Length. Artwork is hidden.
- **Compact queue:** shows only Artist, #, Title, Album, Length.

**Applying a layout** (`bench_track_views.cpp:66-142`):
- Grouped means either albums presentation. Grouped views use `QueueItemDelegate`; plain views use `QStyledItemDelegate`.
- Alternating row colours are on only when **not** grouped (`:87`).
- Row height: default 22, minimum 18 (`:96-97`).
- Side artwork sets the album-artwork column to Artwork (`:125`).
- **Auto-fill columns:** Artist, Title and Album expand; the rest keep their preferred width; every column keeps its spec minimum (`:136-137`). The algorithm is in `queue_table_view.cpp:588-679`:
  - Expanding columns split the remaining width in proportion to (preferred − min).
  - If the viewport is too narrow, every column takes part.
  - The last expanding column takes whatever is left.
  - When the user resizes a column, that width becomes its new preferred width (`:339-346`).

#### 1.4 Grouped rendering: `uicommon/queue_item_delegate.*`, `queue_table_view.cpp`
**Constants:**
- album header height 34
- loose-run gap 10
- hairline offset 5
- disc header height 26 (`queue_item_delegate.hpp:173-181`)
- track row height 22; header-artwork cover 22 px (`queue_item_delegate.cpp:218-219`)
- side artwork maximum 160 px, padding 6 (`queue_table_view.cpp:41-42`)

**Groups:**
- Group key = album_artist + album + date (`queue_item_delegate.cpp:234-241`).
- A group needs at least 2 consecutive rows. Single tracks are "loose".

**Album header** (`queue_item_delegate.cpp:305-369`):
- The album name is DemiBold at ×1.08 size in the Text colour.
- 12 px after it, the details line is drawn in PlaceholderText: `"artist · date · N tracks · h:mm:ss"`, joined with " · ".
  - Fallbacks: "Unknown artist", "Unknown album".
  - Count wording: "1 track" / "%1 tracks".
- Baseline is at rect.bottom − 8. Horizontal padding is 6 on the left and 8 on the right.
- The header is filled with the Base colour.
- Between groups there is a hairline: the Text colour at alpha 38, drawn 5 px below the top.

**Side artwork mode** (`queue_table_view.cpp:100-221`):
- The view paints the headers and covers as an overlay.
- When the cover column is the first visible column, the header sits beside the cover, and the album text lines up with where the rows' text starts. Otherwise the header spans the whole row.
- The hairline runs the full width.
- **Cover:**
  - fills the column width minus padding, capped at 160 and at the group's height
  - top-left anchored, aspect-fitted, rounded 3 px, smooth scaling
  - with no cover: a Mid-coloured rounded rect with a `media-optical-audio` icon (at least 16 px, or a third of the extent), Disabled mode
- **Album rating** is painted over the cover (`rating_stars.cpp:747-786`):
  - a band along the bottom, height = cover height / 6 clamped to 9–18, fill black at alpha 140
  - yellow 5-point stars with inner radius 0.42, a half star clipped
  - skipped when the cover is smaller than 24 px

**Header artwork mode** (`queue_item_delegate.cpp:402-433`):
- A 22 px cover sits at left+6 in the artwork cell of the header strip.
- The header text is drawn in the Title column.

**Inside a group** (`:502-516`):
- Album and Date cells are blanked.
- The Artist cell is blanked when it equals the album artist. Otherwise it is drawn in PlaceholderText (compilations).

**Loose tracks in side mode** (`:517-529, 584-605`):
- The cover (≤ 20 px) is drawn inline, right-aligned, in the # column. If that column is hidden, it goes in the artwork column.
- The number text is removed.

**Title suffix** (`:530-583`):
- When the Artist or Album column is hidden, the title gets `" — " + artist[ · album]` appended in PlaceholderText.
- The artist appears only when it differs from the album artist or the track is loose.
- The album appears only for loose tracks.

**Disc strip** (`:446-463`):
- A 26 px strip above the first row of each disc, only when the album has more than one disc.
- Text is DemiBold at ×0.92 in PlaceholderText, bottom-left, in the Title column.
- Label comes from `local_list_model.cpp:1047-1083`: "Disc N" or "Disc N · subtitle".

**Track number column:**
- Right-aligned, in PlaceholderText.
- Zero-padded to 2 digits; the "/total" part is stripped (`:257-269, 494-501`).
- `track_separate_number_property = true` in the bench, so the title does not include the number.

**Row geometry** (`queue_table_view.cpp:476-565`):
- Group-start rows get extra fixed height: +34 for an album, +10 for a loose run, +26 for a disc start.

#### 1.5 Selection, playing row, focus and empty list
- **Selection** (`queue_item_delegate.cpp:466-477`): drawn as a tint, Base blended 32 % toward Highlight. Text keeps its own colour. The artwork/cover gutter is never highlighted (`:388-393`, `delegate_selection_style.hpp`).
- **Playing row** (`:478-493`):
  - DemiBold font.
  - Text colour = Highlight.lighter(115), except cells with their own foreground colour (the stars).
  - A 14 px `media-playback-start` icon, in the Title cell (side artwork) or the artwork cell (other presentations).
- **Keyboard focus** (`queue_table_view.cpp:225-252`): one outline around the whole current row in Highlight at alpha 200, only while the view has focus. It is inset below any group spacing and starts after a leading cover column.
- **Empty list** (`:1017-1040`):
  - Title at ×1.15 DemiBold; hint in PlaceholderText, word-wrapped.
  - Positioned at 2/5 of the view height.
  - Local: "This list is empty" / "Drop files or folders here, or add albums from the library on the left."
  - Remote: "Nothing from %1 here yet" / "Add albums from %1's library on the left, or drag them here." (`bench_list_tabs.cpp:1488-1498`)
- **View chrome** (`queue_table_view.cpp:325-351`):
  - No frame.
  - `FlatHeaderStyle` (`uicommon/flat_header_style.hpp`): Base ground, bold labels at 58.6 % of the way from Base to Text, 18 px separators at 15.7 %, height = font height + 14, 6 px label margin, no bottom rule.
- **Table setup** (`bench_list_tabs.cpp:945-975`):
  - no grid, row selection, extended selection, no editing
  - per-pixel scrolling, text elided on the right, vertical header hidden
  - header sections movable and interactively resizable, no stretch-last, minimum 24, maximum 4096

#### 1.6 Header behaviour and layout menus
- **Reorder** by dragging a header section; **resize** by dragging a separator. Either change is captured and persisted (`bench_list_tabs.cpp:1115-1143`).
- **Header right-click menu** (`bench_track_views.cpp:274-292`):
  - section "Presentation"
  - the four presentation radio actions
  - "Columns" submenu with a checkbox per column
  - separator
  - "Reset current list layout"
- **Workspace menu › "Track list layout"** (`bench_workspace_layout.cpp:527-580`):
  - exclusive radio actions: "Albums with side artwork", "Albums with header artwork", "Plain columns", "Compact queue"
  - separator
  - "Columns" submenu
  - "Reset current list layout"
  - "Apply current layout to all queues and lists"
- **Behaviour:**
  - Choosing a presentation **replaces** the tab's layout with that preset's defaults (`bench_track_views.cpp:174-183`).
  - The last visible column cannot be hidden (`:185-206`).
  - Showing Play count or Last played triggers a history reload.
  - Hidden columns keep their width.

#### 1.7 Ratings behaviour
- The Rating column is display only; clicking it does nothing.
- Ratings are set from the track context menu's **"Rate"** and **"Rate album"** submenus (`bench_list_tabs.cpp:2208-2309`):
  - Items: "Unrate", then 1–5 stars in whole-star steps (values 0, 2, 4, 6, 8, 10).
  - Each is a painted 5-star strip (`rating_stars.cpp:790-874`): 16 px stars, 3 px gap, 24 px check margin, 26 px height, Highlight band on hover, a check mark on the value the selection shares.
  - Labels from `ratingMenuLabel`: "Unrate", "½ star", "1 star", "%1½ stars", "%1 stars".
  - "Rate album" is disabled when no album hash exists. Both are disabled when there is no library store.
  - A rating is stored on the engine that owns the tab and is pushed to every tab of that engine.

#### 1.8 Status bar selection summary (`bench_track_views.cpp:317-412`)
- Nothing selected: "No tracks selected".
- One track: `"Artist — Title · Album (Date) · m:ss"`, with the raw path as tooltip.
- Several: `"%1 tracks selected · %2 total"`, plus `" · %1 duration unknown"` when any duration is missing.

---

### 2. Tab bar (`bench/bench_list_tabs.cpp`, `bench/playback_tab_widget.hpp`, `bench/bench_workspace_layout.cpp:80-103`)

#### 2.1 Appearance: `PlaybackTabBar` (`playback_tab_widget.hpp:90-248`)
- Document mode, movable.
- **Tab width** = lead 12 + content + slack 12 + close room 13 + trail 10.
  - Content = text + playing dot (11) + icon (14 px + 5 gap).
- **Tab height** = font height + 16.
- **Current tab:** filled with Base as a rounded (5 px) shape open at the bottom. A hovered tab gets the same fill at alpha 110. Other tabs have no fill.
- **Text:** Text colour for the current tab, PlaceholderText for the rest. Content is centred.
- **Playing tab:** a Highlight-coloured dot of radius 3.5 before the text (`tabData == true`, the active playback list).
- **Icon:** `network-server` for remote-engine tabs, drawn Disabled when the tab is not current. Local tabs have no icon.
- **Close button** (`TabCloseButton`):
  - 16×16 cross with arm 3, pen 1.8, PlaceholderText colour (Text on hover)
  - tooltip "Close tab"
  - always on the right, centre 14 px from the tab's right edge
  - hidden for pinned tabs, but its space is kept (`bench_list_tabs.cpp:1750-1752`)
- **Bar tooltip:** "Drop local tracks on a tab to transfer, or on empty tab-bar space to create a tab. Hold Ctrl to copy." (`bench_workspace_layout.cpp:89-91`)

#### 2.2 Tab text and tooltips (`refreshTabChrome`, `:1723-1753`)
- Text is the name, plus `" *"` when dirty.
- Tooltip:
  - kind: "Persistent scratch list" (scratch) or "Named Trackknife working list" (saved)
  - then " · pinned", " · modified", " · Active playback queue" as applicable
- The view's accessible name is "%1 track list".

#### 2.3 Grouping by engine (ADR-0234)
- Tabs are kept grouped by engine in engine order, this computer's first (`:688-709`).
- A tab dragged into another engine's group is moved back.
- A new tab is inserted at the end of its engine's group (`:1076-1084`).
- A remote engine's first tab is created on connect and named after the engine (`bench/bench_remote.cpp:127-150`).
- There is no textual engine label on tabs; only the icon and tooltip ("Plays on the remote engine", `:1089`).

#### 2.4 List kinds
- `scratch` is a working list; `saved` is named.
- `pinned`: the close button is hidden; closing shows "Unpin this list before closing it" (3 s).
- `dirty` shows " *".
- The retired `mpd` kind is skipped on restore.

**Names of new tabs:**
- Initial or last-closed tab: "Untitled"
- Library search: "Search: %1"
- Library selection: "Library selection"
- File drops: "Dropped files", or the folder name when one item is dropped
- Copy/Move to new tab: "Selection"
- Dynamic playlists: "Dynamic playback", "<name> — playback", "Dynamic playlist snapshot"
- Import: "Imported playlist"
- Conflict copy: "%1 (mine)"

#### 2.5 Tab actions (Workspace menu, `bench_workspace_layout.cpp:502-524`)
**Tab context menu** (`:616-626`): Rename tab…, Save list, Pin tab, Duplicate tab, separator, Close tab.

| Action | Shortcut | Behaviour |
|---|---|---|
| "Duplicate tab" | Ctrl+Shift+D | Name "%1 copy"; unpinned; dirty; copies the layout (`:1911-1932`) |
| "Pin tab" (checkable) | Ctrl+Alt+P | `:1934-1943` |
| "Save list" | Ctrl+S | A scratch list asks for a name first; then clears dirty (`:1945-1965`) |
| "Rename tab…" | F2 | Marks the tab dirty (`:1967-1983`) |
| "Close tab" | Ctrl+W | See below |
| "New list…" (File menu) | Ctrl+N | Kind `saved` (`:1889-1909`) |
| "Open list…" (File menu) | Ctrl+Alt+O | See below |

Dialog strings:
- Save list: "Save working list" / "Name:"
- Rename: "Rename list" / "Name:"
- New list: "New list" / "Name:"

**Close** (`:1816-1885`):
- A dirty tab asks "Close unsaved list" / "Discard the unsaved contents of “%1”?" (Yes/No, default No).
- After closing, the previously visited tab becomes current (history of 32).
- Closing the last tab creates a new "Untitled".
- If the closed tab was playing, it keeps playing (`detached_playback_`).

**Double-clicking empty tab-bar space** creates a new list (`bench_main_window.cpp:120-134`).

**Open list dialog** (`:711-801`):
- Title "Open list"; a tree with columns "List" and "Tracks", grouped by engine ("This computer" or the engine name).
- Saved lists first; working lists get " (working)".
- Messages: "No lists", "Cannot say: %1".
- Buttons: Open / Cancel.

**Conflict dialog** (`:845-898`):
- Title "List changed elsewhere": "“%1” was saved from somewhere else since you opened it. Keep which?"
- Buttons: "Reload theirs", "Keep mine", "Save mine as a copy" (default).

#### 2.6 Dropping on tabs (`bench_main_window.cpp:136-290`)
- **Rows dropped on a tab:** move by default; Ctrl copies; rows from a dynamic playlist are always copied. The target tab becomes current.
- **Rows dropped on empty bar space:** a new tab named "Selection".
- **Files or library entries:** dropped on a tab they are appended to it; on empty space they go into a new tab of their engine.

#### 2.7 Lists display setting (ADR-0233)
- **Setting:** `appearance/lists-display` = `tabs` or `panel`.
  - Workspace menu: "Lists in a side panel" (checkable, `bench_workspace_layout.cpp:494-501`).
  - Settings dialog: "Show lists as:" with "Tab bar" / "Side panel" (`settings_dialog.cpp:216-223`).
- **Panel mode** hides the tab bar and shows the pane (`bench_lists_panel.cpp:119-133`).
- **Pane** (`bench_lists_panel.cpp:32-112`):
  - Header: bold "Lists" plus a `list-add` tool button (tooltip "New list").
  - Default width 220, remembered as `lists-panel/width`.
  - Engine lists are fetched with `list.all`, debounced 150 ms.
- **`ListsPanel` tree** (`lists_panel.cpp`):
  - Two columns (name, track count right-aligned and quiet); no header; indentation 10; no frame; single selection; accepts drops.
  - Group headings are bold at ×0.85: "This computer", the engine name, or "Remote", plus "Other tabs" for non-list tabs such as tag editors.
  - A working list is shown in *italic*; a dirty list gets " *"; the playing list gets a `media-playback-start` icon.
  - Tooltip: "Saved list" / "Working list, not saved", then " · open here", " · modified".
  - Empty group: "No lists" or "No lists: %1". An unreachable engine gives "Not connected".
  - Order: saved lists first, sorted by name (`bench_lists_panel.cpp:193-199`).
  - Choosing a row opens or shows the list.
- **Pane context menu** (`bench_lists_panel.cpp:230-321`):
  - "Open" (not open here) or "Close" (disabled when pinned)
  - "Save" (when scratch or dirty)
  - "Rename…"
  - "Delete…": "Delete list" / "Delete “%1”? Its files are not touched." with buttons Delete / Cancel
  - separator
  - "New list"
- Tracks dropped on a list in the pane are added to it (`dropOnPanelList`).

---

### 3. List editing

#### 3.1 Track context menu (`bench_list_tabs.cpp:1995-2134`)
Right-clicking an album header selects the whole album. Right-clicking an unselected row selects it.

Menu order:
1. "Play"
2. "Queue next" (Ctrl+Return), "Queue at end" (Ctrl+Shift+Return)
3. separator
4. "Tools" submenu: "Edit tags…" (Alt+Return), "ReplayGain…", "Convert files…"
5. separator
6. "Locate artist", "Locate album" (disabled when that engine's library is unavailable)
7. "Rate", "Rate album"
8. separator
9. "Copy to list" and "Move to list" submenus: "New tab…" (asks "New tab" / "Name:", default "Selection"), a separator when there are other tabs, then every other tab by name
10. "Remove selected" (Delete)
11. separator
12. "Sort list" submenu, "Reverse list", "Shuffle albums", "Remove duplicate entries"
13. separator
14. Undo, Redo
15. separator
16. "Last.fm" submenu: "Checking loved state…", "Love track" / "Unlove track" (`bench_lastfm.cpp:503-517`)

#### 3.2 Edit menu (`bench_workspace_layout.cpp:319-430`)
| Item | Shortcut | Notes |
|---|---|---|
| "Find in current list…" | Ctrl+F | |
| "Find next in list" | F3 | |
| "Find previous in list" | Shift+F3 | |
| "Undo list edit" | Ctrl+Z | Text becomes "Undo %1" (the edit's label), or "Undo Move tracks between tabs" |
| "Redo list edit" | Ctrl+Shift+Z, Ctrl+Y | Same pattern as Undo (`bench_list_tabs.cpp:2343-2369`) |
| **"Sort list"** submenu | | See below |
| "Reverse list" | | |
| "Shuffle albums" | | Tooltip "Reorder the whole list by album; retain each album's existing track order. Random playback is unchanged." |
| "Remove duplicate entries" | | Tooltip "Keep the first occurrence of each exact local source and logical track" |
| "Edit tags…" | | |
| "ReplayGain…" | | |
| "Convert files…" | | |
| "Settings…" | Ctrl+, | |
| "Remove selected" | Delete | |

**"Sort list" presets:**
- "Title" → `%title%`
- "Artist / album / track" → `$if2(%albumartist%,%artist%)|%album%|%discnumber%|%tracknumber%|%title%`
- "Album / track" → `%album%|%discnumber%|%tracknumber%|%title%`
- "Track number" → `%discnumber%|%tracknumber%`
- "Path" → `$info(path)`
- separator, then "Custom expression…"

Sort, Reverse, Shuffle and Dedupe are enabled only when the list has more than 1 row. There is no Crop or Randomize-tracks command.

#### 3.3 Edit bar at the bottom (`bench/local_list_edit_bar.cpp`)
- Toolbar "Edit local list".
- Expression field: default `%title%`, accessible name "tkfmt-1 sorting expression", tooltip "Sort the entire list using tkfmt-1. Example: %album%|%tracknumber%|%title%".
- Direction combo "Ascending" / "Descending".
- "Sort list" button (Enter in the field also sorts).
- Status label.
- "Close" button, which reads "Cancel" while an edit runs.
- Status strings:
  - "Preparing list edit…"
  - "Reading cached rows… %1 / %2"
  - "Preparing edit… %1 / %2 rows"
  - "List changed — run the command again"
  - "Previous edit is stopping — run the command again"
  - "List edits support at most one million rows"
  - "List edit exceeds the 64 KiB per row or 128 MiB snapshot limit"
  - "No changes needed"
  - "%1 completed — Undo is available" / "%1 completed"
- Edit labels: "Sort list", "Reverse list", "Shuffle albums", "Remove duplicate entries".

#### 3.4 Undo history (`bench/local_list_model.cpp`)
- Per-list history: at most 100 entries and 64 MiB (`:438`).
- Default labels: "Replace list contents", "Add tracks", "Remove tracks", "Reorder tracks".
- Discard messages go to the status bar:
  - "Older list edits were discarded to keep undo history bounded."
  - "List undo was cleared because a removed logical track needs fresh metadata."
  - "List undo was cleared because a removed file's revision changed."
- Undo or redo reselects the restored rows (`historyRowsRestored`, `bench_list_tabs.cpp:914-923`).
- A move between two tabs has its own single-step undo (`:1385-1422`).
- **Note:** despite its name, `local_list_history.cpp` is the listening-history (play count) loader, not undo.

#### 3.5 Find bar (`bench/track_list_find_bar.cpp`)
- Toolbar "Find in list" at the bottom.
- Field placeholder "Find in current list"; tooltip "Search any metadata value, the duration, the audio format, or the source path/URI"; clear button.
- Buttons and tooltips:
  - "Previous" — "Previous match (Shift+Enter or Shift+F3)"
  - "Next" — "Next match (Enter or F3)"
  - "Close" — "Close find (Escape)"
- Search-as-you-type with a 150 ms debounce.
- Case-insensitive (Unicode simple lower) over every tag value, the codec, sample rate, bits, channels, duration and path.
- It wraps around. A match selects the row and scrolls to it. Esc in the list or the field closes the bar and returns focus to the list.
- Status strings:
  - "Searching…" / "Searching… %1 of %2"
  - "Track %1 of %2" / "Wrapped · Track %1 of %2"
  - "No matches"
  - "List is empty"
  - "List changed — search again"
  - "Selection changed — search again"
  - "Invalid search text"
  - "Search stopped: track %1 exceeds find limits (64 KiB / 1024 tags)"
  - "Search stopped: invalid metadata text at track %1"

#### 3.6 Playlist transfer bar (`bench/playlist_transfer_bar.cpp`, `bench_playlists.cpp`)
- File menu: "Import M3U8 playlist…" and "Export list as M3U8…".
  - Export tooltip: "Export local whole-file references, titles and durations. Other cached metadata and logical selections cannot be stored in M3U8."
- Dialogs:
  - Import: "Import M3U8 into a new local list"
  - Export: "Export M3U8 to a new file", filename label "New file (references, titles and durations only):"
  - Filter: "UTF-8 playlists (*.m3u8)"
- Bar "Portable playlist" with a status label and a Cancel/Close button.
- Strings:
  - "%1… %2 entries"
  - "Importing playlist" / "Exporting playlist"
  - "Cancelling playlist transfer"
  - "Playlist transfer cancelled" / "Playlist import cancelled"
  - "Playlist exceeds 100,000 entries"
  - "List changed during capture; export again"
  - "Playlist exceeds the in-memory transfer limit"
  - "Imported %2 entries" / "Exported %2 entries" (from `"%1 %2 entries"`)
- An import opens a new **saved** tab.

#### 3.7 Drag and drop (`uicommon/queue_table_view.cpp`)
- **Drag pixmap:**
  - a Highlight rounded pill (radius 6) with bold text: "Move 1 track" / "Move %1 tracks", or "Copy %1 tracks" from a dynamic-playlist view
  - hotspot at (12, h/2) (`:849-916`)
- **Drop target position:**
  - the upper half of a row inserts before it, the lower half after it
  - dropping on an album header inserts at the start of that album
  - empty space appends (`:729-749`)
- **Drop indicator** (`:254-313`):
  - Base halo at alpha 220 (7 px) with a 3 px Highlight line, inset 8 px each side
  - a Highlight triangle arrow at the left edge
  - a rounded (5 px) Highlight badge on the right, bold, saying "Move here · position N" / "Copy here · position N" / "Move to end" / "Copy to end"
  - accessible description "%1. The highlighted line is the exact track insertion position"
- **Sources:**
  - within the same view: reorder, pushed to undo
  - another table view: transfer, copy or move per the drop action; Qt default is move, Ctrl for copy
  - dynamic-playlist rows: copy only
  - library `LocalFilesMimeData` or file-manager URLs: inserted (discovery), copy
- **Across engines**, paths are translated. Status messages:
  - "%1 of %2 tracks are not in %3's library, so it cannot play them; they were left out"
  - "…not reachable on this computer…Settings → Engine." (`bench_list_tabs.cpp:1642-1668`)
- "A file intake is already running" is shown when another intake is busy.

#### 3.8 Keyboard (`queue_table_view.cpp:751-795` and actions)
- Home / End jump to the first / last row; Shift+Home / Shift+End extend the selection.
- Enter (no modifier) or double-click plays the row.
- Delete removes the selected rows.
- Ctrl+Return / Ctrl+Shift+Return queue the selection next / at the end.
- Alt+Return opens tag editing.
- Ctrl+J jumps to the playing track.
- Ctrl+Shift+J toggles "Cursor follows playback" (setting `workspace/follow-playback`).
- Space is play/pause.
- Clicking an album header selects the whole album.

---

### 4. Up Next (`bench/bench_up_next.cpp`, `bench/up_next_delegate.cpp`)
**Dock:**
- `AnimatedPanelDock` titled "Up Next", left or right area, default right, hidden by default (`up-next/visible`).
- Toggle: `action-show-up-next`, Ctrl+Shift+U.
- A header pill button "Up Next" with a count badge (Highlight, radius 9, 18 px high, shown only when count > 0). Accessible name "Up Next, %1 waiting". Tracks can be dropped on the pill (`bench_transport.cpp:217-228, 536-568`).

**Header area:**
- "Up Next" in DemiBold ×1.08.
- Status line in PlaceholderText: `"<This computer|engine name> · %1 waiting"`, tooltip "Playing: Artist — Title".
- A close button (`window-close`, tooltip "Close Up Next").

**Rows** (`up_next_delegate.cpp`):
- 40 px high; only the title column is shown; no table header.
- Cover 32 px, rounded 2 px. Without a cover: a rect in Base mixed 10 % toward Text.
- Title on the first line; artist below in a font 1 pt smaller (minimum 7 pt) in PlaceholderText; length right-aligned in the small font.
- Selection tint 32 % toward Highlight. Padding 8/4.

**Footer:**
- Hairline border-top (Window mixed 12 % toward Text).
- Icon-only toolbar:
  - "Remove from Up Next" (`list-remove`, Delete)
  - "Move up" (`go-up`)
  - "Move down" (`go-down`)
  - separator
  - "Clear pending tracks" (`edit-clear`)
  - "Undo" (`edit-undo`)
- Right-aligned button "Back to the list" / "Back to %1" (elided), `go-next` icon on the right, tooltip "Skip what is waiting and return to the list now". It clears the queue and skips.

**Interaction:**
- Context menu: "Play" (moves the item to the front and skips to it), separator, then the toolbar actions.
- Enter or activation plays the item.
- Drag to reorder; drop rows from lists or library entries into it.
- Empty state: "Nothing waiting" / "Drag tracks here from a list or the library."

**Limits and messages:**
- At most 500 tracks: "Up Next holds at most 500 tracks."
- One engine at a time: "Up Next holds tracks from %1; finish or clear it before adding tracks from %2".
- "Added %1 to Up Next" (3 s).
- Persisted at `playback/up-next/v1`.

---

### 5. Dynamic playlists (`bench/bench_dynamic_playlists.cpp`, `dynamic_playlist_dialog.cpp`, `dynamic_playlist_service.*`)
**Window:**
- File menu "Dynamic playlists…". Non-modal dialog "Dynamic playlists", 900×720.
- Intro text: "Save rules or a Last.fm source, then refresh to see the matching tracks. Rules update while this window is open. Last.fm refreshes draw a fresh selection when requested. Opening a snapshot keeps that list stable while you listen."

**Top row:**
- Library combo ("This computer" or engine names).
- Catalogue combo: "New dynamic playlist…" then the saved definitions.
- "Save definition" and "Remove" buttons.

**Form** (rows shown per source):
- "Name:"
- "Source:" with "Library rules", "Last.fm similar tracks", "Last.fm loved tracks", "Last.fm top tracks (all time)", "Last.fm tag tracks"
- "Rules:" — placeholder "genre HAS rock AND rating GREATER 6"; tooltip "Ratings use 0–10; 8 means four stars. Example: genre HAS jazz SORT BY %album%"; default rule `rating GREATER 6`
- "Seed artist:", "Seed track:" (similar)
- "Last.fm user:" (loved/top)
- "Last.fm tag:" (tag)
- "Maximum tracks:" 1–500, default 100
- Checkbox "Shuffle results on refresh", or "Shuffle selected tracks" for Last.fm sources, with a tooltip explaining fresh selection.

**Buttons:** "Refresh", "Stop", and on the right "Open snapshot in new tab".

**Status strings:**
- "Choose Refresh to evaluate this definition."
- "%1 tracks · rules update automatically while this window is open"
- "%1 tracks selected from %2 library matches · %3 Last.fm tracks not found[ · all available matches included]"
- "Stopped"
- "Definition saved"
- "Give the playlist a name"
- "The server connection changed. Reopen Dynamic playlists for the current library."
- From the service: "Updating from library tags and ratings…", "Fetching Last.fm tracks…", "Matching library tracks: %1 / %2", "Add a Last.fm API key in Settings → Metadata services", "Enter a seed artist and track", "Enter a Last.fm tag", "Enter a Last.fm username"

**Refresh behaviour:**
- Rules refresh themselves on library, rating or history changes (500 ms debounce) and are polled every 30 s.
- Selection, current row and scroll position are kept across refreshes.

**Result table:**
- A `QueueTableView` with the Plain columns layout.
- Drag only, copy only.
- Enter or double-click plays: this creates a scratch tab "Dynamic playback" / "<name> — playback" and plays the row there.
- The playing marker is synced every 500 ms.

**Result context menu** (`bench_dynamic_playlists.cpp:147-212`):
- "Play"
- Queue next / Queue at end
- "Go to artist", "Go to album"
- "Tools" submenu (Edit tags…, ReplayGain…, Convert files…)
- Rate / Rate album
- "Copy to list" (New tab…, then the tabs)
- "Open editable snapshot…" (tooltip "Manual edits apply to a separate list; the dynamic definition stays unchanged.")
- Last.fm submenu

Definitions are stored at `dynamic-playlists/v1/local`.

---

## Sources panel, library, search and pickers

## Trackknife parity inventory: Sources panel, library, search, pickers, palette, engine folder dialog

All paths are under `/home/carnager/Code/melody-next-engines/`. Strings are quoted exactly as they appear in the code. "L" means line.

---

### 0. The Sources panel container (`src/bench/bench_workspace_layout.cpp`)

- **Panel widget** `bench-panel-folders`, layout title **"Sources"** (L117-122). Minimum width 160. VBox with margins 0 and spacing 0.
- **Heading row** (L126-128): margins (6,4,6,0), spacing 2. It holds only the source switch, with stretch 1.
- **Source switch** is a `QTabBar` `bench-local-source-tabs`, accessible name "Local music source" (L131-157).
  - `setExpanding(false)`, `setDrawBase(false)`, `setDocumentMode(true)`.
  - Stylesheet (L143-150):
    - tab: transparent, no border, margin `3px 1px 0 1px`, padding `5px 12px`, top radii 5px, colour `palette(placeholder-text)`.
    - hover: background is Base at alpha 110.
    - selected: background `palette(base)`, colour `palette(text)`.
  - **Tabs:**
    - Index 0 is **"Folders"**.
    - Index 1 is **"Library"** (this computer's library).
    - After that, **one tab per configured remote engine**, added in `bench_remote.cpp:363-366`:
      - Text is `catalogue->name()`: the name the engine announced, or else its host or socket filename.
      - `tabData` is the engine key text.
      - Tooltip is `catalogue->describe()`.
    - Once the engine connects, the tab is renamed to its announced name (`bench_remote.cpp:297-302`). It is removed when the engine is disconnected (`bench_remote.cpp:218-224`).
    - So there are 2 + N tabs.
  - **Library tab visibility:** index 1 is hidden when Settings "Show this computer's library" (`library_show_local_key`, default true) is off (`bench_list_tabs.cpp:1508-1518`). The Settings checkbox is at `settings_dialog.cpp:394-400`, with tooltip "Hide it when all your music is in the remote engine's library. Files on this computer still open and play here."
  - **Persisted choice:** QSettings `local-library/view` is set to `folders`, `library` or `remote`, and only on a user click (`tabBarClicked`, L160-170).
  - **`selectPreferredSource()`** (`bench_list_tabs.cpp:1520-1549`):
    - A legacy stored value of "0" means folders.
    - Any unknown value means library.
    - If the choice is `remote`, or the local library is hidden, it picks the last remote tab. If there is none, it falls back to Library, then Folders.
  - **Switching pages:** `currentChanged` calls `refreshActiveContext()` (`bench_list_tabs.cpp:1269-1288`).
    - The page stack `bench-source-stack` shows the remote panel for a tab that carries engine data, the local `LocalLibraryPanel` for index 1, and otherwise the folder tree.
    - Bookmarks and their heading are visible only on the Folders tab, and only when there is at least one bookmark.

#### 0.1 Folders view: browses the local filesystem

- **Bookmarks heading:** `QLabel` **"Bookmarks"** (L176-184). Margins (8,4,8,2), font 0.85× and bold.
- **Bookmarks list:** `QListWidget` `bench-folder-bookmarks`, accessible name "Folder bookmarks" (L185-207).
  - No frame, uniform item sizes, max height 150, adjusts to contents, no horizontal scrollbar.
  - Each item has the `folder` theme icon and shows the folder's basename (the full path if there is no basename). The tooltip is the full display path (`bench_workspace_layout.cpp:916-948`).
  - Activating an item (double-click or Enter) calls `revealFolderPath(path)`. This expands the lazy tree step by step and selects the folder (L989-1062). A path that is not under any root is added as a new root and stored in `library/roots`.
  - **Context menu** (`bench-folder-bookmark-menu`) has one action: **"Remove bookmark"** (L257-267, 978-985).
  - Persisted in QSettings `library/bookmarks` as a list of raw byte paths.
  - On first run it is seeded with the home directory plus the old `library/roots` entries (L916-929).
- **Folder tree:** `QTreeView` `bench-folder-tree` (L210-228).
  - Header hidden, uniform row heights.
  - Drag enabled, `DragOnly`, `ExtendedSelection`, custom context menu.
  - **Activation** (double-click or Enter) on a *file* calls `openLocalPaths({path})`. On a directory it does nothing, so the default tree expansion applies.
  - At startup it adds root `"/"` and reveals `$HOME` (L270-273).
- **Folder context menu** `bench-folder-context-menu` (`bench_list_tabs.cpp:2311-2334`; actions defined at `bench_workspace_layout.cpp:249-256, 436-452`). Right-click first ClearAndSelects the row under the cursor.
  1. **"Add folder to current list"** for a directory, or **"Add file to current list"** for a file. Base text is "Add to current list". Calls `openLocalPaths`.
  2. Directories only: **"Expand"** / **"Collapse"**, which toggles expansion.
  3. Directories only: **"Bookmark folder"**, which calls `addFolderBookmark`.
- **`openLocalPaths`** (`bench_media_ingest.cpp:831-862`):
  - Target is the current tab if it is local, else the first local tab, else a new tab named "Untitled".
  - Runs `startDiscovery`, which walks folders and expands CUE files.
  - Status bar messages:
    - "A folder scan is already running" (3 s)
    - "%1 entr(y|ies) could not be opened" (5 s)
    - "Folder scan hit the file limit" (5 s)
- **File menu entries related to this panel:**
  - **"Bookmark folder…"** calls `addFolderRoot`, which opens a directory dialog titled "Bookmark folder", then adds the bookmark and reveals it (`bench_media_ingest.cpp:820-829`).
  - "Open files…" (Ctrl+O) and "Open folder…" (Ctrl+Shift+O).

#### 0.2 `LocalFolderTreeModel` (`src/uicommon/local_folder_tree_model.cpp`)

- **One column.** Display text is the basename, or the full raw path for a root (L39-42). Tooltip is the full escaped path. `Qt::UserRole` holds the raw bytes.
- **Icons** (L66-77):
  - `folder` for directories
  - `audio-x-generic` for audio files
  - `text-x-generic` for anything else
- **Loading:** lazy `fetchMore` on a worker thread (L241-341).
  - Hidden dot-entries are skipped.
  - Only directories and audio files are listed. Audio extensions: flac, wv, mp3, ogg, opus, m4a, mp4, aac, ape, mpc, wav, aiff, aif, wma, mka, dsf, dff, cue, tak, tta, spx (L60-62).
  - Symlinks to directories are skipped.
  - Directories come first, then files, each group in byte order.
  - Limit is 10,000 children, with the error "local folder contains more than 10,000 direct entries".
- **Signals:** `directoryError(QString)` (not connected to any UI) and `directoryLoaded(index)`.
- **Drag:** MIME is `LocalFilesMimeData` with the raw paths, folders included (L194-217). Supported action is Copy.

---

### 1. `LocalLibraryPanel` (`src/bench/local_library_panel.cpp/.hpp`)

There is one instance per engine:
- Local: `bench_list_tabs.cpp:185-311`.
- Remote: `bench_remote.cpp:354-480`. The object name is `bench-remote-library` for the first remote and `bench-remote-library-N` for later ones.

Root object name is `bench-local-library`. VBox with margins 4 and spacing 4 (L190-195).

#### 1.1 Search row (L196-272)

The row order is: search field (stretch 1), **Query** checkbox, **Recently added** toggle, **Refresh/Stop** button, **Folders…** button.

- **Search field** `QLineEdit` `local-library-search`.
  - Placeholder: **"Search albums and tracks"**. In query mode: **"tkq query, e.g. genre HAS jazz"**.
  - Clear button enabled. Accessible name "Search local library".
- **Query checkbox** `local-library-query-toggle`, text **"Query"**.
  - Tooltip: "Interpret the search as a tkq query, e.g. genre HAS jazz AND date GREATER 1990".
  - Persisted in `library/query-mode`.
  - Toggling swaps the placeholder, hides the query error and reloads the tree.
- **Query error label** `local-library-query-error`: word-wrapped, hidden by default, sits under the row. It shows the tkq compile error.
- **Icon tool buttons** (L232-244): icon only, auto-raise, 16×16. Text, tooltip and accessible name are all the same string.
  - **Recently added** `local-library-newest`: icon `document-open-recent`, checkable.
    - Tooltip: "Show albums newest first, as they came into the library".
    - Persisted in `library/newest-first`. Toggling reloads the tree.
  - **Refresh** `local-library-scan`: icon `view-refresh`, tooltip "Refresh".
    - While scanning it becomes text "Stop", tooltip "Stop scanning", icon `process-stop` (L1458-1460).
    - Clicking while scanning cancels the scan and shows "Stopping scan…".
    - It is reset when the scan finishes (L414-416).
  - **Folders…** `local-library-folders`: icon `folder`.
    - Tooltip: "Choose which folders belong to your music library".
    - For the local panel this emits `manageFoldersRequested`, which opens Settings on the Library page (`bench_list_tabs.cpp:187-188`).
    - Remote panels have no receiver, so they open their own dialog (see 1.9).

#### 1.2 Tree (`ui::LibraryTreeView`, L273-352)

- Object name `local-library-tree`, accessible name "Local artists, albums, and tracks".
- Header hidden, `ExtendedSelection`, drag enabled `DragOnly` with Copy action, `setExpandsOnDoubleClick(false)`, no edit triggers, no frame, custom context menu.
- **Hover action labels** (used as tooltips): "Append to current list", "Insert next in current list", "Replace list and play" (L283-284).
- Actions are available only on rows that have an entry with `available > 0`.

**Model structure** (lazy `canFetchMore` / `fetchMore`, L104-183, 633-835):

- **Browse (empty search, newest off):** top level is artists. Artist expands to albums, album expands to tracks. The query limit is 100,000, so a full level loads in one go.
- **Browse, newest on:** top level is albums, newest first, limit 500. No artist level.
- **Word search:** two non-draggable group rows, **"Albums"** then **"Tracks"**, both auto-expanded. Each holds matches for the text. Albums can still expand to their tracks.
- **Query mode:** one group, **"Tracks"**, filled from `library.filter(compiled, 0, 200)`.
  - If there are more results, a disabled row reads **"Showing the first %1 matches — press Enter to keep them all"**.
  - An invalid query sets the status to **"Invalid query"** and shows the error label.
- **"Show more…"** row (L796-822): appears only when a page reports `more`, which is practically never now. Activating it removes the row and loads the next offset (L898-904).
- **Empty rows** (disabled):
  - Inside a group or branch: **"No matches"**.
  - Empty top level:
    - roots unknown: **"The library is empty"**
    - has roots: **"Nothing indexed yet — press Refresh to scan your folders"**
    - no roots: **"No music folders yet — choose Folders… to add one"**
  - These strings are at L625-631 and are updated once the roots are known (L1411-1416).
- **Item label** is `entry.label`, with a suffix when files are missing:
  - **" — unavailable"** if no tracks are available.
  - **" — %1 unavailable"** if only some are (L743-748).
- **Item icons** (fallbacks): artist `avatar-default`, album `media-optical-audio`, track `audio-x-generic`. An album's real cover replaces its icon when loaded.
- **Tooltips:** a track shows its file path. Artist and album rows show "artist — album".
- **Subtitle** (`secondaryTextRole`, L121-137):
  - artist: none
  - album under an artist: **"%1 track(s)"**
  - top-level album (newest or search results): **"<artist> · %1 track(s)"**
  - track: none
- **Artist count:** the artist's album count is drawn quietly at the right edge of the row.
- **Expansion and current row** are remembered by entry key across reloads, and cleared when the search text changes.
- **Cover artwork** (L1494-1569):
  - Only album rows currently visible (up to 128 scanned) are loaded, one at a time on a worker, in a cache of 256.
  - The image comes from `catalogue->artwork_source` then `artwork`, via `ui::artworkThumbnail`, which caps it at 128 px (`local_artwork.cpp:21`).
  - Loading is triggered by scrolling, expand/collapse, row inserts, and viewport show or resize. Covers are invalidated after a scan.

#### 1.3 Row rendering (`src/uicommon/library_tree_view.cpp`)

- **View settings:** mouse tracking on, non-uniform row heights, icon size 32, indentation 18, animated. Selection is drawn by the delegate through `DelegateSelectionStyle`.
- **Row heights** (L235-241): track 26, album 40, artist 30, other top-level rows (group headers, empty rows) 34, anything else 30.
- **Selection** is a tint: 68% Base + 32% Highlight. Text keeps its colour. Focus rect removed (L255-265).
- **Icon slot:** track 16 px, album 30 px, artist 22 px. Content rect is inset (5,2,-8,-2).
- **Artist tile:** up to two uppercase initials (letters or digits) on a rounded rectangle (radius 3). Fill is 88% Base + 12% Text. Font 0.72× (minimum 6pt), DemiBold, PlaceholderText colour (L279-307).
- **Album:** the cover icon, plus a star-rating overlay when rated (`paintRatingOverlay`: a black band at alpha 140 along the bottom of the cover, with stars).
- **Text:** starts 6 px after the icon for tracks, 8 px for others.
  - Primary line: normal font, elided right.
  - Secondary line: font pt −1 (minimum 7), muted PlaceholderText (alpha 220, or 255 when selected). The two lines are centred vertically together.
- **Count** (artist album count): right-aligned, font pt −1, muted. Hidden while the hover actions are shown.
- **Hover/focus actions** (L67-74, 364-384): three 24 px slots at the right edge with a 4 px right margin.
  - Shown on the hovered row, or on the current row while the view has focus.
  - Icons (`libraryActionIcons`, `local_library_panel.cpp:60-67`):
    - `list-add` (fallback SP_DialogOpenButton) = append
    - `go-next` (fallback SP_ArrowRight) = insert next
    - `media-playback-start` (fallback SP_MediaPlay) = replace and play
  - Icons are drawn inset 6 px, so 12×12.
  - The hovered slot gets a rounded rectangle (radius 4, inset 2) in Highlight at alpha 42 (90 when selected).
  - Tooltip is the action label.
- **Mouse** (L76-155):
  - Pressing an action slot selects the row without dragging. Release on the same slot within the drag distance runs the action.
  - A single click (release) anywhere else on a row that has children **toggles expand/collapse**. The disclosure arrow keeps its default behaviour.
  - **Double-click is swallowed** (L103), so a double-click never activates.
- **Keyboard** (L157-166): Enter/Return on a row with children toggles it. On a leaf row, the default `activated` fires, which calls `LocalLibraryPanel::activate`, which runs **append** (or loads "Show more…").
  - Because the view accepts double-clicks without activating, the effective behaviour is: Enter on a track appends it, and clicking an artist or album toggles it.

#### 1.4 Actions enum and their destinations

- **Enum** `LocalLibraryAction { append, next, replace, new_list, request_next, request_end }` (hpp L48).
- **Local panel handling** (`bench_list_tabs.cpp:190-266`):
  - The target is the current tab if it is local, else the first local tab.
  - `next` inserts after the playing row, else after the current row, else at 0.
  - `new_list` creates a scratch tab named after the single entry's label, or "Library selection".
  - `replace` does replace-and-play through discovery.
  - `request_next` / `request_end` go to Up Next (front or end).
  - If a file intake is already running, the status bar shows "A file intake is already running".
- **Remote panel handling** (`bench_remote.cpp:371-439`): same idea, but rows go into a remote tab (the current remote tab or the engine's own tab). The destination tab is made current.
- **Enter in search** calls `commitSearch()` (L1157-1250):
  - Status "Collecting search results…".
  - The results become a new scratch tab named **"Search: %1"** (`bench_list_tabs.cpp:294-309`, `bench_remote.cpp:465-480`).
  - Status then says "Search kept as a new tab." or "No search results to keep.".

#### 1.5 Context menu (`showContextMenu`, L925-1020)

Object name `local-library-context-menu`. It appears only on entry rows. Right-click on an unselected row ClearAndSelects it. The first six actions are disabled when nothing in the selection is available or the selection exceeds 1,000 entries.

1. **"Append to current list"** (icon `list-add`)
2. Submenu **"Add to list"** (icon `view-media-playlist`, object `local-library-add-to-list`): one entry per list of this engine, in tab order. Disabled if there are none.
   - For remote panels, the status bar then shows "Added 1 track / %1 tracks to “name”" for 4 s (`bench_remote.cpp:457-462`).
3. **"Insert next in current list"** (icon `go-next`)
4. **"Replace list and play"** (icon `media-playback-start`)
5. **"Open in new tab"** (icon `tab-new`)
6. **"Play next (Up Next)"** (icon `media-playlist-append`), which is `request_next`
7. **"Add to Up Next"** (icon `media-playlist-append`), which is `request_end`
8. Separator, then submenu **"Rate album"** or **"Rate track"**. Only for album/track rows that have a `rating_hash`.
   - Choices are **"Unrate"** (checkable) and star widgets for 1–5 stars (values 2, 4, 6, 8, 10). Labels come from `ratingMenuLabel`: "1 star", "%1 stars", "½ star".
   - The current rating is checked. Choosing one stores it and updates the row.
9. Separator, then **"Expand"** / **"Collapse"**, if the row has children.

Action object names are `action-local-library-%0-5`, `action-local-library-add-to-%n` and `action-local-library-rate-%n`.

#### 1.6 Drag source (L152-177)

- **Payload:** `LocalFilesMimeData`. It carries a resolver that calls `resolveEntries` when dropped, plus the engine key. The raw entries are also attached under the property `trackknife-library-entries`, which Up Next uses (`bench_up_next.cpp:243-257`, `bench_main_window.cpp:97-106`).
- **Order:** entries follow tree order. A selected parent covers its selected children.
- **Rejected when:** the selection is empty, over 1,000 entries, or entirely unavailable. Group headers and "Show more" rows are not draggable.

#### 1.7 Footer and status line (L353-387)

- **Frame:** `local-library-footer`, with a 1 px top border in 88% Window + 12% Text. Margins (4,6,4,2), spacing 2.
- **Status label** `local-library-status`: font 0.9×, PlaceholderText colour, word-wrapped.
  - Initial text: **"Press Refresh to scan your music folders."**
- **Source label** `local-library-source`: same small font, shown only when the catalogue is **not** reachable. Its text is `CatalogueSource::describe()`.
  - Tooltip when using an engine: "Folders, scanning, search and covers come from that engine."
  - Tooltip otherwise: "Choose an engine in Settings → Library, or leave it empty to use this computer's. It is tried again on the next library action." (L494-506).
  - It is refreshed after every query.
- **Startup:** if no engine is in use, the status is set once to `describe()` (L485-487).

**Every status string:**

| When | Text |
|---|---|
| Typing | "Searching…" |
| Browse results ready | "Browse artists and albums." |
| Word-search results ready | "Search results" |
| Query results ready | "Query results" |
| Bad tkq query | "Invalid query" |
| Task queue full (64) | "Please wait for the current library requests." |
| Locate started | "Locating in library…" |
| Locate found | "Located in library." |
| Locate missed, remote | "This file is not in this library yet; it is found after the engine's next scan." |
| Locate missed, local | "This file is not in the local library. Add its folder and Refresh first." |
| Selection over 1,000 | "Select at most 1,000 library entries." |
| Bad selection size | "Select between 1 and 1,000 library entries." |
| Resolving a selection | "Loading library selection…" |
| Over 100,000 files | "This selection exceeds the 100,000-file limit." |
| All files missing | "These files are unavailable. Reconnect the folder and refresh the library." |
| Some files missing | "Unavailable files were skipped." |
| Selection resolved | "Library selection loaded." |
| Enter pressed | "Collecting search results…" |
| Search committed | "Search kept as a new tab." |
| Nothing to commit | "No search results to keep." |
| Root added | "Folder added. Press Refresh to scan for music." |
| No roots | "Choose Folders… to add your music collection." |
| Offline roots | "%1 folders unavailable. Cached music is still shown." |
| Scanning | "Scanning… %1 entries checked, %2 files updated, %3 unreadable" (polled every 200 ms) |
| Stopping | "Stopping scan…" |
| Cancelled | "Scan stopped. Completed updates were kept." |
| Incomplete | "Scan incomplete. %1 files could not be read; previous entries were kept." |
| Finished | "Library up to date." / "Library up to date. 1 file updated." / "Library up to date. %1 files updated." |

Any error message is shown as-is.

**Scanning UI:** there is no progress bar, only the text line plus the Refresh/Stop button. The panel property "scanning" is set to true during a scan. `refreshLibrary()` reloads the index on a 250 ms debounce and does not scan.

#### 1.8 Locate in library

- **Entry points:** the track context menu offers **"Locate artist"** and **"Locate album"** (`bench_list_tabs.cpp:2047-2069`). They are enabled only when that engine's library exists (and, for local, is shown).
- **What it does:**
  1. Switches the source tab to that engine's library (index 1 for local).
  2. Calls `locatePath` (L562-623): clears the search and turns off "Recently added".
  3. Reloads, expands the artist, and selects, scrolls to and focuses the album row or the artist row.
- Also used from the dynamic playlists dialog (`bench_dynamic_playlists.cpp:174`).
- **Ctrl+L** ("Search the library", `action-focus-library-search`, `bench_transport.cpp:1545-1547`): calls `focusLibrarySearch`. If the Folders tab is showing, it switches to a library first; then it focuses the search field and selects its text (`bench_list_tabs.cpp:1551-1566`).

#### 1.9 Folders management widget and dialog (L1275-1442)

- **Dialog:** `local-library-folders-dialog`, 560×320, with a single Close button.
  - Title: **"Library folders on %1"** for a remote, or **"Local library folders"**.
- **Widget** `local-library-folders-settings`. The local library embeds this in Settings → Library.
  - Explanation text, remote: "Folders on %1 for its engine to index. Give each path as that machine sees it. Folder changes are saved immediately. Removing a folder leaves its files untouched."
  - Explanation text, local: "Choose the folders to browse and search as your local music library. Folder changes are saved immediately. Removing a folder leaves its files untouched. "
  - Note: "Only Refresh in the Library sidebar scans your folders for music."
  - List `local-library-roots`. Each row is the path, optionally followed by " — unavailable" or " — not scanned". The tooltip is the root's error.
  - Error label `local-library-folder-error`.
  - Buttons: **"Add folder…"** and **"Remove"**. Remove is enabled only when a row is selected.
  - Adding a folder:
    - Local: directory dialog titled "Add music folder".
    - Remote: `QInputDialog` titled "Add music folder", with the label "Folder on %1, as that machine sees it:".

---

### 2. Engines behind the tabs (`catalogue_source.*`, `remote_engines.*`)

- **Tab name** (`CatalogueSource::name()`, L419-430): "This computer" for the local engine. For a remote, the announced name, else the host, else the socket filename.
- **`describe()`** (L391-417), used for tab tooltips and the source label:
  - "Library: this computer"
  - "Library unavailable: this computer's engine did not start (%1)"
  - "Library: engine at %1"
  - "Library: %1"
  - "Library unavailable: the engine at %1 refused the password"
  - "Library unavailable: the engine at %1 is unreachable"
  - "Library unavailable: %1"
- **Errors when unavailable:**
  - "no engine is connected[: %1]"
  - "no remote engine is configured"
  - "not an engine address: %1"
  - "this computer's engine is not started here"
- **Remote engine list:** QSettings array `engines/remote`, with fields address, password, music-folder, reachable-at, id and stream-kbps (`remote_engines.cpp`). The older single-remote keys are migrated and mirrored to the first entry.
- **When Settings is accepted:** `syncRemoteEngines()` adds and removes tabs and panels (`bench_remote.cpp:152-243`).

---

### 3. Search dialog (Ctrl+Shift+F), `src/bench/search_dialog.cpp`

- **Menu entry:** Workspace menu **"Search…"**, `action-search-dialog`, Ctrl+Shift+F (`bench_workspace_layout.cpp:476-479`).
- **Instance:** only one. Reopening follows the current tab's engine, raises the dialog and focuses the input (`bench_list_tabs.cpp:1156-1259`).
- **Window:** non-modal `QDialog` titled **"Search"**, object `bench-search-dialog`, 720×480, deleted on close.

**Layout, top to bottom:**

1. **Top row:**
   - **Scope combo** `bench-search-scope` with items **"This computer"** (data `local`), **"Current tab"** (`tab`), then one item per remote named after the engine, or **"Remote library"** if it has no name.
   - **Input** `bench-search-input`, placeholder **"Search…"**, clear button.
   - **"Query"** checkbox `bench-search-query-mode`. Tooltip: "Interpret the input as a tkq query, e.g. genre HAS jazz AND date GREATER 1990. Library history: HISTORY(albumplaycount) EQUAL 0, or HISTORY(albumdayssinceplayed) GREATER 180." Persisted in `search/query-mode`.
2. **Saved row:**
   - **"Browse presets"** menu button. Tooltip: "Choose a starting point; adjust the query and Save as… to keep it".
     - The menu is built from `query::search_presets()`, with submenus per topic.
     - An empty menu shows the disabled item "No presets supported in this scope".
     - Some presets prompt for a value with `QInputDialog` (int or text).
     - Choosing a preset turns on Query mode and fills the input.
   - **Saved searches combo** (accessible name "Saved searches"). The first item is **"Saved searches…"**. Item tooltips read "Current tab|Library database · Query|Words\n<expression>".
   - Buttons:
     - **"Save as…"**: prompts with "Save search" / "Name:".
     - **"Update"**: tooltip "Replace the selected saved search with the current query, mode, and scope". Enabled only when the input differs from the selected saved search.
     - **"Rename…"**: prompts with "Rename search" / "Name:".
     - **"Delete…"**: confirms with "Delete saved search" / "Delete “%1” from saved searches? Existing result tabs remain available.".
3. **Saved status** label:
   - "Loading saved searches…"
   - "Saving search definitions…"
   - "%1 saved search(es) · select one to run it again"
4. **Error label**: the tkq compile error, hidden otherwise.
5. **Results** `QListWidget`: ExtendedSelection, uniform sizes, batched layout.
   - **Word search in a library scope** is grouped with **bold, non-selectable headings**:
     - "Artists (N)" or "Artists (first N)", with rows "Name · N album(s)" (limit 20).
     - "Albums (…)", with rows "Artist — Album" (limit 20).
     - "Tracks (…)" (limit 200).
   - Track rows read "Artist — [track#. ]Title", with "Unknown artist" when the artist is empty.
   - **Query mode or the current-tab scope** gives a flat list, capped at 20,000 shown.
6. **Status** label:
   - "Searching…"
   - "Invalid query"
   - "0 matches"
   - "%1 artist(s) · %2 album(s) · %3 track(s)"
   - "%1 match(es)" plus " · showing first %1"
   - plus " · scanned %1 file(s)" when files had to be probed
   - "No local tab is active."
   - "Current-tab searches support at most 100,000 rows."
   - "Search cancelled"
7. **Buttons:** **"Open results in tab"**, which opens all results as a new list, and **"Close"**.

**Behaviour:**

- Input is debounced by 200 ms.
- The "Current tab" scope re-runs when the tab's model changes.
- Activating an item (double-click or Enter) runs **append** on the selection.
- **Context menu** `bench-search-context-menu`, only when something is selected:
  - "Add to current tab"
  - "Play next"
  - "Replace current tab"
  - "Open in new tab"
- Selected artists and albums are resolved to their tracks.
- Tabs created from the dialog are named **"Search: %1"**. Rows go to a tab of the same engine (`bench_list_tabs.cpp:1207-1255`).

---

### 4. Quick album/track pickers (Ctrl+Shift+A / Ctrl+Shift+T), `src/bench/quick_pick_popup.cpp`

- **Menu entries:** Workspace menu **"Quick album…"** (`action-quick-album`) and **"Quick track…"** (`action-quick-track`), both with a window shortcut (`bench_workspace_layout.cpp:480-491`).
- **Which library:** the current tab's engine, else this computer's (`bench_workspace_layout.cpp:1066-1087`).
- **Popup window:** a `QFrame` with the `Qt::Popup` flag and a StyledPanel frame, deleted on close.
  - Object name `bench-quick-album` or `bench-quick-track`.
  - Margins (10,10,10,8), spacing 8.
  - Width is `clamp(over.width - 80, 420, 640)`, height 420. Positioned horizontally centred, 90 px below the top of the central widget.
- **Contents:**
  - **Input**, font 1.1×, clear button. Placeholders:
    - album: "Album, artist or year — e.g. doors 1967"
    - track: "Title, artist, album or year — e.g. crystal doors"
  - To the right of the input, a **scope label** showing the library name in PlaceholderText colour.
  - **Results** list: no frame, no focus, single selection, rows 30 px high, drawn by a custom delegate:
    - Selection is the same tint (68/32).
    - The name is DemiBold and gets up to 3/5 of the width.
    - Then, 10 px later, the details in PlaceholderText: "artist · [album] · [date] · N tracks · added X ago".
  - **Status** (font 0.88×):
    - idle, album: "Type part of an album, its artist or its year."
    - idle, track: "Type part of a title, its artist, album or year."
    - "Searching…"
    - with nothing typed: "Recently added — type to search the whole library"
    - "No albums match." / "No tracks match."
    - "First %1 — type more to narrow them."
    - otherwise the count: "N albums/tracks"
  - **Key hint** (0.88×, wrapped): "Enter add · Shift+Enter replace and play · Ctrl+Enter play next · Ctrl+Shift+Enter add to Up Next · Alt+Enter new tab".
- **Age wording:** "added today", "added yesterday", "added %1 days ago" (under 14), "added %1 weeks ago" (under 61), "added %1 months ago" (under 730), "added %1 years ago".
- **Search behaviour:**
  - Debounced 120 ms, limit 60 results.
  - An empty input lists the newest first.
  - Only one search runs at a time; a newer query waits.
  - The first row is auto-selected.
- **Keys:**
  - Up / Down move by 1; PageUp / PageDown move by 8; Esc closes.
  - Enter = append, Shift+Enter = replace, Ctrl+Enter = request_next, Ctrl+Shift+Enter = request_end, Alt+Enter = new_list.
  - Activating a row with the mouse = append.
  - A choice is forwarded as the library's own `actionRequested`.

---

### 5. Command palette (Ctrl+Shift+P), `src/uicommon/command_palette.cpp`

- **Menu entry:** Workspace menu **"Commands…"**, `action-command-palette`. If a palette is already open it is raised instead (`bench_workspace_layout.cpp:455-458, 1089-1105`).
- **Commands offered:** the actions listed in `workspace_command_ids` (`bench_main_window_helpers.hpp:53-86`). There are 34 of them, including open-files, search-dialog, focus-library-search, quick-album and quick-track.
- **Window:** `QDialog` `command-palette`, title **"Commands"**, 620×480.
- **Contents:**
  - Filter field, placeholder "Find a command by name or shortcut…", accessible name "Command search".
  - Results list (accessible name "Matching commands"), alternating row colours, with the action icon. The shortcut is right-aligned in the row.
  - Checked actions get the suffix " · On".
  - Status line:
    - "No matching command"
    - "Unavailable for the current selection or connection."
    - "Press Enter to run this command." (or the action's statusTip, if it has one)
  - Label "Change shortcuts in Settings → Shortcuts."
  - Buttons: Close, and **"Run"** (the default button).
- **Behaviour:**
  - Sorted case-folded by name. Hidden or disabled actions are left out.
  - The filter matches every word, case-insensitive, against name + shortcut + objectName.
  - Up / Down in the filter field move the selection.
  - Enter or activating a row runs it: the dialog is accepted, then the action triggers.

---

### 6. Engine folder dialog, `src/bench/engine_folder_dialog.cpp`

- **Purpose:** browse folders on a remote engine's machine. It is used only by Output profiles → destination Browse for a remote place (`output_profiles_widget.cpp:227-241`). The listing comes from `work->folders(path)` (`bench_metadata_operations.cpp:1046-1057`).
- **Window:** `bench-engine-folder-dialog`, title **"Choose a folder on %1"**, 520×420, deleted on close.
- **Contents:**
  - **"Up"** button, then a path label (selectable text).
  - Folder list.
  - Status label: "Listing folders…", "No folders here", or the error.
  - Buttons: Cancel and **"Choose"**.
- **Behaviour:**
  - Activating a folder (double-click or Enter) goes into it.
  - Up goes to the parent; it is disabled when there is none.
  - Choose returns the selected sub-folder, or the folder currently shown if nothing is selected, as raw bytes via `folderChosen(QByteArray)`.
  - Up and Choose are disabled while a listing is loading.

---

### 7. Not present

- There is no scan progress bar. Scan progress is text only.
- The Folders view has no search field.
- The Library view has no column headers.
- The library tree has no "Show in folder" or "Open containing folder" action.
- `directoryError` from the folder model is not connected to any UI.
- `bench_media_ingest.cpp` only contributes the file intake: `openLocalPaths`, `addFolderRoot`, `startDiscovery` and their status-bar messages.

---

## Dialogs and secondary windows

## Trackknife (Qt Widgets): inventory of dialogs and secondary windows

All paths are under `/home/carnager/Code/melody-next-engines/src/`. `file:line` refers to the current tree. Strings are quoted exactly as they appear in the code.

**Terms used below**
- **Window-modal:** `setWindowModality(Qt::WindowModal)` or `QDialog::open()`.
- **Modeless:** `show()`.
- **Popup:** `QFrame(Qt::Popup)`.
- **Singletons:** most windows raise an existing instance instead of opening a second one. This is noted per window.

---

### 0. Where each window is opened

Menus are built in `bench/bench_workspace_layout.cpp:275-634`, `bench/bench_transport.cpp:572-640,950-984` and `bench/bench_playlists.cpp:48-60`.

**File menu**
| Menu entry | objectName | Default shortcut | Opens |
|---|---|---|---|
| New list… | action-new-list | Ctrl+N | QInputDialog "New list" / "Name:" (`bench_list_tabs.cpp:1889`) |
| Open files… | action-open-files | Ctrl+O | `QFileDialog::getOpenFileNames` "Open files" (`bench_media_ingest.cpp:800`) |
| Open folder… | action-open-folder | Ctrl+Shift+O | `getExistingDirectory` "Open folder" (`:812`) |
| Import M3U8 playlist… | action-import-m3u8 | — | QFileDialog "Import M3U8 into a new local list", filter "UTF-8 playlists (*.m3u8)" (`bench_playlists.cpp:14`) |
| Export list as M3U8… | action-export-m3u8 | — | QFileDialog "Export M3U8 to a new file"; filename label "New file (references, titles and durations only):" (`:29`) |
| Open list… | action-open-list | Ctrl+Alt+O | Open list dialog (§9.2) |
| Dynamic playlists… | action-dynamic-playlists | — | §9.4 |
| Back up workspace database… | action-backup-workspace | — | §9.8 |
| Restore workspace database… | action-restore-workspace | — | §9.8 |
| Bookmark folder… | — | — | `getExistingDirectory` "Bookmark folder" |
| Close window | action-close-window | — | status tip "The music keeps playing" |
| Quit and stop playback | action-quit | Ctrl+Q | status tip "Close Trackknife and stop this computer's engine" |

**Edit menu**
| Menu entry | objectName | Default shortcut | Opens |
|---|---|---|---|
| Find in current list… | action-find-in-list | Ctrl+F | bottom toolbar (§9.9) |
| Find next in list / Find previous in list | — | F3 / Shift+F3 | — |
| Undo list edit / Redo list edit | — | Ctrl+Z / Ctrl+Shift+Z, Ctrl+Y | — |
| Sort list ▸ Title, Artist / album / track, Album / track, Track number, Path, Custom expression… | — | — | Custom expression… opens the edit bar (§9.9) |
| Reverse list; Shuffle albums; Remove duplicate entries | — | — | — |
| Edit tags… | action-track-properties | Alt+Return | Tagger (§2) |
| ReplayGain… | action-replaygain-dialog | — | §3 |
| Convert files… | action-convert-files | — | §4 |
| Settings… | action-settings | Ctrl+, | §1 |
| Remove selected | — | Delete | — |

**Workspace menu**
| Menu entry | objectName | Default shortcut | Opens |
|---|---|---|---|
| Commands… | action-command-palette | Ctrl+Shift+P | §9.1 |
| Search… | action-search-dialog | Ctrl+Shift+F | §9.3 |
| Quick album… | action-quick-album | Ctrl+Shift+A | popup §9.5 |
| Quick track… | action-quick-track | Ctrl+Shift+T | popup §9.5 |

The rest of the Workspace menu (Jump to playing Ctrl+J, Cursor follows playback, Lists in a side panel, Duplicate/Pin/Save/Rename/Close tab, Track list layout ▸, Edit panel layout, Panel arrangement ▸, Swap/Reset) opens no windows, except that Rename tab… (F2) opens an input dialog (§9.8).

**Playback menu**
- Desktop notifications: checkable, `action-desktop-notifications`, writes `desktop/notifications` directly (`bench_transport.cpp:583-594`).
- Playback buffer ▸ Custom…: opens Settings on the Playback page and calls `editCustomBuffer()` (`bench_transport.cpp:709-711`).
- ReplayGain ▸ Preamp…: status-bar button menu; opens Settings on the Playback page and calls `focusReplayGainPreamp()` (`:1077-1078`).

**Context menus**
- Track list context menu "Tools" ▸ Edit tags… / ReplayGain… / Convert files… (`bench_list_tabs.cpp:2032-2035`).
- The dynamic playlist result menu has the same three entries (`bench_dynamic_playlists.cpp:177-183`).

**Configurable shortcuts** (`bench_transport.cpp:1545-1573`)
- Every `action-*` that has a default shortcut, plus every ID in `workspace_command_ids` (`bench_main_window_helpers.hpp:53-86`), is added to `configurable_shortcuts_`.
- The list is sorted by text and stored with property `shortcut-default`.
- Overrides are restored from `shortcuts/<objectName>`.
- `action-focus-library-search` "Search the library" defaults to Ctrl+L.

---

### 1. Settings dialog: `bench/settings_dialog.{hpp,cpp}`

**Opening.** `BenchMainWindow::showSettingsDialog(page)` (`bench_workspace_layout.cpp:1107-1186`).
- Singleton: an existing visible instance gets `showPage()`, raise and activate.
- Window-modal (`dialog->open()`), `WA_DeleteOnClose`.
- Title "Settings", objectName `bench-settings-dialog`, size 940×640.

**Callers.**
- Edit ▸ Settings… (General page).
- Library panel "Folders…" (Library page; `bench_list_tabs.cpp:187`).
- Playback buffer Custom… (Playback page).
- Preamp… (Playback page).
- Tagger "Edit…" / "Manage naming layouts…" (Naming page, then `showNamingLayouts()`).
- Tagger "Manage move destinations…" (Naming page, then `showDestinationsOf(engineKey)`).
- Tagger "ReplayGain settings…" (ReplayGain page).
- Artwork "Cover settings…" (Covers page).

**Layout.**
- Left: a `QListWidget` of pages (max width 150, frameless). A custom delegate draws the selected row bold, with a 7:1 base/accent blend and a 3 px accent bar on the left; rows are 12 px taller.
- Right: a `QStackedWidget`. Each page is wrapped in a scroll area headed by the page title (font ×1.3, DemiBold).
- Form fields have a maximum width: spin boxes and combos 260, line edits and key edits 460. Rows wrap when long.
- Below the pages, a save note (placeholder colour) shows only on:
  - Library: "Folder changes save immediately. Cancel does not undo them."
  - Naming: "Save layout, Save destination, and Remove take effect immediately. Cancel does not undo them."
- Buttons: Save and Cancel.

**Save behaviour** (`:886-899`).
1. If the shortcuts are in conflict, show the Shortcuts page and abort.
2. If "Share on the network" is on and the password is empty, show the Engine page, focus the password and abort.
3. Otherwise run `save()` (`:1009-1058`), then accept.

**After accept** (`bench_workspace_layout.cpp:1136-1182`).
- The AcoustID key, ratings-in-tags and rating scale are pushed to every file-work engine (`set_acoustid_key`, `set_rating_tags`, `set_rating_scale`).
- Then: `syncRemoteEngines()`, `applyLocalLibraryVisibility()`, `applyListsDisplay()`.
- If sharing settings changed, the local engine restarts (wait cursor). Status messages: "This computer's engine restarted with its new settings" / "This computer's engine did not restart; see its log".
- Then: `reloadPlaybackPreferences()`, and `refreshStoragePolicy()` on every artwork section.

**Signals.** `outputProfilesChanged` makes every open tagger call `reloadOutputProfiles()`.

Page enum (`hpp:50-61`): general, playback, library, engine, naming, replaygain, covers, metadata_services, lastfm, shortcuts.

#### 1.1 General (`cpp:172-224`)
| Row label | Control | Text / tooltip | QSettings key (default) |
|---|---|---|---|
| Desktop: | QCheckBox `bench-settings-notifications` | "Track-change notifications"; tooltip "Show a notification when playback changes to another track." | `desktop/notifications` (false) |
| (none) | QCheckBox `…-notifications-background` | "Only while the app is in the background" | `desktop/notifications-background-only` (false) |
| QPushButton "Test notification" | status QLabel (word-wrapped) | Click shows "Sending…", then `DesktopNotifier::sendTest()`. Result: "Accepted by your desktop. If no popup appears, check Do Not Disturb and desktop notification rules." or "Notification failed: %1" | — |
| Appearance: | QCheckBox `…-panel-animations` | "Animate panel opening and closing" | `appearance/panel-animations` (true) |
| Show lists as: | QComboBox `…-lists-display` | "Tab bar" (`tabs`), "Side panel" (`panel`) | `appearance/lists-display` ("tabs") |

#### 1.2 Playback (`:226-319`)
- Note: "The engine plays the music and keeps its queue: closing this window never interrupts it, and an engine that restarts restores its queue, paused."
- **Playback buffer:** combo with Responsive / Balanced / Resilient / Custom (data responsive/balanced/resilient/custom). Key `playback/buffer-profile` (default "balanced").
- **Capacity:** spin 10–10000, suffix " ms". Key `playback/buffer-capacity-ms` (750).
- **Start playback at:** spin 1–10000 " ms", maximum tied to Capacity. Key `playback/buffer-start-threshold-ms` (100).
- Choosing a preset fills and disables both spins via `audio::playback_buffer_preset_config`.
- A saved Custom profile with invalid values falls back to balanced.
- Note: "Responsive starts sooner; Resilient tolerates longer interruptions. Buffer changes take effect on the next track."
- **Preamp with ReplayGain data:** double spin ±`audio::maximum_replay_gain_preamp_db`, step 0.5, 1 decimal, " dB". Key `playback/rg-preamp-with` (0).
- **Preamp without ReplayGain data:** same control. Key `playback/rg-preamp-without` (0).
- Note: "Preamps apply when local ReplayGain is enabled. Choose Track, Album, or Automatic from the playback controls."

#### 1.3 Library (`:321-360`)
- Top: the injected `LocalLibraryPanel::createFoldersWidget()` (§9.7). Without a workspace: "Library folders are available from the running workspace."
- Checkbox "Also write track ratings into the files". Tooltip: "Ratings stay in each engine's library either way. With this on, each engine also writes a track's rating into its files as FMPS_RATING, which other players read. Album ratings are not written." Key `library/ratings-in-tags` (false).
- **RATING tags from other players:** combo with "Don't import" (off), "1–5 (foobar2000)" (5), "0–10" (10), "0–100 (MusicBee, MediaMonkey)" (100). Tooltip: "Ratings other players left in your files are taken into the library. FMPS_RATING and MP3 POPM always are; a plain RATING tag has no agreed scale, so it is read only on the one chosen here." Key `library/rating-tag-scale` ("off").

#### 1.4 Engine (`:362-680`)
- Intro: "melodyd plays the music and keeps the library. Trackknife starts this computer's, and it plays on after the window closes."
- **Password:** password-echo line edit, placeholder "the same on every engine and agent". Tooltip: "Needed to share this computer's engine, and given to the remote engine unless it has a password of its own below. It travels unencrypted: across an untrusted network, use WireGuard or a TLS proxy." Key `engine/password`.

**GroupBox "This computer's engine"**
- Checkbox "Show this computer's library". Tooltip: "Hide it when all your music is in the remote engine's library. Files on this computer still open and play here." Key `library/show-local` (true).
- Checkbox "Share on the network". Tooltip: "Lets output agents play this computer's music, and other Trackknife windows control it". Key `engine/share` (false).
- **Address:** line edit. Tooltip: "host:port; 0.0.0.0 listens on every network this computer is on". Key `engine/listen` (default "0.0.0.0:6600"). Enabled only while sharing.
- **Stream port:** spin 1–65535. Tooltip: "Where agents without a copy of the music fetch it, on the same address". Key `engine/stream-port` (6601). Enabled only while sharing.
- **Music root:** line edit, placeholder "optional", plus a "Browse…" button (`getExistingDirectory` "Music root"). Tooltip: "Agents started with their own --music-root are sent paths relative to this folder; agents without one stream". Key `engine/music-root`.
- Live label, selectable (`:451-486`):
  - Not shared: "Not shared: only this computer plays, and Trackknife here controls it."
  - Shared without a password: "Set the password above to share: every connection from the network must give it."
  - Otherwise: "On a machine with speakers, run:\nmelody-agent --server HOST:PORT --password …\nAdd --music-root DIR where it has the music itself; without it, it streams. Changing these restarts this computer's engine; playback comes back paused." HOST becomes the machine hostname when the address is 0.0.0.0 or ::.

**GroupBox "Engines elsewhere"** (multi-engine list, ADR-0234)
- `QListWidget` `bench-settings-engines`, max height 110. Items show the address, or "New engine" when it is blank.
- Buttons "Add" and "Remove". The list always keeps at least one blank entry.
- Tool button "On the network" (instant popup menu). Tooltip: "Engines that announce themselves on this network". Menu items:
  - "None found yet" (disabled), or
  - "<instance> — <addr:port>" plus " · password" when the TXT record has auth=1.
  - Filled by an mDNS `discovery::Browser` while the dialog is open. If the browser cannot start, the button is disabled and its tooltip carries the error.
  - Choosing an item selects the existing entry, fills the blank one, or appends a new one.
- Per-engine form (edits the selected list entry):
  - **Address:** placeholder "host:port or socket path". Changing it clears the stored id.
  - **Password:** password echo, placeholder "the password above", tooltip "Only when that engine's password differs from yours".
  - **Its music folder:** placeholder "e.g. /mnt/nas/Music, as that engine sees it".
  - **Also reachable here at:** placeholder "optional; empty: the same path, or not reachable here". Tooltip: "Where that folder is on this computer, if you have mounted it (NFS, SMB, …). Trackknife mounts nothing itself."
  - **Streamed here:** combo with rates -1, 0, 192, 160, 128, 96, 64. Labels: "Automatic", "Original files", "Opus %1 kbps". Tooltip: "What it streams to this computer's speakers when its music is not reachable here. Automatic: the rates below, by whether it is on this network or reached through a VPN or a router."
  - Saved as QSettings array `engines/remote`, each entry holding address, password, music-folder, reachable-at, id, stream-kbps (`remote_engines.cpp:92-114`). The first entry is mirrored to the legacy keys `library/engine-socket`, `library/engine-token`, `library/remote-folder`, `library/remote-mount`, `library/engine-id`. Entries with an empty address are dropped.
- Checkbox "Let other engines play on this computer's speakers". Tooltip: "This computer's engine appears among the outputs of the remote engine and of any engine found on the network, so no melody-agent is needed here. Whichever starts playing last has the speakers." Key `engine/play-for-remote` (true). Enables the three stream combos.
- **Streamed on this network:** rates 0, 192, 160, 128. Tooltip "From an engine on this computer's own network". Key `engine/stream-nearby-kbps` (0).
- **Through a VPN or router:** rates 192, 160, 128, 96, 64, 0. Tooltip "From an engine reached through WireGuard or another VPN, or through a router". Key `engine/stream-away-kbps` (128).
- A rate that is not in a combo's list is appended and selected.
- Note: "A melodyd on a NAS or server (started with --listen), beside this computer's: its library gets a tab and its tracks play there, and this computer's speakers are offered to each. Unencrypted: for a home network or WireGuard."
- Hidden key (never shown in the UI): `library/local-engine-socket`.

#### 1.5 Naming (`:682-700`)
Hosts the `OutputProfilesManagerWidget` (§1.10), which is signalled through `profilesChanged`. Without a store: "Naming layouts and move destinations are managed from the running application."

#### 1.6 ReplayGain (`:702-723`)
- Checkbox `bench-replaygain-sidecar-only` "Store scan results in sidecar only". Tooltip: "Keep ReplayGain values out of file tags; scans write the loudness sidecar instead". Key `replaygain/sidecar-only` (false).
- Checkbox `bench-replaygain-true-peak` "True peak as ReplayGain peak". Tooltip: "Scan oversampled true peak instead of the plain sample peak". Key `replaygain/true-peak` (false).
- The ReplayGain dialog (§3) writes the same two keys.

#### 1.7 Covers (`:725-782`)
- Checkbox "Embed covers into the files". Key `artwork/embed` (true).
- Checkbox "Write a front-cover image next to the tracks". Key `artwork/write-folder-image` (false).
- **Folder image name:** editable combo with cover.jpg and folder.jpg. Key `artwork/folder-image-name` ("cover.jpg").
- **Fetch covers from:** combo with "Cover Art Archive (front)" (coverartarchive). Key `artwork/fetch-source`.
- **Largest embedded cover:** spin 0–10000, step 100, " px", 0 shown as "No limit". Key `artwork/max-embedded-edge` (0).
- **Largest folder image:** same control. Key `artwork/max-folder-edge` (0).
- Note: "Front covers use this storage policy on Apply. The filename extension follows the image format (.jpg or .png). Folder replacements are reviewed and retain recovery backups. A cover wider or taller than its limit is scaled down and saved as JPEG (PNG if it has transparency) when it is written; covers already in your files are left alone."
- `SettingsDialog::artworkPolicy()` (`:931`) is read by the tagger and the artwork section.

#### 1.8 Metadata services (`:784-853`)
- Note: "MusicBrainz text search works without an account or API key. Lookups start only when you request identification."
- **AcoustID client key:** password echo, placeholder "Client/application key for fingerprint lookup". A "Show" checkbox (accessible name "Show AcoustID client key") toggles the echo. Key `musicbrainz/acoustid-client-key`.
- Note: "Audio fingerprint identification uses AcoustID and the fpcalc tool. Use an application/client key, not an AcoustID user key. The key is stored in your local application settings. Clear it to remove it."
- Link: "Register an application to get an AcoustID client key" → https://acoustid.org/new-application
- **Last.fm API key:** password echo, objectName `bench-settings-lastfm-key`. Key `lastfm/api-key`.
- Note (rich text): "Dynamic playlists can match Last.fm recommendations, loved tracks, top tracks, or tags to your library. Requests send the seed artist/track, username, or tag when you choose Refresh. The key is stored locally. Account authorization and scrobbling are separate under Last.fm settings. <a …/api/account/create>Get a Last.fm API key</a>"

#### 1.9 Last.fm page: `BenchMainWindow::buildLastFmSettings` (`bench/bench_lastfm.cpp:215-490`)
- Note: "Sign in here once. Hand the account to an engine below and it scrobbles what it plays itself, with Trackknife closed; until then, playback is scrobbled while Trackknife is open. Account actions take effect immediately."

**Credentials block** (hidden once credentials are saved)
- Instructions: "1. Register a free Last.fm API application using the link below. Choose an application name such as Trackknife; no callback URL is needed for desktop authorization.\n2. Paste the API key and shared secret here once.\n3. Connect to enable scrobbling, then approve access in your browser. This page connects automatically once you approve."
- **API key:** password echo, prefilled from `lastfm/api-key`.
- **Shared secret:** password echo.
- Links: "Create a Last.fm API account" · "Find your key and shared secret".
- Checkbox "Use this API key for dynamic playlists too" (default on). When on, Connect writes `lastfm/api-key` and syncs `bench-settings-lastfm-key` so that Save cannot overwrite it.

**Rest of the page**
- "Credentials are saved privately on this computer."
- Buttons: "Connect to Last.fm…" (becomes "Reconnect in browser…" once saved), "Cancel" (visible only while waiting), "Disconnect / clear pending".
- Checkbox "Scrobble playback to Last.fm" (runs `enable` 1/0; enabled only when connected).
- Status label, which shows:
  - "%1 · %2 pending\n%3" where %1 is "Connected as X" or "Not connected".
  - "Working…"
  - "Waiting for browser approval…"
  - "Authorization timed out. Connect again to retry." (5-minute deadline)
  - "Stopped waiting for approval. Connect again to retry."
  - Validation: "Paste the 32-character API key and shared secret from your Last.fm API account." (32 hex characters each)

**Auth flow**
- `begin` opens the https://www.last.fm URL returned by the service.
- A poll timer (3 s) then sends `finish`. The error "(code 14)" or `authorization_pending` means keep polling.
- `status` is refreshed every 10 s while the page is visible.

**GroupBox "Engines scrobble what they play"**
- One row per engine: "This computer:", each remote engine's name, then any added through "Another engine…".
- Each row has a state label ("Asking…", then "Not reachable", "Cannot scrobble (engine too old)", "Not scrobbling" or "Scrobbling as X · N waiting") and a button "Use this account" / "In use" (disabled).
- The button hands over the session (`lastfm.set_session`). Row states during handover: "Handing over…", "Not reachable: …", "Not handed over: …", "Scrobbling as …".
- "Another engine…" opens two QInputDialogs, both titled "Another engine": "Address of the engine (host:port):", then "Its password:" (password echo). A bad address gives the status message "Not an engine address: %1".

**Service.** `LastFmService` (`bench/lastfm_service.{hpp,cpp}`) runs on a worker thread; state lives in `AppDataLocation/lastfm-v1.json`.
- Operations: status, session, disconnect, enable, begin, finish, love, unlove, info.
- Errors: "Connect Last.fm first", "Last.fm is busy", "Disconnect before changing API credentials", "Provide the 32-character API key and shared secret", "Start authorization first", "Last.fm request failed (code %1)", "Could not save Last.fm state".
- Messages: "Disconnected; pending scrobbles cleared", "Scrobble submitted", "Scrobble rejected (code %1)".

**Other Last.fm UI**
- Track context submenu "Last.fm" (`bench_lastfm.cpp:491-530`): "Checking loved state…" (becomes "♥ Loved on Last.fm" / "Not loved on Last.fm" / error), "Love track", "Unlove track". Enabled only for a single selection with artist and title.
- Status bar: "Last.fm updated. Refresh loved-track playlists to see the change."

**Page-index quirk.** If no Last.fm factory is supplied but shortcuts exist, an empty "Last.fm" page is inserted so the enum indexes still line up (`settings_dialog.cpp:854-861`).

#### 1.10 Naming page widget: `bench/output_profiles_widget.{hpp,cpp}`
A `QTabWidget` `bench-output-profile-sections` with two tabs, plus a shared status label.

**Tab "Naming layouts"**
- Combo `bench-output-layout-list`, placeholder "New naming layout".
- **Name:** placeholder "For example: Album folders".
- **Folders:** monospace, placeholder "For example: %album artist%/%album%".
- **Filename:** monospace, placeholder "For example: %tracknumber% - %title%".
- **Filename policy:** combo with "Linux filenames" (linux) and "Portable filenames" (portable). Tooltip: "Portable replaces Windows-forbidden characters, trailing dots/spaces, and reserved device names; Unicode spelling is preserved".
- Buttons: New, "Save layout" (needs name and filename), Remove (needs a selected layout).

**Tab "Move destinations"**
- Row: "Move destinations on" + combo `bench-destination-engine` (one per engine/place; disabled if only one) + a hidden button "Copy %1 from this computer". Its tooltip: "Save this computer's destinations that lie under this engine's music folder here too, as the engine names them".
- Combo `bench-destination-list`, placeholder "New move destination".
- **Name:** placeholder "For example: Music library".
- **Root:** placeholder "Choose an absolute folder", plus "Browse…". For a remote place this opens the **EngineFolderDialog** (§9.10); locally it opens `getExistingDirectory` "Choose move destination".
- Buttons: New, "Save destination" (needs name and root), Remove.

**Status texts**
- "%1 naming layout(s) · %2 move destination(s) on %3"
- "Profile storage is unavailable"
- "Could not load output profiles · %1"
- "Could not load the move destinations on %1 · %2"
- "Naming layout is not valid · %1" / "Move destination is not valid · %1"
- "Could not save naming layout · %1" / "Naming layout saved"
- "Could not save move destination · %1" / "Move destination saved"
- "Could not remove naming layout · %1" / "Naming layout removed"
- "Could not remove move destination · %1" / "Move destination removed"
- "Could not copy a move destination · %1"

**Behaviour**
- All actions persist immediately through the async `OutputProfileStore` / `DestinationPlace` callbacks built in `BenchMainWindow::buildOutputProfileStore` (`bench_metadata_operations.cpp:970`).
- Validation uses `operations::validate_output_layout_profile` / `validate_destination_profile`.
- Every change emits `profilesChanged`.

#### 1.11 Shortcuts page: `bench/shortcut_settings.{hpp,cpp}`
- Note: "Click a shortcut and press the new keys. Clear it to disable it. Changes take effect on Save. Shortcuts operate within Trackknife, not across the desktop."
- Form: one row per configurable action. The label is the action text without "&"; the field is a `QKeySequenceEdit` named `shortcut-<objectName>`.
- Error label `shortcut-conflict`: "Conflicting shortcuts: %1 and %2" (prefix-match check).
- Button "Restore defaults" resets every field to its `shortcut-default`.
- `apply()` sets each action's shortcut and writes `shortcuts/<objectName>` in PortableText.
- Uses `tr()` (this page and "Shortcuts" are the only translated strings in Settings).

---

### 2. Tagger: "Edit tags" window

#### 2.1 Opening (`bench/bench_metadata_operations.cpp:1192-1400`)
- `showMetadataForView(view)` first requires engine file work (`requireFileWork`). Without it the status bar shows "Editing tags is done by the engine on %2, which is not available right now".
- The selected rows are captured as persistent indexes and passed to `openMetadataProperties`.
- Services are wired to the engine: plan applier, file-publication applier, artwork applier, `engineLookupService` (MusicBrainz/CAA/AcoustID through the engine), and `engineFileWorkTools`.
- It is a separate top-level window: `setWindowFlags(Qt::Window)`. Several can be open at once.
- Title: "Edit tags · N track(s)".
- Signal wiring:
  - `statusMessage` → status bar (12 s).
  - `openSettingsRequested(page)` → Settings (Naming page also calls `showNamingLayouts()`).
  - `openDestinationsRequested` → Settings Naming page with `showDestinationsOf(work engine)`.
- When the window is destroyed and no tabs remain, an "Untitled" scratch tab is recreated.

#### 2.2 `MetadataPropertiesDialog` (`bench/metadata_properties_dialog.{hpp,cpp}`)
- objectName `bench-metadata-properties`, modeless, 1020×620, `WA_DeleteOnClose`.
- Geometry and splitter state are persisted through `MetadataDialogLayoutStore` under `workspace/metadata-properties-geometry-v1`, `workspace/metadata-properties-metadata-splitter-v1` and `workspace/metadata-field-layouts-v1` (`cpp:119-122`).

**Header row** (`:182-201`)
- Summary label `bench-metadata-summary`: first "N track(s) · preparing", then "%1 of %2 files selected · %3 source(s) · %5 fields", with " · N staged change(s)" appended while drafts exist.
- Technical label `bench-metadata-technical` (right-aligned, selectable, clipped, full text in tooltip; `:3802-3981`). Parts joined with " · ":
  - "N tracks"
  - codec in uppercase, or "mixed codecs"
  - "%1 Hz" or "mixed rates"
  - "%1 bit" or "mixed depths"
  - "%1 ch" or "mixed channels"
  - "%1 kbit/s" or "mixed bitrates"
  - duration "m:ss" / "h:mm:ss", as "total %1" when several files
  - "analyzing %1…", "%1 unreadable", "first 512 files"
- The probes run in the background (maximum 512) through `tools_.probe` or `engine::probe_local_technicals`.

**Loading placeholder.** Centered label "Preparing metadata grid…" or "Preparing metadata grid… %1/%2".
- Errors: "Properties unavailable" + "The selection exceeds the %1-track limit" / "No tracks were selected" / "The selected tracks are no longer available".
- Capture: `metadata::capture_uncached_metadata_sources` with `tools_.access`, then `StagedMetadataSelection::create`.

**Body after load** (`:1024-1237`): a horizontal `QSplitter` `bench-metadata-splitter`, sizes 170:390, stretch 1:3.

1. **File pane.**
   - Breadcrumb label `bench-metadata-files-dir` showing the common folder (hidden if none).
   - `FileScopeView` `bench-metadata-files`, accessible name "Files included in metadata edit". Model: `MetadataGridModel` column 0 only; header "File path". Rows render relative to the common prefix, elided right, extended selection. All rows are selected initially.
   - It is disabled while artwork work is running or artwork changes are pending.
2. **QTabWidget `bench-metadata-sections`** with tabs "Fields" and "Artwork".
   - **Fields tab**, top to bottom:
     - **MetadataFieldReviewBar** (§2.4).
     - Grid tools row: "Add field…" (tooltip "Add an arbitrary metadata field (Insert)"), "Remove field" ("Remove the selected fields from the selected files (Delete)"), "Edit values…" ("Edit the exact ordered value list (Ctrl+Enter)"), label "Fields:" plus combo `bench-metadata-field-layout` with "All fields" and saved sets (shown only when sets exist), "Identify…", tool button "More".
     - Identify… tooltip: "Search MusicBrainz by artist and album — no MusicBrainz tags needed — pick the exact release version, and stage the match as ordinary colored draft edits". Enabled only when a MusicBrainz fetch service exists.
     - "More" menu: "Suggest album totals and artist" (tooltip "Fill album artist and total tracks from agreement across the selected files; suggestions become ordinary colored draft edits"), "Save visible fields as set…", "Delete current field set".
     - Right side of the tools row: icon buttons Undo (edit-undo, "Undo the last draft edit (Ctrl+Z)"), Redo (edit-redo, "Redo the last undone draft edit (Ctrl+Shift+Z)"), Discard (edit-clear, "Throw away every pending draft edit").
     - Fields table `bench-metadata-fields` (`MetadataAggregateModel`, columns Field / Original / Draft). Column 0 is fixed at 190 px, the others stretch, rows are 24 px. Delegate: §2.5.
     - To the right of the table: the compact cover pane from `MetadataArtworkSection::createCompactCover` (§2.8).
   - **Artwork tab**: `MetadataArtworkSection` (§2.8).

**Keyboard in the fields table** (`eventFilter`, `:3690-3716`)
| Key | Action |
|---|---|
| Undo / Redo | grid undo / redo |
| Ctrl+Enter | Edit values… |
| Insert | Add field… |
| Delete | remove the selected fields |
| Ctrl+Backspace | revert the selected fields |

**Hidden "apply options" panel** (`:210-374`). This panel is never shown; its controls hold the state that the Actions popover mirrors.
- Heading "When you apply". Tooltip: "Apply rechecks the files and runs every checked action together; problems stop the run before anything is written".
- "Save tags" (tooltip "Write the drafted tag edits into the files").
- "Rename files" + layout combo (placeholder "None saved yet") + "Edit…" ("Create, change, or remove naming layouts").
- "Move files" + destination combo (placeholder "None saved yet", or "None saved on %1 yet" for a remote engine) + "Edit…" ("Create, change, or remove move destinations [on %1]").
- Rename/Move tooltips switch between: "Generate a new basename with the saved layout", "Select a saved naming layout first", "Move below the saved destination using the saved layout", "Select a saved layout and destination first", "File publication is unavailable". Initial tooltips: "Choose a naming layout first" / "Choose a naming layout and a destination first".
- "ReplayGain scan" (tooltip "Measure EBU R128 loudness for the selected files and stage the ReplayGain tags as colored draft edits — nothing is written until you apply") + grouping combo: "Album by release", "Album merging discs", "Selection as one album", "Track gains only", "Group by expression". The expression line edit (placeholder "tkfmt-1, e.g. %album%") is visible only for the last option.
- "Loudness sources…"
- Status: "Loading output profiles…" / "%1 naming layout(s) · %2 move destination(s)" / "Output-profile persistence is unavailable" / "Could not load output profiles · %1".
- Heading "Scripts". Tooltip: "Checked scripts stage their edits as colored drafts when files load and when suggestions arrive; Apply writes exactly what the grid shows".
- Checkable list of saved scripts; when empty it paints "No saved scripts yet". Item tooltips: "Staged automatically as colored draft edits" / "Not staged automatically". Status: "%1 of %2 checked · run in the order shown". Button "Open script editor…" / "Edit selected script…".
- Remembered in QSettings (`:110-114`): `properties/actions/save-tags` (true), `properties/actions/rename-files`, `properties/actions/move-files`, `properties/actions/naming-layout`, `properties/actions/move-destination/<engineKey>`.

**Footer** (`:595-629`)
- Tool button "Actions" `bench-metadata-actions`. Tooltip: "What Apply does: tags, renaming and moving, ReplayGain, scripts".
- Apply summary label: "Apply: tags · covers · rename · move".
- Status label `read_only_` (plain or rich text; it carries links).
- Progress bar (170 px) and "Stop" ("Stop after the files already in progress are safe"), both hidden until Apply runs.
- Dialog buttons: "Apply" (default; tooltip "Recheck the files, then make every enabled change; problems stop the run and are shown") and "Close".

**Actions popover** (`showActionsPopover`, `:1829-2019`)
- A `QFrame(Qt::Popup)` in a grid, rebuilt every time it opens. It is placed above the button, below it if there is no room, and clamped to the screen.
- Rows:
  - "Save tags"
  - "Rename files" + layout combo
  - "Move files" + destination combo
  - links "Manage naming layouts…" · "Manage move destinations…"
  - "ReplayGain" + grouping combo + "Scan now". Choosing "Group by expression" opens a QInputDialog "Group by expression" / "tkfmt-1 expression:".
  - links "Loudness sources…" · "ReplayGain settings…"
  - "Scripts": one checkbox per saved script, plus the link "Open script editor…"
- Every change writes the remembered-action keys.

**Status line messages** (`read_only_`)
- "Read-only metadata preview · preparing selection"
- "No pending edits"
- "Draft only · nothing is written until you apply"
- "Save tags is off · tag edits stay in the draft and Rename/Move uses the file's current tags"
- "Artwork changes pending · Apply saves tags and covers together"
- "Draft edit rejected · %1"
- "Preparing metadata for %1 selected file(s)…"
- Suggestions: "Suggestions need at least two files that share an album", "Looking for suggestions across %1 files…", "No suggestions · the selected files already agree", "Staged %1 suggestion(s) across %3 file(s) from %5 · review the colored values, then Apply"
- Automatic scripts: "<name|Automatic scripts> staged N edit(s) across M file(s) · <a undo-automatic>Undo</a>", "Automatic scripts exceed the %1-step combined limit; disable or shorten a script."
- Script toggle: "%1 will [no longer ]stage its edits automatically."
- Loudness scan: "Measuring loudness · %1 of %2 files · <a cancel-replaygain>Stop</a>", then " · <a retry-replaygain>Retry N failed</a>" and " · <a export-replaygain>Export results</a>"
- Apply: "Checking files…", "Saving metadata · x of y", "Updating files · x of y", " · stopping…", "Saved N file(s)", "Updated N file(s)", "Nothing was changed · %1 problem(s)", "Select a saved naming layout before applying", "Save folder covers before renaming or moving these files"
- Other: "Matching the MusicBrainz release to the draft…", "Field could not be added · %1"

**Worker operations**
- Suggest: `metadata::propose_selection_consistency` + `metadata_proposal_preview(…, 0.75)`.
- Automatic scripts: the checked saved chains are concatenated and run with `plan_metadata_transformation`.
- ReplayGain scan: `run_replaygain_scan` (§3.2), reading `replaygain/true-peak` and `replaygain/sidecar-only`.
- MusicBrainz proposals: `metadata_proposal_preview(…, 0.5)`.

**Apply pipeline** (`startWritePlan` `:2702-2887`, `finishWritePlan` `:2889-2997`)
1. Build the plan: `metadata::build_metadata_write_plan`, then `stageReplacements` + `operations::plan_artwork_storage` + `merge_artwork_write_plan` for artwork, then `materialize_metadata_draft` + `operations::plan_output_paths` + preflight (`tools.preflight`) for renames/moves, then `assemble_preparation_plan`.
2. If the plan is ready, `reviewFolderImages` (§2.9) runs first when it contains folder images, then `startApply`.
3. `startApply` picks the file-publication applier when a path operation exists, otherwise the metadata applier. Progress is polled every 50 ms.
4. If the plan is blocked, the feedback dialog opens: "Apply blocked" / "Nothing was changed. Fix the problem(s) below, then apply again."
5. On full success the window closes itself.
6. On partial success the feedback dialog opens:
   - Metadata: "Save stopped" / "Saved with problems", summary "%1 saved · %2 failed · %3 stopped. Saved files are done; the files below need attention. Files with recovery problems may already have changed." A retry button "Retry failed / stopped files" is added (tooltip "Retry only unfinished files using the reviewed changes. Changed files and unresolved recovery records remain blocked."). Close becomes "Close editor" once something was committed.
   - File publication: "Update stopped" / "Updated with problems", summary "%1 updated · %2 failed · %3 stopped. Updated files are done; the files below were not touched."
7. Other failures: "Saving metadata failed" / "Updating files failed".
8. Publication notes go to the status bar through `statusMessage`.
9. Per-file state words (`metadata_dialog_helpers.cpp:55-89`):
   - Metadata apply: Waiting / Saving / Saved / Failed / Cancelled.
   - File apply: Waiting / Changing / Unchanged / Changed / Failed / Cancelled.

**Closing** (`:3718-3775`)
- Escape goes through the same path as close.
- Pending artwork changes: QMessageBox warning "Discard artwork changes?" / "The pending artwork changes have not been saved." (Discard/Cancel).
- Artwork operation or Apply running: close is refused with "Cancelling artwork work after in-flight files become safe…" / "Cancelling Apply after in-flight sources become safe…".
- Unsaved drafts: warning "Discard metadata draft?" / "%1 staged change(s) exist only in memory and have not been written to files." (Discard/Cancel).

**Inline child dialogs and prompts**
- **Add metadata field:** `QInputDialog`, window-modal, `bench-metadata-add-field-dialog`, title "Add metadata field", label "Field name:". A case-insensitive unfiltered completer (12 visible) is fed by `metadataFieldNameSuggestions`: present fields, the last 20 recent names, and `metadata_field_suggestion_catalog`. After adding, the new field row is revealed and selected.
- **Save field set:** QInputDialog "Save field set" / "Field set name:". Up to 64 sets of 256 fields each, stored as JSON schema 1 with active/layouts.
- **Loudness sources** (`showLoudnessProvenance`, `:2607-2683`): modeless QDialog `bench-replaygain-provenance-dialog`, title "Loudness sources", 640×320. Read-only `QTableWidget` with columns Track, Track gain, Track peak, Album gain, Album peak, R128 track, R128 album. Cells read "value · draft", "removed · draft", "value · <provenance>" or "—". Close button.
- **Export ReplayGain results:** `getSaveFileName` "Export ReplayGain results", default "replaygain-results.csv", filter "CSV files (*.csv)". CSV header `track,file,integrated_lufs,track_gain_db,track_peak,album_key,album_gain_db,album_peak,status,peak_kind`. Messages: "Exported N row(s) to path" / "Export failed · …".

#### 2.3 `MetadataGridModel` / `MetadataAggregateModel` (`bench/metadata_grid_model.{hpp,cpp}`)
**Roles** (`hpp:23-30`): `metadata_field_state_role` (U+200), `canonical_name` (+201), `cell_values` (+202), `provenance` (+203), `staged` (+204), `baseline_values` (+205), `patch_kind` (+206), `staged_source` (+207).

**Grid model** (rows are tracks; column 0 "File path", then one column per field)
- Column header: "Display · state[ · Draft N]". Tooltip: "Baseline: %1 on %2 of %3 selected tracks[\n%1 staged cells; result state is calculated in preview]".
- Cell display: values joined with "  ·  ", or "(empty field)" / "(remove)" / "—". An empty value is shown as "(empty value)".
- Staged cells use a colour brush and font by change kind (added / changed / removed). Italic means the cell was staged by a script or provider.
- Tooltips:
  - staged: "Draft:\n…\n\nOriginal:\n…\n\nStaged by X\nNot written to file"
  - missing: "Missing on this track"
  - otherwise: "…\n\nSource: <provenance>"
- Undo/redo transactions: 64 MiB per edit, 4 MiB per direct value.

**Aggregate model** (rows are fields; columns Field / Original / Draft; selection-scoped)
- Field column: bold when staged; tooltip "Canonical field: %1".
- Original/Draft display: "(preparing selection…)", "(preparing draft preview…)", "(various across %1 files)", "(various · present on %1 of %2 files)", "—", "(remove)".
- Tooltips: "%1 on %2 of %3 selected files", "Complete draft: … · %4 staged cell(s) · not written to file", "No staged change; Draft currently matches Original", "Computing the complete result for %1 selected files".
- Projections are computed off-thread (`summarize_items` and the draft projection).
- Rejection messages include "A bulk field edit is limited to %1 addressed cells" and "A transformation must change between 1 and %1 cells".

#### 2.4 `MetadataFieldReviewBar` (`bench/metadata_field_review_bar.cpp`)
- Line edit `bench-metadata-field-filter`: placeholder "Filter fields…", clear button, tooltip "Match display or canonical field names; values are not searched."
- Checkbox "Changed fields only" ("Show fields with staged edits in the selected files.").
- Checkbox "Show files" (on by default; toggles the file list's visibility). Tooltip: "Hide the file list to make more room for fields. The selected files stay selected."
- Status: "%1 of %2 fields shown · %3 changed in selected files · Apply includes hidden edits." Also "… · Updating changes… · Apply includes hidden edits." and "Updating changed fields… · Apply includes hidden edits."
- Refresh is debounced by 40 ms. Hidden rows are deselected.

#### 2.5 Scalar delegate (`bench/metadata_scalar_delegate.cpp`)
- Frameless `QLineEdit` editor for the Original/Draft columns (not column 0).
- A single value is prefilled and selected. Otherwise the editor starts empty with placeholder "Type a value" or "Type to replace %1 exact values".
- It commits only when the text was modified.

#### 2.6 Exact value editor (`bench/metadata_exact_value_dialog.cpp`)
- Window-modal, `bench-metadata-exact-values`, title "Edit exact metadata values", 640×420.
- Heading "<Field> — N selected file(s)".
- Context text is either:
  - "The selected files do not currently share one exact value list. Values entered here replace this field on those files.", or
  - "Edit the exact ordered value list applied to the selected files. Duplicates and empty values remain distinct."
- Table with header "Exact value" and numbered rows. Empty values are shown as "(empty value)" in placeholder colour, tooltip "This is one explicit empty metadata value".
- Buttons: "Add value" (maximum 16384; starts editing the new row), "Remove value", "Move up", "Move down".
- Note: "Each row is one value. Empty rows are preserved; use Delete in Properties to remove the field."
- OK (disabled when there are 0 rows) / Cancel. On accept: `aggregate_model_->replaceRowValues`.

#### 2.7 Script editor: `bench/metadata_transformation_dialog.cpp`
**Opening.** "Open script editor…", "Edit selected script…", or a double-click in the script list. Window-modal, `bench-metadata-transformation`, title "Tagging script editor[*]" (the modified marker follows unsaved state), 1080×620. Geometry and splitter are stored under `workspace/metadata-transformation-geometry-v1` / `-splitter-v1`.

**Top row**
- "Script:" combo `…-saved` with "New script" and the saved chains.
- Buttons: "Save" / "Save changes", "Save as new", "Delete".
- "Import…": tooltip "Open a complete native Trackknife tagging script as an unsaved definition for review". Opens "Import Trackknife tagging script" with filter "Trackknife tagging scripts (*.tbtags.json *.json);;All files (*)". If there are unsaved changes, it first asks "Discard unsaved script changes?" ("Importing a native tagging script replaces the current unsaved …").
- "Export…": tooltip "Write the complete current typed definition as versioned native JSON; saved identity and automatic state are not included". Save dialog "Export Trackknife tagging script" with filter "Trackknife tagging scripts (*.tbtags.json);;JSON files (*.json);;All …".

**Left pane**
- "Name:" (default "Untitled script").
- Tabs "Steps" and "Raw script".

**Steps tab**
- "New step:" kind combo. Grouped headers are bold and unselectable:
  - **Set values:** Set one literal value (0), Add one literal value (1), Copy another field (7), Format with tkfmt-1 (10; tooltip "Build the value from an expression, for example %artist% — %title%"), Number by selected-file order (13), Capture fields with tkcapture-1 (16; tooltip "Extract several fields at once from the filename, the full path, formatted text, or another field").
  - **Clean up values:** Trim each value (3), Lowercase each value (4), Uppercase each value (5), Capitalize first character (6), Keep first characters of each value (14).
  - **Split & join:** Split by exact separator (8), Join with exact separator (9).
  - **Remove & replace:** Remove field (2), Remove field when condition matches (15; tooltip "The field is removed when the expression is non-empty, for example $not(%totaldiscs%)"), Remove exact matching values (11), Replace exact matching values (12), Remove listed fields (blocklist) (18; "Remove every named field from the selected files"), Keep only listed fields (allowlist) (19; "Remove every field except the named fields").
- Dynamic fields (`:1207-1266`):
  - "Target field:" placeholder "For example: Title or ALBUM ARTIST", with a completer of present fields.
  - "Value:" is relabelled per kind: "Source field:" / "Separator:" / "Expression:" / "Exact value:" / "Condition:" / "Capture pattern:" / "Fields:". Matching placeholders: "For example: Artist", "Required exact separator" / "May be empty", "For example: %artist% — %title%", "Case-sensitive; may be empty", "For example: $not(%totaldiscs%)", "For example: %tracknumber%. %title%", "Comma-separated, for example: Comment, Encoder" (with a comma-aware completer).
  - "Replacement:"
  - "Start at:" (1–1e9) and "Minimum width:" (0–32)
  - "Characters to keep:" (1–1e6, default 4)
  - "Capture source:" with "Filename and requested parent folders", "Full path", "Formatted tkfmt-1 text", "Metadata field values"
  - "Source expression:" / "Source field:"
- Buttons "Add step" (Enter in a step field also adds) and "Paste script…" (opens §2.7a).
- Steps list: rendered text such as "1. Set X to Y", "Add Y to X", "Remove X", "Remove exact native field X", "… when …", "Remove listed fields: …", "Keep only listed fields: …", "Trim each value of X", "Copy B to A", "Split X by S", "Join X with S", "Remove values of X equal to V", "Replace values of X equal to V with R", "Number X from N by selected-file order", "Keep the first N characters of each value of X", "Format X as E", "Capture … with P".
- Buttons "Remove step", "Move up", "Move down". Maximum 256 steps.
- Tab tooltip: "Steps run in order; each step sees the result of every earlier step".

**Raw script tab**
- `QPlainTextEdit`. Placeholder `$if($eq(%totaldiscs%,1),$delete(discnumber)$delete(totaldiscs))`. Tooltip: "Valid source compiles into the steps on the Steps tab; arbitrary script is never executed".
- Read-only diagnostics box (max 110 px): "Error|Warning · line L, column C · msg", "Ready · N typed rules …", "Raw mode is unavailable: …", "Enter cleanup source to generate typed rules."

**Right pane**
- Bold "Preview" heading. Tooltip: "Updates automatically as you edit; nothing enters the draft until you add the previewed changes".
- Summary label: "Add a step to see a preview.", "Updating preview…", "%1 final cell change(s) across %3 selected file(s) · add to draft when ready", "The script produces no changes for the selected files.", "Transformation preview failed · … · step N · file row M", "The preview is stale or could not fit in the draft. It will refresh automatically.", "Cancelling preview…"
- `QTreeView` with the preview model (§2.7b).
- The preview is debounced by 400 ms and runs `plan_metadata_transformation` off-thread.

**Footer**
- Catalog status. Messages include "Loading saved scripts…", "%1 saved script(s) available", "Editing a new script", "Loaded script · %1", "Unsaved changes · Save to keep them", "Unsaved name change · Save to keep it", "Saving…", "Saved · %1", "Could not save script · %1", "Cannot save script · %1", "Enter a script name before saving.", "Deleting saved script…", "Saved script deleted", "Imported · review, preview, and Save to keep this script", "Native tagging script exported", "Saved scripts are unavailable in this session."
- Buttons "Add to draft" (AcceptRole, not default) and "Close". "Add to draft" calls the stage callback, which runs `grid_model_->stageTransformation`.

**Close.** While a preview runs, close cancels it first. With unsaved changes: QMessageBox "Discard unsaved script changes?" / "This script differs from its saved version. Save it before closing, or explicitly discard the changes." (Discard/Cancel).

**Store.** `MetadataTransformationStore` load/save/remove, async, backed by persistence.

##### 2.7a Paste-script importer: `bench/metadata_rule_script_import_dialog.cpp`
- Window-modal, run with `exec()` from "Paste script…". objectName `bench-metadata-rule-script-import`, title "Generate rules from script", 760×560.
- Explanation: "Paste a Picard-style cleanup script. Trackknife translates the supported $unset/$delete, $set, $if, $and/$or, $eq/$ne, $not, and $left subset into editable typed rules; it does not store or execute the pasted script. Here, $unset generates an actual Remove field rule. Imported removals match the exact native field name, ignoring ASCII case but preserving separators."
- Source editor, placeholder "$unset(comment)\n$set(date,$left(%date%,4))".
- Diagnostics (read-only, max 150 px): "Paste a script to inspect generated rules.", "Ready · N generated rules", "Ready · N generated rules with warnings", or "Error|Warning · line, column · msg" lines.
- Buttons: Cancel, "Append generated rules", "Replace rules" (both enabled only when rules exist and there are no errors). Translation uses `metadata::import_metadata_rule_script`.
- If appending would pass 256 steps, the editor reports "Appending those rules would exceed the 256-step limit."

##### 2.7b Preview model: `bench/metadata_transformation_preview_model.cpp`
- Tree with columns Field / Old / New. Each change row has one child detail row: "File" | track label (or "File N") | "Produced by step N".
- The Field column shows "Exact native: X" for exact-native matches. "(missing)" and "(removed)" mark absent values.
- Tooltips: "Exact native field · expand to see the affected file and producing step", "Semantic field · …", "Step N produced the final value for X".

#### 2.8 Artwork section: `bench/metadata_artwork_section.cpp` (objectName `bench-metadata-artwork-section`)

**Tab layout**
- Status label (starts as "Open Artwork to inspect the selected files"), progress bar (170 px) and "Stop" button. Stop tooltip: "Stop after the files already in progress are safe".
- Action row:
  - "Fetch cover…": "Choose a cover: an image beside the files, or one the Cover Art Archive has for the release. A front cover replaces the existing one."
  - "Add image…": "Add one PNG or JPEG to every selected writable file"
  - "Copy to Selection": "Add the selected image to the other selected writable files"
  - "Export…": "Export selected encoded images without overwriting existing files"
  - "Replace…": "Replace selected embedded covers; external image files are kept with one PNG or JPEG"
  - "Remove": "Remove selected embedded covers; external image files are kept"
- Draft row:
  - Help text: "Select covers with Ctrl/Shift or Ctrl+A. Changes are saved only with Save artwork.\nExternal images are shared files: Remove edits embedded covers and keeps external files." In unified mode (inside the tagger): "… Apply saves tags and artwork together. External image files stay on disk."
  - Buttons "Undo selected" ("Undo the selected pending changes"), "Discard changes", "Save artwork" (hidden in unified mode).
- Pending table (hidden when empty; max 240 px; Delete undoes the selection). Columns: File, Change, Cover, New image, Before, After. Values include "Picture N", "None", "Unavailable", "Removed", "Loading…".
- Inventory table "Artwork inventory" (Delete removes). Columns: (thumbnail), File, Role, Image ("MIME · WxH · size"; tooltip with SHA-256 and description), Source ("Embedded" / "External" / " · same image as row N"). Rows marked for removal are struck out.
- Empty state: "No artwork inventory loaded" / "No artwork found for the selected files".
- "Problems" pane (max 150 px). Columns: File, Artwork source, Type, Problem. "View-only" rows give one of: "Artwork changes need a FLAC, MP3, or MP4 file; this file is view-only", "The file changed since the tag editor opened; reopen to edit artwork", "Artwork changes are unavailable".

**Status messages**
- "Select at least one file to inspect artwork"
- "Artwork inventory is limited to %1 physical sources; narrow the file selection"
- "Preparing artwork inventory for N source(s)…", "Reading artwork for N source(s)…"
- "N image(s) across M source(s)…"
- "%1 pending artwork changes · Apply saves tags and covers together" (unified mode) or "… · review below, then Save artwork"
- "No pending artwork changes"
- "Enable a cover destination in Cover settings"
- "Select at least one writable FLAC, MP3, or MP4 file" / "Select at least one writable embedded cover"
- "Checking %1 artwork … against fresh files…", "Saving artwork · x of y", "Exporting · x of y"
- Feedback dialogs: "Artwork change blocked", "Saving artwork failed", "Artwork save stopped" / "Artwork saved with problems", "Export failed", "Export finished with problems"

**Compact cover pane** (next to the Fields table, `:515-575`; `cover_thumbnail.cpp`)
- 112×112 `CoverThumbnail`. Accessible name "Front cover; drop or paste an image". Tooltip: "Drop an image file or paste an image to stage the front cover". Shows "No front cover" or "Multiple covers" when there is no single image.
- Accepts a single local file drop, an image drop, or Ctrl+V paste, only while editable. A pasted image is cached as PNG (maximum 16 MiB; error "Could not cache the pasted cover (maximum 16 MiB)").
- "Fetch cover" button below it.
- Right-click menu: "Fetch cover", "Choose file…" (`getOpenFileName` "Choose front cover", filter "Artwork images (*.png *.jpg *.jpeg)"), "Remove" (requires embed policy), separator, "Open Artwork tab", "Cover settings…" (opens Settings on Covers).

**File dialogs**
- "Choose artwork to add", filter "Artwork images (*.png *.jpg *.jpeg);;All files (*)", followed by a QInputDialog item picker "Artwork role" / "Store as:" with Front cover, Back cover, Artist, Disc, Icon, Other.
- "Choose replacement artwork".
- `getExistingDirectory` "Choose artwork export directory". Exported files are named `artwork-N-<role>.<ext>`.

**Cover picker** (`openCoverPicker`, `:1170-1333`)
- Window-modal `bench-metadata-artwork-picker`, title "Choose a cover", 600×440.
- Hint: "A front cover replaces the files' existing one; other images are added with their type."
- Tree with columns (icon 72 px), From, Type, Details.
  - Local images beside the files: "Front", details "Beside the files · WxH".
  - Cover Art Archive images: From "Cover Art Archive"; Type is the type list, "Front" or "Untyped"; details add " · not approved" when needed. Thumbnails load asynchronously.
- Status: "Asking the Cover Art Archive…", "The Cover Art Archive has no images for this release", "The Cover Art Archive could not be asked · %1", "No images were found".
- Buttons: Close and "Use this image" (double-click also works).
- Fetch messages: "Fetching the %1 image from the Cover Art Archive…", "No image was added · %1", "The selection changed while fetching; no image was added".
- CAA use requires every selected file to carry the same MUSICBRAINZ_ALBUMID (draft or embedded) (`metadata_properties_dialog.cpp:1376-1398`).

**Services**
- `ArtworkCoverArtService`: `fetch_listing` / `fetch_bytes` / `store_image`, through the MusicBrainz fetch. Images are stored in a temporary directory as `<release>-front.png|jpg`.
- `tools_.artwork` for inventory and image bytes; `plan_artwork_storage` for planning.
- An `ArtworkWritePlanApplier` when not in unified mode.

#### 2.9 Folder cover review: `bench/cover_review.cpp`
- `reviewFolderImages(parent, images, apply)` is called only when a plan has folder images; otherwise `apply` runs directly.
- Window-modal `bench-folder-cover-review`, title "Review folder covers", 720×340.
- Note: "Save publishes these folder images and the reviewed media edits. Each file has its own recovery journal; a later failure can leave earlier files saved. Existing folder images retain a recovery backup."
- Table with columns Destination, Change ("Already matches" / "Replace (retain backup)" / "Create"), Incoming image.
- Buttons Save and Cancel. Save runs `apply()`.

#### 2.10 Preparation feedback: `bench/preparation_feedback_dialog.cpp`
- Generic factory `createPreparationFeedbackDialog(title, summary, rows, parent)`. Window-modal, `bench-preparation-feedback`, 720×140 without rows or 720×340 with rows.
- Word-wrapped summary label.
- `QTreeWidget` "Files that need attention" with columns File and Problem (elided in the middle, tooltips carry the full text).
- Close button (`bench-preparation-feedback-close`) in button box `bench-preparation-feedback-buttons`. Callers may add buttons, e.g. Retry.
- Used by: the tagger (Apply blocked/failed/problems, "ReplayGain scan problems" with "%1 file(s) measured no usable loudness; every other file is staged."), the artwork section, and the window's "Interrupted file work" dialog (§9.6).

#### 2.11 MusicBrainz identify: `bench/musicbrainz_identify_dialog.cpp` and `musicbrainz_track_match_widget.cpp`
**Opening.** The tagger's "Identify…" button. Singleton per tagger. Window-modal, `bench-musicbrainz-identify`, title "Identify with MusicBrainz", minimum 640×400, 940×520.

The dialog is a `QStackedWidget` with two pages.

**Page 1: search**
- "Artist:" prefilled from albumartist/artist; placeholder "Artist name (optional if you enter an album)".
- "Album:" placeholder "Type an album title — existing tags are not required".
- "Search" button (default).
- "Fingerprint files": tooltip "Identify by audio fingerprint (AcoustID) — works with no usable tags at all; candidates are ranked by how many selected files match each release". Enabled only when both the fingerprint and acoustid services exist.
- Status label, starts as "Searches MusicBrainz by text — no MusicBrainz tags are needed".
- Results tree "MusicBrainz release versions" with columns Match, Album, Artist, Tracks, Media, Version.
  - Match reads "Same track count" / "Different track count", or "x/y files" for fingerprint results. Tooltip "Search relevance: %1/100 · ranking score: %2".
  - Version is date · country · disambiguation · label · catalog number.
  - Media is formats joined with " + ", plus " × N" for several media.
- Buttons Close and "Match this version…" (tooltip "Load the release tracks and review their assignments to local files"; double-click also works).
- Status messages: "Searching MusicBrainz…", "%1 release version(s) · every version of an album is its own row", "No releases found — adjust the search text", "Enter an artist or an album to search", "Search failed · %1", "Fingerprinting file x of y…", "Looking up file x of y…", "Fingerprinting failed · …", "AcoustID lookup failed · …", "No AcoustID matches — try the text search, or the files may be unsubmitted", "Loading candidate x of y…", "%1 fingerprint candidate(s) · matched x of y files · every version is its own row", "No AcoustID candidate could be loaded", "Loading the release's track list…", "Loading failed · %1", "This candidate has no usable release id".
- Search: `build_release_search_url` (limit 25), then `parse_release_search` and `rank_release_candidates`.
- Fingerprint: fpcalc, then AcoustID with score ≥ 0.5; release votes; top 5 candidates via `build_release_lookup_url`.

**Page 2: track match widget** (`bench-musicbrainz-track-match`)
- Heading "Match files to <title> · <date> · <country>".
- Help: "Each row pairs a local file with the MusicBrainz track beside it. Drag files in the left pane or use Move file up/down to change pairings. MusicBrainz tracks stay in album order. Review before staging."
- Splitter with two scroll-synced trees:
  - Left (drag-reorder within the view only): columns "Local filename", "Length", "Pairing" ("→ d.t" bold link colour, "— Gap", "Unmatched"; empty slot "No local file").
  - Right: "MusicBrainz track" ("d.t · Title", or "— No tags will be staged"), "Length".
  - Paired rows are tinted.
- Buttons:
  - "Move file up" / "Move file down" (Alt+Up / Alt+Down; tooltips "Swap local files with the row above (Alt+Up)" / "… below (Alt+Down)")
  - "Leave unmatched" ("Move this file below the album tracks, leaving a gap. It will receive no tags.")
  - "Match by filename" ("Pair files in natural filename order with the album tracks. This replaces the current pairings.")
  - "Reset file order" ("Pair files in their original selection order with the album tracks.")
- Status: "Preparing suggested matches…", then "%1 files paired · %2 files unmatched · %3 album tracks without a file. Pairings are suggestions until you Stage matches." Limit message: "Track matching supports up to 2,000 local files and 2,000 release tracks. Choose a smaller selection."
- Footer: "Back to releases" and "Stage matches" ("Confirm the displayed assignments and stage tags for matched files. Unmatched files are untouched; Apply writes the draft later.").
- Core calls: `align_release_tracks` (off-thread, confidence ≥ 0.5), `confirm_release_mapping`, `release_metadata_proposals`. Staging hands the proposals to the tagger and closes the dialog.

#### 2.12 Supporting code
- `metadata_dialog_helpers.cpp`: `display_utf8`, `pluralized`, `display_plan_values` (maximum 8 values of 512 characters each, joined with "  ·  ", "… +N values"), and the apply state words (§2.2).
- `file_work_tools.hpp`: the `FileWorkTools` struct (access, scanner, probe, artwork, stage, preflight), `engineFileWorkTools(RemoteFileWork)` and `engineLookupService`. All tagger, ReplayGain and artwork I/O goes through these; there is no UI.
- `remote_mount.cpp`: `RemoteMount::configured()` from `library/remote-folder` / `library/remote-mount`; `to_local` / `to_remote` path rebasing. No UI. It drives the status message "%1 of %2 tracks are not reachable on this computer and were left out. Where that engine's music is reachable here is set in Settings → Engine." (`bench_metadata_operations.cpp:285-293`).

---

### 3. ReplayGain

#### 3.1 `bench/replaygain_dialog.cpp`
**Opening.** Edit ▸ ReplayGain…, or the track/dynamic context menu Tools ▸ ReplayGain… (`bench_metadata_operations.cpp:902-936`). Requires engine file work; otherwise the status bar shows "Measuring ReplayGain is done by the engine on …". Modeless, a new dialog each time, `bench-replaygain-dialog`, title "ReplayGain N track(s)", 640×500.

**Layout**
- Rich text: "**Calculate ReplayGain tags**<br>Measure loudness and save track and album volume adjustments. Audio samples are not changed."
- "Scan mode:" combo with "Albums by release tags", "Albums — combine disc editions", "Selection as one album", "Track gain only", "Custom album grouping…". Key `replaygain/grouping` (index).
- Expression line edit (only for Custom; placeholder "tkfmt-1, e.g. %album%"). Key `replaygain/grouping-expression`.
- "Album groups in the selected files" + "Preview groups" button.
- Groups list (no selection). Initial text "Preview groups before scanning to check album boundaries."; after a change "Grouping changed — preview again to check album boundaries."
- Group entries: "Artist — Album · N tracks" or "Selection as one album · N", "… N more album groups" (200 maximum), "N tracks without album grouping — track gain only".
- GroupBox "Storage and peak measurement":
  - "Keep audio files untouched — store in sidecar files". Tooltip: "Otherwise write tags where safely supported, with sidecar fallback. CUE tracks store gains in their CUE sheet." Key `replaygain/sidecar-only`.
  - "Use true peak for clipping protection". Tooltip: "Uses inter-sample peak estimates instead of sample peaks for the saved ReplayGain peak values." Key `replaygain/true-peak`.
- Status label, starts as "N tracks selected. Scan writes tags when measurement finishes."
- Progress bar and a read-only problems box (max 140 px; one "file: detail" per line).
- Buttons: "Scan and write tags" (default), "Stop", stretch, "Close".

**Flow**
1. Capture: reader, `capture_uncached_metadata_sources`, `StagedMetadataSelection`, `loudness::assign_loudness_groups`.
2. Scan: `run_replaygain_scan`, progress "Measuring loudness · x of N files…".
3. Stage and write: "Writing ReplayGain tags… Audio samples are not changed." `loudness::stage_replaygain`, then `build_metadata_write_plan`, then the engine applier.
4. Result: "Saved ReplayGain tags to N target(s). Audio samples unchanged. [M failed.]"
- Other messages: "Reading the selection…", "Stopping… Any completed tag writes are retained.", "Stopped. No tags written.", "N tracks ready. Scan and write tags to measure loudness and save the results.", "Nothing to scan.", "Nothing measurable to write.", "Enter a tkfmt-1 grouping expression, e.g. %album%".
- Settings are written when a run or preview starts.

#### 3.2 `bench/replaygain_scan.{hpp,cpp}`
- `run_replaygain_scan` wraps `loudness::measure_replaygain` (EBU R128, grouping, true peak, sidecar) with an atomic progress counter and the `LoudnessScanner` override.
- Returns `ReplayGainScanOutcome`: proposals, problems, retry_items, export_rows (CSV). Shared by the tagger and the dialog. No UI of its own.

---

### 4. Convert: `bench/convert_dialog.{hpp,cpp}`
**Opening.** Edit ▸ Convert files… or Tools ▸ Convert files… (`bench_metadata_operations.cpp:227-380`).
- Remote tabs use the mount path, or an engine download (`download_original`); unreachable items are dropped with a status-bar message.
- Modeless, `bench-convert-dialog`, title "Convert N file(s)", 720×520.
- `filesConverted` triggers a local library refresh.

**Form**
- **Preset:** combo `bench-convert-preset`. Built-in presets come first, then a separator and the saved presets. Presets whose encoder is unavailable are disabled, with the probe detail as tooltip. Buttons:
  - "New…" (tooltip "Save a new encoder preset starting from the selected one")
  - "Export…" ("Export the selected encoder preset as JSON")
  - "Delete" (visible only for saved presets; no confirmation)
- **Into:** destination combo ("Custom" + saved destinations; hidden if there are none), destination line edit (placeholder "Destination folder"), "Browse…" (`getExistingDirectory` "Choose the conversion destination").
- **Layout:** combo ("Custom" + saved naming layouts; row hidden if there are none). Choosing a layout fills Folders/Names; typing switches back to Custom.
- Checkbox "Mirror source folders". Tooltip: "Recreates each source's complete folder path beneath the destination, keeping source file names instead of the expressions". Disables the expressions.
- **Folders:** default "%albumartist%/%album%". **Names:** default "%tracknumber% - %title%".
- **Resample:** "Keep source rate", "44.1 kHz" … "192 kHz", "Downsample to 44.1 kHz if higher", "Downsample to 48 kHz if higher". Tooltip: "Encoders that only speak certain rates still constrain the result — Opus maps every choice into its 48 kHz family".
- **Bit depth:** "Preset default", "16-bit (dithered)", "24-bit", "Keep source depth". Tooltip: "Stored bit depth for lossless output; Opus and other float-based encoders have no stored depth and ignore this".
- **Channels:** "Keep source channels", "Mono", "Stereo".
- **Permanent volume adjustment:** "Off — do not change volume", "Permanently change volume using track ReplayGain", "Permanently change volume using album ReplayGain". Always reset to Off. Tooltip: "Permanently changes PCM before encoding; stale ReplayGain tags are removed". Warning label: "No permanent volume adjustment. This does not calculate ReplayGain tags." or, rich text, "<b>Warning: permanently changes the audio samples in the converted files.</b> Uses existing ReplayGain values; does not calculate or write new gain tags. Removing tags cannot undo this. Source files are not changed."
- Checkbox "Embed cover art". Tooltip: "Carries each source's cover image (embedded pictures first, then cover/folder/front siblings) into the converted file".
- **Parallel files:** spin 1…`convert::maximum_conversion_parallelism` (default 4).

**Below the form**
- Preview list: the first 200 target paths, then "… and N more". Mirror mode shows "Recreating full source paths beneath the destination".
- Progress bar and status label: "Choose a preset.", "Choose a destination folder.", "The destination folder does not exist.", "Nothing to convert.", "No convertible files (%1 with problems).", "%1 problem(s) block the plan.", "%1 file(s) ready.", "Converting · x of N files", "Converted x of N files.[ Stopped early.][ M failed.]", "Stopping after the files already in flight…", "Select an encoder preset to export.", "Exported encoder preset to %1", "Could not export encoder preset: %1".
- Problems box (max 140 px).
- Buttons: "Convert" (default; enabled when the plan is ready), "Stop", "Close".
- The preview refreshes after a 150 ms debounce and uses `operations::plan_output_paths` with `ConvertedPublicationPolicy`.

**Gain confirmation.** When gain is on: QMessageBox Warning `bench-convert-gain-confirmation`, title "Permanently change audio volume?", text "This will bake track|album (with track fallback) ReplayGain into the audio samples of the converted files. It does not calculate or write ReplayGain tags. Removing tags cannot undo the change; recreate the outputs from the original sources instead.\n\nSource files will not be changed." Buttons "Convert and change volume" and Cancel (default and escape).

**Run.** `convert::scan_conversion`. Remote files are first downloaded into `CacheLocation/convert-<id>`.

**Close.** While running, close cancels and is ignored.

**QSettings.**
- Written on run: `convert/preset`, `convert/destination-root`, `convert/directory-expression`, `convert/basename-expression`, `convert/parallelism`, `convert/resample-rate`, `convert/bit-depth`, `convert/channels`, `convert/embed-artwork`, `convert/mirror-structure`; `convert/gain` is removed.
- Per saved preset, group `convert/job-presets/<presetId>`: schema=1, backend-versions, destination-root, directory-expression, basename-expression, mirror, resample, bit-depth, channels, artwork, parallelism (gain removed). Restored when that preset is chosen.

**Encoder preset editor** (`EncoderPresetEditor`, `:76-186`)
- Window-modal (`open()`), title "New encoder preset", `bench-preset-editor`.
- Form: "Name:" (placeholder "Preset name"), "Format:" (FLAC (lossless) / Opus / MP3 / Ogg Vorbis), "Rate control:" (Bit rate / VBR quality), "Bit rate:" (8–2000 " kbps", default 192), "Quality:" (-2–12, default 4).
- Save / Cancel. Save needs a name. The editor is prefilled from the selected preset. Saved through the async `ConvertPresetStore`.
- Export JSON: format "trackknife-encoder-preset-1", saved via "Export encoder preset", default name "<id>.trackknife-preset.json", filter "JSON (*.json)".

---

### 5. Preparation feedback and remote mount
Covered in §2.10 and §2.12.

---

### 6. Desktop notifier and MPRIS (user-visible parts)

**Notifier** (`bench/desktop_notifier.cpp`)
- Sends a freedesktop Notify with app name "Trackknife", icon "audio-x-generic", hints urgency 1, transient, suppress-sound, desktop-entry "trackknife". It replaces the previous notification by id.
- Summary is the title (or the track key); body is "Artist — Album" (markup-escaped).
- Fires only when playback moves to a new track while Playing, respects "background only" (active window), and never retro-notifies.
- Test message: "Trackknife" / "Track-change notifications are working."
- Errors: "No desktop session bus is available". The window status bar shows "Notification failed: %1" (`bench_transport.cpp:2092-2095`).

**MPRIS** (`bench/mpris_service.cpp`)
- Bus name `org.mpris.MediaPlayer2.trackknife` (or `.instanceN`).
- Identity "Trackknife", DesktopEntry "trackknife", CanQuit false, CanRaise true. Raise shows, raises and activates the main window.
- Player: PlaybackStatus; LoopStatus "None"; Rate 1; no shuffle; metadata `mpris:trackid` (`/de/trackknife/track/<hash>`), `mpris:length`, `xesam:title`, `xesam:artist`, `xesam:album` (no artUrl).
- Volume read/write, Position, CanGoNext/Previous/Play/Pause/Seek, CanControl.
- Methods Next, Previous, Pause, PlayPause, Stop, Play, Seek, SetPosition; OpenUri does nothing.

---

### 7. Last.fm
Covered in §1.9. There is no separate window.

---

### 8. QDialog / QMessageBox / QInputDialog / QFileDialog usage not covered above

| Where | Kind | Title / text |
|---|---|---|
| `bench/main.cpp:205-208` | `QMessageBox::information` after startup | "Workspace restore": "Workspace restored. Previous database: %1%2" or "Workspace restore failed: %1" |
| `bench/bench_list_tabs.cpp:487-546` | QFileDialog (save, window-modal) | "Back up Trackknife workspace database", filter "Trackknife workspace database (*.sqlite)", default "trackknife-workspace.sqlite". Also writes `<path>.settings.ini`. Status messages: "Backing up workspace database…", "Workspace backed up to %1 and %2", "Workspace backup failed: …" |
| `bench_list_tabs.cpp:548-570` | `getOpenFileName` + `QMessageBox::warning` | "Restore Trackknife workspace database", filter "Trackknife workspace database (*.sqlite);;All files (*)". Then "Restore workspace database": "Trackknife will validate and restore this database at the next start. The current database will be retained beside it for rollback. Close Trackknife now?" (Close/Cancel). Sets `recovery/pending-workspace-restore` and `recovery/pending-settings-restore`, then closes the window |
| `bench_list_tabs.cpp:850-897` | QMessageBox (open, non-native) `bench-list-conflict` | "List changed elsewhere": "“%1” was saved from somewhere else since you opened it. Keep which?" Buttons "Reload theirs", "Keep mine", "Save mine as a copy" (default; creates "%1 (mine)") |
| `bench_list_tabs.cpp:1846-1858` | QMessageBox exec | "Close unsaved list": "Discard the unsaved contents of “%1”?" Yes/No (default No). A pinned tab gives the status message "Unpin this list before closing it" |
| `bench_list_tabs.cpp:1889,1952,1975,2091` | QInputDialog::getText | "New list"/"Name:"; "Save working list"/"Name:" (for scratch lists); "Rename list"/"Name:"; "New tab"/"Name:" (default "Selection", from Copy/Move to list ▸ New tab…) |
| `bench/bench_lists_panel.cpp:230-316` | Lists side-panel context menu | Open / Close / Save / "Rename…" (QInputDialog "Rename list"/"Name:"; engine `list.rename`) / "Delete…" (QMessageBox `bench-lists-delete`, "Delete list": "Delete “%1”? Its files are not touched.", buttons Delete and Cancel; engine `list.delete`) / "New list" |
| `bench/search_dialog.cpp:297,303,459,481,496` | Input and message boxes | Preset parameter prompts (`getInt`/`getText` titled with the preset title and prompt); "Save search"/"Name:"; "Rename search"/"Name:"; "Delete saved search": "Delete “%1” from saved searches? Existing result tabs remain available." |
| `bench/local_library_panel.cpp:1345,1355` | Add music folder | Remote engine: QInputDialog "Add music folder" / "Folder on %1, as that machine sees it:". Local: `getExistingDirectory` "Add music folder" |
| `bench/bench_media_ingest.cpp:800-830` | File dialogs | "Open files", "Open folder", "Bookmark folder" |
| `bench/bench_playlists.cpp` | QFileDialog | M3U8 import/export (§0) |
| `bench/settings_dialog.cpp:436` | `getExistingDirectory` | "Music root" |
| `bench/output_profiles_widget.cpp:248` | `getExistingDirectory` | "Choose move destination" |

`QProgressDialog` is included in `bench_metadata_operations.cpp` but never used. There are no `QColorDialog` uses.

---

### 9. Other secondary windows, popups and bars

#### 9.1 Command palette: `uicommon/command_palette.cpp`
- Opened from Workspace ▸ Commands… (Ctrl+Shift+P). Singleton; window-modal `open()`; `WA_DeleteOnClose`; title "Commands"; 620×480.
- Filter: placeholder "Find a command by name or shortcut…". Up/Down keys move the list selection. Matching is on all words against name, shortcut and objectName.
- List "Matching commands": the right-aligned shortcut is drawn by a delegate; checkable commands that are on get " · On"; tooltip from the action.
- Status: "No matching command", "Unavailable for the current selection or connection.", "Press Enter to run this command.", or the action's status tip.
- Static label "Change shortcuts in Settings → Shortcuts."
- Buttons "Run" (default) and "Close". Only enabled, visible actions from `workspace_command_ids` are listed.

#### 9.2 Open list dialog (`bench_list_tabs.cpp:711-801`)
- File ▸ Open list… (Ctrl+Alt+O). Singleton, window-modal, `bench-open-list`, title "Open list", 480×420.
- Tree with columns List and Tracks. Engines are grouped ("This computer" or the engine name). Saved lists come first; working lists get " (working)". Empty groups show "No lists" or "Cannot say: %1".
- Filled by engine request `list.all`. Opening a list uses `list.get`; error status "Could not open the list: %1".
- Buttons Open and Cancel; activating an item also opens it.

#### 9.3 Search dialog: `bench/search_dialog.cpp`
- Workspace ▸ Search… (Ctrl+Shift+F). Persistent modeless singleton (`search_dialog_`), title "Search", 720×480. It follows the current tab's engine.
- Top row:
  - Scope combo: "This computer" (local), "Current tab" (tab), plus other engines.
  - Input: "Search…" with clear button.
  - Checkbox "Query". Tooltip: "Interpret the input as a tkq query, e.g. genre HAS jazz AND date GREATER 1990. Library history: HISTORY(albumplaycount) EQUAL 0, or HISTORY(albumdayssinceplayed) GREATER 180." Key `search/query-mode`.
- Saved-searches row:
  - "Browse presets" (tooltip "Choose a starting point; adjust the query and Save as… to keep it"). Menu grouped by topic; empty: "No presets supported in this scope".
  - Combo "Saved searches…" (item tooltip "scope · Query/Words\nexpr").
  - Buttons "Save as…", "Update" (tooltip "Replace the selected saved search with the current query, mode, and scope"), "Rename…", "Delete…".
- Saved status: "Loading saved searches…", "Saving search definitions…", "N saved search(es) · select one to run it again".
- Error label.
- Results list (grouped headings "Artists (n)", "Albums (n)", "Tracks (n)"). Context menu: "Add to current tab", "Play next", "Replace current tab", "Open in new tab". Activating an item adds it.
- Status line: "Searching…", "Invalid query", "No local tab is active.", "N artist(s) · N album(s) · N track(s)" / "N match(es)", "Current-tab searches support at most 100,000 rows."
- Buttons "Open results in tab" (tab named "Search: <query>") and "Close".
- Core: `engine::Workspace` saved searches; `query::compile_tkq` / `compile_tkq_word_search`.

#### 9.4 Dynamic playlists: `bench/dynamic_playlist_dialog.cpp` and `bench_dynamic_playlists.cpp`
- File ▸ Dynamic playlists…. Singleton; modeless; `bench-dynamic-playlists`; title "Dynamic playlists"; 900×720.
- Explanation: "Save rules or a Last.fm source, then refresh to see the matching tracks. Rules update while this window is open. Last.fm refreshes draw a fresh selection when requested. Opening a snapshot keeps that list stable while you listen."
- Catalog row: library combo (engines) + saved-definitions combo ("New dynamic playlist…" + names) + "Save definition" + "Remove" (no confirmation).
- Form:
  - "Name:"
  - "Source:" with "Library rules", "Last.fm similar tracks", "Last.fm loved tracks", "Last.fm top tracks (all time)", "Last.fm tag tracks"
  - "Rules:" (placeholder "genre HAS rock AND rating GREATER 6"; tooltip "Ratings use 0–10; 8 means four stars. Example: genre HAS jazz SORT BY %album%")
  - "Seed artist:", "Seed track:", "Last.fm user:", "Last.fm tag:" (rows shown per source)
  - "Maximum tracks:" (1–500, default 100)
  - Checkbox "Shuffle results on refresh" / "Shuffle selected tracks". Tooltip for Last.fm sources: "Each refresh picks a fresh selection, favouring tracks outside the previous result. This option also randomizes their order; otherwise Last.fm ranking determines the order."
- Actions: "Refresh", "Stop", "Open snapshot in new tab".
- Status messages: "Choose Refresh to evaluate this definition.", "%1 tracks · rules update automatically while this window is open", "%1 tracks selected from %2 library matches · %3 Last.fm tracks not found[ · all available matches included]", "Stopped", "Give the playlist a name", "Definition saved", "The server connection changed. Reopen Dynamic playlists for the current library."
- Result table: `QueueTableView`, drag-only.
- Right-click menu: Play, Up Next actions, Go to artist / Go to album, Tools ▸ Edit tags… / ReplayGain… / Convert files…, rate menus, Copy to list ▸ New tab… / tabs, "Open editable snapshot…" (tooltip "Manual edits apply to a separate list; the dynamic definition stays unchanged."), Last.fm submenu.
- Rules re-run on library changes and on a 30 s poll. Uses the `lastfm/api-key` QSettings key.

#### 9.5 Quick album / Quick track popup: `bench/quick_pick_popup.cpp`
- Ctrl+Shift+A / Ctrl+Shift+T. `QFrame(Qt::Popup)`, width 420–640 × 420, centred 90 px below the top of the central widget.
- Input placeholder: "Album, artist or year — e.g. doors 1967" or "Title, artist, album or year — e.g. crystal doors". A scope label shows the library name.
- Results list with a custom row delegate; details include "N tracks" and "added today / yesterday / N days/weeks/months/years ago".
- Status: "Searching…", "Recently added — type to search the whole library", "No albums match." / "No tracks match.", "First %1 — type more to narrow them.", idle hints.
- Keys label: "Enter add · Shift+Enter replace and play · Ctrl+Enter play next · Ctrl+Shift+Enter add to Up Next · Alt+Enter new tab". Up/Down/PgUp/PgDn/Esc also work.

#### 9.6 "Interrupted file work" (`bench_metadata_operations.cpp:1556-1616`)
- Opened automatically when engines report unfinished journals. Uses the preparation feedback dialog, modeless `show()`.
- Summary: "Trackknife could not finish or safely undo N earlier operation(s). The listed files were left as they are — check them before editing further."
- Acknowledged IDs are stored in `workspace/acknowledged-interrupted-operations-v2` (v1 is migrated).
- Status bar: "Recovered N interrupted file operation(s)".

#### 9.7 Library folders widget and dialog (`local_library_panel.cpp:1275-1442`)
- The library panel's "Folders…" button. For the local library it opens Settings on the Library page. For other libraries it opens a standalone modeless dialog `local-library-folders-dialog`, titled "Library folders on %1" or "Local library folders", 560×320, with a Close button.
- Widget contents:
  - Explanation, local: "Choose the folders to browse and search as your local music library. Folder changes are saved immediately. Removing a folder leaves its files untouched."
  - Explanation, remote: "Folders on %1 for its engine to index. Give each path as that machine sees it. …"
  - "Only Refresh in the Library sidebar scans your folders for music."
  - Roots list with suffixes " — unavailable" / " — not scanned" (tooltip is the error), error label, buttons "Add folder…" and "Remove".
- Engine calls: `add_root`, `remove_root`, `roots`.

#### 9.8 List naming, backup and restore prompts
See §8.

#### 9.9 Bottom toolbars (non-modal, bottom dock area)
- **`TrackListFindBar`** "Find in list" (`track_list_find_bar.cpp`): query field (placeholder "Find in current list"; tooltip "Search any metadata value, the duration, the audio format, or the source path/URI"), Previous (tooltip "Previous match (Shift+Enter or Shift+F3)"), Next ("Next match (Enter or F3)"), Close ("Close find (Escape)"). Status: "Searching…", "No matches", "Track %1 of %2", "Wrapped · Track …", "List changed — search again", "Selection changed — search again", "List is empty".
- **`LocalListEditBar`** "Edit local list" (`local_list_edit_bar.cpp`): tkfmt-1 sort expression (tooltip "Sort the entire list using tkfmt-1. Example: %album%|%tracknumber%|%title%"), direction Ascending/Descending, "Sort list", Close/Cancel. Status: "Preparing list edit…", "Reading cached rows… x / y", "No changes needed", "%1 completed — Undo is available", and the limit messages.
- **`PlaylistTransferBar`** "Portable playlist" (`playlist_transfer_bar.cpp`): progress and status ("Importing playlist… N entries" / "Exporting playlist…"), Cancel/Close. Results: "Imported|Exported N entries", "Playlist transfer cancelled", and the limit errors.

#### 9.10 Engine folder chooser: `bench/engine_folder_dialog.cpp`
- Used only by the Naming page's "Browse…" for a remote engine's destinations. Modeless `show()`, `bench-engine-folder-dialog`, title "Choose a folder on %1", 520×420.
- Row: "Up" button + selectable path label. Folder list (activate to descend). Status: "Listing folders…", "No folders here", or the error.
- Buttons Cancel and "Choose". Choose returns the selected subfolder, or the current path if none is selected.
- Listing is asynchronous through `Lister` (the engine lists folders).

---

### 10. Porting notes and non-obvious behaviour
- **Settings Save is partial.** The Naming page and library folders persist immediately; Save only writes the simple QSettings values. Cancel does not undo immediate changes.
- **Lists display is set in two places.** Workspace ▸ "Lists in a side panel" writes `appearance/lists-display` directly; Settings writes the same key on Save.
- **Desktop notifications are set in two places.** Playback ▸ Desktop notifications writes `desktop/notifications` directly; Settings writes it on Save.
- **ReplayGain dialog and Settings share keys** (`replaygain/sidecar-only`, `replaygain/true-peak`).
- **The tagger's apply-option panel is never shown.** Its controls are the state model behind the Actions popover. A QML port can drop the hidden panel but must keep the state plus the remembered `properties/actions/*` keys.
- **All tagger, ReplayGain and artwork file I/O runs on the engine** that holds the files (`FileWorkTools` / `engineFileWorkTools`, ADR-0237). Without engine file work those windows do not open, and the status bar explains why.
- **Singletons:** Settings, Command palette, Open list, Search, Dynamic playlists. The tagger allows several windows; within one tagger only one Identify, script editor, exact-value editor, add-field prompt, feedback dialog and cover picker can be open at a time.
