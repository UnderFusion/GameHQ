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

### Discord Desktop (`discord.desktop`, t14)

`src/share/providers/DiscordDesktopProvider.{h,cpp}`, tests
`tests/tst_sharediscorddesktop.cpp`. No Discord login, bot, webhook or user
token in GameHQ (`access: none`). Discord has no documented "send this file"
launch option and `discord://` cannot carry a file, so the hand-off does not
depend on it.

- **Detection:** the registered `discord` URL handler (`HKCU`, then `HKLM`
  `Software\Classes\discord\shell\open\command`), else the default per-user
  `Update.exe`. Only an existing `Update.exe` / `Discord*.exe` counts;
  otherwise the destination is listed as unavailable. PTB/Canary are used only
  if they own the `discord` handler.
- **Hand-off:** the capture is put on the clipboard first (file, plus image
  data for a screenshot, via `ClipboardShareProvider::copyCapture`), then
  Discord is started or brought forward with `Update.exe --processStart
  Discord.exe` (the URL placeholder is dropped). The user pastes into a chat.
- **Result:** `handed_off` with detail `paste` when Discord was opened
  ("Copied and opened Discord. Paste it into a chat to send it."); `copied` /
  `launch_failed` when only the clipboard step worked; `failed` /
  `clipboard_unavailable` when nothing was copied (Discord is then not
  opened). Never `sent`. In the overlay a hand-off hides the overlay so
  Discord is visible.
- **Manual check pending (owner):** PNG, JPG and MP4 paste into a Discord chat
  with Discord running and closed; large clips are subject to the account's
  upload limit.

### Discord channel (`discord.webhook`, t17)

`src/share/providers/DiscordWebhook{Provider,Store,Upload}.{h,cpp}`, tests
`tests/tst_sharediscordwebhook.cpp`. The user adds named channels (incoming
webhooks) in Settings > Sharing; Share then uploads the
capture natively (multipart file, no link, no third-party host). The post
appears under the **webhook's identity, not the user's personal Discord
account**, and the privacy notice says so. `access: share_token`: a webhook can
post to its one channel and nothing else.

- **Storage:** the webhook URL is a credential and lives only in Windows
  Credential Manager (`GameHQ.Share/discord.webhook/dest-<id>`). The metadata
  file `gamehq-data/share/discord-webhooks.json` holds ids, names and a
  last-used time and a pinned flag only. Up to 20 channels. Order: pinned first,
  then the rest; inside each group the most recently used first, and channels
  never used keep the order they were added in. Renaming changes only the
  label (id, secret, pin and position stay). Disconnect removes every channel
  and secret. The URL is validated (https, a Discord
  host, `/api[/vN]/webhooks/<id>/<token>`, no query/port/userinfo) before it is
  stored.
