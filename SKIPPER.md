# Skipper Pod firmware notes

Target: Waveshare ESP32-S3-AUDIO-Board (8 MB PSRAM, 16 MB flash), ESP-IDF 6.0.3.
Build config: `sdkconfig.skipper-pod` in this repo (microWakeWord "Hey Skipper", the config of the
current device build). Copy it to `sdkconfig` before `idf.py build` (the skipper-ai wrapper
`hardware/skipper-pod/scripts/build.sh` does this). This tree lives in skipper-ai as the submodule
`hardware/skipper-pod/firmware`, so `../wakeword` below is `hardware/skipper-pod/wakeword`.

## "Hey Skipper" wake word (microWakeWord on TFLite Micro)

Selected with `CONFIG_USE_MICRO_WAKE_WORD=y` (Wake Word Implementation Type ->
"microWakeWord model (TFLite Micro) with AFE"). WakeNet and MultiNet are not used,
and no `srmodels.bin` is packaged into the assets partition.

Pieces:

| File | Role |
|---|---|
| `main/audio/wake_words/micro_wake_word.{h,cc}` | `MicroWakeWord : WakeWord`. Frontend, TFLM interpreter, ESPHome streaming/detection logic |
| `main/audio/wake_words/micro_wake_word_settings.h` | ESPHome `preprocessor_settings.h` values + ESPHome int8 feature quantization (plain C, shared with host check) |
| `main/audio/wake_words/models/micro_wake_word.tflite` | the model (embedded via `EMBED_FILES`) |
| `main/audio/wake_words/models/micro_wake_word.json` | microWakeWord/ESPHome manifest; CMake turns it into `build/esp-idf/main/mww_generated/micro_wake_word_manifest.h` |
| `main/audio/engines/afe_audio_engine.{h,cc}` | `WakeDetector::kMicroWakeWord`: AFE is still created for AEC/VAD; its AEC'd mono 16 kHz output (`HandleWakeWordResult`) is fed to `MicroWakeWord::FeedMono` |
| `main/idf_component.yml` | `espressif/esp-tflite-micro ~1.4.1`, `espressif/esp-nn ~1.5.0`, `esphome/esp-micro-speech-features ==1.2.3` (ESPHome's pins) |
| `tools/mww_hostcheck/` | host check: firmware frontend vs pymicro-features + streaming detection with the real model |

Pipeline (mirrors `esphome/components/micro_wake_word`):

1. micro_speech frontend: 30 ms window, `feature_step_size` ms step (10), 40 channels,
   125–7500 Hz, noise reduction (10 bits, 0.025/0.06, 0.05), PCAN (0.95, 80, 21 bits),
   log scale (shift 6).
2. uint16 → int8: `((v*256 + 333) / 666) - 128`, clamped (ESPHome's mapping; within 1 LSB
   of the model's own input quantization).
3. Each feature is written into the `[1, stride, 40]` input tensor; `Invoke()` every
   `stride` features (stride is read from the model, 3 for hey_skipper → one inference
   per 30 ms).
4. uint8 output goes into a `sliding_window_size` ring; detection when
   `sum > int(cutoff*255) * window` and the model is past the 100-feature warmup/cool-off
   (ESPHome `MIN_SLICES_BEFORE_DETECTION`). After a detection, probabilities reset.
5. `Start()` (every time wake-word detection is (re)enabled) resets frontend state,
   streaming resource variables and the probability window. So detection is armed about
   1 s after the device returns to idle/listening.

Memory: model copied to a 16-byte aligned PSRAM buffer (the EMBED_FILES symbol is not aligned),
tensor arena and the 1 KB resource-variable arena are in PSRAM. At init the arena is
probed with a 64 KB scratch arena, then allocated as `arena_used_bytes() * 1.1`. The AFE
processing task stack is 8 KB (PSRAM) when microWakeWord is enabled (inference runs inline in it).

Boot log lines to look for (tag `MicroWakeWord`):

```
'Hey Skipper' model 62304 bytes, stride 3, step 10 ms, cutoff 229/255 (0.90), window 5; arena used N bytes (manifest 30000), allocated M in PSRAM
Timing over 100 inferences: invoke avg X us per 3-feature stride (30 ms of audio), frontend avg Y us per 10 ms feature
Detected 'Hey Skipper' (avg 0.97, max 1.00)
```
Real time requires `X + 3*Y` well under 30 000 us (and the AFE task must keep up with
its own fetch cadence).

### Swapping in a retrained model

1. Train/export as usual (`../wakeword/out/TRAINING.md`). You get `<name>.tflite` + `<name>.json`
   (microWakeWord v2 / ESPHome manifest format; `feature_step_size` must stay 10 unless you
   also retrain with another step).
2. Copy them over the two files in `main/audio/wake_words/models/`, keeping the names
   `micro_wake_word.tflite` / `micro_wake_word.json`. Edit `probability_cutoff`,
   `sliding_window_size` or `wake_word` (the display/greeting text) in the json if needed.
3. Optional: run the host check (below) on a few positive clips.
4. `idf.py build && idf.py merge-bin`. The json is a CMake configure dependency, so edits
   are picked up automatically. Only the app (`xiaozhi.bin` @ 0x20000) changes, so it can go
   out by OTA. Assets are unaffected.
5. If the model uses an op ESPHome doesn't register (unlikely for microWakeWord), add it in
   `RegisterStreamingOps()` and bump the resolver size.

### Host check

```
cd tools/mww_hostcheck
F=../../managed_components/esphome__esp-micro-speech-features
clang -O2 -I$F/include -I$F/src -o /tmp/frontend_dump frontend_dump.c $F/src/*.c -lm
cd ../../../wakeword
.venv-mww/bin/python -I ../firmware/tools/mww_hostcheck/compare.py /tmp/frontend_dump \
    out/hey_skipper.tflite 0.90 5 data/samples/pos_test/o0000*.wav
```
Result for the release-v7 model (20 holdout positives, 1.2 s silence lead-in): 0 feature mismatches vs
pymicro-features over 217 000 values, 20/20 detected with the firmware's exact int8 features
and ESPHome streaming logic. The int8 features differ from the model's own input quantization
by at most 1 LSB on about 25% of values, which is the expected effect of ESPHome's /666 mapping.
