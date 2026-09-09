"""Generate exact 2x PCM assets for 0-200% playback; refuse clipping.

Original WAVs stay unchanged. Run after generate_sounds.py; --check verifies
all samples and WAV parameters against the original assets without writing.
"""
import argparse
from pathlib import Path
import struct
import wave

ROOT = Path(__file__).resolve().parent
EVENTS = ("nav_tick", "overlay_open", "overlay_close", "favorite", "confirm",
          "error", "screenshot", "replay_saved", "capture_accepted")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    for event in EVENTS:
        with wave.open(str(ROOT / f"{event}.wav"), "rb") as source:
            params = source.getparams()
            if params.sampwidth != 2 or params.comptype != "NONE":
                raise ValueError(f"{event}: expected uncompressed 16-bit PCM")
            raw = source.readframes(params.nframes)
        samples = [s[0] for s in struct.iter_unpack("<h", raw)]
        boosted = [s * 2 for s in samples]
        if not boosted or min(boosted) < -32768 or max(boosted) > 32767:
            raise ValueError(f"{event}: insufficient headroom for 200%; refusing clipping")
        payload = struct.pack(f"<{len(boosted)}h", *boosted)
        path = ROOT / "boosted" / f"{event}.wav"
        if args.check:
            with wave.open(str(path), "rb") as result:
                if result.getparams() != params or result.readframes(params.nframes) != payload:
                    raise ValueError(f"{event}: stale boosted asset")
        else:
            path.parent.mkdir(exist_ok=True)
            with wave.open(str(path), "wb") as result:
                result.setparams(params)
                result.writeframes(payload)
        print(f"{event}: exact 2x PCM, peak {max(abs(s) for s in boosted)}/32768, no clipping")


if __name__ == "__main__":
    main()
