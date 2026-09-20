# Named mapping presets — semantics contract

Status: **draft for review (rev 3)** — produced by plan item `cpo-p01` (investigation, no storage,
UI or migration is implemented here). Rev 2 applied review round 1: alias-complete migration
equivalence (bounded compatibility phase), game identity without row-id fallback, typed
assignment targets, and per-row portability. Rev 3 applies review round 2: historical opaque
`controller-…` keys are unverified migration sources (never durable assignments without runtime
proof), and `cpo-p03` owns the whole migration **and** compatibility/materialization lifecycle —
no extra leaf. Implementation leaves: `cpo-p02` (storage), `cpo-p03` (migration + compatibility/
materialization), `cpo-p04` (assignment resolution), `cpo-p05` (switching), `cpo-p06` (settings
UI), `cpo-p07` (game sessions).

Terminology note: the product already uses "preset" for capture/encoder quality presets
(SettingsCombo, tray tooltip). A mapping preset must always be called a **mapping preset** in
UI text and code (`MappingPreset`), never bare "preset".

## 1. The model this must fit (evidence)

- Bindings today are a **merge, not a selection**: `BindingResolver::effectiveBindings()` starts
  from `gamehqDefaultBindings()` and overlays sparse rows of `binding_overrides` in one fixed
  order — group-wide rows (`device_profile = ''`) < alias rows < the profile's own rows
  (precedence comment in `src/input/BindingResolver.cpp`; unique key
  `(device_group, device_profile, action_id, slot)`).
- A profile key is a **device identity string**. `InputEngine::canonicalProfile()` resolves every
  provider attachment to a `PhysicalControllerRegistry` `logicalId`;
  `configureLogicalProfile()` registers alias keys (migration aliases, model fingerprint,
  provider device ids) under it. `PhysicalControllerRegistry::createLogicalId()` hashes the
  strongest available evidence (appLocalDeviceId > endpointId > containerId > topologyRoot)
  *without* the generation counter — strong identities are stable across sessions and are already
  persisted in binding rows; weak identities (no stable evidence) include a per-run generation and
  are **session-local**.
- Legacy device rows are keyed on pre-identity fingerprints (`xinput.slotN`, `winmm.slotN`); the
  alias map keeps them applying to the correlated pad, and Settings → Input offers an explicit
  copy onto the stable profile ("Adopt per-slot bindings",
  `BindingEditorModel::copyLegacyOverridesToController()`).
- The alias chain is **runtime state, not stored state**: `InputEngine::configureLogicalProfile()`
  (line 609-630) builds it from the live registry (`modelFingerprint`, the in-memory `providers`
  attachment list) plus `logicalControllerRekeyed` previous-id events
  (`m_profileMigrationAliases`). Nothing persists it. Strong and weak ids are indistinguishable
  offline — both are `controller-<16 hex>` over `strong|…` or `weak|…|generation` material
  (`PhysicalControllerRegistry::createLogicalId()`). An offline schema upgrade therefore **cannot
  reconstruct which stored keys are aliases of which controller** (section 6).
- The editor edits one device group at a time and offers two scopes: "All controllers"
  (group-wide rows) and "This controller" (the active backend's profile,
  `BindingEditorModel::selectedProfile()`). Keyboard and mouse rows are always group-wide.
- Game identity exists: `GameDetector` + `CurrentGameService` resolve the foreground game to a
  `games` row (created by `rememberGameExecutable()` even before any capture exists);
  `GameIdentity::key()` canonicalizes names; `GameRowRepair` may delete duplicate rows, so a row
  id alone is not a durable game key. Per-game persistence precedent: the overlay's per-game
  gallery category (`NavigationState::overlayCategory`, keyed by `gameId`).
- Switch-safety primitives already exist: `BindingRuntime::reload()` and
  `InputEngine::activateBackend()` both call `BindingRuntime::cancelAll()` →
  `InputPatternRecognizer::invalidate()` (generation bump; queued tap/hold/chord timers resolve to
  nothing).

## 2. Definition — what a mapping preset is

A mapping preset is a named, **device-group-scoped** set of sparse binding rows:

- One preset belongs to exactly one device group (`controller`, `keyboard` or `mouse`, the same
  values storage already uses) and is listed only under that group.
