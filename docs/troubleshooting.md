# Troubleshooting

## Windows says Unknown publisher or shows SmartScreen

Current Beta builds may be unsigned. Continue only when the file came from the
official GameHQ Releases page and every available version, filename, SHA-256,
manifest, or Authenticode check in
[Download verification](download-verification.md) matches. For a verified
unsigned Beta, Windows may offer **More info → Run anyway**.

Do not use that bypass when Microsoft Defender reports a specific malware or
potentially unwanted application detection. Do not disable Defender, add an
exclusion, or download another copy from a mirror. Preserve the artifact and
hash and submit a private report through
https://github.com/underfusion/GameHQ/security/advisories/new.

## Setup says GameHQ is running

Close GameHQ normally from its tray menu and let active capture or clip work
finish. Setup and Uninstall deliberately do not force-close the application.
For silent Setup automation, exit code `20` means the application mutex is
active; exit code `21` means an updater or recovery transaction is active.
Inno Uninstall reports only success (`0`) or a nonzero refusal/failure, so
automation must not assign a specific cause to its nonzero value.

## A controller is hidden by HidHide

Open Settings → Input and use the explicit HidHide repair action, or add the
installed `app\GameHQ.exe` to HidHide's application allow-list manually. This
is the only normal GameHQ action that requests administrator permission; Setup
itself remains per-user.
