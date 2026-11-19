// microWakeWord feature-generation settings.
//
// These mirror ESPHome's esphome/components/micro_wake_word/preprocessor_settings.h
// and the feature quantization in MicroWakeWord::generate_features_(). Every
// microWakeWord model (including hey_skipper.tflite) is trained on features from
// exactly this pipeline, so do not change these values for a retrained model.
//
// Plain C so the same header is used by the host-side check in
// tools/mww_hostcheck/.
#ifndef MICRO_WAKE_WORD_SETTINGS_H
#define MICRO_WAKE_WORD_SETTINGS_H

#include <stdint.h>
#include "frontend_util.h"

#define MWW_SAMPLE_RATE 16000
#define MWW_FEATURE_SIZE 40
#define MWW_FEATURE_DURATION_MS 30
#define MWW_FILTERBANK_LOWER_BAND_LIMIT 125.0f
#define MWW_FILTERBANK_UPPER_BAND_LIMIT 7500.0f
#define MWW_NOISE_REDUCTION_SMOOTHING_BITS 10
#define MWW_NOISE_REDUCTION_EVEN_SMOOTHING 0.025f
#define MWW_NOISE_REDUCTION_ODD_SMOOTHING 0.06f
#define MWW_NOISE_REDUCTION_MIN_SIGNAL_REMAINING 0.05f
#define MWW_PCAN_GAIN_CONTROL_ENABLE_PCAN 1
#define MWW_PCAN_GAIN_CONTROL_STRENGTH 0.95f
#define MWW_PCAN_GAIN_CONTROL_OFFSET 80.0f
#define MWW_PCAN_GAIN_CONTROL_GAIN_BITS 21
#define MWW_LOG_SCALE_ENABLE_LOG 1
#define MWW_LOG_SCALE_SCALE_SHIFT 6

static inline void mww_fill_frontend_config(struct FrontendConfig* c, int step_ms) {
    c->window.size_ms = MWW_FEATURE_DURATION_MS;
    c->window.step_size_ms = step_ms;
    c->filterbank.num_channels = MWW_FEATURE_SIZE;
    c->filterbank.lower_band_limit = MWW_FILTERBANK_LOWER_BAND_LIMIT;
    c->filterbank.upper_band_limit = MWW_FILTERBANK_UPPER_BAND_LIMIT;
    c->noise_reduction.smoothing_bits = MWW_NOISE_REDUCTION_SMOOTHING_BITS;
    c->noise_reduction.even_smoothing = MWW_NOISE_REDUCTION_EVEN_SMOOTHING;
    c->noise_reduction.odd_smoothing = MWW_NOISE_REDUCTION_ODD_SMOOTHING;
    c->noise_reduction.min_signal_remaining = MWW_NOISE_REDUCTION_MIN_SIGNAL_REMAINING;
    c->pcan_gain_control.enable_pcan = MWW_PCAN_GAIN_CONTROL_ENABLE_PCAN;
    c->pcan_gain_control.strength = MWW_PCAN_GAIN_CONTROL_STRENGTH;
    c->pcan_gain_control.offset = MWW_PCAN_GAIN_CONTROL_OFFSET;
    c->pcan_gain_control.gain_bits = MWW_PCAN_GAIN_CONTROL_GAIN_BITS;
    c->log_scale.enable_log = MWW_LOG_SCALE_ENABLE_LOG;
    c->log_scale.scale_shift = MWW_LOG_SCALE_SCALE_SHIFT;
}

// uint16 frontend output (~0..670) -> int8 model input, exactly as ESPHome:
// input = (feature * 256) / (25.6 * 26.0) - 128, rounded, clamped.
static inline int8_t mww_quantize_feature(uint16_t v) {
    const int32_t value_scale = 256;
    const int32_t value_div = 666;
    int32_t value = (((int32_t)v * value_scale) + (value_div / 2)) / value_div;
    value -= 128;
    if (value < -128) value = -128;
    if (value > 127) value = 127;
    return (int8_t)value;
}

#endif
