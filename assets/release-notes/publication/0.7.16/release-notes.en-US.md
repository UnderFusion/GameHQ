# GameHQ 0.7.16 (2026-09-09)

## Fixed

- Interface sounds no longer report themselves as loaded before Windows has finished loading them. Each sound now records when it becomes playable, or why it could not be used.
- If any interface sound is unavailable, GameHQ shows a single warning instead of staying silent without explanation. The new warning text uses English until the planned translation pass.