- Its content is the same row shape the resolver already understands:
  `(actionId, slot ∈ {1,2}, triggerCode, activation/tapCount/holdMs, unbound)`. Sparse means:
  only rows that differ from — or deliberately re-state — the shipped defaults; every action the
  preset does not mention is resolved by the shipped default binding.
- An empty preset is legal and means "built-in defaults" for its group. There is no separate
  "none" value.
- Preset metadata is user data: `id` (immutable, opaque), `name` (mutable display label),
  group, provenance (created by user / converted by migration), timestamps.
- Content is canonical action + canonical control id rows (position-based `ControlId` codes).
  A preset stores **no device identity of its own**, but individual rows can be device-bound:
  raw-HID trigger codes embed an anonymized endpoint identity, so **portability is a property of
  the row's control id, not of the preset**. A preset may legally mix portable and device-bound
  rows; sharing it with another controller never implies the device-bound rows will fire there
  (sections 9 and 10).

Presets are the *content*; **assignments** (section 4) decide which content a device or a game
uses. Saving and assigning are separate operations.

## 3. Identity separation

- A preset's identity is its opaque `id`, never its name. Renaming never affects assignments.
- A preset is **not** a controller identity and must not become one: nothing in preset content or
  assignment lookup may add a new hardware-keying scheme. Assignment targets reuse exactly the
  identity keys the binding profiles already use today — a durable `controller` key (strong
  `logicalId`, established by runtime proof, never inferred from stored syntax — section 6), a
  `legacy_slot` fingerprint, a game executable key, or a group default — each stored with its
  kind (section 4). An existing alias chain is used to *find* a controller target
  at runtime; it is never stored as a target.
- One preset is assignable to several controllers at once; one controller can switch presets
  freely without touching its physical identity, its logical id or its alias registrations.
- Preset content must exclude everything in section 9 (endpoint/container ids, topology,
  calibration, overlay/capture state, timing settings, scope definitions).

## 4. Precedence — exactly one winner

For a mapping query with device group `G`, controller identity `C` (controller group only) and
running game `X`, resolution produces **one** preset and one source:

1. **Game rule** — `X` has an assignment for `G` → that preset. Source `game`.
2. **Controller rule** — `G` is `controller` and `C` has an assignment → that preset.
   Source `controller`. `C` is matched through the same durable key + alias chain the resolver
   uses for binding rows today.
3. **Group default** — `G` has a default assignment → that preset. Source `group_default`.
4. **Built-in defaults** — no assignment anywhere → the shipped default table. Source `builtin`.

Assignment targets are **typed**: the kind is stored with the assignment, so `cpo-p02` can never
conflate a physical controller with a slot key.

1. `controller` — a **durable** controller identity (a strong, generation-free registry
   `logicalId`). This is "this controller"; it follows the pad exactly as far as today's identity
   + alias resolution already does (`cpo-c03b`).
2. `legacy_slot` — a **compatibility target**: a pre-identity fingerprint key (`xinput.slotN`,
   `winmm.slotN`, unresolved provider keys). Slot-scoped by definition — another pad can occupy
   that slot later — so it is never presented or reasoned about as "this physical controller";
   Settings wording is slot-based ("slot 2 (adapter order)"), never the old pad's name.
3. `game` — a game target keyed by the durable executable identity (section 8).
4. `group_default` — the per-group default (no controller or game involved).

Session-local weak `logicalId`s are never valid persisted targets (they cannot recur after a
restart). Because `createLogicalId()` renders weak and strong identities in the same
`controller-…` shape, an upgrade cannot classify stored keys — so no stored `controller-…` key is
promoted to a durable target by syntax alone. Every historical key is an **unverified migration
source** (section 6): preserved for compatibility bookkeeping, promoted to a `controller`
assignment only when a live runtime chain proves it durable; an unproven key stays inert recovery
evidence, is reported once to diagnostics, and is never re-pointed at a live pad by guessing.

Content is then `effective(G) = shipped defaults overlaid by the winning preset's rows` — a single
overlay over layer 0, one row per `(actionId, slot)`. There is no runtime merging of
group-wide + alias + device-specific layers any more: after migration those layers exist only as
*content inside presets*, never as an active second layer. This is the deliberate change that
gives "one deterministic winner"; `cpo-p03` must prove the migration makes it behavior-neutral
(before/after equality per profile).

