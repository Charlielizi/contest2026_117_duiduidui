#!/usr/bin/env python3
"""Check recorded PCM, not just WAV transport. No speech-quality claim.

Usage: python3 tools/Check-RecordingSignal.py captured.wav
Exit 1 rejects silent/stuck capture; exit 2 rejects invalid or short WAV.
Skip the initial 0.5 s so a hardware startup transient cannot hide stuck input.
"""
import array
import json
import math
import sys
import wave


def inspect(path):
    with wave.open(str(path), 'rb') as wav:
        rate, channels, width, frames = (wav.getframerate(), wav.getnchannels(),
                                        wav.getsampwidth(), wav.getnframes())
        if (rate, channels, width) != (16000, 1, 2):
            raise ValueError('Expected 16 kHz, mono, signed 16-bit PCM WAV')
        if frames < rate:
            raise ValueError('Record at least one second')
        skipped = rate // 2
        if len(wav.readframes(skipped)) != skipped * 2:
            raise ValueError('Truncated startup frames')
        count = constant = clipped = 0
        ac_energy = 0.0
        minimum, maximum = 32767, -32768
        while count < frames - skipped:
            n = min(rate // 10, frames - skipped - count)
            data = wav.readframes(n)
            if len(data) != n * 2:
                raise ValueError('Truncated PCM payload')
            samples = array.array('h', data)
            if sys.byteorder != 'little':
                samples.byteswap()
            low, high = min(samples), max(samples)
            minimum, maximum = min(minimum, low), max(maximum, high)
            constant += n if low == high else 0
            clipped += sum(abs(v) >= 32112 for v in samples)
            mean = sum(samples) / n
            ac_energy += sum((v - mean) ** 2 for v in samples)
            count += n
    ac_rms = math.sqrt(ac_energy / count)
    stuck_fraction = constant / count
    return dict(duration_seconds=frames / rate, checked_frames=count,
                skipped_startup_seconds=0.5, minimum=minimum, maximum=maximum,
                ac_rms=ac_rms, constant_fraction=stuck_fraction,
                clipped_fraction=clipped / count,
                signal_present=stuck_fraction < 0.95 and ac_rms >= 1.0)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    try:
        result = inspect(sys.argv[1])
    except (OSError, ValueError, EOFError, wave.Error) as error:
        print(json.dumps({'error': str(error)}))
        raise SystemExit(2)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['signal_present'] else 1)
