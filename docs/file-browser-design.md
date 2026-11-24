# The file browser: design

Status: design, 2026-11-23. Built so far: step 2 (§3: lib/Files, text/NameOrder,
collections/MergeSort), step 3 (§11: lib/Files' operations, without UI), and of §10 the buttons,
menus, text editing, key focus, list selection, the row window, the
scrolling, dividers, scrollbars and thumbnails (steps 4 to 6), step 7, the column browser, and 7b,
its operations: `lib/ui/fileBrowser` (its use is in `FileBrowser.rae`; what differs from the text below
is in §18). The steps are the `[file browser ...]` tasks in
QUEUE.md; §16 maps this document to them.

A file browser in Rae UI that any app can use: the media editor needs one for its projects, and so
will other apps. It is built from components and systems in `lib/`, shown alone in an example
(`examples/153_fileBrowser`) and inside a real app (the UI editor, `examples/121_ui_editor`).

## 1. What it looks like and does

The view is **columns**, as macOS Finder's column view (Miller columns). It is the only view: there
is no icon grid and no list with sortable headers.

```
+-----------------------------------------------------------------------------+
| <  >   Projects  >  Film  >  Sounds                             [ search  ] |  bar
+-----------+---------------+---------------+---------------+-----------------+
| Home      | Archive     > | Cuts        > | door.wav      |   [ picture ]   |
| Desktop   | Film        > | Sounds      > | rain.wav  <<< |   rain.wav      |
| Documents | Photos      > | film.project  | steps.wav     |   WAV, 4.2 MB   |
| ---       | notes.txt     |               |             # |   changed 3 Nov |
| Disk      |             # |               |             # |                 |
+-----------+---------------+---------------+---------------+-----------------+
  sidebar     <=============== the strip, with its scrollbar ================>
```

- **The strip** holds the columns, left to right. The first lists the browser's root folder.
- **Selecting a folder** opens its column to the right and drops every column beyond it.
- **Selecting a file** drops the columns beyond and opens the **preview column**: its picture,
  name, kind, size and dates.
- **Selecting several** entries (one column only) opens no folder; the preview column says how many
  and their total size.
- **Scrolling.** The strip scrolls sideways and brings a newly opened column into view. Each column
  scrolls on its own and remembers its place. The strip has a horizontal scrollbar and each column
  a vertical one. Columns keep their width at any window width: a narrow browser scrolls, it never
  squeezes them.
- **Widths.** A divider after each column resizes it (default 220 points, 120 to 600). A double
  click on the divider fits the names. A width belongs to the column's place in the strip, not to
  the folder, as in Finder.
- **The bar** has back and forward, the trail of folders (a click on one goes there; a click on the
  empty part turns it into a text field with the path), and a search field when the app wants one.
- **The sidebar** is optional: known folders, pinned, recent, volumes.

Reference for behavior: the TypeScript browser in the SUMU AI repo
(`apps/ui/src/components/fileBrowser`). Kept from it: it knows no host; a root folder; a remembered
trail restored as far as it still exists; events for "file opened" and "folder changed"; refresh; a
chevron or a spinner on a folder; the open-count tint (an option here). Not kept: arrow keys that
walk files only, one selection, fixed widths, no live update, every row live, hidden files never
shown. Nothing is ported.

### 1.1 Keys

`Cmd` is Command on macOS and Control elsewhere.

| key | does |
|---|---|
| Up, Down | the row above or below; moving onto a folder opens its column, as a click does |
| Right | into the open column, onto its remembered row, else its first |
| Left | to the parent column; its row is the open folder |
| Shift+Up/Down, Cmd+A | extend the selection in the column, select all of it |
| Home, End, Page Up/Down | within the column |
| letters | to the first name that starts so; the typed start is forgotten after 1 s |
| Return | rename in place (option `returnKey`: `rename`, the default, or `open`) |
| Cmd+Down, Cmd+O, double click | open: activates a file; a folder is already open |
| Cmd+Up | to the parent column, past the root if the root limits allow |
| Cmd+[ , Cmd+] | back, forward |
| Space | the preview large, over the window; arrows change it; Space or Escape closes |
| Cmd+Shift+N, Cmd+D | new folder, duplicate |
| Cmd+C, Cmd+V, Cmd+Alt+V | copy, paste as copies, paste as a move |
| Cmd+Backspace | to the trash |
| Cmd+Z, Cmd+Shift+Z | undo, redo the last operation |
| Cmd+Shift+. | show hidden files |
| Cmd+F | the search field |
| Escape | ends a rename, a drag, a search, then clears the selection |

A fast walk with Down held does not list every folder it passes: a folder's column opens once the
row has been selected for 120 ms, or at once on Right, Return or a click.

### 1.2 Selecting

- Click: that row. Cmd+click: toggles it. Shift+click: the range from the anchor, within the column.
- A selection in another column replaces this one's. The columns left of the keys' column show their
  open folder's row **muted**; the column with the keys shows it in the accent color. A browser
  without the keys shows all of them muted.
- The selection is by name, not by row number, so it survives a sort, a filter and a live update. An
  entry that goes away leaves the selection; if it was the only one, the next row takes it (the one
  that slid into its place), else the last.
- Empty space below the rows: clears the column's selection, and the column is then the folder that
  "new folder" and a drop act in.

### 1.3 States of a column

| state | shown |
|---|---|
| listing | nothing for 150 ms, then a spinner (so a quick folder never flashes one) |
| ready | rows; under 1 entry "Empty folder" |
| no permission | "No permission to read this folder", and on macOS where to allow it |
| gone | the column and those beyond it close; the parent's row is gone too (§7) |
| not answering | after 5 s: "This disk is not answering", the rest of the browser works (§6) |
| failed | the error's text and a Retry button |

## 2. Where the code lives

| what | where |
|---|---|
| entries, listing, kinds, volumes, operations, trash | `lib/Files/` (today's `lib/Files.rae` becomes a module folder of the same name; no import changes) |
| known folders | `lib/Filesystem.rae` (add movies, music, downloads) |
| the order of names | `lib/text/NameOrder.rae` |
| who has the keys; list selection; dividers; scrollbars; menus; drags; thumbnails | `lib/ui/` modules, each general (§10) |
| the browser | `lib/ui/fileBrowser/`: one module per system, `FileBrowserComponents.rae`, and `FileBrowser.rae`, the part an app calls |
| its scenes | `lib/ui/fileBrowser/scenes/*.raescene`, read with `Sys.readAsset` as `lib/app3d/scenes` are |

`lib/ui/fileBrowser` is a plain folder because each system is a module of its own (AGENTS.md). Its
scenes folder must be added where `lib/app3d/scenes` is named: the browser bundle's list in
`compiler/src/main.c` (`lib_runtime_paths`) and the device staging.

No layer below imports one above: `lib/Files` knows no UI, the general widgets know no files.

## 3. The file model (`lib/Files`, no UI)

Today `File` is a name and a folder, and `listDir` cannot tell an unreadable folder from an empty
one. Added beside them:

```rae
enum EntryKind { file, folder, symlink, other }

type FileEntry {
  name: String              # as the disk has it
  kind: EntryKind
  # a symlink's target, followed: what it is (other when it is broken or a loop)
  targetKind: EntryKind = EntryKind.other
  hidden: Bool = false
  # -1 until the second pass read them
  size: Int = -1
  modifiedSeconds: Float64 = -1.0
}

enum ListError { none, notFound, noPermission, notFolder, other }
```

- **Hidden**: a leading dot, or the platform's flag (macOS `UF_HIDDEN`).
- **A folder shown as a file**: on macOS a bundle (`.app`, `.photoslibrary`) is a folder on disk. A
  kind table (§9) says which extensions are opened as files; the model itself says folder.
- **Two passes.** Names and kinds come first (`readdir`'s `d_type`; `lstat` only where it says
  unknown): `readFolder`. Sizes, dates and the OS's hidden flag follow per entry:
  `readEntryDetails`. `listFolder` reads them at once when the order needs them, and for a folder of
  at most 2,000 entries, so that a folder the OS's flag hides (macOS's `~/Library`) does not show
  and go. Measured on 100,000 files in a release build: names and kinds 55 ms, ordering 70 ms, all
  of it by size 375 ms.
- **Order.** By name (natural: `file2` before `file10`; case ignored; accents after their base
  letter), size, date, kind; ascending or not; folders first or mixed. Ties go by name, then bytes,
  so an order is total and stable. `NameOrder` compares a key made once per name. It folds case and
  decomposes Latin, Greek and Cyrillic letters, so a composed and a decomposed é match (macOS stores
  either); other scripts go by code point.
- **Sorting.** `List.sort` is an insertion sort and takes numbers only. 100,000 names need a
  merge sort, written in `lib/collections` over an index list and a key list.
- **Filter.** A name part (folded as above), extensions, kinds; folders always pass an extension
  filter, so a filtered tree can still be walked.
- **Volumes** (`mountedVolumes`): name, mount folder, removable, read-only, free space; the system's
  first; not the images macOS marks as not for browsing. Reading them asks each disk: off the main
  thread too.

`docs/filesystem-and-paths.md` planned `isDir`, `size` and `list` in `Filesystem`; they are these.

### 3.1 Listing off the main thread

```rae
type ListRequest {
  generation: Int
  folder: String
  order: EntryOrder
  filter: EntryFilter
}

# What a listing sends back, in order: names once, details in batches, then done
type ListUpdate {
  generation: Int
  kind: ListUpdateKind      # names, details, done
  entries: List(FileEntry)  # names: all of them, sorted and filtered
  details: List(EntryDetail)
  error: ListError
}
```

- One `spawn thread` per listing, at most four at a time; more wait in the job list, newest first.
  Not a pool task: a disk that does not answer would hold a pool worker, and `blocking` externs would
  hop threads once per entry. Not a loop that polls: nothing may wake an idle app.
- Results come over a `Channel(ListUpdate)`. The thread's function is the browser's
  (`ListingSystem`): it calls `lib/Files`, sends, and calls `ui/EventLoop`'s `wake()`, so `lib/Files`
  itself knows no UI and no threads. The channel holds 4,096 messages; a listing sends one for the
  names and one per 2,000 details.
- Cancel: a stop channel per job, read between batches. A result whose generation is not the
  column's current one is dropped.
- Sorting and filtering happen on that thread; the main thread only takes the finished list.

### 3.2 A disk that never answers

A thread in a `readdir` that never returns cannot be stopped, and a `Task` is joined when dropped,
which would hang the app's quit. So the job list keeps such a task apart (no result for 5 s), counts
it against the four, and an app that quits with one left ends with `Sys.exit`, which joins nothing.
Case 1572 proves it with a thread that never returns: the other listings go on and the program quits.

## 4. The ECS model

**A browser is an entity** (the node it is mounted on) and **a column is an entity** (the root of its
column scene). A file on disk is never an entity: only the rows in view are, and they are the
`ListView`'s pooled rows.

The browser's tables and resources are on one struct the app owns, as `ecs-systems-own-their-
tables.md` asks. They are not on `UiWorld`: nothing else reads them.

```rae
type FileBrowserSystems {
  # components
  browsers: ComponentTable(FileBrowser)
  columns: ComponentTable(FileColumn)
  entries: ComponentTable(ColumnEntries)
  widths: ComponentTable(BrowserWidths)
  history: ComponentTable(BrowserHistory)
  previews: ComponentTable(BrowserPreview)
  renames: ComponentTable(RenameEdit)
  searches: ComponentTable(BrowserSearch)
  # resources: one of each, shared by every browser of the app
  listing: ListingJobs       # the running listings and their channel (§3.1)
  watches: FolderWatches     # what is watched, and by how many columns
  operations: FileOperations # running operations, their progress, the undo list
  clipboard: FileClipboard   # copied or to-be-moved paths
  openCounts: OpenCounts     # the tint's counts, when an app wants it
  # events
  events: FileBrowserEvents
}
```

| component | on | holds |
|---|---|---|
| `FileBrowser` | browser | root folder, root limit (may it go above the root), source (`disk` or `app`, §9), order, filter, show hidden, options (§4.1), the column that has the keys |
| `FileColumn` | column | its browser, its place in the strip, folder, state (§1.3), generation, its `ListView` entity, the open child's name, the anchor row |
| `ColumnEntries` | column | the sorted entries, and a map from name to index |
| `BrowserWidths` | browser | a width per place in the strip; the preview column's |
| `BrowserHistory` | browser | trails visited, the current one's index, scroll and focused row per folder (bounded: 200 folders) |
| `BrowserPreview` | browser | the previewed path, its picture's state, the large preview open or not |
| `RenameEdit` | column, while renaming | the entry's name, the field's entity |
| `BrowserSearch` | browser, while searching | the query, the generation, the results' column |

A column's selection is a `ListSelection` on its `ListView` (§10), and its scroll is the list's
`ScrollState`: the general components, not copies of them. `ColumnEntries` is large, so it is always
taken by `view` or `mod`, never copied, and moved in with `own`.

Authored in a scene: only the host node, `"FileBrowser": { "root": "~/Projects", ... }` with the options
of §4.1. It reaches `FileBrowserSystems.authored` as any app component does (`world.pendingComponents`
and `applyComponentInto`): `fileBrowserUpdate` takes it and opens the browser, and opens it anew when the
component changes (case 1596). An app that reports unknown components before its first
`fileBrowserUpdate` calls `applyFileBrowserComponents` before that. An app can also call
`openFileBrowser(...)` on a node.

### 4.1 Options of a browser

`order` (in a scene `orderBy`, `descending`, `foldersFirst`), `showHidden`, `extensions`, `selection`
(`none`, `single`, `multiple`), `returnKey`, `allowAboveRoot`, `showBar`, `showPreview`,
`openCountTint`, `columnWidth`, `previewWidth`, `rowHeight`, `source` (§9), `allowOperations` (false: look
and pick only). To come: `showSidebar` and `mode` (§8; 7d).

### 4.2 Events

One `EventQueue` per type, on `FileBrowserEvents`; each names its browser's entity.

| event | sent when |
|---|---|
| `FolderOpened` | a column opened (its folder; the deepest one is the browser's folder) |
| `FileActivated` | open, double click, or Return with `returnKey: open`; several at once |
| `SelectionChanged` | the paths selected now |
| `PickAnswered` | a pick or save ended (§8): paths, or canceled |
| `OperationAsked` | an operation needs an answer: a conflict, a delete with no trash |
| `OperationDone` | per operation: what happened to each item, and the undo record |
| `BrowserMenuChosen` | an item the app added to the context menu |
| `EntriesRequested` | a browser with the `app` source wants a folder's entries (§9) |
| `FilesDropped` | paths dropped on the browser from the OS or another view |

The app reads them after `fileBrowserUpdate` and before the next frame's. There are no callbacks.

## 5. The systems

Each is a module in `lib/ui/fileBrowser/`. All take `systems: mod FileBrowserSystems`; those that
touch nodes take the `UiWorld` and `UiSystems`. They loop over every browser or column, so two
browsers are the same code with two rows in a table.

| # | system | reads | writes |
|---|---|---|---|
| 1 | `KeysSystem` | `KeyFocus`, `UiInput`, keys, text input, `ListSelection` | intents on `FileColumn` / `FileBrowser`, `BrowserSearch` |
| 2 | `SelectionSystem` | list events (§10), `ColumnEntries` | which column has the keys, `SelectionChanged`, `FileActivated` |
| 3 | `NavigationSystem` | intents, selection, `BrowserHistory` | opens and drops column entities, history, `FolderOpened`, the strip's scroll request |
| 4 | `ListingSystem` | `FileColumn` (a folder not listed, or stale), the channel | `ListingJobs`, `ColumnEntries`, the column's state |
| 5 | `WatchSystem` | `FolderWatches`, live `FileColumn`s | watches, "stale" on columns |
| 6 | `SearchSystem` | `BrowserSearch`, the channel | the results' column |
| 7 | `RenameSystem` | `RenameEdit`, its `TextField` | an operation request, `RenameEdit` removed |
| 8 | `OperationsSystem` | requests, `OperationAsked`'s answers, the channel | `FileOperations`, `FileClipboard`, `OperationDone`, "stale" |
| 9 | `DragSystem` | the drag payload (§10), captures | a drop's operation request, the row shown as target |
| 10 | `PreviewSystem` | selection, `ColumnEntries`, the thumbnails | `BrowserPreview`, the preview column's nodes |
| 11 | `RowsSystem` | `ColumnEntries`, the list's window, selection, thumbnails, counts | the `ListViewData` of the rows in view |
| 12 | `ChromeSystem` | `FileBrowser`, history, widths | bar, sidebar, states, column widths |

The frame, in an app:

```
updateUiInput
general: menuSystem, dragSystem, dividerSystem, scrollBarSystem, listSelectionSystem
fileBrowserUpdate            systems 1 to 10
updateScrollRoots            (every scroll root, §10)
fileBrowserRows              systems 11 and 12
listViewSystem -> layout -> fit -> animation -> transform -> bounds -> paint
the app reads FileBrowserEvents
fileBrowserFrameEnd          the queues advance
```

`RowsSystem` runs after the scroll step, so the rows bound are the rows shown: no frame of blank rows
while scrolling. `fileBrowserUpdate` answers whether anything changed, for the app's loop; with
nothing running it does no work per column beyond comparing generations, and the app sleeps.

## 6. Time and size limits

| | limit |
|---|---|
| a frame's work | the rows in view, never the entries: O(columns + rows shown) |
| 100,000 entries | listed and sorted off the main thread; scrolling costs the same as for 100 |
| taking a finished list on the main thread | a move, no copy |
| idle | no wakes: no polling thread, no timer but the caret's |
| columns open | any number; those out of the strip's view keep their entries but no rows |
| memory | entries of open columns only; a dropped column's entries are freed |
| history | 200 folders' scroll and focus |

Why rows must be windowed: building today's `ListViewData` for 100,000 entries took 50 ms in a
release build and copying it 93 ms (600,000 Strings). A list now takes only the rows in view
(`ui/ListWindow`, case 1586).

## 7. Live update

- Each distinct folder shown is watched by one `FileNotify` (its signal says "look now" and names no
  path, so one per folder says which). Folders shown twice share one. At most 32; beyond that, and
  where there are no notifications, a shown folder's modified time is compared when the window gets
  the focus and every 2 s while it has it.
- A signal marks the folder stale; a stale column lists again, at most once per 200 ms. The old rows
  stay until the new list is there.
- The new list replaces the old by name: selection, focused row, the open child and the scroll (the
  first shown row keeps its place on screen) are kept.
- A folder that is gone: its column and those beyond close, and the browser falls back to the
  deepest that exists. If the root itself is gone, the root column says so, with Retry.
- A renamed open folder is a removal and an addition to the OS. If the rename was this app's own the
  trail follows it; otherwise the columns close.
- Our own operations do not wait for the notification: they mark their folders stale when done.
- `refreshFileBrowser` lists every open column again.

## 8. The uses

The pieces are the same; only the host scene and options differ.

| use | how |
|---|---|
| a whole window | the host node fills it; bar, sidebar, preview on |
| two side by side | two host nodes and a divider; `KeyFocus` says which has the keys; Tab moves it |
| a docked panel | a host node in a panel; sidebar off, bar small; several columns, the strip scrolls |
| pick one or many | `mode: pickFile` / `pickFiles` / `pickFolder` in a dialog page; Open and Cancel buttons; only fitting entries can be chosen, others are muted; answers `PickAnswered` |
| save as | `mode: save`: a name field and New Folder; an existing name asks before `PickAnswered` says replace |

`Sdl3.openFileDialog` stays the OS's own panel. An app chooses; on a sandboxed macOS or iOS build
only the OS panel can give access outside the app's own folders (§12).

The two-pane example also copies or moves the selection to the other pane by a key: an ordinary
operation request whose destination is the other browser's folder.

## 9. What an app supplies

Rae passes no functions, so an app supplies data, or answers events.

| | how |
|---|---|
| file kinds | a `FileKinds` table: extension to kind name, icon (`mat:...` or a texture key), tint slot, "opened as a file" for a folder kind. A default table covers images, sound, video, text, code, archives |
| thumbnails | `ThumbnailRequested` for kinds the app marks as its own; it answers with pixels or a failure (§10). The media editor answers for video and sound; `lib` needs neither package |
| the preview | a scene id per kind, mounted in the preview column with the entry's fields bound; the app may add systems that fill it (a player) |
| menu items | a list of `{ id, label, icon, shortcut, kinds, enabled }`; `BrowserMenuChosen` |
| the folders allowed | the root and `allowAboveRoot`; a path outside is refused in every way in: trail, path field, symlink, drop, restored trail |
| its own tree | `source: app`: the browser sends `EntriesRequested`, the app answers with `setColumnEntries`. A project's assets, an archive or a remote machine can be browsed so. Operations are then events too, never disk calls |
| what is remembered | `fileBrowserState` gives the trail, widths, order and options as a value the app saves; `restoreFileBrowser` takes it and keeps what still exists |

## 10. General pieces that are missing

None of these is about files; each is its own module with its own cases.

| piece | today | needed |
|---|---|---|
| **row window** | built (docs/ui-scene-format.md section 50), was: `ListViewData` holds every row; its count is `rows.length` in the list, the layout, the scroll range and `ScrollToRow` | a `ListWindow { count, first }` component: with it, `rows` are the rows `first...` only, and those four read `count`; `listWindowWanted` says which the list needs now. `ListReorder` and list motion refuse a windowed list for now |
| **live row count** | built (section 50), was: `visibleItemCount` is authored | 0 means "as many as fit", from the list's height, so a resized column fills |
| **sideways wheel** | built (section 51), was: `updateScrollRoot` reads the vertical wheel on both axes; `wheelX` goes to the one `wheelTarget` | each axis goes to the nearest scroll root of that axis under the pointer; Shift+wheel is sideways; a trackpad gesture keeps the axis it began on |
| **several scroll roots** | built (section 51), was: `findScrollRoot` answers one per page | `updateScrollRoots`: every shown root is stepped; those at rest and not under the pointer cost a comparison |
| **key focus** | built (docs/ui-scene-format.md section 48) | `KeyFocus`, a resource naming the entity that has the keys; a press on a node marked `TakesKeys` moves it; a text field has it while focused and gives it back |
| **list selection** | built (section 49) | `ListSelection` and `listSelectionSystem` (§1.2), keyed by `itemKeyField` |
| **pointer buttons, double click** | built (docs/ui-scene-format.md section 46) | press, release and click count per button through `UiInput` and captures; Control+click and a long press as the second button |
| **menus** | built: `ui/MenuSystem` (section 47) | `menuSystem` with items as data, keys, one submenu level, kept in the window |
| **dividers, scrollbars** | built (docs/ui-scene-format.md sections 52, 53) | `Divider` and `ScrollBar` components with a system each |
| **text editing** | built (docs/ui-scene-format.md section 11) | a caret moved by keys and a press, a selection, word and line keys. A rename selects the name without its extension, so it cannot do without |
| **drags** | per widget | one `UiDrag` resource: payload kind, values, a label to draw, what accepts it, copy or move; a drop is an event. OS drops (`SDL_EVENT_DROP_*`) arrive as the same event |
| **images** | built (docs/ui-scene-format.md section 54), was: a registered image is never released | release behind in-flight frames; decode off the main thread; a budget. `lib/Gpu2dCanvas/Canvas.rae` has 999 lines, so it is split first |
| **sorting** | insertion sort, numbers only | §3 |

## 11. Operations

All are requests handled by `OperationsSystem`; the disk work runs on its own thread (one per
operation, at most two; others wait), with progress over a channel and a stop channel.

Built (`lib/Files/Operations.rae`, `OperationSteps.rae`, `CopyTree.rae`, `Trash.rae`): an operation is a value that is
stepped (`stepOperation`), each step a bounded piece of work: a chunk of a file, an entry. Between
two steps its thread can send progress (`bytesDone`, `index`; `measureFiles` gives the whole), take a
cancel (`cancelOperation`), or wait for an answer (`state` is `asking`, `conflict` says what is in
the way, `answerConflict`). One that was begun is stepped to its end, which is also how a canceled
one takes its temporary copies away. `undo` lists what was done; `undoFiles` makes the operation
that undoes it, whose own `undo` is the redo. A step remembers the thing itself (disk and file
number): a file that an editor replaced since is not undone (`changed`).

| operation | rules |
|---|---|
| new folder | "untitled folder", then 2, 3 ...; opens the rename |
| rename | checked first (empty, `/`, `.`, `..`, too long, control characters; `\ : * ? " < > \|` refused on Windows only); a case-only change is allowed |
| duplicate | "name copy", "name copy 2" |
| copy, move | several items; symlinks copied as symlinks; permissions and dates kept; into itself refused |
| move across volumes | copy, written through to the disk and compared, then exactly what was copied is removed, and only while it is unchanged; a late file stays and the item says `changed` |
| trash | the OS's trash (macOS `trashItemAtURL`, freedesktop on Linux). Where there is none, `OperationAsked` says so, and only an explicit yes deletes |
| undo, redo | from each operation's record: rename and move go back, copies and new folders go to the trash, trashed items return. Refused, with a reason, when something is now in the way |

Safety:
- **Nothing is overwritten without an answer.** A conflict asks: skip, keep both, replace, for this
  one or for all. Replace sends the old one to the trash first.
- A copy is written under a temporary name beside the target and renamed when complete. A canceled
  or crashed copy leaves no file under the real name.
- Other programs may create the target between the check and the rename: the final rename uses the
  OS call that fails when the target exists, where there is one (macOS `renamex_np`, Linux
  `renameat2`).
- A folder is walked without following symlinks, so a delete or a copy stays inside it.
- `allowOperations: false` disables every one, keys and menu included.
- Not moved, trashed or deleted: the root, a mounted disk, the user's home and the folders it is in.
  A delete does not go into a disk mounted in what it deletes.
- `RAE_FILES_ONLY_IN=<folder>` holds every operation of the process to a folder, by where paths
  really lead, and then the user's own trash is never used (`RAE_FILES_TRASH=<folder>` names one).
  The tests set both. An app's root limit is the same rule (`OperationRules.onlyIn`).
- Not done: a folder is not merged into one of its name (replace trashes the old one), and on Linux
  extended attributes are not copied.

## 12. Platforms

| | |
|---|---|
| macOS, Linux | all of it. macOS privacy: a denied folder is "no permission", with where to allow it |
| macOS sandboxed, iOS | the app's own folders; others only through the OS panel |
| Android | the app's own folders |
| Windows | nothing here was checked on it; drives would be the volumes, and the Recycle Bin the trash |
| browser | no disk: `source: app` only |

The choice is a capability constant next to the code (`hasDiskBrowsing`, `hasTrash`,
`hasFolderNotifications`), tested with `when`; the modules are added to `checkTargets`.

## 13. Narrow windows and touch

- Below 600 points of the browser's own width (not the window's) **on touch**, one column is shown
  at a time: opening a folder pushes its column, a swipe from the left edge or Back pops it. It is
  the same strip, with each column as wide as the browser, so the entities and systems do not
  change. With a mouse the columns keep their size and the strip scrolls, however narrow.
