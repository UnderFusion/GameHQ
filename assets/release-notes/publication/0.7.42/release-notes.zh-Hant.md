# GameHQ 0.7.42 (2026-09-22)

> This document is the original **English (en-US)** release note. No reviewed Chinese (Traditional) translation exists for this version, so the complete English text is published unchanged.

## Added

- Closing the overlay while a button, a trigger or a stick direction is still held no longer passes that held state straight to the game. GameHQ now waits - briefly, never longer than about half a second - until the controller is physically back at rest before it hands control back, and it does so without pressing or releasing anything on the game's behalf. When the controller is already at rest, closing is as immediate as it always was.

## Changed

- Every overlay close now records how that hand-over went: whether the controller came back to rest in time, whether the short wait ran out and the close finished anyway, whether something else had already given the controller back, or whether the close never needed to wait - for example when the overlay steps aside for the GameHQ window, or when the game it covered is gone. A close that ran out its wait says so, instead of being reported as a clean hand-over.
- If the controller is disconnected while the overlay is closing, the close finishes straight away: a controller that is no longer there cannot pass anything on to the game, so there is nothing to wait for.
