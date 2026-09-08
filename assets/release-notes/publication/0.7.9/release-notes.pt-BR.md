# GameHQ 0.7.9 (2026-09-08)

> This document is the original **English (en-US)** release note. No reviewed Portuguese (Brazil) translation exists for this version, so the complete English text is published unchanged.

## Fixed

- A replay save keeps its recorded segments when the game changes or the buffer restarts. Old files unrelated to the save can still be cleaned up, and closing GameHQ waits for the export to finish safely.
- Saving two replay clips within the same second no longer replaces the first one. Each clip and preview claims its own file name, and a failed save cannot delete an existing clip.

## Changed

- Release notes in other languages stay in the repository, linked from the release page, so the download list shows the installable packages and verification files.