- **Settings UI:** generic. `Service::savedDestinationProviders()` /
  `addSavedDestination()` / `removeSavedDestination()` and the `Provider`
  `savedDestinations()` hooks serve any provider with `SavedTargets`; the
  section is hidden when none exists. Each channel has Pin/Unpin and Remove;
  rename is available on the service/provider API
  (`renameSavedDestination`) but has no Settings control yet. The link field is masked and cleared
  after saving. The Share dialog needs no Discord-specific code; with no
  channel configured the destination shows as unavailable ("Add a Discord
  channel in Settings first.").
- **Upload:** `POST <webhook>?wait=true`, multipart `payload_json`
  (`allowed_mentions.parse = []`, one attachment) plus `files[0]`, streamed in
  64 KiB chunks so a clip never sits in memory and the UI thread never blocks.
  `DiscordWebhookUpload` is a small single-shot HTTP client on `QSslSocket`
  instead of `QNetworkAccessManager`, because Qt silently re-sends a POST whose
  connection drops before the answer (seen in testing: two requests), which
  could post a capture twice. It never retries, never follows redirects, does
  not use an ambient proxy, and does not ignore certificate errors.
- **Outcomes:** `sent` only for a 2xx whose body carries the created message
  id. `failed`: 401/403/404 `webhook_revoked`, 429 `rate_limited`, 413 or
  Discord code 40005 `too_large`, other 4xx `rejected`, connect/TLS errors
  `network_error`. `unconfirmed`: 5xx `server_error`, a 2xx without a message id
  `unexpected_response`, a dropped connection or a cancel after the whole body
  left (`network_error` / `cancelled_late`). `cancelled` only when the upload
  was still incomplete. Nothing is retried.
- **Secrecy:** the URL never appears in logs, error codes, results or the
  metadata file (asserted by the test).
- **Size:** GameHQ enforces no upload size limit of its own. The file is
  streamed in 64 KiB chunks (no memory reason for a cap), and Discord's limit
  is not in the webhook reference and varies with the server, so Discord's
  413 / code 40005 answer (`too_large`) is authoritative.
- **Not done:** the OAuth `webhook.incoming` setup flow (needs a backend to
  keep the client secret) and a rename control in Settings. **Manual check pending (owner):** send a screenshot and a clip to a
  real channel over real TLS from the overlay.

### Telegram account (`telegram.integrated`, t13/t22-t24)

Optional. `share::TelegramIntegratedProvider` sends the picked capture natively
through the user's own Telegram account using TDLib (`docs/tdlib-runtime.md`
for the pinned runtime). Layers:

- `telegram::TdRuntime` locates, hashes and lazily loads the pinned
  `tdjson.dll`; missing or wrong runtime means the provider is unavailable
  with a plain reason, never a crash. Telegram Desktop sharing is independent
  of all of this.
- `telegram::TdTransport` is the seam to TDLib (real `TdJsonTransport`, or a
  scripted fake in tests). `telegram::Account` is the authorization state
  machine (`disconnected -> starting -> wait_phone -> wait_code ->
  [wait_password] -> connected`), the credentials (`api-id`, `api-hash`) and a
  random database key in Credential Manager, and the confined session
  directory `<data>/share/sessions/telegram.integrated`.
  The phone number, code and password go straight to TDLib and are never
  stored or logged. Unsupported steps (e-mail login, QR, registration) end as
  `unsupported_auth`.
- The client is released after five idle minutes (no background traffic); the
  saved session resumes on the next Share. `Disconnect` logs out on Telegram's
  side and deletes the local session and database key.
- TDLib is configured with no message database and no file cache, `online` set
  to false, and every update that is not authorization or the answer to a
  request GameHQ made is dropped: there is no inbox, incoming-message UI or
  Telegram notification path.
- Targets: private chats and non-broadcast groups (Telegram's own recency
  order; search merges chats and contacts). Secret chats, bots and channels
  are not offered; groups without photo/video permission are hidden. Ids are
  `c:<chat>` / `u:<user>`, validated again at send time.
- Sending: a screenshot goes as a photo (a PNG over 10 MB as a document), a
  clip as a video (over 2000 MB refused). **Sent is reported only after TDLib's
  `updateMessageSendSucceeded` for that message.** A lost connection ends as
  `unconfirmed`; nothing is ever retried. Cancel deletes the still-pending
  message.

Tests: `tst_tdruntime`, `tst_telegramaccount`, `tst_telegramprovider`.

### External providers (`ext.*`, t18)

Out-of-process providers register over a dedicated same-user pipe
(`GameHQ.Share.Provider.v1`) and appear in Share like any built-in one. The
whole contract, trust model and honest limits (it is not an OS sandbox) are in
[share-provider-api-v1.md](share-provider-api-v1.md); a runnable sample and walkthrough
are in `integrations/share-provider/`. Code: `src/share/external/`
(protocol codec, `ExternalProvider`, `ExternalProviderHost`). Opt-in through
`share.external_providers` (off by default, read at startup); GameHQ never
launches or discovers providers.

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
