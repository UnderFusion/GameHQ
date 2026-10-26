# Storage & Folders

## Managed capture trees

```txt
<ScreenshotsRoot>/<Game Name>/Screenshots/<yyyy-MM-dd_HH-mm-ss>.png   (.jpg when JPEG is selected)
<ClipsRoot>/<Game Name>/Clips/<yyyy-MM-dd_HH-mm-ss>.mp4
<EitherRoot>/Unknown Game/{Screenshots,Clips}/
```

A name already taken in the same second gets `_2`, `_3`, ... appended. `<Game Name>` is the folder-safe form of the display name (Windows-forbidden characters replaced).

Screenshots and clips share the same default root: `%USERPROFILE%\Videos\GameHQ` (never Game Bar's `Videos\Captures`). Full-portable mode defaults both to `Captures/` next to the portable root. Settings may override them independently through `storage.screenshots_root` and `storage.clips_root`; an empty value means the current portable/installed default, so moving a portable package does not bake in its old absolute path.

Changing a root affects new writes only. GameHQ never auto-moves or deletes existing media. The default root and every previous custom managed root remain in an internal scan history, including after a per-location reset or Restore all settings, so rebuilding/rescanning the library can still find earlier captures. Duplicate or shared roots are de-duplicated before scanning.

## Portable resolution (`Paths.cpp`)

1. `portable.flag` next to the exe (dev build tree) ⇒ everything under `<exe>/gamehq-data/` + `<exe>/Captures/`.
2. `portable.flag` in the exe's **parent** directory (dist package: root launcher + `app/GameHQ.exe`, see [packaging.md](packaging.md)) ⇒ data at the package root: `<root>/gamehq-data/` + `<root>/Captures/`.
3. Otherwise: data in `%APPDATA%\GameHQ\`, default screenshot and clip output in `Videos\GameHQ`.

`gamehq-data/`: `config.json`, `gamehq.db`, `pending-captures.journal` (see below), `thumbnails/`, `game-icons/` (cached foreground executable icons, including icons recovered from historical detector logs), `logs/`, `replay-cache/`, `sound-packs/`.

### Stored paths

In portable mode, a path genuinely below the package root is persisted as `portable:/<relative>` so the folder can be moved; everything else is persisted absolutely. "Below the root" is decided by `QDir::relativeFilePath` **plus** a check that the answer is actually relative: on another drive, or for a UNC share, Qt hands the absolute path straight back, and that result does not start with `../`. It used to be stored as `portable:/D:/Shots/...` and later resolved as `<package>/D:/Shots/...`, which lost the capture. `Paths::fromStoredPath` also recognises that shape and resolves such a row to what it really says, so libraries written by an affected build recover on their own. `toStoredPath`/`fromStoredPath` have overloads taking an explicit root, which is what `tst_storedpaths` exercises.

## Legacy name adoption (`LegacyMigration.cpp`)

The app was previously called SavePlay, then PlayHQ ([branding.md](branding.md)). Old installs are adopted on first start, and all of it lives in `LegacyMigration`:

- **Data folder** — `saveplay-data`/`playhq-data` (portable) or `%APPDATA%\SavePlay\SavePlay`/`%APPDATA%\PlayHQ\PlayHQ` (installed) is renamed to the current path, but only when the current one does not exist. If the rename fails (e.g. the folder is locked), the legacy folder is used in place rather than starting empty.
- **Database** — a `saveplay.db`/`playhq.db` next to it is renamed to `gamehq.db` under the same condition; `Paths::databasePath()` is the only way in.
- **Capture root** — resolved read-only: an existing `Videos\SavePlay`/`Videos\PlayHQ` keeps being used as-is. User media is never moved.

Each helper is a no-op once the current path exists, so later starts cost only an existence check. Registry Run-key entries under the old names are cleaned up separately by `StartupManager`.

## Watched/import folders

Any local folder the user adds (e.g. Game Bar Captures, Steam screenshots, NVIDIA ShadowPlay output) is a row in the `folders` table with `source = 'Imported'`; captures found there are tagged `source = 'Imported'`, captures under managed roots `GameHQ`. The scanner indexes images (`png`, `jpg`, `jpeg`, `bmp`, `webp`) and videos (`mp4`, `mkv`, `mov`, `avi`, `webm`). **GameHQ never writes, moves or deletes files in imported folders on its own**; the only removal is an explicit user delete of a capture ([database.md](database.md#hard-rules)). Removing a watched folder only deletes its `folders` row.

## Log retention (`Logger.cpp`)

`gamehq.log` rotates once it reaches 8 MiB, keeping `gamehq.1.log` … `gamehq.3.log` and dropping the oldest — at most four files, ~32 MiB, no matter how long the install lives. Rotation is best effort: if a generation cannot be renamed because something still has it open, the current log is left in place and keeps growing, because losing the log is worse than exceeding the limit. Logs are local only and are never uploaded ([privacy-data-flow.md](privacy-data-flow.md)); delete them from **Settings → About → Open logs folder** at any time.

If the log directory cannot be created or the file cannot be opened, GameHQ still starts and writes diagnostics to stderr instead of running silently, and the copied diagnostic summary says so on its "Logs folder" line.

## Cleanup

GameHQ has no storage quota and no automatic deletion of finished captures: screenshots and clips are only removed by an explicit user delete. **Favorites are never auto-deleted.** The only automatic cleanup is of GameHQ's own temporary files:

- **Unfinished captures (`.part`, `CapturePublisher`).** A screenshot or clip is encoded into `<final name>.part` and only renamed to its final name when complete; the scanner does not index `.part`. Every reservation is appended to `gamehq-data/pending-captures.journal` and dropped from it once published or discarded. On startup, `ScreenshotService` and `FramePumpService` sweep the current screenshot and clip base roots and delete `.part` files older than 10 minutes, but only those listed in the journal and not being written by this process; any other `.part` (a browser download, another app's) is left alone, and without a journal nothing is deleted. A `.part` whose publish failed stays on disk and keeps its journal entry until a later sweep.
- **Replay cache.** When the capture worker thread starts, `*_clip.mp4` segments in `gamehq-data/replay-cache/` older than 10 minutes and not leased are removed, and emptied per-game cache folders are pruned.
- **Thumbnails.** A capture's thumbnail is removed with it on user delete (best effort).
- **Logs.** See *Log retention* above.
