# GameHQ 0.7.38 (2026-09-21)

## Added

- The diagnostic report now records what really happened each time the in-game overlay opened and closed: which window Windows kept in the foreground, whether the game window stayed visible or was minimised, whether GameHQ's own window became active, which controller was serving input and which Windows controller focus policy was in force. This is groundwork for stopping overlay navigation from also reaching the game - it changes no overlay behaviour, and it states plainly that whether the game still receives the controller is not something GameHQ can measure about itself.
