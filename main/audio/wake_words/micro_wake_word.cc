#include "micro_wake_word.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>

#include <tensorflow/lite/micro/micro_allocator.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>
#include <tensorflow/lite/micro/schema/schema_generated.h>

#include "frontend.h"
#include "frontend_util.h"
#include "micro_wake_word_settings.h"
#include "micro_wake_word_manifest.h"  // generated from models/micro_wake_word.json

#define TAG "MicroWakeWord"

// Model + manifest values are embedded at build time from
// main/audio/wake_words/models/micro_wake_word.{tflite,json} (see main/CMakeLists.txt).
extern const uint8_t micro_wake_word_tflite_start[] asm("_binary_micro_wake_word_tflite_start");
extern const uint8_t micro_wake_word_tflite_end[] asm("_binary_micro_wake_word_tflite_end");

#ifndef MWW_MANIFEST_WAKE_WORD
#error "MWW_MANIFEST_* definitions missing (generated from models/micro_wake_word.json)"
#endif

namespace {

uint8_t* AllocPsram(size_t size) {
    auto* p = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p == nullptr) {
        p = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, size, MALLOC_CAP_8BIT));
    }
    return p;
}

// Same op set ESPHome registers in StreamingModel::register_streaming_ops_().
bool RegisterStreamingOps(tflite::MicroMutableOpResolver<20>& r) {
    return r.AddCallOnce() == kTfLiteOk && r.AddVarHandle() == kTfLiteOk &&
           r.AddReshape() == kTfLiteOk && r.AddReadVariable() == kTfLiteOk &&
           r.AddStridedSlice() == kTfLiteOk && r.AddConcatenation() == kTfLiteOk &&
           r.AddAssignVariable() == kTfLiteOk && r.AddConv2D() == kTfLiteOk &&
           r.AddMul() == kTfLiteOk && r.AddAdd() == kTfLiteOk && r.AddMean() == kTfLiteOk &&
           r.AddFullyConnected() == kTfLiteOk && r.AddLogistic() == kTfLiteOk &&
           r.AddQuantize() == kTfLiteOk && r.AddDepthwiseConv2D() == kTfLiteOk &&
           r.AddAveragePool2D() == kTfLiteOk && r.AddMaxPool2D() == kTfLiteOk &&
           r.AddPad() == kTfLiteOk && r.AddPack() == kTfLiteOk && r.AddSplitV() == kTfLiteOk;
}

}  // namespace

MicroWakeWord::MicroWakeWord() = default;

MicroWakeWord::~MicroWakeWord() {
    interpreter_.reset();
    if (frontend_ != nullptr) {
        FrontendFreeStateContents(frontend_);
        delete frontend_;
    }
    heap_caps_free(tensor_arena_);
    heap_caps_free(var_arena_);
    heap_caps_free(model_copy_);
}

void MicroWakeWord::ResetVariableAllocator() {
    // Resource variables (streaming state) live in their own small arena, as in ESPHome.
    var_allocator_ = tflite::MicroAllocator::Create(var_arena_, kVariableArenaSize);
    resource_vars_ = tflite::MicroResourceVariables::Create(var_allocator_, 20);
}

bool MicroWakeWord::CreateInterpreter(size_t arena_size, bool keep) {
    interpreter_.reset();
    ResetVariableAllocator();
    uint8_t* arena = keep ? tensor_arena_ : AllocPsram(arena_size);
    if (arena == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u byte tensor arena", (unsigned)arena_size);
        return false;
    }
    auto interp = std::make_unique<tflite::MicroInterpreter>(
        tflite::GetModel(model_data_), *op_resolver_, arena, arena_size, resource_vars_);
    const bool ok = interp->AllocateTensors() == kTfLiteOk;
    if (!keep) {
        // Probe run: remember the real requirement, then release everything.
        if (ok) {
            tensor_arena_size_ = interp->arena_used_bytes();
        }
        interp.reset();
        heap_caps_free(arena);
        ResetVariableAllocator();
        return ok;
    }
    if (!ok) {
        return false;
    }
    interpreter_ = std::move(interp);
    return true;
}

