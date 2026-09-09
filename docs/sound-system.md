# Sound System

> Subtle console-style feedback — polished PlayStation vibe, not a noisy desktop utility.

## Events

| Event key | Trigger | Character |
|---|---|---|
| `screenshot` | capture taken | short camera-like click |
| `replay_saved` | clip exported | distinct positive chime |
| `capture_accepted` | capture request accepted, before the result is known | short bright double-tap |
| `overlay_open` / `overlay_close` | overlay toggled | soft whoosh in/out |
| `nav_tick` | gallery navigation | very quiet tick |
| `favorite` | favorite toggled | small positive blip |
| `confirm` | action confirmed | soft confirm |
| `error` | blocked action / buffer off | quiet error tone |

Files in `assets/sounds/`, overridable per event via `sound_settings` table (enabled, volume 0–100, custom file). Sound packs = folders in `gamehq-data/sound-packs/`.

## Volume levels

Two levels, chosen by event, not multiplied together:

| Level | Config key | Default | Events |
|---|---|---|---|
| Capture | `sounds.capture_volume` | 100 | `screenshot`, `replay_saved`, `capture_accepted` |
| Interface | `sounds.volume` | 80 | everything else |

Capture feedback is heard over a running game and judged against the game's own
audio, so it gets its own slider. `SoundLevels::levelFor()`
(`src/sound/SoundLevels.cpp`) owns the split as an explicit event list, so
adding an event is a deliberate decision about which slider it belongs to.

`error` stays on the interface level on purpose: the same asset also reports
settings failures such as a refused portable import, and moving it would put an
unrelated sound under a capture slider.

The capture slider is read **perceptually** —
`QtAudio::convertVolume(value, LogarithmicVolumeScale, LinearVolumeScale)` — so
half-way sounds about half as loud rather than carrying half the amplitude,
which the ear barely separates. The interface slider keeps its original straight
percentage mapping; changing it would silently move the UI sounds of every
existing user who never touched it.

Settings previews the capture level with `AppController::previewCaptureSound()`,
which plays the real `screenshot` asset through the ordinary `play()` path. It
obeys the UI-sounds master switch and the capture level exactly as a capture
does, so what the user hears in Settings is what a capture will sound like.

## Settings

Master on/off · per-event on/off · interface volume · capture volume · capture
sound preview · pack selection · **"Include GameHQ UI sounds in recordings" — default OFF** (MVP records them via loopback anyway; later use process-loopback capture to exclude our own session).

## Implementation

`SoundEngine` on Qt Multimedia (`QSoundEffect` — low latency, WAV, FFmpeg backend). All 9 effects pre-load from embedded resources (`qrc:/sounds/`) at startup; `play(event)` reads `sounds.enabled` live and takes its level from `SoundLevels` (see above). Exposed to QML as `sounds`.

Pre-loading is asynchronous, so the log reports what actually happened rather
than a count taken before Windows had finished. `SoundLoadTracker`
(`src/sound/SoundLoadTracker.cpp`) maps each `QSoundEffect::statusChanged`
transition onto a single decision:

- `Ready` — logged once per event as `Sounds: ready <event> <source>`.
- `Error`, or `Null` after the source was set — logged once per event as
  `Sounds: failed to load <event> ...` with the reason.
- `Loading`, or any repeat of a status for an already-resolved effect — not
  logged, so a chatty backend cannot produce duplicate lines.

The first failure of a run also emits `SoundEngine::loadFailed`, which `App`
turns into exactly one notification ("Some interface sounds are unavailable").
Later failures still appear in the log but never raise a second notification.
The signal is emitted queued, because an effect can resolve inside the
constructor before anything is connected. `play()` stays non-blocking and never
waits for a load.

The **default pack is synthesized** by `assets/sounds/generate_sounds.py` (pure stdlib, license-free, regenerable) — subtle sine tones with fast attack and exponential decay. Replace individual WAVs or add packs under `gamehq-data/sound-packs/` later (1.0).

Run `python assets/sounds/generate_sounds.py` from the repository root to
regenerate the pack. Output always goes beside the script in `assets/sounds/`,
regardless of the caller's working directory.

Capture assets target an RMS level at least 6 dB above `nav_tick`, without
clipping. `capture_accepted` acknowledges that a request was accepted; it does
not indicate a completed save.

Measured over each complete 44.1 kHz, mono, 16-bit WAV in the default pack
(0 dBFS = 32768; silence included):

| Asset | RMS (dBFS) | Above `nav_tick` (dB) | Peak (dBFS) |
|---|---:|---:|---:|
| `nav_tick` | -41.92 | 0.00 | -30.10 |
| `screenshot` | -30.14 | +11.78 | -14.70 |
| `replay_saved` | -23.95 | +17.97 | -9.83 |
| `capture_accepted` | -32.79 | +9.13 | -16.82 |

All four assets have zero clipped or saturated samples. These are asset levels;
the configured level for the event and the playback device still affect
perceived volume.
