# Local wake-word training: 你好 vela

Task: TASK-20260908-02. Primary Module: Voice Interaction.

The selected Chinese wake phrase is “你好 vela”. Recognition runs locally;
ASR and TTS after wake still require their own configured services. No microphone
audio should be uploaded merely to detect this wake phrase.

The current working tree may contain a private debug model. It is not approved
for publication or release and must be replaced with the untrained placeholder
before uploading source. UI and injected events are not acoustic acceptance evidence.

## Dataset

Keep consenting-speaker audio outside source control. Use source/speaker-disjoint
train, validation and test recordings. The legacy four-class order is
`zh,en,unknown,silence`; the Chinese binary microfrontend contract is
`unknown,zh` and is identified by the `zh2-mf1:` version prefix.

Start with at least 10 training speakers and 20 repetitions per positive phrase
per speaker; validation and test speakers must not appear in training. Preserve
a private speaker manifest. The script rejects identical files across training
and validation, but cannot automatically prove speaker separation.

Include close/far microphone positions, fan noise, laboratory room noise,
ordinary Chinese conversation, confusing phrases, and the device's own TTS.
Record the complete phrase within 1.5 seconds without clipping. Do not use
synthetic-only audio to claim real microphone acceptance.

## Train and package

New models should use the exact fixed-point board frontend: 16 kHz PCM, 25 ms
frames, 10 ms step, 32 Mel-filterbank channels and 148 frames per 1.5 s window.
Preserve temporal order in the classifier. Package an evaluated model with:

```sh
python package_model.py hello_vela_mf32_int8.tflite ../../src/voice/wakeword_model_data.cc \
  --version YYYYMMDD.N --labels unknown-zh-mf32
```

The model is INT8 with a `[1,148,32,1]` input and two scores, maximum 250 KiB.
The runtime retains compatibility with legacy raw-PCM/four-score models.
Target tensor allocation must fit the 256 KiB arena; file size alone does not
prove this. Record model SHA, data split manifest, training version and results.

## Proposed acceptance gates (not measured)

- Test unseen speakers on the physical board: target >=95% wake recall at 1 m
  in a quiet room; report sample count and noisy-room results separately.
- At least 8 hours of negative audio: target <=1 false wake per hour, including
  board TTS playback; report actual events and duration.
- Verify wake-to-dialogue foreground, partial ASR updates when provided, final
  reply display even when TTS fails, cancellation, and return to listening.
- Measure inference latency, heap/stack headroom and audio ownership on R528;
  run repeated wake/cancel/playback cycles before approving a release image.

Until a trained model passes these checks, retain touch-to-talk fallback and
do not label the firmware as supporting validated offline “你好 vela” wake.