bool MicroWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* /*models_list*/) {
    codec_ = codec;
    wake_word_ = MWW_MANIFEST_WAKE_WORD;
    // ESPHome: quantized cutoff = int(probability_cutoff * 255)
    probability_cutoff_ = static_cast<uint8_t>(MWW_MANIFEST_PROBABILITY_CUTOFF * 255.0f);
    sliding_window_size_ = std::max(1, MWW_MANIFEST_SLIDING_WINDOW_SIZE);
    feature_step_ms_ = MWW_MANIFEST_FEATURE_STEP_SIZE;
    recent_probabilities_.assign(sliding_window_size_, 0);

    const size_t model_size = micro_wake_word_tflite_end - micro_wake_word_tflite_start;
    model_data_ = micro_wake_word_tflite_start;
    if (reinterpret_cast<uintptr_t>(model_data_) % 16 != 0) {
        // TFLM expects a 16-byte aligned flatbuffer; EMBED_FILES does not guarantee it.
        model_copy_ = AllocPsram(model_size);
        if (model_copy_ == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate model copy");
            return false;
        }
        memcpy(model_copy_, model_data_, model_size);
        model_data_ = model_copy_;
    }
    const tflite::Model* model = tflite::GetModel(model_data_);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Unsupported model schema %lu", (unsigned long)model->version());
        return false;
    }

    op_resolver_ = std::make_unique<OpResolver>();
    if (!RegisterStreamingOps(*op_resolver_)) {
        ESP_LOGE(TAG, "Failed to register streaming ops");
        return false;
    }
    var_arena_ = AllocPsram(kVariableArenaSize);
    if (var_arena_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate variable arena");
        return false;
    }

    // Probe the arena requirement with generous room, then allocate used + ~10%.
    const size_t manifest_arena = MWW_MANIFEST_TENSOR_ARENA_SIZE;
    const size_t probe_size = (std::max<size_t>(manifest_arena * 2, 65536) + 15) & ~size_t(15);
    if (!CreateInterpreter(probe_size, false)) {
        ESP_LOGE(TAG, "AllocateTensors failed even with a %u byte arena", (unsigned)probe_size);
        return false;
    }
    const size_t used = tensor_arena_size_;
    tensor_arena_size_ = ((used + used / 10) + 15) & ~size_t(15);
    tensor_arena_ = AllocPsram(tensor_arena_size_);
    if (tensor_arena_ == nullptr || !CreateInterpreter(tensor_arena_size_, true)) {
        ESP_LOGE(TAG, "Failed to create interpreter with %u byte arena", (unsigned)tensor_arena_size_);
        return false;
    }

    TfLiteTensor* input = interpreter_->input(0);
    TfLiteTensor* output = interpreter_->output(0);
    if (input->dims->size != 3 || input->dims->data[0] != 1 ||
        input->dims->data[2] != kFeatureSize || input->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "Unexpected model input tensor");
        return false;
    }
    if (output->dims->size != 2 || output->dims->data[0] != 1 || output->dims->data[1] != 1 ||
        output->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "Unexpected model output tensor");
        return false;
    }
    stride_ = input->dims->data[1];

    frontend_ = new FrontendState();
    struct FrontendConfig config;
    mww_fill_frontend_config(&config, feature_step_ms_);
    if (!FrontendPopulateState(&config, frontend_, MWW_SAMPLE_RATE)) {
        ESP_LOGE(TAG, "Failed to initialize audio frontend");
        delete frontend_;
        frontend_ = nullptr;
        return false;
    }

    ResetDetectionState();
    ESP_LOGI(TAG,
             "'%s' model %u bytes, stride %d, step %d ms, cutoff %u/255 (%.2f), window %u; "
             "arena used %u bytes (manifest %u), allocated %u in %s",
             wake_word_.c_str(), (unsigned)model_size, stride_, feature_step_ms_,
             probability_cutoff_, MWW_MANIFEST_PROBABILITY_CUTOFF, (unsigned)sliding_window_size_,
             (unsigned)interpreter_->arena_used_bytes(), (unsigned)manifest_arena,
             (unsigned)tensor_arena_size_,
             esp_ptr_external_ram(tensor_arena_) ? "PSRAM" : "internal RAM");
    return true;
}

void MicroWakeWord::OnWakeWordDetected(std::function<void(const std::string&)> callback) {
    callback_ = std::move(callback);
}

void MicroWakeWord::Start() {
    reset_pending_ = true;
    running_ = true;
}

void MicroWakeWord::Stop() { running_ = false; }

void MicroWakeWord::ResetDetectionState() {
    // ESPHome StreamingModel::reset_probabilities()
    std::fill(recent_probabilities_.begin(), recent_probabilities_.end(), 0);
    ignore_windows_ = -kMinSlicesBeforeDetection;
    unprocessed_probability_ = false;
}