- Touch: rows 44 points; tap opens; long press is the menu; a Select button gives check marks.
- Mouse: rows 24 points.

## 14. Scenes and theme

Scenes, in points: `browser` (bar, strip, states), `column`, `row`, `previewColumn`, `sidebarRow`,
`largePreview`, `conflictDialog`, `progressRow`. An app may register its own under the same ids
before `openFileBrowser`, and they are used instead.

Colors come from the theme's slots (`surface`, `surfaceAlt`, `outline`, `textPrimary`, `textMuted`,
`accent`, `accentText`). The muted selection and the tint are the accent at lower opacity. A row is
an icon or thumbnail (16 points; 28 on touch), the name (cut in the middle, so the extension stays
visible), and a chevron on a folder. Two bundles, `fileRow` and `fileColumn`, are what a theme
replaces. Text is `body`; details are `muted`.

A name cut in the middle is `Text.cut` (docs/ui-scene-format.md section 55).

## 15. Tests

- `lib/Files`: cases over a made tree, with every kind, a symlink loop, an unreadable folder, names
  with spaces, emoji, composed and decomposed accents, 100,000 files; operations with conflicts,
  cancel, case-only rename, across volumes (a small disk image, where the test can make one).
- Each general piece: cases with simulated input.
- The browser: cases that drive it by simulated input and read components and events: walk, keys,
  ranges, live update, gone folder, restored trail, root limit by every way in, two browsers, a drag
  between them, pick and save. Each leak-checked; those with threads on the TSan gate's list.
