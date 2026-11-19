"""Host-side sanity check for the firmware microWakeWord pipeline.

1. Feature parity: the firmware C frontend (built as /tmp/frontend_dump from
   managed_components/esphome__esp-micro-speech-features + micro_wake_word_settings.h)
   vs pymicro-features (what microWakeWord trains on).
2. Quantization parity: firmware int8 mapping vs the model's input scale/zero point.
3. Streaming detection: run hey_skipper.tflite on the firmware int8 features with
   ESPHome's streaming logic (stride from input shape, uint8 sliding window,
   cutoff*255, 100-slice warmup) and report detections.

usage: python compare.py <frontend_dump> <model.tflite> <cutoff> <window> wav...
"""
import subprocess, sys, wave
import numpy as np
from pymicro_features import MicroFrontend
from ai_edge_litert.interpreter import Interpreter

dump, model_path, cutoff, window = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4])
wavs = sys.argv[5:]

def read_wav(p):
    with wave.open(p) as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2, p
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)

def py_features(pcm):
    fe = MicroFrontend(); b = pcm.tobytes(); out = []; i = 0
    while i + 320 < len(b):
        r = fe.process_samples(b[i:i + 320]); i += r.samples_read * 2
        if r.features: out.append(r.features)
    return np.array(out, dtype=np.float32)

def fw_features(pcm):
    txt = subprocess.run([dump], input=pcm.tobytes(), capture_output=True, check=True).stdout.decode()
    rows = [list(map(int, l.split())) for l in txt.strip().splitlines()]
    a = np.array(rows, dtype=np.int32)
    return a[:, :40], a[:, 40:].astype(np.int8)

interp = Interpreter(model_path=model_path)
interp.allocate_tensors()
inp = interp.get_input_details()[0]; outd = interp.get_output_details()[0]
stride = inp['shape'][1]
in_scale, in_zp = inp['quantization']
print(f"model input {inp['shape']} {inp['dtype'].__name__} scale={in_scale} zp={in_zp}; output {outd['dtype'].__name__} q={outd['quantization']}; stride={stride}")
cut_u8 = int(cutoff * 255)

def stream(int8_feats):
    # Fresh state per clip: re-create interpreter (resets VAR_HANDLE state).
    it = Interpreter(model_path=model_path); it.allocate_tensors()
    i_d = it.get_input_details()[0]['index']; o_d = it.get_output_details()[0]['index']
    # ESPHome: probabilities start at 0, ignore_windows=-100. Prepend 1.2 s of silence
    # features is not needed for the max-probability report, but detection requires warmup.
    probs = [0] * window; last = 0; ignore = -100; step = 0; buf = np.zeros((1, stride, 40), np.int8)
    maxp = 0; detections = []
    for n, f in enumerate(int8_feats):
        buf[0, step] = f; step += 1
        if step >= stride:
            step = 0
            it.set_tensor(i_d, buf); it.invoke()
            p = int(it.get_tensor(o_d)[0][0])
            last = (last + 1) % window; probs[last] = p; maxp = max(maxp, p)
            if ignore >= 0 and sum(probs) > cut_u8 * window:
                detections.append(n); probs = [0] * window; ignore = -100
        if probs[last] < cut_u8:
            ignore = min(ignore + 1, 0)
    return maxp, detections

max_abs = 0; tot = 0; mism = 0; q_mism = 0; det = 0
silence = np.zeros(16000 * 12 // 10, np.int16)  # 1.2 s lead-in so the 100-slice warmup passes
for p in wavs:
    pcm = np.concatenate([silence, read_wav(p), silence[:8000]])
    pyf = py_features(pcm)
    u16, i8 = fw_features(pcm)
    n = min(len(pyf), len(u16))
    if len(pyf) != len(u16):
        print(f"{p}: frame count differs py={len(pyf)} fw={len(u16)}")
    d = np.abs(pyf[:n] - u16[:n] * 0.0390625)  # pymicro returns value/25.6
    max_abs = max(max_abs, float(d.max())); tot += d.size; mism += int((d > 1e-4).sum())
    # Quantize pymicro float features with the model's own input quantization.
    q_ref = np.clip(np.round(pyf[:n] / in_scale + in_zp), -128, 127).astype(np.int8)
    q_mism += int((q_ref != i8[:n]).sum())
    maxp, dets = stream(i8)
    det += bool(dets)
    print(f"{p.split('/')[-1]}: frames={n} max|d|={d.max():.4g} q_diff={(q_ref != i8[:n]).sum()} maxprob={maxp/255:.3f} detections={len(dets)}")
print(f"SUMMARY clips={len(wavs)} feature_values={tot} mismatches={mism} max_abs_diff={max_abs:.4g} int8_mismatches_vs_model_quant={q_mism} clips_detected={det}/{len(wavs)}")
