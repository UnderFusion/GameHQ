# Database (SQLite)

Game display names are de-duplicated against their folder-safe form on startup and insert. `GameRowRepair` owns startup duplicate merging, and insert-time matching reuses the same display-name preference rule. This prevents managed-captures rescans from creating a second game row when a real title contains Windows-forbidden path characters, e.g. `The First Berserker: Khazan` saved on disk as `The First Berserker_ Khazan`.

When GameHQ detects a foreground game or saves a GameHQ-created screenshot/replay from one, `games.executable_path` is updated if Windows exposes the executable path, and `GameIconCache` caches an icon as a PNG in `gamehq-data/game-icons/`. For MSIX/GDK package installs (e.g. Xbox app titles under `XboxGames\<title>\Content`), it first resolves the package's key art from its `AppxManifest.xml` or `MicrosoftGame.config` manifest; it falls back to the Windows shell icon otherwise. The cache key includes the extractor's format version, so upgrading the extractor re-extracts every cached icon once on the next startup rather than leaving old cache entries stuck on a lower-quality result. `AppController::games()` exposes that `icon_path` as `iconPath`; `CurrentGameService` also uses `executable_path` to keep the dynamic `Game` section attached to a captured game while that executable is still running, even if GameHQ or the overlay has foreground focus. Imported/rescanned captures without an executable path simply keep a text-only game row until the same game is detected in the foreground. `GameMetadataBackfill` can repair older rows from historical `gamehq.log` detector lines at startup.

