#ifndef MICRO_WAKE_WORD_H
#define MICRO_WAKE_WORD_H

// microWakeWord (ESPHome micro_wake_word v2 model format) detector running on
// TensorFlow Lite Micro. Feature generation and streaming/detection logic mirror
// esphome/components/micro_wake_word (preprocessor_settings.h, streaming_model.cpp,
// micro_wake_word.cpp). See SKIPPER.md.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "audio_codec.h"
#include "wake_word.h"

#include <tensorflow/lite/micro/micro_allocator.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>

struct FrontendState;

class MicroWakeWord : public WakeWord {
public:
    MicroWakeWord();
    ~MicroWakeWord() override;

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override;
    // Interleaved codec input; only channel 0 is used.
    void Feed(const std::vector<int16_t>& data) override;
    // Mono 16 kHz audio (e.g. AFE AEC output). Runs frontend + inference inline.
    void FeedMono(const int16_t* data, size_t samples);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override { return 160; }
    void EncodeWakeWordData() override {}
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override { return false; }
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }
    std::vector<std::string> GetWakeWords() const override { return {wake_word_}; }

private:
    static constexpr int kFeatureSize = 40;
    static constexpr int kMinSlicesBeforeDetection = 100;  // ESPHome MIN_SLICES_BEFORE_DETECTION
    static constexpr size_t kVariableArenaSize = 1024;     // ESPHome STREAMING_MODEL_VARIABLE_ARENA_SIZE
    static constexpr int kTimingSamples = 100;

    using OpResolver = tflite::MicroMutableOpResolver<20>;

    AudioCodec* codec_ = nullptr;
    std::string wake_word_;
    std::string last_detected_wake_word_;
    std::function<void(const std::string&)> callback_;
    std::atomic<bool> running_{false};
    std::atomic<bool> reset_pending_{false};

    // Model / interpreter
    const uint8_t* model_data_ = nullptr;
    uint8_t* model_copy_ = nullptr;
    uint8_t* tensor_arena_ = nullptr;
    size_t tensor_arena_size_ = 0;
    uint8_t* var_arena_ = nullptr;
    std::unique_ptr<OpResolver> op_resolver_;
    tflite::MicroAllocator* var_allocator_ = nullptr;
    tflite::MicroResourceVariables* resource_vars_ = nullptr;
    std::unique_ptr<tflite::MicroInterpreter> interpreter_;
    int stride_ = 1;

    // Frontend
    FrontendState* frontend_ = nullptr;
    int feature_step_ms_ = 10;
    int8_t features_[kFeatureSize] = {};

    // Streaming detection state (ESPHome StreamingModel)
    uint8_t probability_cutoff_ = 0;
    size_t sliding_window_size_ = 1;
    std::vector<uint8_t> recent_probabilities_;
    size_t last_n_index_ = 0;
    int current_stride_step_ = 0;
    int ignore_windows_ = -kMinSlicesBeforeDetection;
    bool unprocessed_probability_ = false;

    // Timing
    int timed_invokes_ = 0;
    int64_t invoke_us_total_ = 0;
    int64_t frontend_us_total_ = 0;
    int timed_features_ = 0;
    bool timing_logged_ = false;

    bool CreateInterpreter(size_t arena_size, bool keep);
    void ResetVariableAllocator();
    void ResetDetectionState();
    void ApplyPendingReset();
    bool PerformStreamingInference();
    void ProcessProbabilities();
};

#endif
