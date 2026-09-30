<!-- SPDX-License-Identifier: MIT -->

# Share smoke check (current beta, about 10 minutes)

The short owner check for the current beta. The comprehensive
provider/surface/failure/privacy matrix is
[share-acceptance-full-rc.md](share-acceptance-full-rc.md) and is run once, on the
final 0.7.9 release candidate, not on every beta.

Nothing here is pre-filled as passing.

## Build

| | |
|---|---|
| Code commit | `f644065` on `dev` (later commits only touch `docs/`) |
| Build | `out\GameHQ.exe` SHA-256 `8f6329df31ceace31f9d98f8445343e5a9f23375432521bbc2a0627075cbf780` |
| Run | `start.bat` (stops any running GameHQ, including the installed copy) |

Have one saved screenshot in the gallery. Telegram Desktop and Discord Desktop
must be installed for rows 5 and 6. Row 7 needs a Discord channel webhook added
under **Settings > Sharing**; skip it if you have none.

## Checks

Result for each: Pass / Fail / Not run.

| # | Do | Expect |
|---|---|---|
| 1 | Open Settings and look at the left menu. | **Sharing** is its own entry between Notifications & sound and Advanced; it opens a Sharing page. |
| 2 | Open **Capture**. | No Share settings there (no destinations, no add-ons, no Share switches). |
| 3 | On **Sharing**, switch each method off one at a time (Telegram, Discord, Discord channel, Copy to clipboard), then open Share on a screenshot. | Only the switched-off method is missing from the chooser; switching it on brings it back. |
| 4 | Turn **Discord channel** off and on again. Also turn **Enable Sharing** off and on. | Your saved channels are still listed and nothing had to be re-entered. |
| 5 | Share one screenshot to **Telegram**. | "Opened in Telegram. Finish sending there." (**Handed off**, never "Sent."); Telegram shows its chooser for that file. |
| 6 | Share one screenshot to **Discord**. | "Copied and opened Discord. Paste it into a chat to send it." (**Handed off**); Ctrl+V pastes the image. |
| 7 | Share one screenshot to a **Discord channel**. | "Sent." and the image appears in the channel as the webhook. |
| 8 | With the **controller only** (gallery or overlay): Square on a capture, choose Share, pick a destination with D-pad and Cross, Circle to back out. | The whole flow works without mouse or keyboard; nothing underneath reacts while Share is open. |

## Receipt

```
Build/hash checked:
1:  2:  3:  4:  5:  6:  7:  8:
Anything unexpected (what you pressed, what happened):
```

A Fail is a bug and is fixed under the matching Share item. Sent may only appear
for a Discord channel; Telegram and Discord Desktop must say Handed off.
