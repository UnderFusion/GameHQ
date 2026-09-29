# Privacy and network data-flow inventory

GameHQ is local-first. It has no account, telemetry, advertising, automatic
crash upload, media upload, or GameHQ-operated server.

## Current network and process boundaries

| Feature | Trigger and destination | Data sent | Local state / stop control |
|---|---|---|---|
| Update discovery | Automatic at most once per 24 hours when enabled, or manual; `api.github.com/repos/underfusion/GameHQ/releases` | GameHQ user-agent, normal HTTPS/IP metadata, and release-list request | Disable automatic checks in Settings; cached state remains local. |
| Update download | Explicit user action; HTTPS release asset URL returned by GitHub | Normal HTTPS/IP metadata and requested artifact path | Cancel in UI; staged downloads remain under the owned `.update/` root and are bounded/verified. |
| Project links | Explicit button; visible GitHub repository, releases, issues, license, security or policy URL | Browser performs the request under its own policy | No background request by GameHQ. |
| Playnite integration | Local plugin connection to `GameHQ.Local.v1` | Local game IDs, titles, executable paths and lifecycle state | Same-user local named pipe only; disabling the plugin stops messages. |
| Share to Discord channel | Explicit Share of one capture to a channel the user added; `discord.com` / `discordapp.com` webhook URL the user pasted | The chosen screenshot or clip as a multipart upload, GameHQ user-agent, normal HTTPS/IP metadata | Opt-in per channel (Settings > Capture > Share destinations); remove a channel or use Disconnect to delete the link from Windows Credential Manager. Nothing is sent unless the user shares. |
| Share hand-off (Telegram, Discord Desktop, clipboard) | Explicit Share; installed client on this PC | The selected capture path or file is passed to that local app | No network transfer by GameHQ; GameHQ never signs in to those services. |
| Share add-ons | Optional; other programs connect to `GameHQ.Share.Provider.v1` | Target lists they send; the path and details of a capture only when the user shares to that add-on | Off by default (Settings > Capture > Share add-ons, restart). Same-user local pipe only; see [share-provider-api-v1.md](share-provider-api-v1.md). |
| HidHide repair | Explicit elevated action | Executable path to the local HidHide command-line tool | No network transfer; user sees the elevation prompt. |

Logs may contain executable names, game/window titles, device information and
local paths. They remain local and must be reviewed and redacted before sharing.
Screenshots, clips, audio, transcripts, watched-folder media and library data
are never transferred by the current product.

## Gate for future cloud or AI features

Local processing is the preferred default. A networked or AI feature must be
off until the user explicitly opts in. Before its first transfer, the UI must
identify the provider, exact data categories, purpose, retention implications,
costs, privacy-policy link, and how to disable the feature. Consent to use a
feature is separate from consent to provider training or data improvement.

Disabling the feature stops future transfers and explains local cache deletion
and credential revocation. No feature may silently upload screenshots, clips,
audio, transcripts, prompts, library metadata, diagnostics, or logs.

API keys and refresh credentials belong in Windows Credential Manager or a
separately reviewed OS secret store, never plain-text `config.json`. Secrets,
signed URLs, prompt/media contents, sensitive provider responses, and avoidable
personal paths must be redacted from diagnostics, logs, crash reports, exports,
CI, and release artifacts.

Every new endpoint, SDK, model, voice, dataset, or prompt pack requires updates
to this inventory plus license, privacy, security, retention, and SignPath-scope
review before implementation can ship.
