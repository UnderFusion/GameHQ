# Local capture qualification receipt

The implementation starts at `37081a4de4c9f39bcca415d98eecc19398d9d110`, a direct child of `v0.8.2-beta3` at `71a56d46f819ad3f2d63d68d1b1a2a1b76c3dc8f`. The commit containing this receipt adds the qualification fixes. The Qt 6.11.3 Release executable has SHA-256 `D636347B4A2F1D559E5FE33B1DAFB4CB3154DEA56036C3A3B230C6AD308741CD`.

| Check | Result |
| --- | --- |
| 356×196 preview | Before the fix, the Windows decoder returned no image for the checked-in synthetic clip. After the fix, the test decoded the blue center and orange patch at their expected positions. |
| Gallery identity | The export returned the expected game and executable. The gallery cache kept two previews distinct when clips in different game folders shared the same basename. |
| Short replay | The 10-frame fixture exported all 10 samples and the test checked pixels decoded from the exported MP4. The recorder also exported a one-second partial replay. |
| Damaged replay | The tests rejected invalid bytes, a missing segment, missing required audio and a cut inside the first fragment. The replay handoff rejected a file shortened after finalization and removed its pending output. |
| Media Foundation tail recovery | Both encoder modes exported 150 video samples and 235 audio samples through the shifted-fragment case. The 50 ms duration guard remains in force. |
| Generation and file ownership | Stale screenshot, replay and stop commands left the current generation intact. An active export could read its leased files after recorder replacement. |
| Build and tests | The Windows Release build linked. Five focused CTest suites and ten selected recorder test entries passed. |

The native capture fixture from the implementation run verified covered and partly off-screen SDR/HDR screenshots and exported a readable client-sized replay. This qualification run decoded media from synthetic clips. The [owner-controlled Windows procedure](README.md) covers live games, the physical controller, audio attribution and the matched Beta 3 performance comparison.

The Reviewer’s registered checkout reported `cc0214930db56e7af014102213740c37bc9052b6` on this run. The candidate lives on the assigned worktree branch. Full local logs, executable identity and the review bundle are in `out/qualification/capture-38-40`.