Keyboard and mouse skip step 2 (no per-instance identity); their resolution is game rule → group
default → built-in defaults.

Gesture arbitration is unchanged by presets: scope unions, `OverlayInputPolicy` selection,
`BindingRelation` conflict classification and `ContextOverrideCatalog` substitution stay exactly
as they are and are evaluated against the winning preset's rows.

## 5. Switching

An effective switch happens when the user changes a selection, a game starts/exits, an assignment
changes, or migration completes. The contract:

- **Atomic and immediate within one event-loop turn**: apply the new table, then cancel
  in-flight gesture state (`cancelAll()`/`invalidate()`), exactly as `reload()` and
  `activateBackend()` already do. No gesture may ever observe a partial old/new table.
- **No old-preset action fires after the switch**: pending tap/hold/chord timers from the old
  preset are invalidated; a control that was physically held across the switch cannot fire its
  new-preset action until it is released and pressed again (edge semantics + invalidation).
- **No new-preset action fires from an old press**: the recognizer `Context` carries the resolved
  preset at press time, and dispatch resolves against that same preset, so a delayed gesture can
  never execute against a newer selection.
- **The next input cycle** observes the new preset: the recognizer's facts provider
  (`factsFor` → `effectiveBindings`) answers from the newly resolved preset for any press that
  starts after the switch.
- **Overlay-safe**: a switch while the overlay is open must not close the overlay, must not
  disturb overlay navigation state and must invalidate only pending *mapping* gestures. This is
  the contract `cpo-p07` implements for game start/exit with the overlay open.
- **Latency**: a switch decision is made on the same signals that already drive the runtime
  (assignment change, `currentGameChanged`); no polling is required.

## 6. Migration (owned end-to-end by `cpo-p03`)

- **Timing**: the migration has a deterministic offline part and a bounded runtime phase
  (below). The **offline part** runs at schema upgrade (next free `applyV*`), **transactional and
  idempotent** — all statements in one transaction, failure rolls back to the current behavior,
  repeat runs are no-ops. No user ever edits bindings in the old model: the compatibility phase
  changes *resolution only* and always yields the old table until materialization proves equality
  for that chain.
- **Ownership (reviewer decision, round 2)**: `cpo-p03` owns the entire migration lifecycle — the
  deterministic offline schema upgrade *and* the bounded compatibility/materialization phase. It
  must not be read as schema-upgrade-only, and no separate leaf owns the read bridge.
- **Sources**: the existing `binding_overrides` rows and their profile keys.
- **Rules**:
  - The group-wide rows (`device_profile = ''`) of group `G` become the content of a
    migration-created preset (proposed name "Default"), which becomes `G`'s **group default
    assignment**. A group without group-wide rows gets no generated preset (its default stays
    built-in).
  - Every non-empty stored profile key `K` with rows produces a converted preset for its group
    with content `group-wide rows ∪ K's own rows`. Slot-fingerprint keys (`xinput.slotN`,
    `winmm.slotN`) are assigned to `K` as a `legacy_slot` target. A `controller-…` key is **not**
    assigned offline: it becomes an *unverified migration source* — converted content plus a
    bookkeeping record keyed by the historical key (below).
  - **Historical opaque `controller-…` keys are unverified migration sources, not durable
    controller assignments.** Offline migration may create and preserve converted preset content
    associated with the source key for compatibility bookkeeping, but it must not create a
    `controller` assignment until a live runtime chain proves that key belongs to a durable
    logical controller. Materializing the shared rows is what keeps the visible mapping
    byte-for-byte identical for users who had both layers; rows that are only reachable as
    another controller's **alias** cannot be folded in offline — that is what the compatibility
    phase below exists for.
  - **Promotion happens only at runtime, never by syntax.** When a live chain proves a historical
    key belongs to a durable logical controller, materialization records the durable `controller`
    assignment (below) and retires the source key as bookkeeping. If the live controller is
    weak/session-local, no persistent assignment is created. A key never observed again stays
    inert migration/recovery evidence — reported once to diagnostics, never re-pointed at a live
    pad, never guessed onto another controller.
  - Rows the resolver currently rejects (`validateRow` failures) are skipped during conversion
    exactly as they are skipped at `reload()` today, and are reported to diagnostics instead of
    blocking the migration.
  - Rows are copied, never deleted: `binding_overrides` is retained for recovery and diagnostics,
    and as the read bridge's source until a chain has been materialized and proven equivalent.
