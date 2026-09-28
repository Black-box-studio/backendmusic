"""Generate original, deterministic PCM demo loops using only Python's stdlib."""
import math
import struct
import wave
from pathlib import Path

RATE = 22050
SECONDS = 16
ROOT = Path(__file__).resolve().parent
for name, notes in {
    "aurora": [220.0, 261.63, 329.63, 392.0],
    "afterhours": [146.83, 174.61, 220.0, 293.66],
    "coastline": [196.0, 246.94, 293.66, 369.99],
}.items():
    samples = bytearray()
    for i in range(RATE * SECONDS):
        t = i / RATE
        envelope = min(1.0, t / 0.8, (SECONDS - t) / 1.5)
        chord = sum(math.sin(2 * math.pi * f * t) for f in notes) * 0.065
        beat = t % 0.5
        note = notes[int(t * 2) % len(notes)] * 2
        melody = math.sin(2 * math.pi * note * t) * math.exp(-beat * 9) * 0.15
        pulse = math.sin(2 * math.pi * 55 * t) * math.exp(-(t % 1.0) * 14) * 0.12
        samples.extend(struct.pack("<h", int((chord + melody + pulse) * envelope * 32767)))
    with wave.open(str(ROOT / (name + ".wav")), "wb") as output:
        output.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        output.writeframes(samples)
