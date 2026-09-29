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

## Security and privacy

Source: `src/share/ShareSecurity.{h,cpp}`. Tests: `tests/tst_sharesecurity.cpp`.

- **Secrets never live in `config.json`.** Tokens, webhook URLs and session
  keys go to `share::SecretStore`: generic credentials in the Windows
  Credential Manager, DPAPI-protected, `CRED_PERSIST_LOCAL_MACHINE` (this user
  on this PC, never roaming), named `GameHQ.Share/<providerId>/<name>`, at most
  2560 bytes. Invalid ids/names and oversized values are refused, never
  truncated.
- **Local session data** (e.g. an account library's database) lives only in
  `share::SessionStorage`: one directory per provider under a fixed root.
  `removeAll(id)` deletes exactly that directory, refuses invalid ids and never
  follows a link out of the root.
- **Disconnect** (`shareService.disconnectProvider(id)` →
  `Provider::disconnectAccount()`) must log the account out and remove that
  provider's secrets and session directory. Refused while its job runs.
- **Redaction.** `share::redactSecrets()` strips Discord webhook tokens (the id
  is kept), Telegram bot tokens, Bearer/Bot/Basic values, secret query/form
  parameters, `"token": …`-style fields and 40+ character opaque runs.
  Providers run it over anything they log or show that came from a server or
  from user configuration; the service runs it over every result `detail`.
- **Access scope** is declared per provider (`accountAccess()`) and shown with
  a plain-language `privacyNotice()` next to the destination:
  `none` (hand-off, clipboard: no account in GameHQ), `share_token` (can only
  post to one destination, e.g. a channel webhook), `full_account_session` (a
  real signed-in session such as Telegram via TDLib: technically able to do
  everything the account can, even though GameHQ only sends the capture the
  user picked, with no inbox, notifications or chat features).
- **Least data.** A provider receives one frozen `Request` for the capture the
  user explicitly picked and nothing else: no library, no other files, no
  game list.
- **No automatic publishing.** Every send starts from an explicit user
  confirmation in the Share dialog; nothing is shared in the background and a
  resend of the same capture to the same target needs a second confirmation.
- **No blind retries.** Neither the service nor a provider retries after an
  ambiguous failure; the outcome is `unconfirmed` and the user decides.

## QML surface (`shareService`)

- `open(filePath, gameName)` / `close()`
- `providers()` → `[{ id, name, icon, available, availability, reason, auth, capabilities, access, privacy }]`
- `requestTargets(providerId, query)` → then `targets()` → `[{ id, providerId, kind, name, subtitle }]`
- `share(providerId, targetId, confirmResend)` → job id or `""` with `lastError`
- `cancel()`
- `disconnectProvider(providerId)`
- properties: `active`, `fileName`, `mediaKind`, `phase` (`idle|choosing|sending|finished`), `busy`, `lastError`, `lastResult`, `targetsProviderId`, `targetsLoading`
- signal `finished(result)`

## UI

`src/ui/qml/components/ShareDialog.qml` is the one Share flow. Each window
hosts an instance, because the desktop `Lightbox` is its own full-screen window
and would cover the main window's dialog: `Main.qml` (`shareDialog`),
`Lightbox.qml` (`lightboxShare`, exposed as `lightbox.shareDialog`) and
`OverlayWindow.qml` (`overlayShare`).

Entry points:

- **Square** on a gallery tile or overlay strip capture opens the action menu;
  its entries are `{ id, label }` (`share`, `show_in_folder`, `delete`, plus
  `bulk_select` on the desktop) and the host runs `runMenuAction(id)`.
- **Square** in the desktop lightbox or the overlay's full-screen viewer opens
  Share for the shown capture directly (neither has a menu of its own).
- **Mouse:** a Share icon on every capture tile (desktop and overlay strip) and
  a Share pill next to the lightbox's close button.

Pad model (modal): Up/Down move the highlight (clamped, unavailable providers
are skipped), Cross activates, Circle steps back (recipients → destinations →
closed). While a job runs, Circle asks the service to cancel and the dialog
stays until the result arrives. After a Sent/Unconfirmed result, sharing the
same capture to the same recipient shows a confirmation that lands on Cancel.
A provider whose only target is its external app (desktop hand-off) goes
straight from the destination to the hand-off. Nothing underneath acts while
the dialog is open: `Main.qml` routes every desktop pad signal through
`activeShareDialog()`, `DesktopGalleryGrid.inputBlocked` stops the grid's own
shortcuts, and the overlay handlers check `overlayShare.isOpen` first. The
dialog tracks an explicit `isOpen` state rather than item visibility. Hiding
the overlay closes an idle dialog; a running send keeps going and its result
is shown on the next open.

## Providers

### Telegram Desktop (`telegram.desktop`, t12)

`src/share/providers/TelegramDesktopProvider.{h,cpp}`, tests
`tests/tst_sharetelegramdesktop.cpp`. No Telegram login in GameHQ
(`access: none`).

- **Detection:** the registered `tg` URL handler
  (`HKCU`, then `HKLM` `Software\Classes\tg\shell\open\command`), else
  `%APPDATA%\Telegram Desktop\Telegram.exe`. Only an existing `Telegram.exe`
  counts; otherwise the destination is listed as unavailable ("Telegram Desktop
  isn't installed."). The Microsoft Store build does not register a classic
  handler and is not detected.
- **Hand-off:** `Telegram.exe [-workdir <dir>] -sendpath <capture>`. The
  `-workdir` pair is copied from the registered handler so a portable/custom
  profile gets the file; the `-- %1` URL placeholder is dropped. Telegram shows
  its own "choose a chat" box for that one file; the user picks and sends
  there. `AllowSetForegroundWindow(ASFW_ANY)` lets an already-running
  Telegram come to the front.
- **Result:** `handed_off` when the process starts, `failed`/`launch_failed`
  when it does not, never `sent`. In the overlay a hand-off closes the dialog
  and hides the overlay so Telegram's window is visible.

### Copy to clipboard (`clipboard`)

Built-in provider: `share::ClipboardShareProvider` (`clipboard`) puts the file
on the clipboard (plus image data for screenshots). Result: `copied`.

Tests: `tests/tst_sharedialog.cpp` drives the real `ShareDialog.qml` with pad
calls against the real service.

## Workstream

Plan items t9–t20: core (t9), controller-first Share UI with stable action ids
(t10), secrets/privacy (t11), Telegram Desktop hand-off (t12), Telegram TDLib
(t13), Discord Desktop hand-off (t14), Discord Social SDK feasibility (t15),
Discord integrated (t16), Discord server/channel webhooks (t17), external
out-of-process Provider API v1 (t18), SDK/sample/conformance (t19),
end-to-end acceptance (t20). Providers are never loaded as DLLs into GameHQ:
third-party providers will run out of process (t18).
