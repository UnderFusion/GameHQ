<!-- SPDX-License-Identifier: MIT -->

# Local Integration Protocol

How other local tools — currently the Playnite companion plugin, and any
future second GameHQ instance — talk to a running GameHQ over a private,
same-user-only channel. See [updater.md](updater.md) for how this channel
also carries update-maintenance signals.

## Transport

- **Pipe name:** `GameHQ.Local.v1` (fixed identifier, never changed without a
  protocol version bump).
- **Mechanism:** `QLocalServer` / `QLocalSocket` on the app side,
  `NamedPipeClientStream` on the .NET plugin side. `QLocalServer` is opened
  with `UserAccessOption` so only processes running as the same Windows user
  can connect — there is no network listener and no cross-user access.
- **Framing:** every message is a 4-byte little-endian payload length prefix
  followed by that many bytes of UTF-8 JSON. Maximum frame size is 64 KiB;
  larger, zero-length, or malformed frames cause the connection to be
  dropped without a crash.
- **Server startup:** the local server is started early in `App::init()` so
  it is available as soon as possible after launch.

## Security posture

The server treats every inbound message as untrusted input:

- Strict message-type allowlist — unrecognized `type` values are rejected.
- No message ever causes GameHQ to execute an arbitrary path or shell
  command. Payload fields like install directories are used only for
  matching against processes GameHQ already observes, never invoked.
- Bounded queues and timeouts prevent a slow or hostile client from
  exhausting resources.
- Abusive clients (repeated malformed frames) are disconnected.

## Handshake

A client must introduce itself before any other message is accepted.

```json
// client -> app
{
  "type": "hello",
  "client": "GameHQ.Playnite",
  "clientVersion": "1.0.0",
  "protocolMin": 1,
  "protocolMax": 1,
  "requestId": "..."
}
```

```json
// app -> client
{
  "type": "hello.ack",
  "appVersion": "<current GameHQ version>",
  "protocolMin": 1,
  "protocolMax": 1,
  "protocolSelected": 1,
  "capabilities": ["app.activate", "app.open_gallery", "game.lifecycle.v1", "status.v1"],
  "requestId": "..."
}
```

`capabilities` is the authoritative list of what this build accepts; a client
must not send a message outside it. `protocolSelected` is the highest version
both sides support and is what the rest of the connection speaks.

If the client's `[protocolMin, protocolMax]` range does not overlap the
app's, the app replies with `error` (below) carrying
`code: "protocol_incompatible"` instead of `hello.ack`, so the client can tell
its user which side needs an update; the connection is then closed.

### Request correlation

Any client message may carry a `requestId` string. Every app reply to that
message echoes the same `requestId`. Broadcasts the app originates
(`app.maintenance`) have no `requestId`. Clients must tolerate replies
arriving out of order and must not block on one.

## Message set

| Type | Direction | Purpose |
|---|---|---|
| `hello` / `hello.ack` | client -> app / app -> client | Handshake and capability negotiation. |
| `app.activate` | client -> app | Focus the running app's window (used by second-instance forwarding, see below). |
| `app.open_gallery` | client -> app | Focus the app and navigate to the gallery. |
| `ack` | app -> client | Confirms an accepted action and echoes its `requestId`. |
| `app.maintenance` | app -> client | Broadcast that an update is in progress; see below. |
| `status.request` / `status.response` | client -> app / app -> client | Ask for current app status (used by plugin diagnostics). |
| `error` | app -> client | Rejected message or failed handshake; carries `code` and human-readable `message`. |
| `playnite.application.started` | client -> app | Playnite itself has started. |
| `playnite.application.stopping` | client -> app | Playnite is shutting down. |
| `playnite.game.starting` | client -> app | A game launch has been initiated. |
| `playnite.game.started` | client -> app | A game process is confirmed running. |
| `playnite.game.stopped` | client -> app | A game session has ended normally. |
| `playnite.game.startup_cancelled` | client -> app | A game launch was cancelled before it started. |
| `playnite.state.sync` | client -> app | Full snapshot of currently-running games, sent after every successful connection. |

