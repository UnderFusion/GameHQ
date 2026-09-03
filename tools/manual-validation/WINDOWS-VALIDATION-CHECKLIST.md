# GameHQ Windows validation

Record the artifact SHA-256, Windows edition/build, Defender definition version,
SmartScreen setting, Smart App Control state, result, and screenshot filename.

## Automated or script-assisted mechanics

- Run `packaging/test-installer-regression.ps1 -SetupPath <setup.exe>` for the
  isolated fresh-install, upgrade, silent failure, recovery, locale-selection,
  and uninstall baseline before the remaining clean-VM observations.
- Install without elevation and into a custom directory.
- Confirm Start Menu/Desktop shortcuts and uninstall registry metadata.
- Upgrade an older Setup, reinstall, uninstall, and verify AppData/Captures remain.
- Confirm portable and installed copies coexist without sharing data.
- Confirm Setup refuses a running GameHQ or active updater transaction.
- Confirm Uninstall refuses the same two cases, not only a running GameHQ.
- Confirm a leftover `.update\maintenance.lock` with no updater running and an
  older timestamp reports recovery guidance instead of blocking forever, and
  that neither Setup nor Uninstall deletes that marker or `transaction.phase`.
- Confirm a `maintenance.lock` whose `transaction.phase` reads `healthy` or
  `rolled_back` blocks neither Setup nor Uninstall.
- Confirm Setup and Uninstall both refuse while GameHQ runs in a second Windows
  session of the same account (Fast User Switching), which the per-session mutex
  alone cannot see.
- Exercise offline update check, cached result, read-only path, locked file, and stale cleanup.
- Run `Start-MpScan -ScanType CustomScan -ScanPath <artifact-or-folder>` when Defender is available.

## Human clean-VM observations

- Launch Setup and Portable and record SmartScreen wording.
- Record Smart App Control audit/enforcement behavior where available.
- Record every Defender malware/PUA result; do not disable protections or add broad exclusions.
- After signing, verify publisher and RFC 3161 timestamp on launcher, app, updater, uninstaller, and Playnite DLL.
- Exercise visible install, upgrade, uninstall, rollback, HidHide elevation, and Playnite maintenance behavior.

Stop release promotion on any Defender detection or signature/hash mismatch.