- **Why the offline part cannot prove equality alone (evidence)**: today's effective table also
  layers **alias rows** between group-wide and the profile's own rows — `effectiveBindings()`:
  defaults < `''` < `m_profileAliases[profile]` in registration order < own key, later wins
  (`BindingResolver.cpp` line 191-197). That alias chain is built at runtime by
  `InputEngine::configureLogicalProfile()` from the live registry (`modelFingerprint`, provider
  attachments) and from `logicalControllerRekeyed` events; **it is not persisted**, and strong vs
  weak ids look identical offline (`createLogicalId()`). A one-shot `applyV*` therefore cannot
  materialize `group-wide ∪ alias rows ∪ own rows` for a pad whose chain is not yet known, and
  must not pretend it can.
- **Bounded compatibility phase (read bridge + per-chain materialization)**: until a controller's
  chain is proven equivalent, its resolution still applies the legacy alias layering — reading
  alias rows from the retained converted preset of each alias key — in exactly today's order
  (`''` < aliases in registration order < own key, later wins). When a controller attaches and
  its chain becomes known (or Settings opens for it), a **one-time transactional
  materialization** atomically folds `group-wide < ordered aliases < exact live profile` into the
  winning preset — the full union `group-wide ∪ alias-layer rows ∪ own rows` — verifies
  effective-table equality against the legacy resolution, and only then records the durable
  `controller` assignment and retires the chain layers for that controller. If runtime identifies
  the live controller as weak/session-local, no persistent controller assignment is created and
  the chain keeps its compatibility resolution. Materialization is idempotent, per-controller,
  and journaled — it is a bridge to retire the old layer, not the default model.
- **Disabling legacy resolution**: the legacy layer is retired for a chain only after
  equivalence is proven — a diagnostic sweep compares the legacy effective table against the
  resolved preset table for that chain and records equality. The global legacy layer is removed
  once every stored key is either proven or classified inert (unmatchable weak key); anything
  unresolved is reported to diagnostics, never silently dropped.
- **Proof obligation (key invariant)**: for every old profile that could resolve today, the old
  `effectiveBindings(profile)` must equal the new winning preset table before legacy resolution
  is disabled for that profile. A database with only group-wide rows ends with one preset, one
  group default assignment and no controller assignments.

## 7. Assignment, fallback, rename, delete

- **No controller assignment** → next step in the precedence chain (group default, then built-in
  defaults). This is the normal state right after migration for controllers without their own
  rows.
- **Assigned preset missing or corrupt**: step down the chain (game → controller → group default
  → built-in), report it once to diagnostics and show "assigned preset missing" in Settings.
  Individual invalid rows keep today's policy: skip the row, the action falls back to its
  default; one bad row never rejects the whole preset.
- **Reconnect under a different provider identity** (the current split, until `cpo-c03b` proves a
  cross-provider key): assignment lookup uses the same alias chain as today, so an assignment made
  under a durable key still applies while that key is aliased; a pad that comes back as a *different*
  logical controller falls to the group default. Settings must word this honestly
  ("applies while connected as …"), never imply more correlation than exists.
- **Two controllers sharing one preset**: fully supported. Editing the preset affects both;
  editing controls must show "used by N controllers" and offer "duplicate for this controller"
  so a per-pad change never happens by accident.
- **Rename**: display-only. The `id` is stable; all assignments keep working; uniqueness of names
  within a group is enforced (trimmed, case-insensitive) so assignment pickers stay unambiguous.
- **Delete**:
  - Deleting a preset requires resolving its references in the same transaction: the user either
    reassigns every referencing target (controller, legacy slot, game, group default) to another
    preset of the same group, or the delete is refused. Never leave a dangling reference.
  - Deleting a preset that a connected controller currently uses re-resolves immediately
    (section 5 switch-safety applies).
  - The group default assignment must always point at an existing preset or at built-in defaults;
    a generated "Default" preset may be renamed or emptied but not deleted while it is the group
    default — the user first points the group default elsewhere.
  - Deletion is always an explicit user action; the app never deletes user presets on its own.

## 8. Relationship to games

- Presets are **global** user data, not per-game files: a game never owns mappings; it only has
  assignments that point at presets.
