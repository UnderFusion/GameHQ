"""Generates GameHQ's default UI sound pack (docs/sound-system.md).

Pure-stdlib synthesis (wave + math) so the pack is license-free and
regenerable: python generate_sounds.py
Design goals: subtle, soft, console-like — sine tones with fast attack,
exponential decay, gentle volumes. 44.1 kHz, 16-bit, mono.
"""
import math
from pathlib import Path
import struct
import wave

RATE = 44100
OUTPUT_DIR = Path(__file__).resolve().parent


def render(name, samples):
    with wave.open(str(OUTPUT_DIR / name), "w") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767)))) for s in samples))
    print("wrote", name, f"{len(samples)/RATE*1000:.0f} ms")


def tone(freq, ms, vol=0.3, attack_ms=4, decay=6.0, harmonics=((1, 1.0), (2, 0.25))):
    n = int(RATE * ms / 1000)
    out = []
    for i in range(n):
        t = i / RATE
        env = min(1.0, (i / (RATE * attack_ms / 1000 + 1))) * math.exp(-decay * i / n)
        s = sum(a * math.sin(2 * math.pi * freq * h * t) for h, a in harmonics)
        out.append(vol * env * s)
    return out


def mix(*parts):
    out = []
    for offset_ms, samples in parts:
        start = int(RATE * offset_ms / 1000)
        if len(out) < start + len(samples):
            out.extend([0.0] * (start + len(samples) - len(out)))
        for i, s in enumerate(samples):
            out[start + i] += s
    return out


# Navigation tick — very quiet, very short
render("nav_tick.wav", tone(1900, 28, vol=0.10, decay=9))

# Overlay open — soft rising pair
render("overlay_open.wav", mix((0, tone(520, 160, 0.20)), (90, tone(780, 220, 0.20))))

# Overlay close — mirrored falling pair
render("overlay_close.wav", mix((0, tone(780, 160, 0.20)), (90, tone(520, 220, 0.18))))

# Favorite — small positive double-note
render("favorite.wav", mix((0, tone(660, 110, 0.22)), (70, tone(990, 160, 0.20))))

# Confirm — single mid note
render("confirm.wav", tone(740, 130, 0.22))

# Error — low, dull double thud
render("error.wav", mix((0, tone(210, 120, 0.25, harmonics=((1, 1.0),))),
                        (130, tone(180, 160, 0.22, harmonics=((1, 1.0),)))))

# Capture sounds share a loudness floor: their RMS must sit at least
# +6 dB above nav_tick so a capture confirmation is unmistakable but unclipped.

# Screenshot — bright camera-like tick-tock
render("screenshot.wav", mix((0, tone(2400, 30, 0.32, decay=10)),
                             (45, tone(1500, 60, 0.29, decay=8))))

# Replay saved — three-note ascending chime
render("replay_saved.wav", mix((0, tone(523, 140, 0.30)), (110, tone(659, 140, 0.30)),
                               (220, tone(784, 260, 0.32))))

# Capture accepted — short bright double-tap, distinct from the single-note
# confirm and from navigation ticks; reserved for request acceptance.
render("capture_accepted.wav", mix((0, tone(1880, 26, 0.36, decay=11)),
                                   (60, tone(2512, 40, 0.36, decay=10))))
