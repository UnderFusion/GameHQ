# GameHQ 0.7.17 (2026-09-09)

## Fixed

- A capture request that is refused early now says so in the log. Pressing save clip while the replay buffer was cold, busy or paused previously left no record of the press at all.
- A screenshot skipped because the foreground window is not a game is now recorded with the reason, instead of passing without a trace.

## Added

- Every screenshot and clip request now carries an id and the device that pressed it, so one press reads as one chain in the log from start to finish or to the reason it failed.
- The copied diagnostics now list the bindings of the controller in your hands next to the shared controller bindings.