### Game lifecycle payload fields

`playnite.game.*` messages carry a subset of these fields; fields Playnite
cannot always supply are optional and must be treated as such:

- `sessionId` — stable identifier for this game session
- `playniteGameId` — Playnite's own game identifier
- `name`, `sourceName`, `platformNames` — display metadata
- `installDirectory`, `selectedRomFile` — used only for identity matching,
  never executed (see [architecture notes on `GameDetector`](architecture.md))
- `launchExecutable` — optional absolute path from the selected Playnite file action.
  When Playnite supplies no PID, a single started session may identify a
  windowed game by an exact foreground executable match inside its installation
  directory. A conflicting process or a second matching session disqualifies this match.
- `startedProcessId` — the PID Playnite reports for the launched game, when
  available; not assumed valid for launchers or emulators that spawn a
  child process
- `occurredAtUtc` — event timestamp

### `playnite.state.sync` semantics

Sent by the client immediately after every successful handshake, including
reconnects. The snapshot **replaces** any state the app previously held for
that client — it is not merged — so a missed disconnect never leaves a
phantom "game running" state behind. The app also expires external game
context after a bounded grace period following a disconnect with no
reconnect, after which normal (non-Playnite) detection continues to work
unaffected. Only the departure of the *last* handshaken Playnite client starts
that grace period: a reconnect can complete before the superseded connection
reports its disconnect, and expiring the source then would discard the state
the live connection had just published.

The message carries a `games` array. Each entry uses the lifecycle fields
above and must include a unique `sessionId`; at most 64 active games are
accepted in one snapshot.

### `app.maintenance`

```json
{
  "type": "app.maintenance",
  "reason": "update",
  "retryAfterSeconds": 30
}
```

Sent by the app when it enters the quiescence/handoff sequence described in
[updater.md](updater.md). A well-behaved client must suspend auto-launch and
reconnect attempts until one of: the maintenance marker clears, a
success/rollback signal is observed, or the bounded retry window passes. An
update-related disconnect is expected behavior, not a crash to relaunch
from.

The durable package-side marker is `.update/maintenance.lock`. The root
launcher and direct app entry refuse ordinary launches while it is active;
the helper removes it only after `healthy` or `rolled_back` becomes durable.
If no helper owns the updater mutex, a marker older than five minutes is
treated as stale and startup is allowed to enter bounded recovery instead of
being blocked forever. `--post-update` validation is the only launcher bypass.

### `error`

```json
{
  "type": "error",
  "code": "protocol_incompatible",
  "message": "This plugin requires GameHQ 0.7 or newer.",
  "requestId": "..."
}
```

Defined codes: `protocol_incompatible`, `unknown_type`, `malformed`,
`not_handshaken`, `unavailable` (the requested action cannot run right now,
e.g. during maintenance). An `error` is advisory except for
`protocol_incompatible` and repeated `malformed`, which are followed by a
disconnect.

## Identity authority

Playnite-supplied fields are treated strictly as **identity hints**, never
as authority. GameHQ's own foreground-window/process checks, exclusion
lists, and capture-safety gates always take precedence; see
`src/games/GameDetector.cpp` and the matching-priority rules in the plan's
Phase 3 item for how identity hints are combined with existing detection.

## Second-instance activation

When a second GameHQ process finds the single-instance lock already held, it
connects to `GameHQ.Local.v1`, sends `app.activate` (optionally followed by a
recognized CLI intent such as `--show` or `--open-gallery`), waits briefly
for an acknowledgement, and exits. If the pipe is absent or does not respond
within a short bounded timeout, the second instance still exits rather than
leaving two full instances running — the `QLockFile` remains the source of
truth for single-instance enforcement, this channel is only a convenience.

## Versioning

The protocol has its own `protocolMin`/`protocolMax` integers, independent
of the GameHQ application version and independent of the Playnite plugin's
own release versioning. A breaking wire change bumps `protocolMax`; the
handshake's negotiated-range check is what keeps old and new builds able to
give the user a clear compatibility message instead of a silent malfunction.