- A game assignment is a `game` target `(game key, group) → preset id`. It is the top precedence
  rule while that game runs, for the groups it names; unnamed groups fall through to
  controller/group rules.
- **Game key** is the durable game identity: the canonicalized executable identity
  (`CurrentGameService` / `GameDetector` foreground data, already normalized and lowercased).
  That key is the **only** thing a game assignment matches on. A `games` row id may be cached
  next to an assignment as a convenience for UI joins, but it **never makes an assignment
  match** — `GameRowRepair` may merge or delete rows, and a row id that now belongs to a
  different game must not attract a stale assignment. If the executable identity is unavailable
  or the row cannot be resolved, the rule **falls through** (controller → group default →
  built-in), is reported in diagnostics and shown unresolved in Settings — it never guesses by
  row id.
- Game start/exit are exactly the switch events of section 5; with the overlay open they must not
  close it (`cpo-p07`).
- Games without captures are supported: `rememberGameExecutable()` already creates their row, and
  the key above does not depend on captures existing.

## 9. Exclusions — what a preset does not own

- **Identity fields**: endpoint ids, container ids, HID paths, VID/PID lists, provider names,
  `logicalId`s, slot numbers — none of these are preset content fields, and a preset name/id is
  never used as an identity key. (An individual row's trigger code may still embed an anonymized
  endpoint hash — see the non-portable rows bullet below.)
- **Calibration/runtime**: stick drift or trigger calibration, deadzones, rumble, LED, battery,
  layout reconfirmation state (`controller_layouts`), raw-HID layout signatures, backend
  arbitration and mirror suppression, HidHide/cloak state, press history.
- **Scope and substitution policy**: `ActionCatalog` definitions/scopes,
  `OverlayInputPolicy` arbitration and `ContextOverrideCatalog` substitutions stay code/product
  owned — a preset supplies rows, not routing rules.
- **Gesture timing settings**: multi-tap interval, chord window, default hold duration
  (`GestureTiming`) remain global settings; a `holdMs = 0` row keeps resolving through them.
- **Overlay/capture/product state**: overlay visibility and position, per-game overlay category,
  capture mode, quality/encoder presets, sound settings, replay buffer state, notifications,
  onboarding flags.
- **Session-local identity keys** as persisted assignment targets (section 6); historical opaque
  `controller-…` keys are unverified until runtime proof (sections 4 and 6).
- The legacy `bindings` table stays unused (database-compatibility only).
- **Non-portable rows**: raw-HID trigger codes (`gamepad.raw.<hash>.usage.<page>.<usage>`) embed an
  anonymized endpoint identity. They are legal preset content and are **preserved by migration**
  (dropping them would break behavior equivalence), but portability is per-row: they cannot fire
  on another pad. The editor must mark them as device-bound, and sharing the preset must warn
  that those rows will not work elsewhere rather than advertising the whole preset as portable.

## 10. Semantic matrix

