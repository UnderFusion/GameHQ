# GameHQ 0.7.18 (2026-09-09)

> This document is the original **English (en-US)** release note. No reviewed Korean translation exists for this version, so the complete English text is published unchanged.

## Added

- Screenshot and replay requests now immediately play an acknowledgement sound and show a request-received notification, according to your feedback settings. Saving can still fail; the final outcome is reported separately.
- Replay preview thumbnails are now decoded and encoded on the export thread, keeping image work off the capture worker while saving. The gallery still receives the thumbnail associated with the saved clip.
- Capture results update the original request notification in place. The visible notification stack is limited to four cards; older cards leaving the stack do not affect saved captures.
