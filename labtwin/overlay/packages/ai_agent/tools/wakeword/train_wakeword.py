#!/usr/bin/env python3
"""Train the bilingual openvela KWS model.

Dataset layout (16-bit WAV; any sample rate is resampled to 16 kHz):
  dataset/zh/*.wav       "你好 vela"
  dataset/en/*.wav       "Hello, openvela"
  dataset/unknown/*.wav  ordinary speech and confusing phrases
  dataset/silence/*.wav  room/device noise

Speaker-disjoint train/validation/test manifests are required for release.
This script intentionally does not download or synthesize data.
"""
from __future__ import annotations

import argparse
import pathlib
import random
import wave

import numpy as np
import tensorflow as tf

RATE = 16000
SAMPLES = 24000
LABELS = ("zh", "en", "unknown", "silence")


def read_wav(path: pathlib.Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() not in (1, 2):
            raise ValueError(f"unsupported WAV: {path}")
        rate = w.getframerate()
        channels = w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    if channels == 2:
        x = x.reshape(-1, 2).mean(axis=1)
    if rate != RATE:
        old = np.arange(len(x), dtype=np.float64)
        new = np.linspace(0, max(len(x) - 1, 0),
                          round(len(x) * RATE / rate))
        x = np.interp(new, old, x)
    if len(x) > SAMPLES:
        start = (len(x) - SAMPLES) // 2
        x = x[start:start + SAMPLES]
    else:
        left = (SAMPLES - len(x)) // 2
        x = np.pad(x, (left, SAMPLES - len(x) - left))
    return np.clip(x, -32768, 32767).astype(np.float32) / 32768.0


def load_dataset(root: pathlib.Path):
    xs, ys = [], []
    for label, name in enumerate(LABELS):
        files = sorted((root / name).glob("*.wav"))
        if not files:
            raise ValueError(f"no WAV files in {root / name}")
        for path in files:
            xs.append(read_wav(path))
            ys.append(label)
    order = list(range(len(xs)))
    random.Random(20260714).shuffle(order)
    return np.stack([xs[i] for i in order]), np.asarray([ys[i] for i in order])


def build_model() -> tf.keras.Model:
    inp = tf.keras.Input((SAMPLES,), name="pcm")
    x = tf.keras.layers.Reshape((SAMPLES, 1, 1))(inp)
    x = tf.keras.layers.Conv2D(24, (80, 1), strides=(16, 1),
                               activation="relu", padding="same")(x)
    for dilation in (1, 2, 4):
        x = tf.keras.layers.DepthwiseConv2D(
            (9, 1), dilation_rate=(dilation, 1), padding="same",
            activation="relu")(x)
        x = tf.keras.layers.Conv2D(24, (1, 1), activation="relu")(x)
        x = tf.keras.layers.AveragePooling2D((2, 1))(x)
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    out = tf.keras.layers.Dense(4, activation="softmax", name="scores")(x)
    return tf.keras.Model(inp, out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("dataset", type=pathlib.Path)
    ap.add_argument("--validation", type=pathlib.Path, required=True,
                    help="separate validation directory, using different speakers")
    ap.add_argument("--output", type=pathlib.Path, default=pathlib.Path("openvela_wake_int8.tflite"))
    ap.add_argument("--epochs", type=int, default=40)
    args = ap.parse_args()
    x, y = load_dataset(args.dataset)
    if args.dataset.resolve() == args.validation.resolve():
        raise ValueError("training and validation directories must differ")
    import hashlib
    training_hashes = {hashlib.sha256(p.read_bytes()).digest()
                       for p in args.dataset.glob("*/*.wav")}
    if any(hashlib.sha256(p.read_bytes()).digest() in training_hashes
           for p in args.validation.glob("*/*.wav")):
        raise ValueError("duplicate WAV across training and validation")
    vx, vy = load_dataset(args.validation)
    model = build_model()
    model.compile(optimizer="adam", loss="sparse_categorical_crossentropy",
                  metrics=["accuracy"])
    model.fit(x, y, validation_data=(vx, vy),
              epochs=args.epochs, batch_size=16,
              callbacks=[tf.keras.callbacks.EarlyStopping(
                  patience=6, restore_best_weights=True)])

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = lambda: ([row[None, :]] for row in x[:min(200, len(x))])
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    data = converter.convert()
    if len(data) > 250 * 1024:
        raise SystemExit(f"model is too large: {len(data)} bytes")
    args.output.write_bytes(data)
    print(f"wrote {args.output} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
