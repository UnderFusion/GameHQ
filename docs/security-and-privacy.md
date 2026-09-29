# Security & Privacy

GameHQ is an open-source, local-first Windows application. It requires no
account and contains no advertising, analytics, crash-reporting SDK, or usage
telemetry. It does not inject into game processes, install a Windows service,
create a scheduled task, add Defender exclusions, or disable Windows security
features.

## Local data

Portable mode stores configuration, the SQLite library, thumbnails, game icons,
logs, replay cache, sound packs, screenshots, and clips beside GameHQ. Installed
mode stores application state in the current user's standard AppData location
and new media in `Videos\GameHQ`. Watched folders and historical capture roots
remain where the user selected them. Uninstall removes program integration but
does not remove AppData, media, capture history, or portable copies.

Logs can contain executable names, game/window titles, device information and
local paths needed for diagnostics. Review a log before sharing it publicly.
GameHQ does not upload logs automatically.

## Network connections

GameHQ contacts GitHub only for update discovery and user-approved release
downloads. Automatic update checks can be disabled in Settings and are bounded
to at most once per 24 hours. Project, issue, release, license, and security
buttons open their visible GitHub URLs. The optional Playnite plugin communicates
with GameHQ through the local `GameHQ.Local.v1` named pipe and does not send game
activity to GameHQ servers; there is no GameHQ server.

Share sends a capture only when you choose a destination for one specific
screenshot or clip. Desktop hand-offs (Telegram, Discord, clipboard) give the
file to an app already on your PC and never sign GameHQ in to a service. A
Discord channel destination uploads the capture directly to the webhook you
added, which posts as that webhook rather than as your Discord account; the
webhook link is stored only in Windows Credential Manager. Optional add-ons
from other programs are off by default and can only receive the capture you
explicitly share to them (see [Share Provider API v1](share-provider-api-v1.md)
for what that does and does not protect).

## Controller access and elevation

GameHQ reads Windows controller APIs. It does not inject into or modify games.
If HidHide is installed and hides a physical controller, Settings can offer an
explicit **Fix automatically** action. Only that user-initiated action relaunches
the GameHQ executable with administrator permission to add its path to HidHide's
application allow-list. Setup itself remains per-user and does not elevate for
HidHide.

## Downloads and Windows warnings

Current Beta artifacts may not yet be Authenticode-signed, so Windows can show
an **Unknown publisher** or SmartScreen warning. That reputation warning is not
the same as a Microsoft Defender malware or potentially unwanted application
detection. Never disable Defender or SmartScreen for GameHQ. Use only the
official Releases page and follow [Download verification](download-verification.md).

If Defender reports a named detection, stop and do not choose **Run anyway**.
Preserve the exact artifact and hash, then report the result through the private
security channel. Publication is paused while a reproducible detection is
investigated and, when appropriate, submitted to Microsoft as an incorrect
detection.

## Reporting

Security vulnerabilities must be reported through the repository's enabled
[private vulnerability-reporting form](https://github.com/underfusion/GameHQ/security/advisories/new).
See [SECURITY.md](../SECURITY.md) for the response policy.

The complete endpoint and consent inventory is maintained in
[Privacy and network data flows](privacy-data-flow.md). Maintainer identity
handling for signing is documented separately in
[Maintainer identity privacy](maintainer-privacy.md).
