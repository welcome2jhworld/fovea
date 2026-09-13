"""Writes alert.wav: a two-tone chime synthesized from sine waves (no recorded audio).

Run from this directory with any Python 3: python3 generate_alert_wav.py
"""
import math
import struct
import wave

RATE = 22050
TONES = ((880.0, 0.0, 0.20), (1318.5, 0.16, 0.30))  # frequency Hz, start s, length s
LENGTH_S = 0.48
ATTACK_S = 0.006
PEAK = 0.55


def sample(t: float) -> float:
    value = 0.0
    for freq, start, length in TONES:
        local = t - start
        if 0.0 <= local < length:
            attack = min(1.0, local / ATTACK_S)
            decay = math.exp(-5.0 * local / length)
            value += math.sin(2.0 * math.pi * freq * local) * attack * decay
    return value


def main() -> None:
    frames = bytearray()
    for i in range(int(RATE * LENGTH_S)):
        v = max(-1.0, min(1.0, PEAK * sample(i / RATE)))
        frames += struct.pack("<h", int(v * 32767))
    with wave.open("alert.wav", "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(frames))


if __name__ == "__main__":
    main()
