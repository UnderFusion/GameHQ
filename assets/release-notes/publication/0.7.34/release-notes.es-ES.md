# GameHQ 0.7.34 (2026-09-19)

> This document is the original **English (en-US)** release note. No reviewed Spanish translation exists for this version, so the complete English text is published unchanged.

## Fixed

- The in-game overlay keeps the game in the foreground on every path that opens, moves or raises it. The overlay window is marked as never-activating before it appears, and that mark is restored whenever Windows rebuilds the window, so opening the overlay no longer pulls focus away from a borderless game.
