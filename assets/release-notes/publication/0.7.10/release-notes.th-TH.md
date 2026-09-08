# GameHQ 0.7.10 (2026-09-08)

> This document is the original **English (en-US)** release note. No reviewed Thai translation exists for this version, so the complete English text is published unchanged.

## Fixed

- Replay status now reflects successful capture startup and finalized footage. Saving before the buffer is ready gives an explicit retry message, and older callbacks cannot overwrite a newer recording session.
- With always-on replay disabled, a manual save keeps its buffer through settings changes and HDR screenshot activity until the save finishes. A first save that only starts the buffer expires after 90 seconds without a follow-up, so manual mode does not record indefinitely.
