"""Regression for false-positive recording acceptance on constant -1 PCM."""
import array
import importlib.util
import math
from pathlib import Path
import sys
import tempfile
import unittest
import wave

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_signal', root / 'tools/Check-RecordingSignal.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RecordingSignalTest(unittest.TestCase):
    def analyze(self, samples, truncate=False):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'capture.wav'
            pcm = array.array('h', samples)
            if sys.byteorder != 'little':
                pcm.byteswap()
            with wave.open(str(path), 'wb') as w:
                w.setparams((1, 2, 16000, 0, 'NONE', 'not compressed'))
                w.writeframes(pcm.tobytes())
            if truncate:
                with path.open('r+b') as f:
                    f.truncate(100)
            return module.inspect(path)

    def test_stuck_nonzero_input(self):
        self.assertFalse(self.analyze([-1] * 32000)['signal_present'])

    def test_zero_input(self):
        self.assertFalse(self.analyze([0] * 32000)['signal_present'])

    def test_startup_transient_does_not_mask_stuck_input(self):
        startup = [int(30000 * math.sin(i)) for i in range(8000)]
        self.assertFalse(self.analyze(startup + [-1] * 24000)['signal_present'])

    def test_dynamic_audio_with_dc_offset(self):
        samples = [int(-20 + 300 * math.sin(2 * math.pi * 600 * i / 16000)) for i in range(32000)]
        self.assertTrue(self.analyze(samples)['signal_present'])

    def test_truncated_payload(self):
        with self.assertRaises(ValueError):
            self.analyze([10] * 32000, truncate=True)


if __name__ == '__main__':
    unittest.main()