| Case | Result |
| --- | --- |
| Two identical pads, one shared preset | Both assigned the same preset id; identical behavior; no identity is stored in the preset. If only a legacy slot fingerprint can identify one of them, the assignment is slot-scoped and Settings says so. |
| One pad switching presets | Only that controller's assignment changes; other controllers keep theirs; live switch cancels in-flight gestures (section 5); the group default is untouched. |
| Reconnect under another provider identity | Assignment found through the durable key/alias chain applies; a different logical controller falls back to the group default. Honest UI wording; cross-provider follow waits for `cpo-c03b`. |
| Existing user with device-specific overrides | Migration creates "Default" (group-wide rows) + one converted preset per stored profile; alias layers are folded in by per-chain materialization when the pad attaches; effective table before == after at every step; the pad's buttons do not change. |
| Deleted preset | Delete is refused unless every reference is reassigned in the same transaction; a connected user of the deleted preset re-resolves immediately. |
| Per-game session selecting a preset | While the game runs, its assignment wins for the named groups; on exit (or game change) the previous winner returns — with overlay-safe switching. |
| Held button when the game changes | Press started under the old preset; switch invalidates it; nothing fires until the button is released and pressed again. |
| Game rule pointing at a missing preset | Falls to controller assignment, then group default, then built-in defaults; reported in diagnostics and Settings. |
| Empty preset as game rule | Means "built-in defaults for this game" — the supported way to opt out of custom mappings for one game. |
| Keyboard/mouse | Same model without the controller step: game rule → group default → built-in defaults. |
| Canonical controller + legacy alias rows + exact rows | Three old layers, one new table. Offline migration converts each stored key (content = group-wide ∪ own rows; the controller key stays an unverified source, no assignment yet). Until the chain is observed, the compatibility phase resolves exactly today's merge (group-wide < alias < own). On attach, materialization folds all three layers into the controller's one preset and records the durable assignment, after which the alias layer is retired — the user's buttons never change. |
| Legacy slot assignment occupied later by another controller | The `legacy_slot` target is slot-scoped by contract: whatever pad occupies that slot inherits it. Settings words it as the slot ("slot 2 (adapter order)"), never as the old pad; only a durable `controller` assignment can follow one pad (`cpo-c03b`). |
| Repaired/deleted game row, unchanged executable | The `game` target still resolves by executable identity; a changed `games` row id updates at most a UI cache, never the match. No other game can inherit the assignment, and if the executable key is unresolvable the rule falls through unresolved. |
| Raw-HID row in a shared preset | Both controllers share the preset; the device-bound row fires only for the pad whose endpoint hash it embeds. The editor marks the row non-portable and warns on sharing instead of promising shared behavior. |
| Historical `controller-…` key, strong/weak provenance unknowable offline | Kept as an unverified migration source: converted content and a bookkeeping record survive, but no durable `controller` assignment is created. Resolution runs through the compatibility phase until a live chain proves the key. |
| Later runtime attachment proves the key durable | Materialization atomically folds `group-wide < ordered aliases < exact live profile` into the winning preset, verifies effective-table equality, then records the durable `controller` assignment and retires legacy resolution for that chain — the user's buttons never change. |
| Historical key never observed again | Stays inert migration/recovery evidence: reported once to diagnostics, never promoted, never re-pointed or guessed onto another controller. |

## 11. Findings for the reviewer

1. **Assignment following across providers is limited until `cpo-c03b` lands.** The contract
   deliberately reuses today's identity + alias resolution, so a preset assigned to a pad under
   one backend's logical controller does not follow it to another backend's view of the same pad.
   Shipping presets with that limitation (plus the honest UI wording) is the recommendation;
   `cpo-p04` already depends on `cpo-c03`, which stays open for exactly this reason.
2. **Weak-identity orphaning trap**: today, per-controller rows saved for a session-local
   `logicalId` are orphaned after restart. Presets must not inherit this silently — persisted
   targets on a session-local key are forbidden (section 4), and because weak and strong ids are
   indistinguishable offline, migration keeps every historical `controller-…` key as an
   unverified source — never promoted by syntax, never guessed, promoted only by runtime proof;
   the UI decision (offer "this model" / "this slot" scoping instead) is flagged for `cpo-p06`.
3. **`cpo-p05` depends on `cpo-c04`** (route/gesture lifetime stabilization, still `todo`). The
   switching guarantees in section 5 are written as target semantics; the leaf sequencing already
   reflects that dependency.
4. **Naming**: "preset" already means encoder/quality preset in Settings and the tray tooltip;
   the contract mandates "mapping preset" wording and distinct i18n ids.
5. **Single overlay-route question** (open from `cpo-o04`): with keyboard users having no
   capture-switch control inside the overlay, mapping presets do not introduce one; a preset
   switch hotkey is deliberately not part of this contract.
6. **`cpo-p03` owns the whole migration lifecycle — reviewer decision, round 2, resolved.** A
   pure offline "one-time transactional upgrade with proven equality" is impossible for
   alias-backed chains (section 6): the alias map is runtime-only and weak ids do not look
   different from strong ones. Decision: no extra leaf; `cpo-p03` is extended to own the
   deterministic offline part, the bounded compatibility phase, the per-chain materialization
   and the equivalence sweep together (see the Ownership bullet in section 6).
7. **Target kinds are storage contract** (`cpo-p02`): a `controller` assignment and a
   `legacy_slot` assignment must be distinguishable rows with an explicit kind, because one
   follows a pad and the other follows a slot — conflating them silently re-creates the orphaning
   trap. The UI wording half of this stays with `cpo-p06` (finding 2).
