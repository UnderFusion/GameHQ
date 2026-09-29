<!-- SPDX-License-Identifier: MIT -->

# Share Provider SDK (sample + walkthrough)

Add your own destination to GameHQ's Share dialog from a separate program.

The extension boundary is the **wire protocol**, not a library: there is no
DLL to load and no C++ ABI to link. Everything you need is the specification,
[`docs/share-provider-api-v1.md`](../../docs/share-provider-api-v1.md), plus
this folder.

| File | What it is |
|---|---|
| `sample_provider.py` | A complete provider in one dependency-free Python file. Copies the shared capture into a folder on your PC; nothing is uploaded. |
| this README | How to turn add-ons on, connect, and check your work. |

## 1. Turn add-ons on in GameHQ

External providers are **off by default**.

1. Open GameHQ > Settings > Capture > **Share add-ons**.
2. Switch on **Allow add-ons from other programs**.
3. Restart GameHQ (the setting is read at start-up).

While it is off nothing listens on the pipe and providers simply cannot
connect. GameHQ never launches, installs or discovers providers: you start your
own process.

## 2. Run the sample

Requirements: Windows, Python 3.8 or newer, **no third-party packages**.

```
python sample_provider.py --dest-dir C:\Temp\ShareSample
```

Take a screenshot or open one in GameHQ and press Share: **Sample folder**
appears in the destination list with two targets, *Inbox* and *Archive*
(type in the search box to filter them). Choosing one copies the capture to
`C:\Temp\ShareSample\<target>\<file name>`, and Share reports **Copied**, since
the file is in a folder and nothing was sent.

Closing the program (or Ctrl+C) removes the destination from Share. If GameHQ is
not listening the sample exits with code 4 and says so; add `--reconnect` to have
it wait and re-register whenever GameHQ starts or restarts.

Exit codes: `0` clean, `2` GameHQ rejected the provider, `3` protocol
incompatible, `4` GameHQ not listening, `5` connection lost, `6` bad arguments.

## 3. The minimum a provider does

Connect to `\\.\pipe\GameHQ.Share.Provider.v1`. Every message is a 4-byte
little-endian length followed by that many bytes of UTF-8 JSON (max 64 KiB).

**Introduce yourself** (minimum manifest):

```json
{ "type": "hello", "protocolMin": 1, "protocolMax": 1,
  "provider": { "id": "ext.acme.share", "name": "Acme Share",
                "capabilities": ["image", "video"] } }
```

GameHQ answers `hello.ack` with the protocol it selected and the capabilities it
accepted. The `id` must start with `ext.`; you need at least one of `image` /
`video`.

**GameHQ asks for targets** (a search box in the UI sends `query`; you only get
the text if you declared `target_search`):

```json
{ "type": "targets.request", "requestId": "q1", "query": "arch", "mediaKind": "image" }
{ "type": "targets.result",  "requestId": "q1",
  "targets": [ { "id": "archive", "name": "Archive", "kind": "external" } ] }
```

**The user shares** a capture to one of your targets. The job carries the
capture's path and a one-time token; echo both back:

```json
{ "type": "job.start", "jobId": "5c0f", "token": "a3b1", "targetId": "archive",
  "file": { "path": "C:\\Users\\me\\Pictures\\shot.png", "fileName": "shot.png",
            "sizeBytes": 182345, "mimeType": "image/png", "mediaKind": "image" } }
{ "type": "job.progress", "jobId": "5c0f", "token": "a3b1", "state": "transferring", "progress": 0.5 }
{ "type": "job.result",   "jobId": "5c0f", "token": "a3b1", "outcome": "copied" }
```

**Cancellation.** GameHQ may send `job.cancel` (same `jobId` and `token`) at
any time, and the sample checks between chunks. Stop, clean up, and answer with
`job.result` (`cancelled` if nothing left the PC, `unconfirmed` if it might
have). The sample deletes its half-written copy.

**Leave** with `{ "type": "goodbye" }` or by closing the pipe.

