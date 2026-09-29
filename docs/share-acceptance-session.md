<!-- SPDX-License-Identifier: MIT -->

# Share acceptance session (owner)

One manual session that closes the pending Share checks together: Settings >
Sharing (t21), Telegram Desktop (t12), Discord
Desktop (t14), Discord Channels (t17), add-ons (t18/t19) and the end-to-end
matrix (t20). It replaces the earlier session written before Settings > Sharing
existed. About 45 minutes. It launches GameHQ, Telegram and
Discord windows, so pick a time you are not in the middle of something.

Nothing here is pre-filled as passing. Automated tests cover the protocol and
result logic; this session covers what only a real client, real network, real
controller and real screen can show.

## 0. What you are testing

| | |
|---|---|
| Code commit | **`4dae02b`** on `dev` (`feat(share): hide deferred providers from the Sharing UI`). Later commits that only touch `docs/` do not change the app. |
| Version | `0.7.9` (`VERSION`) |
| Build | Debug build from `out\`. `out\GameHQ.exe` SHA-256 = `e866e8efcea4997eadccc8381cb5053feaf9253454a007a33ea368c8cd5cdf51` (`out\GameHQLauncher.exe` = `787e2e7b1b5f870cb956974db122c8d017812d951471904de7f8ab96f010642f`) |
| How to run | `start.bat` in the repo root: rebuilds `out\`, assembles a clean package in `build\`, keeps your data, and launches `build\GameHQ.exe`. **It stops any running `GameHQ.exe` first, including the installed copy.** |
| Data / log | `build\gamehq-data\` (separate from the installed copy). Log: `build\gamehq-data\logs\gamehq.log` |

Confirm the identity before you start:

```powershell
Get-FileHash build\GameHQ.exe -Algorithm SHA256   # should equal the out\GameHQ.exe hash above
```

Scope: only the providers a user can see in this build: Telegram Desktop, Discord
Desktop, Discord channels, Share add-ons and Copy to clipboard.

## 1. Destinations and what each may report

| Destination | Where it comes from | Configure | May report | UI text |
|---|---|---|---|---|
| Copy to clipboard | built in | none | **Copied** | "Copied. Paste it wherever you want to share it." |
| Telegram | Telegram Desktop on this PC | none (Telegram must be installed) | **Handed off** | "Opened in Telegram. Finish sending there." |
| Discord (Desktop) | Discord Desktop on this PC | none (Discord must be installed) | **Handed off** | "Copied and opened Discord. Paste it into a chat to send it." |
| Discord channel | webhook you add | Settings > Sharing > Share destinations | **Sent** (only after Discord returns the created message) | "Sent." |
| Sample folder | add-on process (optional) | Settings > Sharing > Share add-ons, then run the sample | **Copied** | "Copied. Paste it wherever ..." |

Rules to hold GameHQ to throughout:

* Only the Discord channel may ever say **Sent**. Telegram and Discord Desktop must say **Handed off**; the clipboard
  and the sample must say **Copied**. Any "Sent." from those is a **fail**.
* An uncertain end must say "GameHQ couldn't confirm whether it was sent. Check
  before sharing again." and must never be retried automatically.
* Cancel ends as "Sharing cancelled. Nothing was sent." (or the "couldn't
  confirm" message if the upload had already fully left).

## 2. Setup (once, about 10 minutes)

1. Run `start.bat`. Check the version on the About page is `0.7.9` and compare the
   hash (section 0).
2. Make test captures (any game, or the desktop, whichever your capture mode
   allows):
   * **PNG** screenshot (Settings > Capture > format `PNG`).
   * **JPG** screenshot (switch the format to `JPG`, take one, switch back).
   * **short MP4 clip** (a few seconds).
   * optional **large clip** larger than your Discord server's upload limit, to
     exercise the "too large" path.
3. Discord: make a private test channel, then *Channel settings > Integrations >
   Webhooks > New Webhook*, **Copy Webhook URL**. Make **two** webhooks (A and B)
   in that channel or two channels.
4. GameHQ: *Settings > Sharing*: add **A** ("Test A") and
   **B** ("Test B") by pasting the link (the field is masked) and pressing Save.
   Press **Pin** on B.
5. GameHQ: *Settings > Sharing > Share add-ons* > switch on **Allow add-ons from
   other programs**. Then **quit GameHQ from the tray and run `start.bat` again**
   (add-ons are read at start-up).
6. In a terminal, start the sample provider and leave it running:
   `python integrations\share-provider\sample_provider.py --dest-dir C:\Temp\ShareSample`
   (Python 3.8+, no packages). It should print `registered: protocol 1 ...`.
7. Have Telegram Desktop and Discord Desktop signed in and reachable. Keep one
   friend/private chat in each as the recipient.

## 3. How to open Share (all inputs)

| Where | Mouse | Controller |
|---|---|---|
| Desktop gallery | hover a capture, click the **Share** icon | focus the capture, **Square** opens the action menu, **Share** is the first entry |
| Lightbox (full screen) | **Share** button | **Square** |
| Overlay | Share icon on a strip tile | focus a capture in the strip, **Square** opens the menu, **Share** first; in the full-screen viewer **Square** opens Share directly |

In the Share dialog: **D-pad up/down** moves, **Cross** picks, **Circle** steps
back (target list > destinations > closed) and, while "Sharing..." is showing,
cancels. While Share is open nothing underneath may react to the pad or the
keyboard.

## 4. Matrix

Mark each row Pass / Fail / Not run and note anything odd. "Dialog text" means
the message in the Share dialog.

### 4.0 Settings > Sharing (closes t21)

| ID | Do | Expect |
|---|---|---|
| S1 | Open Settings and look for **Sharing** between Capture and Replay. Nothing Share-related is left under Capture. | A Sharing page with Enable Sharing, a Destinations list (Telegram, Discord, Discord channel), channel destinations and Share add-ons. |
| S2 | Switch **Discord** off, open Share on a capture. | Discord is not offered. Switch it back on: it returns. |
| S3 | Add channel **A** (section 2), switch **Discord channel** off, share, switch back on. | While off it is hidden; when on again channel **A** is still there and still works without re-adding it (disabling never deletes destinations). |
| S4 | Switch **Enable Sharing** off, then try Share from the gallery, lightbox and overlay. | Each says "Sharing is turned off. Turn it on in Settings > Sharing." Switch it on: Share works again. |
| S5 | Restart GameHQ with a provider switched off. | It stays off (the switches persist). |
| S6 | Settings > Advanced > Restore all settings. | Sharing and all providers return to on. |

### 4.1 Clipboard (control)

| ID | Do | Expect |
|---|---|---|
| C1 | Gallery, mouse: share the PNG to *Copy to clipboard*. Paste into Paint / a chat. | Dialog says **Copied**; the image pastes. |

### 4.2 Telegram Desktop (closes t12)

| ID | Do | Expect |
|---|---|---|
| T1 | Gallery, **mouse**, Telegram **running**: share the **PNG** to Telegram. | Dialog **Handed off**; Telegram comes forward with its own "choose a chat" box for that file. |
| T2 | Lightbox, **controller**, Telegram running: share the **MP4**. | Same. |
| T3 | Overlay, **controller**, Telegram **closed**: share the **JPG**. | Overlay steps aside; Telegram starts and shows the chooser. |
| T4 | In each case pick your test chat and send. | Recipient receives **exactly the chosen file** (same type and size), no extra files, nothing sent before you pick. |

### 4.3 Discord Desktop (closes t14)

| ID | Do | Expect |
|---|---|---|
| D1 | Gallery, **mouse**, Discord **running**: share the **PNG** to Discord. | Dialog "Copied and opened Discord. Paste it into a chat to send it."; Discord comes forward. **Ctrl+V** in a chat pastes the image; you send it. |
| D2 | Lightbox, **controller**, Discord running: share the **MP4**. | Same; the clip pastes as an attachment. |
| D3 | Overlay, **controller**, Discord **closed**: share the **JPG**. | **The message stays on screen** until you press **Cross** (Done); then the overlay steps aside and Discord is visible. Ctrl+V pastes the file. (This is the fix in `598a7fa`; if the overlay vanishes before you can read the message, that is a **fail**.) |
| D4 | Any of the above. | The result is **never** "Sent."; GameHQ never asked you to sign in to Discord. |

### 4.4 Discord channel webhook (closes t17)

| ID | Do | Expect |
|---|---|---|
| W1 | Gallery, **mouse**: share the **PNG** to *Discord channel > Test A*. | "Sharing..." then **Sent.**; the post appears in the channel **as the webhook** (not as your account) with the image attached. |
| W2 | Lightbox, **controller**: share the **MP4** to *Test B*. | **Sent.**; the clip is attached and plays. |
| W3 | Overlay, **controller**: share the **JPG**; look at the destination list. | **Test B (pinned) is listed first**, then A. **Sent.** |
| W4 | Settings > Share destinations: **Unpin** B, share again. | Order follows recent use; pinning survives a restart of GameHQ. |
| W5 | Share the **same capture to the same channel again**. | "You already shared this capture here. Share it again?" with **Cancel** preselected; Cross must not resend. |
| W6 | Before sending, read the line under *Discord channel*. | It says it posts as a webhook, not as your Discord account, and that everyone in the channel can see it. |

### 4.5 Add-on sample (t18/t19, optional but quick)

| ID | Do | Expect |
|---|---|---|
| A1 | Share the PNG to *Sample folder > Archive* (try searching "arch"). | Dialog **Copied**; the file is in `C:\Temp\ShareSample\archive\`. The line under the destination says it is an **add-on from another program**. |
| A2 | Close the sample (Ctrl+C). | *Sample folder* disappears from Share. |
| A3 | With add-ons **off** (switch off, restart) start the sample. | It exits with code 4 ("are Share add-ons enabled?"). |

### 4.6 Cross-cutting

| ID | Do | Expect |
|---|---|---|
| X1 | Open Share on capture **A** in the gallery (mouse). While the dialog is open take **another** capture (so the newest item changes). Finish the share. | The file shared is **A**, not the new one. |
| X2 | With Share open, press pad directions, Square, Triangle, Delete and gallery shortcuts. | Nothing underneath reacts; Circle steps back one level at a time. |
| X3 | After each hand-off (T*, D*) close Telegram/Discord. | Focus returns somewhere sensible; GameHQ is not stuck behind a hidden modal. |
| X4 | Throughout. | No Telegram/Discord inbox, message list or notification is ever shown by GameHQ. |

## 5. Failure paths

| ID | Do | Expect |
|---|---|---|
| F1 | **Offline:** disconnect the network, share the PNG to a Discord channel. | "GameHQ couldn't reach the service. Check your connection." Failed, not "Sent."; after reconnecting **nothing is posted** by itself. |
| F2 | **Revoked webhook:** delete webhook A in Discord, then share to A. | "That channel's webhook no longer works. Remove it in Settings and add it again."; GameHQ keeps working. |
| F3 | **Oversized:** share the large clip to a channel (skip if you have none). | Either **Sent.** (the server allows it) or "This capture is too large for that channel."; never a false success. |
| F4 | **Cancel:** share the large (or any) clip to a channel and press **Circle** while "Sharing..." shows. | "Sharing cancelled. Nothing was sent." or, if it had already fully uploaded, the "couldn't confirm" message. Check the channel: what you were told matches what arrived. |
| F5 | **Changed/missing source:** open Share on a capture (mouse), delete or rename that file in Explorer, then pick a destination. | Nothing is sent; a message says the capture changed or is no longer on disk. |
| F6 | **Rate limit** (only if you hit it): several quick uploads. | "Discord is limiting uploads right now. Wait a moment and try again."; no automatic retry. |
| F7 | **Restart:** quit GameHQ from the tray right after starting an upload, start it again. | It does not resend anything on its own. |
| F8 | **Add-on dies:** while the sample is copying the large clip, kill the sample. | The job ends with "couldn't confirm"; GameHQ keeps running; the sample destination disappears. |

## 6. Privacy and secrets (about 5 minutes)

Replace `TOKEN` with the first 12 characters after the last `/` of your webhook link.

```powershell
# 1. Nothing sensitive in the log (expect no output from these two):
Select-String -Path build\gamehq-data\logs\gamehq.log -Pattern 'TOKEN'
Select-String -Path build\gamehq-data\logs\gamehq.log -Pattern 'api/webhooks'
# 2. What the log does say about Share (provider, outcome, code only):
Select-String -Path build\gamehq-data\logs\gamehq.log -Pattern 'Share:' | Select-Object -Last 40
# 3. The metadata file holds names only, never a link (expect no match):
Select-String -Path build\gamehq-data\share\discord-webhooks.json -Pattern 'discord|http|TOKEN'
Select-String -Path build\gamehq-data\config.json -Pattern 'webhooks|TOKEN'
# 4. The link lives only in Windows Credential Manager:
cmdkey /list | Select-String 'GameHQ.Share'
```

| ID | Expect |
|---|---|
| P1 | Checks 1 and 3 print nothing; check 4 lists `GameHQ.Share/discord.webhook/dest-...` entries, one per saved channel. |
| P2 | After you **Remove** a channel in Settings, its Credential Manager entry is gone. |
| P3 | Check 2 shows lines with a provider, an outcome and a short code only: no URLs, tokens, file paths or message text. |

## 7. Receipt

Copy this into your reply (or a file). One line per row you ran; unlisted rows count as *Not run*.

```
Session date:            
Code commit / exe SHA-256 checked:   4dae02b / <first 16 chars of the hash you computed>
Windows build:           
Controller model:        
Telegram Desktop version:            Discord Desktop version:

ID   Result (Pass/Fail/Not run)   Capture   Destination   Observed result text   Notes
C1
S1
T1
...
F1
P1

Anything unexpected (screenshots or a log excerpt help):
```

For a Fail, add the last 60 lines of `Select-String -Pattern 'Share:'` from the
log. **A Fail is a bug** and will be fixed under the matching Share item; a
Pass is recorded against the gate named below.

## 8. What each result closes

| Rows | Closes |
|---|---|
| S1-S6 | t21 (Settings > Sharing and per-provider enablement) |
| T1-T4 | t12 (Telegram Desktop hand-off: PNG/JPG/MP4, running and closed) |
| D1-D4 | t14 (Discord Desktop hand-off: image and clip, no GameHQ login) |
| W1-W6, F1-F4 | t17 (real channel over real TLS from the overlay: screenshot and clip) |
| A1-A3, F8 | confirms t18/t19 outside the automated suite |
| C1, X1-X4, F5-F7, P1-P3 | t20 (controller/mouse navigation, focus, privacy and resilience) |

