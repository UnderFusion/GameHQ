# Sound System

> Subtle console-style feedback — polished PlayStation vibe, not a noisy desktop utility.

## Events

| Event key | Trigger | Character |
|---|---|---|
| `screenshot` | capture taken | short camera-like click |
| `replay_saved` | clip exported | distinct positive chime |
| `capture_accepted` | reserved for request acceptance; not triggered yet | short bright double-tap |
| `overlay_open` / `overlay_close` | overlay toggled | soft whoosh in/out |
| `nav_tick` | gallery navigation | very quiet tick |
| `favorite` | favorite toggled | small positive blip |
| `confirm` | action confirmed | soft confirm |
| `error` | blocked action / buffer off | quiet error tone |

Files in `assets/sounds/`, overridable per event via `sound_settings` table (enabled, volume 0–100, custom file). Sound packs = folders in `gamehq-data/sound-packs/`.

## Settings

Master on/off · per-event on/off · master volume · pack selection · **"Include GameHQ UI sounds in recordings" — default OFF** (MVP records them via loopback anyway; later use process-loopback capture to exclude our own session).

## Implementation

`SoundEngine` on Qt Multimedia (`QSoundEffect` — low latency, WAV, FFmpeg backend). All 9 effects pre-load from embedded resources (`qrc:/sounds/`) at startup; `play(event)` reads `sounds.enabled` / `sounds.volume` from config live. Exposed to QML as `sounds`.

The **default pack is synthesized** by `assets/sounds/generate_sounds.py` (pure stdlib, license-free, regenerable) — subtle sine tones with fast attack and exponential decay. Replace individual WAVs or add packs under `gamehq-data/sound-packs/` later (1.0).

Run `python assets/sounds/generate_sounds.py` from the repository root to
regenerate the pack. Output always goes beside the script in `assets/sounds/`,
regardless of the caller's working directory.

Capture assets target an RMS level at least 6 dB above `nav_tick`, without
clipping. `capture_accepted` is embedded and preloaded; its runtime trigger is
reserved for a later change and does not indicate a completed save.

Measured over each complete 44.1 kHz, mono, 16-bit WAV in the default pack
(0 dBFS = 32768; silence included):

| Asset | RMS (dBFS) | Above `nav_tick` (dB) | Peak (dBFS) |
|---|---:|---:|---:|
| `nav_tick` | -41.92 | 0.00 | -30.10 |
| `screenshot` | -30.14 | +11.78 | -14.70 |
| `replay_saved` | -23.95 | +17.97 | -9.83 |
| `capture_accepted` | -32.79 | +9.13 | -16.82 |

All four assets have zero clipped or saturated samples. These are asset levels;
the configured master volume and playback device still affect perceived volume.
