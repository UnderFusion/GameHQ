# GameHQ 0.7.40 (2026-09-22)

> This document is the original **English (en-US)** release note. No reviewed Chinese (Traditional) translation exists for this version, so the complete English text is published unchanged.

## Changed

- While the in-game overlay is the active window, GameHQ now also asks Windows' GameInput runtime for exclusive foreground input, so other GameInput clients are meant to stop receiving the pad while the overlay is open. The policy has exactly one owner that applies it only after the overlay has really taken the foreground, and releases it again on every way out: closing the overlay, another app taking the focus, the game going away, or GameInput being switched off. Guide and Share handling is unchanged. Whether another program actually stops seeing the pad is not something GameHQ can confirm from inside itself, so the diagnostics report the policy it requested and explicitly do not claim isolation.
