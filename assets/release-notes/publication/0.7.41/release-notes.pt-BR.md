# GameHQ 0.7.41 (2026-09-22)

> This document is the original **English (en-US)** release note. No reviewed Portuguese (Brazil) translation exists for this version, so the complete English text is published unchanged.

## Added

- A developer-only tool now watches the controller from outside GameHQ. It is a separate program that only observes - no injection, no hooks, no drivers, no virtual devices - and it is never part of an installed copy. It records, phase by phase, whether the pad still reaches another program through the three Windows input paths, and it says "not measurable" rather than claiming isolation whenever it had nothing to compare against.

## Changed

- The exclusive input mode the overlay asks for while it is active is now measured from outside GameHQ instead of being described as unverified. On the test machine, with a controller streaming in the background, another program's controller input stopped while the overlay had the exclusive mode and came back as soon as it was released - twice, in two independent runs. Games that read the controller through other Windows interfaces are unaffected by that mode, which is documented as a limit rather than hidden; the diagnostics keep saying the effect is not verified from inside GameHQ, because that is what they can honestly know.
