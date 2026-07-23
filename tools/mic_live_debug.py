import sounddevice as sd
import numpy as np

DEVICE = 6
SAMPLE_RATE = 48000
BLOCKSIZE = 480

print("Starting live mic debug... Ctrl+C to stop")

with sd.InputStream(
    samplerate=SAMPLE_RATE,
    channels=1,
    dtype="int16",
    blocksize=BLOCKSIZE,
    device=DEVICE,
) as stream:
    while True:
        data, overflowed = stream.read(BLOCKSIZE)
        amp = np.abs(data).mean()
        print(f"amplitude={amp:.0f} overflowed={overflowed}", end="\r", flush=True)