void MicroWakeWord::ApplyPendingReset() {
    if (!reset_pending_.exchange(false)) {
        return;
    }
    // Fresh start, equivalent to ESPHome restarting its inference task: new
    // frontend state, zeroed streaming variables, empty probability window.
    if (frontend_ != nullptr) {
        FrontendReset(frontend_);
    }
    if (resource_vars_ != nullptr) {
        resource_vars_->ResetAll();
    }
    current_stride_step_ = 0;
    last_n_index_ = 0;
    ResetDetectionState();
}

void MicroWakeWord::Feed(const std::vector<int16_t>& data) {
    const int channels = codec_ != nullptr ? std::max(1, codec_->input_channels()) : 1;
    if (channels == 1) {
        FeedMono(data.data(), data.size());
        return;
    }
    std::vector<int16_t> mono(data.size() / channels);
    for (size_t i = 0; i < mono.size(); ++i) {
        mono[i] = data[i * channels];
    }
    FeedMono(mono.data(), mono.size());
}

void MicroWakeWord::FeedMono(const int16_t* data, size_t samples) {
    if (!running_ || interpreter_ == nullptr || frontend_ == nullptr) {
        return;
    }
    ApplyPendingReset();

    while (samples > 0 && running_) {
        size_t used = 0;
        const int64_t t0 = esp_timer_get_time();
        FrontendOutput out = FrontendProcessSamples(frontend_, data, samples, &used);
        if (!timing_logged_) {
            frontend_us_total_ += esp_timer_get_time() - t0;
        }
        data += used;
        samples -= used;
        if (out.size == static_cast<size_t>(kFeatureSize)) {
            if (!timing_logged_) {
                ++timed_features_;
            }
            for (int i = 0; i < kFeatureSize; ++i) {
                features_[i] = mww_quantize_feature(out.values[i]);
            }
            if (!PerformStreamingInference()) {
                return;
            }
            ProcessProbabilities();
        }
        if (used == 0) {
            break;
        }
    }
}

bool MicroWakeWord::PerformStreamingInference() {
    // ESPHome StreamingModel::perform_streaming_inference()
    TfLiteTensor* input = interpreter_->input(0);
    current_stride_step_ = current_stride_step_ % stride_;
    memcpy(input->data.int8 + kFeatureSize * current_stride_step_, features_, kFeatureSize);
    ++current_stride_step_;

    if (current_stride_step_ >= stride_) {
        const int64_t t0 = esp_timer_get_time();
        if (interpreter_->Invoke() != kTfLiteOk) {
            ESP_LOGW(TAG, "Invoke failed");
            return false;
        }
        if (!timing_logged_) {
            invoke_us_total_ += esp_timer_get_time() - t0;
            if (++timed_invokes_ == kTimingSamples) {
                timing_logged_ = true;
                ESP_LOGI(TAG,
                         "Timing over %d inferences: invoke avg %lld us per %d-feature stride "
                         "(%d ms of audio), frontend avg %lld us per %d ms feature",
                         kTimingSamples, invoke_us_total_ / kTimingSamples, stride_,
                         stride_ * feature_step_ms_, frontend_us_total_ / std::max(1, timed_features_),
                         feature_step_ms_);
            }
        }
        TfLiteTensor* output = interpreter_->output(0);
        ++last_n_index_;
        if (last_n_index_ == sliding_window_size_) {
            last_n_index_ = 0;
        }
        recent_probabilities_[last_n_index_] = output->data.uint8[0];
        unprocessed_probability_ = true;
    }
    if (recent_probabilities_[last_n_index_] < probability_cutoff_) {
        // Cool-off after a detection / warm-up after start: needs
        // kMinSlicesBeforeDetection features below the cutoff.
        ignore_windows_ = std::min(ignore_windows_ + 1, 0);
    }
    return true;
}

void MicroWakeWord::ProcessProbabilities() {
    // ESPHome MicroWakeWord::process_probabilities_() + WakeWordModel::determine_detected()
    if (!unprocessed_probability_) {
        return;
    }
    unprocessed_probability_ = false;
    if (ignore_windows_ < 0) {
        return;
    }
    uint32_t sum = 0;
    uint8_t max_p = 0;
    for (auto p : recent_probabilities_) {
        sum += p;
        max_p = std::max(max_p, p);
    }
    if (sum > static_cast<uint32_t>(probability_cutoff_) * sliding_window_size_) {
        ESP_LOGI(TAG, "Detected '%s' (avg %.2f, max %.2f)", wake_word_.c_str(),
                 sum / (255.0f * sliding_window_size_), max_p / 255.0f);
        ResetDetectionState();
        last_detected_wake_word_ = wake_word_;
        if (callback_) {
            callback_(last_detected_wake_word_);
        }
    }
}