- Checks (pictures): desktop and narrow, light and dark, long names, 100,000 rows scrolled.
- A memory test: walking a big tree for a minute levels off.

## 16. Steps

| task | sections |
|---|---|
| 2 what a folder holds | 3, 6, 12 |
| 3 operations on files | 11 |
| 4 pointer buttons, double click, menus | 10 |
| 4b a caret and a selection in `TextField` | 10 |
| 5 key focus and list selection | 1.1, 1.2, 10 |
| 5b row window, sideways wheel, several scroll roots | 6, 10 |
| 5c dividers and scrollbars | 10 |
| 6 thumbnails | 9, 10 |
| 7 the column browser | 1, 2, 4, 5, 7, 14 |
| 7b operations, rename and menu in the browser | 11, 4.2 |
| 7c drags | 10 |
| 7d sidebar, search, pick and save, touch | 8, 13 |
| 8 the example with a split view | 8 |
| 9 the UI editor's panel | 8 |

Order: 2, 4, 4b, 5, 5b, 5c and 6 can be done in any order; 3 needs 2; 7 needs 2, 4, 5, 5b, 5c and
6; 7b needs 3, 4b and 7; 7c and 7d need 7b.

## 17. Not in it

Other views, tags and colors, spotlight-like content search, network mounts, permissions editing,
archives opened as folders (an app can, with `source: app`), several windows sharing one browser.

