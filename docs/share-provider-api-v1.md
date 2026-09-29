<!-- SPDX-License-Identifier: MIT -->

# Share Provider API v1

How a separate program adds a **destination** to GameHQ's Share dialog. A
provider is its own process. It connects to GameHQ over a private local pipe,
describes itself, lists targets, and receives one explicit share job at a time.
This document is the whole contract: everything a provider needs is here, and
nothing depends on GameHQ's internal classes.

- Wire protocol version: **1** (`protocolMin` = `protocolMax` = 1)
- Pipe: **`GameHQ.Share.Provider.v1`**
- Status: stable for the fields marked *stable*; see [Stability](#stability)
- Implementation: `src/share/external/`, tests `tests/tst_shareexternalprovider.cpp`
  (protocol) and `tests/tst_shareexternalconformance.cpp` (a real separate process)
- Sample provider and walkthrough: [`integrations/share-provider/`](../integrations/share-provider/README.md)
- Related: [share-platform.md](share-platform.md) (the in-process contract this
  maps onto), [integration-protocol.md](integration-protocol.md) (a different
  channel: `GameHQ.Local.v1` is for Playnite/app signals, not for Share)

## 1. Model and trust

```
 your program  ──(named pipe, same Windows user)──►  GameHQ
   provider                                            registers you as a Share destination
```

- **GameHQ never launches, installs or discovers providers.** You start your
  own process; it connects. There is no manifest file GameHQ reads from disk
  and no executable path in any message. (This is deliberate: provider
  discovery would be part of the attack surface.)
- **Off by default.** The user enables *Settings > Sharing > Share add-ons >
  Allow add-ons from other programs* (`share.external_providers`) and restarts
  GameHQ. While it is off, nothing listens on the pipe.
- **Same user only.** The pipe is created with the same-user access option;
  other Windows users cannot connect. There is no network listener.
- **You are untrusted.** Every value you send is validated and bounded. You can
  register a destination, answer target queries and report job results. You
  cannot make GameHQ read, list, browse or execute anything.

### What this does and does not protect

Be precise about the claim: **this protocol is not an operating-system
sandbox.** A provider runs as the same Windows user as GameHQ, so the OS
already lets it read that user's files. What the protocol guarantees is what
*GameHQ* does:

- GameHQ gives a provider **exactly one file path: the capture the user chose in
  a user-initiated Share, and only inside that job's `job.start`.**
- There is **no message** to request another path, list a folder, read a file,
  or ask GameHQ to run anything. Such messages are `unknown_type` violations.
- Results and progress are accepted only for the **current job's id and its
  one-time token**; anything else is `stale_job`.
- GameHQ treats what you say about the outcome with the same care as any other
  provider: it never shows `sent` for a provider that did not declare
  `direct_send`, and a silent or dead provider ends the job as `unconfirmed`.

If you need real isolation, run your provider in a sandbox of your own
(low-integrity process, AppContainer, separate account). GameHQ does not do that
for you, and the file path it hands you is a normal Windows path.

## 2. Transport and framing

- `QLocalServer`/named pipe `GameHQ.Share.Provider.v1` (Windows path
  `\\.\pipe\GameHQ.Share.Provider.v1`). Connect as a byte-stream client
  (.NET `NamedPipeClientStream`, Python `open(r'\\.\pipe\...', 'r+b', 0)`, Win32
  `CreateFile`).
- Every message is **one frame**: a 4-byte **little-endian unsigned length**
  followed by that many bytes of **UTF-8 JSON** (a single JSON *object*).
- Maximum frame payload: **65,536 bytes**. Zero length, longer than the limit,
  invalid UTF-8, invalid JSON, a non-object, or a missing/empty string `type`
  breaks the stream: GameHQ **closes the connection without a reply** (it cannot
  safely answer on a stream it can no longer parse).
- GameHQ buffers at most 4 frames of your input and at most 4 frames of output
  to you. If you stop reading, GameHQ drops the connection instead of buffering
  without bound.
- Control messages are small. **Capture bytes never travel over the pipe**; you
  read the file yourself (see [Jobs](#5-jobs-and-file-access)).
- Unknown extra fields inside a message are **ignored** (forward compatibility).

## 3. Lifecycle

1. Connect. You have **5 seconds** to send `hello`; otherwise GameHQ sends
   `error: handshake_timeout` and closes.
2. Send `hello`. On success you get `hello.ack` and appear in Share.
3. Answer `targets.request` messages; receive `job.start`, report progress and a
   `job.result`; honour `job.cancel`.
4. Send `goodbye` (or just close) to leave. Your destination disappears from
   Share immediately.

At most **8 connections** are accepted at once; a ninth gets
`error: too_many_providers`. There is one live registration per provider `id`.

Anything sent before a valid `hello` (other than `hello`) earns
`error: not_handshaken` and a disconnect.

## 4. Messages

All messages have a string `type`. Directions: **P→G** provider to GameHQ,
**G→P** GameHQ to provider. `requestId` is an optional string (max 256 chars)
that GameHQ echoes in the reply to that message.

### 4.1 `hello` (P→G)

```json
{
  "type": "hello",
  "requestId": "1",
  "protocolMin": 1,
  "protocolMax": 1,
  "provider": {
    "id": "ext.acme.share",
    "name": "Acme Share",
    "version": "1.2.0",
    "privacy": "Uploads to acme.example with your saved login.",
    "access": "full_account",
    "jobTimeoutMs": 120000,
    "capabilities": ["image", "video", "contacts", "target_search", "direct_send"]
  }
}
```

| Field | Rules |
|---|---|
| `protocolMin`, `protocolMax` | Integers, `1 ≤ min ≤ max ≤ 1000`. The range you support. |
| `provider.id` | **Required.** Must start with `ext.`, then lowercase letters, digits, `.`, `-`, `_`; 2–64 characters total, starting with a letter. The `ext.` prefix keeps you out of the built-in namespace. One live registration per id. |
| `provider.name` | **Required.** Shown in Share. Control/format characters are stripped, whitespace collapsed, cut to 48 characters. |
| `provider.version` | Optional string, ≤ 32 chars. |
| `provider.privacy` | Optional plain-language text, ≤ 200 chars. Shown *after* GameHQ's own fixed disclosure that this is an add-on from another program; it cannot replace it. |
| `provider.access` | Optional: `none`, `share_token`, `full_account`. What your provider can reach. **Omitted means `full_account`** (the widest disclosure). |
| `provider.jobTimeoutMs` | Optional integer, clamped to 1,000–600,000; default 120,000. How long a job may go silent before GameHQ ends it as `unconfirmed`. Any `job.progress` restarts the window. |
| `provider.capabilities` | **Required** array (≤ 32 strings). See below. |

Declarable capabilities:

| Capability | Meaning |
|---|---|
| `image`, `video` | Media you accept. **At least one is required** (`no_media_capability` otherwise). |
| `contacts`, `groups`, `channels` | What kinds of targets you list (display grouping). |
| `target_search` | You honour the `query` in `targets.request`. Without it GameHQ never sends you the user's search text. |
| `caption` | Reserved for captions; accepted, unused in v1. |
| `direct_send` | You deliver the media yourself and can confirm it. Only then may your result be `sent`. |
| `external_handoff` | Another app finishes the send. Your result is never `sent`. |

`direct_send` and `external_handoff` together are `invalid_capabilities`.
`requires_account` and `saved_targets` are **reserved for built-in providers**
and any other unknown name is from a newer API: both are **ignored, not
errors**, and listed back in `ignoredCapabilities` so you can see what was
dropped. Capabilities are a *declaration*, not a permission: GameHQ validates
them at runtime (a target's `media` can only narrow them; results are checked
against them).

### 4.2 `hello.ack` (G→P)

```json
{
  "type": "hello.ack",
  "requestId": "1",
  "appVersion": "0.7.9",
  "protocolMin": 1,
  "protocolMax": 1,
  "protocolSelected": 1,
  "providerId": "ext.acme.share",
  "capabilities": ["image", "video", "contacts", "target_search", "direct_send"],
  "ignoredCapabilities": ["teleport"],
  "limits": { "maxFrameBytes": 65536, "maxTargets": 200, "jobTimeoutMs": 120000 }
}
```

`protocolSelected` is the highest version both sides support; the connection
speaks it. `capabilities` is what was **accepted**: send only messages that fit.

### 4.3 `targets.request` (G→P) and `targets.result` (P→G)

GameHQ asks for destinations inside your provider (people, channels, "the
app itself").

```json
{ "type": "targets.request", "requestId": "q-42", "query": "ann", "mediaKind": "image" }
```

```json
{
  "type": "targets.result",
  "requestId": "q-42",
  "targets": [
    { "id": "u1", "name": "Ann", "kind": "contact", "subtitle": "friend" },
    { "id": "c9", "name": "#clips", "kind": "channel", "media": ["image", "video"] }
  ]
}
```

- `query` is empty for "default list", and always empty unless you declared
  `target_search`. `mediaKind` is `image` or `video`.
- Answer **exactly once**, echoing `requestId`. You have **10 seconds**; after
  that GameHQ shows an empty list with error `timeout`, and a late answer is
  silently dropped (it is *not* a violation).
- Target fields: `id` 1–128 chars of `[A-Za-z0-9._:-]` (unique in the list),
  `name` 1–64 chars, `subtitle` ≤ 96 chars, `kind` one of `contact`, `group`,
  `channel`, `external` (default), optional `media` (`image`/`video`) which can
  only **narrow** what you declared. Sanitising is as for `name` above.
- One bad row never poisons the list: invalid rows are skipped. At most **200**
  targets are used; the rest are dropped.
- To report a failure instead of a list, send `"error": "<code>"` (lowercase
  identifier, e.g. `"not_signed_in"`) with an empty or missing `targets`.

### 4.4 `job.start` (G→P)

Sent only when the user explicitly shares a capture to one of your targets.

```json
{
  "type": "job.start",
  "jobId": "5c0f…",
  "token": "a3b1…",
  "targetId": "u1",
  "file": {
    "path": "C:\\Users\\me\\Videos\\GameHQ\\clip.mp4",
    "fileName": "clip.mp4",
    "sizeBytes": 18234567,
    "mimeType": "video/mp4",
    "mediaKind": "video"
  },
  "gameName": "Some Game"
}
```

`jobId` names the job; `token` is a random one-time secret for this job. You
must echo **both** in every message about the job. The `file` object describes
the capture GameHQ verified when the user opened Share; it is a normal absolute
Windows path (see [Jobs](#5-jobs-and-file-access)).

### 4.5 `job.progress` (P→G)

```json
{ "type": "job.progress", "jobId": "5c0f…", "token": "a3b1…", "state": "transferring", "progress": 0.4 }
```

`state`: `preparing` or `transferring`. `progress`: optional number, `-1` for
unknown, else 0–1 (out-of-range values are clamped). Send it at least every
`jobTimeoutMs` while you work; it proves you are alive.

### 4.6 `job.result` (P→G)

```json
{ "type": "job.result", "jobId": "5c0f…", "token": "a3b1…", "outcome": "sent" }
```

| `outcome` | Use when |
|---|---|
| `sent` | You delivered it **and got confirmation** (requires `direct_send`; otherwise GameHQ records `handed_off`). |
| `handed_off` | Another app now has it and the user finishes there. |
| `copied` | It is on the clipboard / in a folder; nothing was sent. |
| `failed` | Definitely not sent. |
| `cancelled` | Stopped before anything left the PC. |
| `unconfirmed` | It may or may not have been delivered (timeout, lost link). |

Optional `errorCode` (lowercase identifier `[a-z][a-z0-9_]{0,47}`; anything else
becomes `provider_error`) and `detail` (≤ 200 chars, shown to the user after
GameHQ redacts token-like strings). **Never put secrets, URLs with tokens, or
message contents in either.** After the result the token is dead: any later
message about the job is `stale_job`.

Golden rule: report `sent` only when the far side confirmed. When unsure,
`unconfirmed` is the honest answer, and **never retry a send on your own after
an ambiguous failure** (the user may get it twice).

### 4.7 `job.cancel` (G→P)

```json
{ "type": "job.cancel", "jobId": "5c0f…", "token": "a3b1…" }
```

Best effort. Stop work and send `job.result`: `cancelled` if nothing left the PC,
`unconfirmed` if delivery might already have happened, or whatever really
happened. GameHQ keeps the job open until your result arrives or the timeout
ends it.

### 4.8 `goodbye` (P→G)

`{ "type": "goodbye" }` — leave cleanly. Closing the pipe has the same effect.

### 4.9 `error` (G→P)

```json
{ "type": "error", "code": "unknown_type", "requestId": "z" }
```

| Code | Meaning | Then |
|---|---|---|
| `protocol_incompatible` | No overlap between your range and `[1,1]`. | disconnect |
| `invalid_manifest` | Bad/missing `provider`, id, name or field types. | disconnect |
| `no_media_capability` | Neither `image` nor `video` declared. | disconnect |
| `invalid_capabilities` | `direct_send` with `external_handoff`. | disconnect |
| `duplicate_provider` | That `id` is already registered by a live provider. The running one is unaffected. | disconnect |
| `not_handshaken` | A message other than `hello` before the handshake. | disconnect |
| `handshake_timeout` | No `hello` within 5 s. | disconnect |
| `too_many_providers` | Connection limit reached. | disconnect |
| `already_handshaken` | A second `hello`. | violation |
| `unknown_type` | A `type` outside the provider→GameHQ allowlist. | violation |
| `stale_job` | Wrong/finished `jobId` or `token`. | violation |
| `invalid_field` | A field of the wrong type/value (e.g. bad `outcome`). | violation |
| `too_many_violations` | The 5th violation on one connection. | disconnect |

Violations are counted per connection; the **5th** disconnects you. A broken
frame (section 2) disconnects immediately with no `error`.

**Delivery of a final error.** For every "disconnect" above GameHQ removes your
registration at once, but keeps the pipe open for a short grace period (about
300 ms) so you can read the `error` before it closes. Tearing down the server end
of a Windows pipe discards unread data, so a provider that is not reading at that
instant would otherwise see only a bare disconnect. Read until the pipe closes.

## 5. Jobs and file access

- **One job at a time** across all of Share. A job exists only after a
  user-initiated share of a capture to one of your targets.
- The `file.path` in `job.start` is the *only* file GameHQ ever names to you. It
  is the capture the user selected, and GameHQ has just confirmed it is
  unchanged since the user opened Share. Open it **read-only**; do not modify,
  move or delete it (that would be the user's screenshot or clip).
- The path grants no capability beyond ordinary Windows permissions (see the
  trust note in section 1). Do not treat the path as a token you can re-use later:
  GameHQ may rename, move or delete captures at any time.
- There is no file-transfer message, and bytes are never carried in control
  frames. If your service needs the content, read the file yourself and stream
  it to your service.
- Never log the path together with credentials, and do not send it anywhere.

## 6. Timeouts, disconnects, failure isolation

| Situation | What GameHQ does |
|---|---|
| `hello` not sent in 5 s | `handshake_timeout`, close. |
| `targets.result` not sent in 10 s | Empty list with error `timeout`; late answer ignored. |
| Job silent for `jobTimeoutMs` | Sends `job.cancel`, ends the job as `unconfirmed` (`timeout`). |
| You disconnect or crash during a job | `unconfirmed` (`provider_disconnected`): `job.start` was delivered, so the media may have gone out. Your destination is removed. |
| Malformed/oversized input | Connection dropped; GameHQ and other providers are unaffected. |

A misbehaving or crashing provider never takes GameHQ down and never affects
other providers.

## 7. Versioning and compatibility

- `protocolMin`/`protocolMax` are independent of GameHQ's application version.
  A breaking wire change raises `protocolMax`; GameHQ answers a provider whose
  range does not overlap with `protocol_incompatible` so you can tell the user
  which side needs updating.
- Additive changes (new optional fields, new capability names, new message
  types) do not change the version: unknown fields are ignored and unknown
  capabilities are reported in `ignoredCapabilities`. Write your provider to
  ignore what it does not understand.
- A provider that supports `[1, 3]` talking to a GameHQ that speaks 1 is served
  at version 1 (`protocolSelected: 1`).

## Stability

What GameHQ promises to keep working for a v1 provider, and what it does not.

**Stable in v1.** Changing any of these needs a new protocol version
(`protocolMax` 2); an old provider then gets a clean `protocol_incompatible`
instead of a silent malfunction.

| Area | Stable |
|---|---|
| Transport | Pipe name `GameHQ.Share.Provider.v1`; same-user access; framing (4-byte LE length + UTF-8 JSON object); 64 KiB frame limit. |
| Messages | The message types in section 4 and the meaning and type of every field shown there. |
| Manifest | `provider.id` rules (`ext.` prefix), `name`, `version`, `privacy`, `access` (`none`/`share_token`/`full_account`), `jobTimeoutMs`, `capabilities`. |
| Capabilities | `image`, `video`, `contacts`, `groups`, `channels`, `target_search`, `direct_send`, `external_handoff`. |
| Outcomes | `sent`, `handed_off`, `copied`, `failed`, `cancelled`, `unconfirmed` and their meanings, including "`sent` needs `direct_send`". |
| Errors | Every code in section 4.9 and whether it disconnects you. |
| Job rules | One job at a time; `jobId` + `token` on every job message; the token dies with the job; `job.start` is the only message that names a file, and only the selected capture. |
| Limits | The documented values are **floors**: GameHQ may raise them but will not lower them within v1 (5 s handshake, 10 s target answer, 200 targets, 8 connections, 5 violations, 1-600 s job timeout range). Read `hello.ack.limits` rather than hard-coding. |
| Compatibility | Unknown fields are ignored; unknown capabilities are ignored and reported in `ignoredCapabilities`; a provider range overlapping v1 is served at v1. |

**Experimental: may change in any GameHQ release without a protocol bump.**
Do not build behaviour on these.

| Area | Experimental |
|---|---|
| `caption` capability | Accepted and echoed back, but has no effect in v1. |
| Presentation | Where and how `name`, `privacy`, `subtitle`, `detail` and progress are shown; sorting and grouping of targets; icons. |
| `hello.ack.appVersion` | Informational; do not parse or gate on it (use `protocolSelected`). |
| Timing details | The exact length of the grace period before a rejected connection closes (only "the final error can be read" is promised), polling intervals, and log wording. |
| Anything not in this document | Including undocumented fields, message types or behaviour observed in a particular build. |

*Not part of the contract at all*: GameHQ's internal C++ types and classes (there
is no ABI to link against), its file layout, and its own settings and storage.

## 8. Minimal provider (Python, no dependencies)

This loop reads and writes from one thread and only writes in reply to a
message, which is why a plain blocking read is safe here. A provider that also
sends progress while it listens for a cancel needs non-blocking reads (a blocked
read on a Windows pipe handle blocks writes on that handle too); see
`integrations/share-provider/sample_provider.py`, which polls `PeekNamedPipe`.

```python
import json, struct

pipe = open(r"\\.\pipe\GameHQ.Share.Provider.v1", "r+b", buffering=0)

def send(obj):
    data = json.dumps(obj).encode("utf-8")
    pipe.write(struct.pack("<I", len(data)) + data)

def recv():
    n = struct.unpack("<I", pipe.read(4))[0]
    return json.loads(pipe.read(n))

send({"type": "hello", "requestId": "1", "protocolMin": 1, "protocolMax": 1,
      "provider": {"id": "ext.sample.folder", "name": "Sample folder",
                   "access": "none",
                   "capabilities": ["image", "video", "external_handoff"]}})
assert recv()["type"] == "hello.ack"

while True:
    m = recv()
    if m["type"] == "targets.request":
        send({"type": "targets.result", "requestId": m["requestId"],
              "targets": [{"id": "here", "name": "Sample folder"}]})
    elif m["type"] == "job.start":
        # read m["file"]["path"] (read-only), do the work, then report honestly
        send({"type": "job.result", "jobId": m["jobId"], "token": m["token"],
              "outcome": "copied"})
    elif m["type"] == "job.cancel":
        send({"type": "job.result", "jobId": m["jobId"], "token": m["token"],
              "outcome": "cancelled"})
```

## 9. Conformance checklist

A provider is well-behaved when it:

1. Connects, sends `hello` within 5 s, and treats `hello.ack.capabilities` as
   the list of what it may rely on.
2. Sends only message types in section 4 for the provider→GameHQ direction and
   ignores unknown fields and unknown message types from GameHQ.
3. Answers every `targets.request` once, within 10 s, with valid target rows.
4. Echoes `jobId` and `token` in `job.progress`/`job.result`, sends progress
   while working, and sends exactly one `job.result` per job.
5. Reports `sent` only with confirmation and never without `direct_send`.
6. Never retries after an ambiguous failure and never puts secrets in
   `errorCode`/`detail`.
7. Opens the capture read-only and only for the running job.
8. Reconnects (with backoff) after GameHQ restarts; treats a disconnect as
   normal, not as a crash.
9. Copes with `protocol_incompatible` by telling the user, not by retrying in a
   loop.