### Reading from the pipe (Windows gotcha)

A pipe opened the ordinary way serialises operations on the handle: a thread
blocked in `read` also blocks your `write`, so you could not send progress or
notice a cancel. The sample avoids this by polling `PeekNamedPipe` and only
reading when bytes are waiting. Use overlapped I/O or a separate reader if you
prefer, but never block a read on the same handle you write to.

## 4. Security and privacy expectations

* **This is not an operating-system sandbox.** Your process runs as the same
  Windows user as GameHQ. What the protocol guarantees is what GameHQ does: it
  hands you exactly one file path, the capture the user chose, inside that
  job's `job.start`, and never offers a way to ask for another.
* **Treat the path as that one file, nothing more.** Open it read-only. Do not
  list, glob or walk the folder it sits in, and do not keep it for later
  (captures move and get deleted). The sample never enumerates a directory, and
  the conformance suite fails if it starts to.
* **Be honest about the outcome.** `sent` only with confirmation and only if you
  declared `direct_send`; otherwise `handed_off`, `copied`, `failed` or
  `unconfirmed`. Never retry after an ambiguous failure: the user may get it
  twice.
* **Keep secrets out of `errorCode` and `detail`;** they are shown to the user.
* **Disclose what you do.** `provider.access` (`none`, `share_token`,
  `full_account`) and `provider.privacy` are shown in Share, after GameHQ's own
  fixed note that this is an add-on from another program. An undeclared `access`
  is shown as the widest.

## 5. Compatibility and versioning

* Send the range you support in `protocolMin`/`protocolMax`. GameHQ picks the
  highest version both sides speak (today: 1). No overlap gets
  `error: protocol_incompatible` and a disconnect: tell your user which side
  needs updating instead of retrying in a loop.
* **Ignore what you do not understand:** unknown fields in messages from GameHQ,
  unknown message types, and capabilities GameHQ lists in `ignoredCapabilities`.
  GameHQ does the same for you: unknown fields and capabilities in your
  messages are ignored, never fatal.
* Read limits (`maxFrameBytes`, `maxTargets`, `jobTimeoutMs`) from
  `hello.ack.limits` instead of hard-coding them.
* **Stable vs experimental** is spelled out in the *Stability* section of the
  specification. Only what it lists as stable is promised.
* **After a rejection GameHQ keeps the pipe open for a moment** so you can read
  the final `error`; read until the pipe closes.
* Reconnect with backoff if GameHQ restarts; a dropped connection is normal,
  not a crash.

## 6. Conformance suite

`tests/tst_shareexternalconformance.cpp` launches `sample_provider.py` as a
**separate process** and talks to GameHQ's real provider host over the real
local pipe, so it exercises what an in-process peer cannot:

* start-up and connect timing, the handshake window, and a late `hello`;
* the full lifecycle: register, list, search, share, progress, result;
* cancellation crossing the process boundary, and the provider's clean-up;
* a clean exit, a crash mid-job (`unconfirmed`) and reconnecting after GameHQ
  restarts;
* frames split into 1-7 byte writes, two frames in one write, and a frame that
  spans many OS reads;
* the version and forward-compatibility matrix: v1, a range that includes v1, a
  newer overlapping range, only-newer, an invalid range, unknown capabilities,
  unknown optional fields and an unknown message type;
* that the sample never enumerates the capture's folder.

Run it from a GameHQ test build (see [`docs/dev-setup.md`](../../docs/dev-setup.md)):

```
ctest -R shareexternalconformance --output-on-failure
```

It needs Python 3 on `PATH` (or set `GAMEHQ_SHARE_PYTHON` to the interpreter)
and skips itself, rather than failing, if there is none. The in-process protocol
tests are `tst_shareexternalprovider`.

To check **your own provider**, run it against a GameHQ with add-ons enabled and
walk the checklist at the end of the specification; the sample's flags marked
"test" show how each failure path (crash, silence, bad ranges) is driven.