## 18. Built in step 7, where it differs from the text above

- The frame is `fileBrowserUpdate` and, after the scroll roots, `fileBrowserRows`; the queues advance in
  `fileBrowserUpdate`, so there is no `fileBrowserFrameEnd`. `fileBrowserDeadline` says when a frame is
  needed though nothing happens. `closeFileBrowsers` ends it.
- The listing thread is `FolderListing` (no UI in it, so it runs under ThreadSanitizer, case 1595). It
  sends the names, then all details at once, not in batches. Listings are not canceled: an answer its
  column no longer wants is dropped.
- A folder below the root waits for the root's own listing, whose real path is its limit; a folder that
  is outside by its real path (a symlink) shows "This folder is outside the one shown here".
- Widths are on `OpenBrowser` (an open browser; `FileBrowser` is the component of a scene), the large
  preview on `BrowserPreview`. The muted selection is the slot `surfaceAlt`.
- The scenes are `fileBrowser`, `fileColumn`, `fileRow`, `filePreview`, `fileLargePreview` and
  `fileTrailPart`, in the app's `SceneRegistry`.
- The folders that are not watched are looked at on a thread (§7), so a disk that does not answer holds
  nothing; a folder's time is taken before its listing reads it, so no change between the two is missed.
  Case 1597.

Step 7b:

- The thread of an operation is `OperationThread` (no UI, under ThreadSanitizer, case 1599); the jobs, the
  undo list and the clipboard are `FileBrowserSystems.operations`, whose `rules` an app may set (its own
  trash folder). The question and the rows are `OperationRowsSystem`, the menu is `FileMenuSystem`.
- The root limit holds what is put (`OperationRules.placeIn`), not where it comes from: a paste takes what
  another browser copied.
- The dialog: Escape stops the operation, Return keeps both. Nothing that deletes has a key.
- A paste as a move takes the paths off the clipboard. There is no cut. The clipboard is the browsers'
  own, not the system's.
- While a rename, a move or an undo of ours runs (for a second at most), what a listing found waits, so an
  open folder is not taken for gone before its new name is known.
- The bytes of a copy are measured before it begins; a cancel waits for that.
