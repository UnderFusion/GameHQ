# Share Platform

Sharing a **saved** screenshot or clip with another app or person. This is not
the physical Share/Create button: that button still takes captures and its
meaning never changes.

One flow serves the desktop gallery, the lightbox and the overlay. Every
destination is a *provider* behind one contract, so adding Telegram, Discord,
a webhook, a NAS or a future out-of-process integration never needs
provider-specific QML.

Source: `src/share/`. Tests: `tests/tst_sharecore.cpp`.

## Model

| Type | Role |
| --- | --- |
| `share::Request` | One capture the user explicitly picked, frozen when Share opens: absolute path, MIME type, media kind, size, mtime. Only plain files (no symlinks/junctions) with a known image/video suffix. |
| `share::Target` | A destination inside one provider (contact, group, channel, external app, saved destination). `id` is opaque to everyone but its provider. |
| `share::Job` | One send attempt: request id + provider id + target id + state (`Pending`, `Preparing`, `Transferring`, `Finished`). |
| `share::Result` | How a job ended (see outcomes) plus a machine `errorCode` and a short user-safe `detail`. |
| `share::Provider` | Abstract provider: id, name, icon, capabilities, auth state, availability, `requestTargets()`, `start()`, `cancel()`, `jobTimeoutMs()`. |
| `share::ProviderRegistry` | Ordered list of providers; rejects invalid and duplicate ids; filters by media. |
| `share::Service` | The one QML-facing orchestrator (`shareService` context property). |

### Capabilities

`image`, `video`, `contacts`, `groups`, `channels`, `direct-send`,
`external-handoff`, `target-search`, `caption`, `requires-account`,
`saved-targets`. The UI decides what to show from these flags only. A target may
narrow media support further (a channel that takes images but not video).

### Outcomes

| Outcome | Meaning | Who may report it |
| --- | --- | --- |
| `sent` | The remote side confirmed delivery. | Only providers with `direct-send`; anything else is coerced to `handed_off`. |
| `handed_off` | Another app has the file; the user finishes the send there. | Desktop hand-off providers. |
| `copied` | Put on the clipboard or in a folder; nothing was sent. | Clipboard/folder style providers. |
| `failed` | Definitely not sent. | Any. |
| `cancelled` | Stopped before anything left GameHQ. | The service. |
| `unconfirmed` | Might have been delivered (timeout, provider lost mid-transfer). | The service, or a provider that lost its connection mid-upload. |

GameHQ never says "Sent" for a hand-off, and never turns an ambiguous end into
a failure the user would "fix" by sending again.

## Service guarantees

These hold for every provider, including future external ones:

1. **Right file.** The request is snapshotted at open. A job starts only if the
   file is still at the same path with the same size and mtime; otherwise
   `capture_changed`. The provider receives that one request only.
2. **Right recipient.** A target id is accepted only if it came from the
   provider's latest `targetsReady` answer for this request. Answers to a
   superseded query are dropped, and a provider cannot issue targets in another
   provider's name.
3. **No duplicate sends.** One job at a time (`busy`). After `sent` or
   `unconfirmed`, sending the same capture to the same target again returns
   `resend_needs_confirmation` until the UI passes explicit confirmation.
   Nothing is retried automatically.
4. **No false Sent.** See outcomes. A job with no result or progress for
   `jobTimeoutMs()` ends `unconfirmed` (`timeout`) and the provider is told to
   cancel. Progress resets that window.
5. **Cancel is honest.** Before transfer starts, cancel ends the job
   `cancelled` at once. Once data may be in flight, the service waits for the
   provider's own verdict (or the timeout).
6. **No secrets in results or logs.** Error codes must look like identifiers
   (`^[a-z][a-z0-9_]{0,47}$`); anything else, such as a URL or server text,
   becomes `provider_error` and its detail is dropped. Service logs carry the
   provider id, target *kind*, media kind, outcome and code only, never target
   names, paths of other files, tokens or webhook URLs.
7. **Gates.** Unavailable providers are listed (so the UI can say why) but
   cannot list targets or send. `requires-account` providers must be
   `Connected`.

## QML surface (`shareService`)

- `open(filePath, gameName)` / `close()`
- `providers()` → `[{ id, name, icon, available, availability, reason, auth, capabilities }]`
- `requestTargets(providerId, query)` → then `targets()` → `[{ id, providerId, kind, name, subtitle }]`
- `share(providerId, targetId, confirmResend)` → job id or `""` with `lastError`
- `cancel()`
- properties: `active`, `fileName`, `mediaKind`, `phase` (`idle|choosing|sending|finished`), `busy`, `lastError`, `lastResult`, `targetsProviderId`, `targetsLoading`
- signal `finished(result)`

## Workstream

Plan items t9–t20: core (t9), controller-first Share UI with stable action ids
(t10), secrets/privacy (t11), Telegram Desktop hand-off (t12), Telegram TDLib
(t13), Discord Desktop hand-off (t14), Discord Social SDK feasibility (t15),
Discord integrated (t16), Discord server/channel webhooks (t17), external
out-of-process Provider API v1 (t18), SDK/sample/conformance (t19),
end-to-end acceptance (t20). Providers are never loaded as DLLs into GameHQ:
third-party providers will run out of process (t18).