If an older row is missing executable/icon metadata, startup also checks prior `GameDetector` lines in `gamehq.log` and backfills matching game rows from those historical detections. Matching considers every title candidate the detector logged (window title, `ProductName`, `FileDescription`, executable name), not only the Steam name, so non-Steam titles (Xbox/MSIX, itch, standalone) recover an icon too; candidates are ranked (`fromExe` first, since a launcher shim can log the game's window title one line ahead of the real executable) and the whole log is scanned before any row is updated. This lets rows that existed before icon caching recover an icon without requiring a new capture or another foreground poll first.

Existing v1 databases are repaired on startup with compatibility `ALTER TABLE` checks for `games.executable_path`, `games.icon_path`, and `games.last_seen_at`; the schema version stays at v1 because these columns are treated as v1-compatible metadata fields.

File: `gamehq-data/gamehq.db`. Access via Qt SQL (`QSQLITE`) from the app/controller/model paths. Keep queries short on the UI path; larger repair work should stay in helpers such as `GameMetadataBackfill` and `GameRowRepair`. Schema versioned with `PRAGMA user_version`; migrations run in `CaptureDatabase::migrate()` — one numbered step per schema change, never edit past migrations. The startup repair pass (brand paths, duplicate collapse, path renormalization, game-row/metadata repair) runs inside a single transaction so a mid-run crash cannot leave the library half-repaired. The two heavy one-time passes — `GameRowRepair::normalizeDuplicateNames` (O(n²) display-name scan) and `GameMetadataBackfill::run` (full `gamehq.log` rescan) — are gated behind the `internal.repairs_v1_done` sentinel in the `settings` table: they run once after an upgrade and skip on every later launch. The sentinel is written inside the same repair transaction, so it only persists if those repairs commit.

`CaptureDatabase` is the public storage facade for callers. Read-only capture/game listing and boolean lookup SQL lives in `CaptureQueries`; mutations, migrations, and insert-time game resolution stay in `CaptureDatabase`.

## Schema v1

```sql
CREATE TABLE captures (
  id             INTEGER PRIMARY KEY AUTOINCREMENT,
  file_path      TEXT NOT NULL UNIQUE,
  type           TEXT NOT NULL CHECK(type IN ('screenshot','video')),
  game_id        INTEGER REFERENCES games(id),
  process_name   TEXT,
  window_title   TEXT,
  created_at     TEXT NOT NULL,            -- ISO 8601 UTC
  duration_ms    INTEGER,
  width          INTEGER, height INTEGER, fps INTEGER,
  codec          TEXT, bitrate INTEGER,
  is_favorite    INTEGER NOT NULL DEFAULT 0,
  source         TEXT NOT NULL DEFAULT 'GameHQ',  -- GameHQ/GameBar/Steam/NVIDIA/OBS/Imported
  thumbnail_path TEXT,
  deleted_at     TEXT                       -- soft delete
);
CREATE INDEX idx_captures_game ON captures(game_id);
CREATE INDEX idx_captures_created ON captures(created_at);

CREATE TABLE games (
  id             INTEGER PRIMARY KEY AUTOINCREMENT,
  display_name   TEXT NOT NULL,
  process_name   TEXT UNIQUE,
  executable_path TEXT,
  icon_path      TEXT,
  created_at     TEXT NOT NULL,
  last_seen_at   TEXT,
  is_whitelisted INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE settings (
  key   TEXT PRIMARY KEY,
  value TEXT
);

CREATE TABLE bindings (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  device_type TEXT NOT NULL CHECK(device_type IN ('keyboard','controller')),
  input_code  TEXT NOT NULL,
  action      TEXT NOT NULL,
  press_type  TEXT NOT NULL DEFAULT 'tap' CHECK(press_type IN ('tap','hold','combo')),
  hold_ms     INTEGER
);

CREATE TABLE folders (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  path       TEXT NOT NULL UNIQUE,
  source     TEXT NOT NULL DEFAULT 'Custom',
  is_watched INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE sound_settings (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  event_name TEXT NOT NULL UNIQUE,
  enabled    INTEGER NOT NULL DEFAULT 1,
  volume     INTEGER NOT NULL DEFAULT 80,
  sound_file TEXT
);
```

## Schema v2

```sql
CREATE TABLE binding_overrides (
  id             INTEGER PRIMARY KEY AUTOINCREMENT,
  device_group   TEXT NOT NULL CHECK(device_group IN ('keyboard','controller','mouse')),
  device_profile TEXT NOT NULL DEFAULT '',   -- device family/fingerprint scope; '' = every device in the group
  action_id      TEXT NOT NULL,               -- ActionCatalog id, e.g. "global.screenshot"
  slot           INTEGER NOT NULL DEFAULT 1 CHECK(slot IN (1,2)),  -- 1 = primary, 2 = secondary
  trigger_code   TEXT,                        -- canonical control id or key chord; NULL when unbound
  activation     TEXT NOT NULL DEFAULT 'press' CHECK(activation IN ('press','tap','hold','double_tap')),
  hold_ms        INTEGER,                     -- NULL/0 on a hold = use the configured default duration
  unbound        INTEGER NOT NULL DEFAULT 0 CHECK(unbound IN (0,1)),
  tap_count      INTEGER NOT NULL DEFAULT 1   -- v4: taps a 'tap' activation needs, 1-3
);
CREATE UNIQUE INDEX idx_binding_overrides_scope
  ON binding_overrides(device_group, device_profile, action_id, slot);
```

Built-in trigger defaults live in `src/input/BindingResolver`, never in the database. `binding_overrides` only holds explicit user changes — one row per (device group, device profile, action, slot) — so an empty table means every action uses its code-side default. Schema v3 widens `device_group` to include safe extra mouse buttons. The legacy v1 `bindings` table remains untouched and is superseded by `binding_overrides`; its seeded rows represented defaults rather than user choices and are not migrated.

**Schema v4 — tap counts.** `tap_count` is added with a plain `ALTER TABLE ADD COLUMN` (no rebuild: no CHECK constraint and no index changes), and its `NOT NULL DEFAULT 1` fills every existing row with the "one tap" they all meant. The same migration retires the `double_tap` spelling — those rows become `activation = 'tap'`, `tap_count = 2`, so the count lives in one place instead of being half in a string and half in a column. The `activation` CHECK still lists `double_tap` so a database written by an older build keeps opening; nothing writes that value any more. `trigger_code` additionally carries serialized chord triggers (`chord:v1:<first>><second>`) — see [controller-input.md](controller-input.md#binding-patterns-trigger--gesture-model). A row that cannot be parsed back into a valid pattern is skipped at load, never executed.

## Hard rules

- **Favorites (`is_favorite = 1`) are never auto-deleted** — cleanup queries must always include `AND is_favorite = 0`.
- Deletes from GameHQ UI are soft (`deleted_at`) first; file removal is a separate confirmed step.
- Media missing on disk is never tombstoned. `CaptureScanner` lists it in the connection-local `temp.missing_captures` table, which every capture/game listing excludes; the row, its favorite flag and metadata stay in `gamehq.db`, and the key leaves the table as soon as the file is back (Recycle Bin restore, drive reconnected). Full scans (startup, *Rescan*) rebuild it; `CaptureFolderWatcher` (one subtree change notification per capture root, file/dir names only, 600 ms debounce) triggers `CaptureScanner::reconcile()` for just the changed roots. Files modified in the last 3 s are left for a recheck so a capture GameHQ is still committing keeps its executable metadata.
- Imported folders' files are never modified/deleted unless the user explicitly enables it.
