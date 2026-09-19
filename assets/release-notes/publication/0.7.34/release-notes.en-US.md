# GameHQ 0.7.34 (2026-09-19)

## Fixed

- The in-game overlay keeps the game in the foreground on every path that opens, moves or raises it. The overlay window is marked as never-activating before it appears, and that mark is restored whenever Windows rebuilds the window, so opening the overlay no longer pulls focus away from a borderless game.
