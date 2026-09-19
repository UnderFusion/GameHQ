# GameHQ 0.7.35 (2026-09-20)

> This document is the original **English (en-US)** release note. No reviewed Portuguese (Brazil) translation exists for this version, so the complete English text is published unchanged.

## Fixed

- Controllers are identified more precisely. GameHQ now weighs the strongest available evidence about a device first, and when the evidence is ambiguous it keeps controllers separate instead of guessing. Two identical pads connected through one receiver can no longer be treated as a single controller, so their saved bindings stay apart.
