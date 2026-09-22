# GameHQ 0.7.39 (2026-09-22)

> This document is the original **English (en-US)** release note. No reviewed Russian translation exists for this version, so the complete English text is published unchanged.

## Changed

- Opening the in-game overlay now makes it the active window, so the keyboard and the controller drive the overlay instead of the game behind it. The game stays on screen and is never minimised, moved or changed in any way, and closing the overlay hands the focus straight back to it. If Windows refuses to move the focus, the overlay behaves exactly as before and keeps warning that the game may still react to the controller. This is a first step: a game that deliberately reads the controller in the background can still receive it, and that is reported honestly rather than hidden.
