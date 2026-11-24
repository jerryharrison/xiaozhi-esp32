// Skipper Porthole: the avatar render loop. See avatar_display.h and README.md.
#include "avatar_display.h"
#include "porthole_font.h"
#include "avatar_dirty.h"
#include "avatar_overlay.h"
#include "avatar_card_draw.h"

#include "board.h"
#include "settings.h"
#include "system_info.h"
#include "audio_codec.h"
#include "assets/lang_config.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cJSON.h>

#define TAG "AvatarDisplay"

namespace {

const float* const kBone = kPhBone;
const float* const kAmber = kPhAmber;
constexpr float kPi = PORTHOLE_PI;
constexpr float kRimFrac = PORTHOLE_RIM_FRAC;   // spec.json geometry.rim
constexpr float kTimerMoodSeconds = 10;   // bridge mood "timer": how long the tide takes to drain

// Pixel helpers live in avatar_overlay.h (shared with the host checks).
inline void Blend(uint16_t* px, const float c[3], float a) { ph_blend(px, c, a); }

const pf_glyph_t* Glyph(const pf_font_t& f, char ch) {
    const char* p = strchr(f.chars, ch);
    if (ch == 0 || p == nullptr) return nullptr;
    return &f.glyphs[p - f.chars];
}

int TextWidth(const pf_font_t& f, const std::string& s) {
    int w = 0;
    for (char ch : s) {
        auto g = Glyph(f, ch);
        w += g ? g->adv : f.glyphs[0].adv;
    }
    return w;
}

uint32_t Fnv(uint32_t h, const void* data, size_t n) {
    auto p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

bool IsSpecState(const char* e) {
    static const char* const kNames[] = {"steady", "working", "listening", "storm", "star", "quiet", "timer"};
    for (auto n : kNames) if (strcmp(n, e) == 0) return true;
    return false;
}

}  // namespace

AvatarDisplay::AvatarDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel),
      av_(*static_cast<avatar_t*>(heap_caps_malloc(sizeof(avatar_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))),
      frame_(*static_cast<avatar_frame_t*>(heap_caps_calloc(1, sizeof(avatar_frame_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))) {
    assert(&av_ != nullptr && &frame_ != nullptr);
    pace_ = xSemaphoreCreateBinary();
    esp_timer_create_args_t targs = {};
    targs.callback = [](void* arg) { xSemaphoreGive(static_cast<AvatarDisplay*>(arg)->pace_); };
    targs.arg = this;
    targs.name = "avatar_pace";
    ESP_ERROR_CHECK(esp_timer_create(&targs, &pace_timer_));
    width_ = width;
    height_ = height;
    lock_ = xSemaphoreCreateRecursiveMutex();
    buf_free_ = xSemaphoreCreateCounting(kBuffers, kBuffers);
    for (auto& b : buf_) {
        b = static_cast<uint16_t*>(heap_caps_malloc(width_ * kBandRows * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        assert(b != nullptr);
    }
    avatar_init(&av_, width_);
    for (int i = 0; i < 3; i++) av_.cur.sky[i] = av_.tgt.sky[i] = 0;  // true-black sky: pixels off
    avatar_prepare(&av_, &frame_);

    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done = OnTransDone;
    esp_lcd_panel_io_register_event_callbacks(panel_io_, &cbs, this);
    ESP_LOGI(TAG, "Avatar display %dx%d, %d-row DMA bands", width_, height_, kBandRows);
}

AvatarDisplay::~AvatarDisplay() {
    if (task_) vTaskDelete(task_);
    if (helper_) vTaskDelete(helper_);
    if (flusher_) vTaskDelete(flusher_);
    for (auto b : buf_) heap_caps_free(b);
    if (half_buf_) heap_caps_free(half_buf_);
    if (pace_timer_) esp_timer_delete(pace_timer_);
    heap_caps_free(&frame_);
    heap_caps_free(&av_);
}

bool AvatarDisplay::OnTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
    auto self = static_cast<AvatarDisplay*>(ctx);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->buf_free_, &woken);
    return woken == pdTRUE;
}

bool AvatarDisplay::Lock(int timeout_ms) {
    TickType_t t = timeout_ms <= 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lock_, t) == pdTRUE;
}

void AvatarDisplay::Unlock() { xSemaphoreGiveRecursive(lock_); }

void AvatarDisplay::Start(esp_lcd_touch_handle_t touch) {
    touch_ = touch;
    // Lowest priority above idle on both cores: audio (opus is 2, audio I/O 4-8), wake word and
    // networking always win; the avatar just drops frames. Each band is split across the cores.
    // The flush task barely uses the CPU (it queues a transfer and waits for the previous one), but
    // it must start each transfer as soon as the last one ends, so it runs above the audio tasks.
    flush_q_ = xQueueCreate(kBuffers, sizeof(FlushJob));
    xTaskCreatePinnedToCore(FlusherEntry, "avatar_flush", 3072, this, 9, &flusher_, 0);
    xTaskCreatePinnedToCore(TaskEntry, "avatar", 6144, this, 1, &task_, 0);
    if (xTaskCreatePinnedToCore(HelperEntry, "avatar2", 4096, this, 1, &helper_, 1) != pdPASS) helper_ = nullptr;
    // The card client's HTTPS poll spends 1.5-2 s of CPU in the TLS handshake. Above the render tasks
    // (it was priority 2) that took a whole core and stalled a frame for as long; at idle priority it
    // only runs while both render cores wait for the next frame. It shares no lock with rendering:
    // the reply goes over in card_poll_ under in_mutex_, which the render task holds only briefly.
    xTaskCreate(CardClientEntry, "cards", 8192, this, tskIDLE_PRIORITY, &card_task_);
}

void AvatarDisplay::TaskEntry(void* arg) { static_cast<AvatarDisplay*>(arg)->RenderLoop(); }

void AvatarDisplay::FlusherEntry(void* arg) {
    auto self = static_cast<AvatarDisplay*>(arg);
    FlushJob j;
    while (true) {
        if (xQueueReceive(self->flush_q_, &j, portMAX_DELAY) != pdTRUE) continue;
        self->Lock(0);
        esp_lcd_panel_draw_bitmap(self->panel_, j.x0, j.y0, j.x1, j.y1, j.buf);
        self->Unlock();
    }
}

void AvatarDisplay::HelperEntry(void* arg) {
    auto self = static_cast<AvatarDisplay*>(arg);
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        self->RenderShared();
        xTaskNotifyGive(self->task_);
    }
}

void AvatarDisplay::RenderSplit(uint16_t* fb, int stride, int y0, int y1, int x0, int x1) {
    // Rendering only reads the avatar and the prepared frame, so both cores share each band, taking
    // rows one at a time: costly rows (the light, rings) and a busy core even out by themselves.
    job_ = {fb, stride, y0, y1, x0, x1};
    next_row_.store(y0);
    if (helper_ == nullptr || y1 - y0 < 2) {
        RenderShared();
        return;
    }
    xTaskNotifyGive(helper_);
    RenderShared();
    int64_t t = esp_timer_get_time();
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    stat_helper_wait_us_ += esp_timer_get_time() - t;
}

// ------------------------------------------------------------------ inputs from other tasks

void AvatarDisplay::SetStatus(const char* status) {
    std::lock_guard<std::mutex> lk(in_mutex_);
    status_ = status ? status : "";
    if (status_ == Lang::Strings::ERROR) {
        error_ = true;
        error_until_us_ = esp_timer_get_time() + 15000000;
    } else {
        error_ = false;
    }
}

void AvatarDisplay::ShowNotification(const char* notification, int duration_ms) {
    std::lock_guard<std::mutex> lk(in_mutex_);
    notification_ = notification ? notification : "";
    notification_until_us_ = esp_timer_get_time() + (int64_t)duration_ms * 1000;
}

void AvatarDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

bool AvatarDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) return false;
    std::lock_guard<std::mutex> lk(in_mutex_);
    if (strcmp(emotion, "neutral") == 0) {
        mood_.clear();
    } else if (strcmp(emotion, "thinking") == 0) {
        thinking_ = true;
    } else if (IsSpecState(emotion)) {
        mood_ = emotion;   // bridge mood cue, held until the turn ends
    }
    // Other xiaozhi emoji names (happy, gear, ...) have no avatar form; errors come via SetStatus.
    return true;
}

void AvatarDisplay::SetChatMessage(const char* role, const char* content) {
    if (role == nullptr || strcmp(role, "system") != 0) return;
    std::lock_guard<std::mutex> lk(in_mutex_);
    system_msg_ = content ? content : "";
}

void AvatarDisplay::ClearChatMessages() {
    std::lock_guard<std::mutex> lk(in_mutex_);
    system_msg_.clear();
}

void AvatarDisplay::UpdateStatusBar(bool update_all) {
    // Called every second from the main task. Polling the battery here also lets the board
    // enable its power-save timer when it runs on battery (as the stock status bar did).
    int64_t now = esp_timer_get_time();
    if (!update_all && now - last_battery_poll_us_ < 5000000) return;
    last_battery_poll_us_ = now;
    if (auto codec = Board::GetInstance().GetAudioCodec()) current_volume_ = codec->output_volume();
    int level = 0;
    bool charging = false, discharging = false;
    if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) SetBattery(level, charging, discharging);
}

void AvatarDisplay::SetPowerSaveMode(bool on) {
    std::lock_guard<std::mutex> lk(in_mutex_);
    power_save_ = on;
}

void AvatarDisplay::OnDeviceState(DeviceState state) {
    std::lock_guard<std::mutex> lk(in_mutex_);
    device_state_ = state;
    if (state == kDeviceStateIdle || state == kDeviceStateConnecting) {
        mood_.clear();
        thinking_ = false;
    } else if (state == kDeviceStateListening) {
        mood_.clear();
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) error_ = false;
}

void AvatarDisplay::SetThinking(bool thinking) {
    std::lock_guard<std::mutex> lk(in_mutex_);
    thinking_ = thinking;
}

void AvatarDisplay::ShowCard(const std::string& json) {
    std::lock_guard<std::mutex> lk(in_mutex_);
    if (card_msgs_.size() < 16) card_msgs_.push_back(json);
}

void AvatarDisplay::ShowVolume(int volume) {
    current_volume_ = volume;
    std::lock_guard<std::mutex> lk(in_mutex_);
    volume_ = std::clamp(volume, 0, 100);
    volume_until_us_ = esp_timer_get_time() + 1500000;
}

void AvatarDisplay::SetBattery(int level, bool charging, bool discharging) {
    std::lock_guard<std::mutex> lk(in_mutex_);
    battery_ = level;
    charging_ = charging;
    discharging_ = discharging;
}

void AvatarDisplay::FeedPlayback(const int16_t* pcm, size_t samples, int channels, int sample_rate) {
    // Audio task. Split into 10 ms windows, RMS * 3.5 like the bridge's avatar-stream level(),
    // stamped with the time each window reaches the speaker (after the I2S DMA queue).
    if (pcm == nullptr || samples == 0 || channels <= 0 || sample_rate <= 0) return;
    const int64_t now = esp_timer_get_time();
    const int64_t kDmaLatencyUs = 60000;
    if (playback_clock_us_ < now + kDmaLatencyUs / 2) playback_clock_us_ = now + kDmaLatencyUs;
    const size_t win = (size_t)(sample_rate / 100) * channels;
    for (size_t i = 0; i < samples; i += win) {
        size_t n = std::min(win, samples - i);
        float s = 0;
        for (size_t j = 0; j < n; j += channels) {
            float v = pcm[i + j] / 32768.0f;
            s += v * v;
        }
        float lvl = std::min(1.0f, sqrtf(s / (float)std::max<size_t>(1, n / channels)) * 3.5f);
        uint32_t h = level_head_.load(std::memory_order_relaxed);
        levels_[h % kLevelRing] = {playback_clock_us_, lvl};
        level_head_.store(h + 1, std::memory_order_release);
        playback_clock_us_ += (int64_t)n / channels * 1000000 / sample_rate;
    }
}

// ------------------------------------------------------------------ render task

float AvatarDisplay::PlaybackLevel(int64_t now_us) {
    uint32_t head = level_head_.load(std::memory_order_acquire);
    if (head - level_tail_ > kLevelRing) level_tail_ = head - kLevelRing;
    float lvl = -1;
    int64_t at = 0;
    while (level_tail_ != head && levels_[level_tail_ % kLevelRing].at_us <= now_us) {
        lvl = levels_[level_tail_ % kLevelRing].level;
        at = levels_[level_tail_ % kLevelRing].at_us;
        level_tail_++;
    }
    if (lvl >= 0) {
        last_level_ = lvl;
        last_level_at_us_ = at;
        return lvl;
    }
    // Playback stopped: drop to silence quickly instead of waiting out the renderer's 1 s timeout.
    if (last_level_ > 0 && now_us - last_level_at_us_ > 80000) {
        last_level_ = 0;
        return 0;
    }
    return -1;
}

void AvatarDisplay::ApplyInputs(int64_t now_us, float* frame_period_ms) {
    std::string want;
    int taps;
    DeviceState ds;
    bool ps;
    {
        std::lock_guard<std::mutex> lk(in_mutex_);
        taps = taps_;
        taps_ = 0;
        ds = device_state_;
        ps = power_save_;
        bool err = error_ && now_us < error_until_us_;
        if (ps) {
            want = "quiet";
        } else if (ds == kDeviceStateWifiConfiguring) {
            want = "quiet";
        } else if (err || ds == kDeviceStateFatalError) {
            want = "storm";
        } else {
            switch (ds) {
                case kDeviceStateSpeaking:
                    want = thinking_ ? "working" : (mood_.empty() ? "steady" : mood_);
                    break;
                case kDeviceStateListening:
                    want = thinking_ ? "working" : (mood_.empty() ? "listening" : mood_);
                    break;
                case kDeviceStateConnecting:
                    want = "listening";
                    break;
                case kDeviceStateActivating:
                case kDeviceStateUpgrading:
                    want = "working";
                    break;
                default:
                    want = thinking_ ? "working" : (mood_.empty() ? "steady" : mood_);
                    break;
            }
        }
        BuildText(now_us);
    }
    {
        std::vector<std::string> msgs;
        std::string poll;
        {
            std::lock_guard<std::mutex> lk(in_mutex_);
            msgs.swap(card_msgs_);
            poll.swap(card_poll_);
        }
        for (const auto& m : msgs) {
            cJSON* root = cJSON_Parse(m.c_str());
            if (root) { if (cards_.ApplyMessage(root, now_us)) ESP_LOGI(TAG, "card pushed (%d live)", (int)cards_.LiveCount()); cJSON_Delete(root); }
        }
        if (!poll.empty()) {
            cJSON* root = cJSON_Parse(poll.c_str());
            if (root) { if (cards_.Reconcile(root, now_us)) ESP_LOGI(TAG, "cards polled (%d live)", (int)cards_.LiveCount()); cJSON_Delete(root); }
        }
    }
    // Cards set the mood only between conversations, and never over an error or setup screen.
    UpdateCards(now_us, !ps && ds == kDeviceStateIdle && mood_.empty() && !thinking_ && want == "steady", &want);
    for (int i = 0; i < taps; i++) avatar_tap(&av_);
    std::string mood_key = card_mood_ ? "card:" + card_mood_id_ + ":" + want : want;
    if (mood_key != requested_state_) {
        // "timer" (leave soon) from the bridge: the tide drains over kTimerMoodSeconds with the amber
        // rim sweep, then the renderer returns to steady by itself. A timer card drives the drain itself.
        if (want == "timer" && !card_mood_) avatar_countdown(&av_, kTimerMoodSeconds);
        else if (avatar_set_state(&av_, want.c_str()) != 0) avatar_set_state(&av_, "steady");
        if (card_mood_)
            for (const auto& p : card_params_) avatar_set_param(&av_, p.key, p.value);
        ESP_LOGI(TAG, "avatar state -> %s (device state %d%s)", want.c_str(), (int)ds, card_mood_ ? ", card" : "");
        requested_state_ = mood_key;
    }
    if (card_mood_ && card_drain_ >= 0) avatar_set_param(&av_, "level", card_drain_);   // a timer card's tide
    float lvl = PlaybackLevel(now_us);
    // Only Skipper's voice drives the swell, not the UI sounds (wake chime, Wi-Fi prompts).
    if (lvl >= 0 && (ds == kDeviceStateSpeaking || lvl == 0)) avatar_feed(&av_, lvl, 0.5f);

    // About 30 fps in every state (the renderer keeps up at full resolution); 10 fps in power save.
    *frame_period_ms = ps ? 100 : 33;
}


void AvatarDisplay::UpdateCards(int64_t now_us, bool idle, std::string* want) {
    bool ring = false;
    const Card* c = cards_.Tick(now_us, &ring);
    if (ring) avatar_tap(&av_);   // a finished timer rings
    auto events = cards_.TakeEvents();
    if (!events.empty()) {
        for (const auto& e : events) ESP_LOGI(TAG, "card %s: %s", e.first.c_str(), e.second.c_str());
        {
            std::lock_guard<std::mutex> lk(card_ev_mutex_);
            for (auto& e : events) if (card_events_.size() < 16) card_events_.push_back(std::move(e));
        }
        if (card_task_) xTaskNotifyGive(card_task_);
    }
    card_mood_ = false;
    card_drain_ = -1;
    if (c == nullptr || !idle) return;
    card_mood_ = true;
    card_mood_id_ = c->id;
    card_params_.assign(c->set, c->set + c->nset);
    *want = c->mood.empty() ? "steady" : c->mood;
    if (c->kind == CardKind::Timer && c->has_ends && c->done_us == 0) {
        // The tide drains as the countdown runs, capped at 10% of the screen (Jerry's default).
        float total = c->total > 0 ? c->total : 600;
        card_drain_ = std::min(0.1f, c->drain) * std::clamp(1 - CardSet::Left(*c, now_us) / total, 0.0f, 1.0f);
    } else if (c->kind == CardKind::Timer && c->done_us) {
        card_drain_ = 0;   // finished: the tide comes back in
    }
}

int AvatarDisplay::AddWrapped(const std::string& text, int y, int max_width, int max_lines, bool amber) {
    const pf_font_t& f = pf_text;
    int lines = 0;
    size_t pos = 0;
    while (pos < text.size() && lines < max_lines && text_count_ < kMaxTextItems) {
        // Take words while they fit; a single overlong word is cut.
        size_t end = pos, best = pos;
        while (end <= text.size()) {
            size_t sp = text.find_first_of(" \n", end);
            if (sp == std::string::npos) sp = text.size();
            if (TextWidth(f, text.substr(pos, sp - pos)) > max_width) break;
            best = sp;
            if (sp >= text.size() || text[sp] == '\n') break;
            end = sp + 1;
        }
        if (best == pos) {  // overlong word
            best = pos;
            while (best < text.size() && TextWidth(f, text.substr(pos, best + 1 - pos)) <= max_width) best++;
            if (best == pos) best = pos + 1;
        }
        auto& item = text_[text_count_++];
        item.text = text.substr(pos, best - pos);
        item.y = y + lines * f.line_h;
        item.big = false;
        item.amber = amber;
        lines++;
        pos = best;
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n')) pos++;
    }
    return lines;
}

void AvatarDisplay::RenderShared() {
    const RenderJob j = job_;
    for (int y; (y = next_row_.fetch_add(1)) < j.y1;)
        avatar_render_cols(&av_, &frame_, j.fb + (y - j.y0) * j.stride, j.stride, y, y + 1, 1, j.x0, j.x1, AVATAR_RGB565_SWAP);
}

void AvatarDisplay::BuildText(int64_t now_us) {
    // Called with in_mutex_ held. Text layout for setup/errors/notifications/volume/battery.
    text_count_ = 0;
    bool body = false;
    if (device_state_ == kDeviceStateWifiConfiguring && !system_msg_.empty()) {
        // "Hotspot: <ssid> Config URL: http://192.168.4.1"
        std::string m = system_msg_;
        size_t h = m.find(Lang::Strings::CONNECT_TO_HOTSPOT), u = m.find(Lang::Strings::ACCESS_VIA_BROWSER);
        auto add = [&](const std::string& s, int y, bool amber) {
            if (text_count_ < kMaxTextItems) text_[text_count_++] = {s, y, false, amber};
        };
        add("Wi-Fi setup", 130, false);
        if (h != std::string::npos && u != std::string::npos && u > h) {
            size_t hs = h + strlen(Lang::Strings::CONNECT_TO_HOTSPOT);
            std::string ssid = m.substr(hs, u - hs);
            std::string url = m.substr(u + strlen(Lang::Strings::ACCESS_VIA_BROWSER));
            if (url.rfind("http://", 0) == 0) url = url.substr(7);
            add("Join the Wi-Fi", 266, false);
            AddWrapped(ssid, 299, 340, 1, true);
            add("then open", 340, false);
            AddWrapped(url, 373, 300, 1, true);
        } else {
            AddWrapped(m, 266, 330, 4, false);
        }
        body = true;
    } else if (((error_ && now_us < error_until_us_) || device_state_ == kDeviceStateActivating ||
                device_state_ == kDeviceStateUpgrading || device_state_ == kDeviceStateFatalError) &&
               !system_msg_.empty()) {
        AddWrapped(system_msg_, 292, 320, 4, false);
        body = true;
    }
    int foot_y = body ? 70 : 384;
    if (now_us < battery_until_us_ && battery_ >= 0) {
        char b[32];
        snprintf(b, sizeof b, "%s %d%%", charging_ ? "Charging" : "Battery", battery_);
        AddWrapped(b, foot_y, 260, 1, false);
    } else if (now_us < notification_until_us_ && !notification_.empty()) {
        AddWrapped(notification_, foot_y, 260, 2, false);
    }
    volume_arc_ = now_us < volume_until_us_ && volume_ >= 0;
    if (volume_arc_) {
        volume_shown_ = volume_;
        if (text_count_ < kMaxTextItems) text_[text_count_++] = {std::to_string(volume_), 52, true, false};
    }
    low_battery_ = battery_ >= 0 && battery_ <= 15 && discharging_;
}

void AvatarDisplay::PollTouch(int64_t now_us) {
    if (touch_ == nullptr) return;
    esp_lcd_touch_point_data_t pt = {};
    uint8_t cnt = 0;
    esp_err_t terr = esp_lcd_touch_read_data(touch_);
    // The CST9217 answers without its ACK byte when it has no fresh report (the driver returns
    // ESP_ERR_INVALID_RESPONSE before touching its points): nothing new this frame, not a failure.
    if (terr == ESP_ERR_INVALID_RESPONSE) return;
    if (terr != ESP_OK) {
        if (!touch_error_logged_) ESP_LOGW(TAG, "touch read failed: %s (counted in the fps line from now on)", esp_err_to_name(terr));
        touch_error_logged_ = true;
        stat_touch_errors_++;
        return;
    }
    esp_lcd_touch_get_data(touch_, &pt, &cnt, 1);
    const float R = width_ / 2.0f;
    if (cnt > 0) {
        float x = pt.x, y = pt.y;
        if (!touching_) {
            touching_ = true;
            touch_t0_ = now_us;
            touch_x0_ = touch_x_ = x;
            touch_y0_ = touch_y_ = y;
            gesture_dial_ = gesture_swipe_ = gesture_long_ = gesture_moved_ = gesture_card_ = false;
            dial_angle_ = atan2f(y - R, x - R);
            dial_accum_ = 0;
            gesture_volume_ = gesture_base_ = current_volume_.load();
            if (activity_cb_) activity_cb_();
            return;
        }
        touch_x_ = x;
        touch_y_ = y;
        float mx = x - touch_x0_, my = y - touch_y0_;
        if (!gesture_moved_ && mx * mx + my * my > 18 * 18) gesture_moved_ = true;
        float d0 = hypotf(touch_x0_ - R, touch_y0_ - R);
        int new_volume = gesture_volume_;
        if (gesture_moved_ && !gesture_long_) {
            if (gesture_card_ || (d0 <= R * 0.68f && !gesture_swipe_ && cards_.LiveCount() > 0 && fabsf(mx) > 1.5f * fabsf(my))) {
                // Sideways in the middle with cards live: drag the showing card (swipe on release).
                gesture_card_ = true;
                cards_.Drag(mx);
            } else if (d0 > R * 0.68f && !gesture_swipe_) {
                // Rim dial: every 15 degrees clockwise = +5, counter-clockwise = -5.
                gesture_dial_ = true;
                float a = atan2f(y - R, x - R), da = a - dial_angle_;
                if (da > kPi) da -= 2 * kPi;
                if (da < -kPi) da += 2 * kPi;
                dial_angle_ = a;
                dial_accum_ += da;
                const float step = 15 * kPi / 180;
                while (dial_accum_ >= step) { new_volume += 5; dial_accum_ -= step; }
                while (dial_accum_ <= -step) { new_volume -= 5; dial_accum_ += step; }
            } else if (!gesture_dial_ && fabsf(my) > 2 * fabsf(mx)) {
                // Vertical swipe in the middle: every 40 px up = +10, down = -10.
                gesture_swipe_ = true;
                float dy = touch_y0_ - y;   // up is positive
                int steps = (int)(dy / 40);
                if (steps != 0) new_volume = (gesture_base_ + 5) / 10 * 10 + steps * 10;
            }
        }
        new_volume = std::clamp(new_volume, 0, 100);
        if (new_volume != gesture_volume_) {
            gesture_volume_ = new_volume;
            ShowVolume(new_volume);
            if (volume_cb_) volume_cb_(new_volume);
        }
        if (!gesture_moved_ && !gesture_long_ && now_us - touch_t0_ > 700000 && cards_.Showing()) {
            gesture_long_ = true;   // hold dismisses the showing card (declines a call)
            cards_.Hold(now_us);
        } else if (!gesture_moved_ && !gesture_long_ && now_us - touch_t0_ > 700000) {
            gesture_long_ = true;
            if (long_press_cb_) long_press_cb_();
            std::lock_guard<std::mutex> lk(in_mutex_);
            battery_until_us_ = now_us + 2500000;
            volume_ = current_volume_.load();
            volume_until_us_ = now_us + 2500000;
        }
    } else if (touching_) {
        touching_ = false;
        if (gesture_card_) {
            float mx = touch_x_ - touch_x0_;
            cards_.Drag(0);
            if (fabsf(mx) > 50 || fabsf(mx) / std::max(0.01f, (now_us - touch_t0_) / 1e6f) > 500) cards_.Swipe(mx < 0 ? 1 : -1, now_us);
        } else if (!gesture_moved_ && !gesture_long_ && now_us - touch_t0_ < 500000) {
            ESP_LOGI(TAG, "tap at %d,%d", (int)touch_x0_, (int)touch_y0_);
            if (cards_.Showing()) cards_.Tap(now_us);   // pins a glance, plays a message, answers a call
            std::lock_guard<std::mutex> lk(in_mutex_);
            taps_++;
        }
        if (gesture_dial_ || gesture_swipe_) ESP_LOGI(TAG, "volume gesture -> %d", gesture_volume_);
    }
}

void AvatarDisplay::ActiveRows(int* y0, int* y1) {
    avatar_active_rows(&av_, y0, y1);
    *y0 = std::min(*y0 * scale_, height_);
    *y1 = std::min(*y1 * scale_, height_);
}

void AvatarDisplay::SetScale(int scale) {
    if (scale == scale_) return;
    scale_ = scale;
    if (scale_ == 2 && half_buf_ == nullptr) {
        half_buf_ = static_cast<uint16_t*>(heap_caps_malloc((width_ / 2) * (kBandRows / 2) * sizeof(uint16_t),
                                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    avatar_init(&av_, width_ / scale_);
    for (int i = 0; i < 3; i++) av_.cur.sky[i] = av_.tgt.sky[i] = 0;
    requested_state_.clear();
    force_full_ = true;
}

void AvatarDisplay::RenderRows(int y0, int y1, bool clip, int cx0, int cx1) {
    for (int y = y0; y < y1; y += kBandRows) {
        int yb1 = std::min(y + kBandRows, y1);
        int spans[4][2] = {{0, width_}}, nspans = 1;
        if (cx0 >= 0 && scale_ == 1) {
            spans[0][0] = cx0;
            spans[0][1] = cx1;
        } else if (clip && scale_ == 1) {
            // Away from the horizon only the light's reach and the tap ripples' rings can change:
            // send just those columns (avatar_band_spans, even-aligned for the CO5300). Overlays are
            // drawn inside each span, so text and arcs there stay right.
            nspans = avatar_band_spans(&shape_, &shape_prev_, y, yb1, width_, spans);
        }
        for (int k = 0; k < nspans; k++) {
            const int x0 = spans[k][0], x1 = spans[k][1], w = x1 - x0;
            if (w <= 0) continue;
            int64_t t0 = esp_timer_get_time();
            xSemaphoreTake(buf_free_, portMAX_DELAY);
            int64_t t1 = esp_timer_get_time();
            uint16_t* buf = buf_[buf_next_];
            buf_next_ = (buf_next_ + 1) % kBuffers;
            if (scale_ == 1) {
                RenderSplit(buf, w, y, yb1, x0, x1);
            } else {
                const int hw = width_ / 2, hy0 = y / 2, hy1 = (yb1 + 1) / 2;
                RenderSplit(half_buf_, hw, hy0, hy1, 0, hw);
                for (int r = y; r < yb1; r++) {
                    const uint16_t* src = half_buf_ + (r / 2 - hy0) * hw;
                    uint16_t* dst = buf + (r - y) * width_;
                    for (int x = 0; x < hw; x++) dst[2 * x] = dst[2 * x + 1] = src[x];
                }
            }
            int64_t to = esp_timer_get_time();
            DrawOverlays(buf, w, x0, x1, y, yb1);
            int64_t t2 = esp_timer_get_time();
            stat_overlay_us_ += t2 - to;
            FlushJob job = {buf, x0, y, x1, yb1};
            xQueueSend(flush_q_, &job, portMAX_DELAY);
            stat_flush_us_ += t1 - t0;
            stat_render_us_ += t2 - t1;
            if (k == 0) stat_rows_ += yb1 - y;
            stat_px_ += (yb1 - y) * w;
        }
    }
}

void AvatarDisplay::DrawText(uint16_t* band, int stride, int x0, int x1, int y0, int y1, const std::string& text,
                             int y_top, bool big, const float rgb[3]) {
    const pf_font_t& f = big ? pf_big : pf_text;
    if (y_top >= y1 || y_top + f.line_h <= y0) return;
    int x = (width_ - TextWidth(f, text)) / 2;
    for (char ch : text) {
        auto g = Glyph(f, ch);
        if (g == nullptr) { x += f.glyphs[0].adv; continue; }
        int gx = x + g->xo, gy = y_top + g->yo;
        if (gx < x1 && gx + g->w > x0)
            for (int r = std::max(gy, y0); r < std::min(gy + g->h, y1); r++) {
                uint16_t* row = band + (r - y0) * stride - x0;
                for (int c = std::max(0, x0 - gx); c < g->w && gx + c < x1; c++) {
                    int idx = (r - gy) * g->w + c;
                    uint8_t byte = f.bitmap[g->off + idx / 2];
                    int a = (idx & 1) ? (byte & 15) : (byte >> 4);
                    if (a) Blend(&row[gx + c], rgb, a / 15.0f);
                }
            }
        x += g->adv;
    }
}

void AvatarDisplay::DrawCards(uint16_t* band, int stride, int x0, int x1, int y0, int y1) {
    porthole_draw_cards(card_layer_, &av_, scale_, width_, band, stride, x0, x1, y0, y1);   // avatar_card_draw.h
}


void AvatarDisplay::DrawOverlays(uint16_t* band, int stride, int x0, int x1, int y0, int y1) {
    DrawCards(band, stride, x0, x1, y0, y1);
    for (int i = 0; i < text_count_; i++) {
        const auto& t = text_[i];
        DrawText(band, stride, x0, x1, y0, y1, t.text, t.y, t.big, t.amber ? kAmber : kBone);
    }
    if (volume_arc_) porthole_volume_arc(band, stride, x0, x1, width_, y0, y1, volume_shown_);   // avatar_overlay.h
    // A small amber dot low in the sea: battery at 15% or less.
    if (low_battery_) porthole_dot(band, stride, x0, x1, y0, y1, width_ / 2.0f, height_ - 34.0f, 5.0f, kAmber, 1.0f);
}

void AvatarDisplay::RenderLoop() {
    int64_t last = esp_timer_get_time();
    stat_t0_ = last;
    while (true) {
        int64_t frame_start = esp_timer_get_time();
        float dt = (frame_start - last) / 1e6f;
        last = frame_start;

        PollTouch(frame_start);
        float period_ms = 50;
        ApplyInputs(frame_start, &period_ms);
        // True-black sky (pixels off) through every glide, including the renderer's own
        // hold-and-return transitions inside avatar_step (star -> steady).
        for (int i = 0; i < 3; i++) av_.tgt.sky[i] = 0;
        avatar_step(&av_, dt);
        for (int i = 0; i < 3; i++) av_.cur.sky[i] = av_.tgt.sky[i] = 0;
        avatar_settle(&av_);
        avatar_prepare(&av_, &frame_);

        // Cards: lay out this frame's text (on the sea, below the horizon's rest height) and find
        // what changed since the last frame: the text box, the dots and the rim progress.
        cards_.Layout(frame_start, (float)width_, width_ * (0.5f + av_.cur.level), &card_layer_);
        uint32_t csig;
        int cbox[4];
        porthole_card_damage(card_layer_, width_, height_, &csig, cbox);
        float carc = card_layer_.arc > 0 ? roundf(card_layer_.arc * 2000) / 2000 : -1;
        card_layer_.arc = carc;

        // Rows outside the active span only change with the sea/rim colours, the overlays, or
        // (rim rows only) the timer sweep and the volume level.
        uint32_t sig = avatar_static_sig(&av_);
        uint32_t osig = 2166136261u;
        int vol_text = -1;   // the volume number (big digits), redrawn on its own rows
        for (int i = 0; i < text_count_; i++) {
            if (text_[i].big) { vol_text = i; continue; }
            osig = Fnv(osig, text_[i].text.data(), text_[i].text.size());
            osig = Fnv(osig, &text_[i].y, sizeof text_[i].y);
        }
        osig = Fnv(osig, &volume_arc_, sizeof volume_arc_);
        osig = Fnv(osig, &low_battery_, sizeof low_battery_);
        int vsig = volume_arc_ ? volume_shown_ : -1;

        int a0, a1;
        ActiveRows(&a0, &a1);
        bool full = force_full_ || sig != static_sig_ || osig != overlay_sig_;
        avatar_dirty_shape(&av_, &frame_, &shape_);
        if (full) shape_prev_ = shape_;
        struct Span { int a, b; } spans[4];
        int nspans = 0;
        auto add_span = [&](int s0, int s1) {
            s0 = std::max(0, s0) / kBandRows * kBandRows;   // band-aligned, so always even
            s1 = std::min(height_, (std::min(s1, height_) + kBandRows - 1) / kBandRows * kBandRows);
            if (s1 > s0 && nspans < 4) spans[nspans++] = {s0, s1};
        };
        struct Rect { int x0, y0, x1, y1; } rects[4];
        int nrects = 0;
        auto add_rect = [&](int x0, int y0, int x1, int y1) {   // even-aligned for the CO5300
            x0 = std::max(0, x0) & ~1;
            y0 = std::max(0, y0) & ~1;
            x1 = std::min(width_, (x1 + 1) & ~1);
            y1 = std::min(height_, (y1 + 1) & ~1);
            if (x1 > x0 && y1 > y0 && nrects < 4) rects[nrects++] = {x0, y0, x1, y1};
        };
        int act0 = 0, act1 = 0;   // the active rows: rendered with column clipping
        if (full) {
            act1 = height_;
        } else {
            act0 = std::max(0, std::min(a0, dirty_prev_[0])) / kBandRows * kBandRows;
            act1 = std::min(height_, (std::max(a1, dirty_prev_[1]) + kBandRows - 1) / kBandRows * kBandRows);
            // The timer's amber sweep: the rim rows it moved across since the last frame.
            float sweep = av_.timer_on ? frame_.sweep : -1;
            if (sweep >= 0 && prev_sweep_ >= 0 && sweep != prev_sweep_) {
                int s0, s1;
                avatar_sweep_rows(&frame_, prev_sweep_, sweep, &s0, &s1);
                add_span(s0 * scale_, s1 * scale_);
            }
            // Cards: the old and new text boxes (only their columns), the rim between the old and new progress.
            if (csig != card_sig_) {
                int b[4] = {cbox[0], cbox[1], cbox[2], cbox[3]};
                if (card_box_[2] > card_box_[0]) {
                    b[0] = std::min(b[0], card_box_[0]); b[1] = std::min(b[1], card_box_[1]);
                    b[2] = std::max(b[2], card_box_[2]); b[3] = std::max(b[3], card_box_[3]);
                }
                card_rect_[0] = std::max(0, b[0]) & ~1;
                card_rect_[1] = std::max(0, b[1]) & ~1;
                card_rect_[2] = std::min(width_, (b[2] + 1) & ~1);
                card_rect_[3] = std::min(height_, (b[3] + 1) & ~1);
            }
            // A rim arc's end moved (a card's progress, the volume level): only the rim's box between the
            // old and new end can change (porthole_rim_arc_box), not whole rows.
            if (carc != card_arc_drawn_) {
                int b[4];
                porthole_rim_arc_box(width_, height_, 2 * kPi * std::max(0.0f, carc), 2 * kPi * std::max(0.0f, card_arc_drawn_), b);
                add_rect(b[0], b[1], b[2], b[3]);
            }
            // A new volume level: the rim between the old and new level, and the number's box.
            if (vsig >= 0 && volume_sig_ >= 0 && vsig != volume_sig_) {
                int b[4];
                porthole_rim_arc_box(width_, height_, 2 * kPi * volume_sig_ / 100.0f, 2 * kPi * vsig / 100.0f, b);
                add_rect(b[0], b[1], b[2], b[3]);
                if (vol_text >= 0) {
                    int tw = std::max(TextWidth(pf_big, std::to_string(volume_sig_)), TextWidth(pf_big, text_[vol_text].text));
                    add_rect((width_ - tw) / 2 - 8, text_[vol_text].y, (width_ + tw) / 2 + 8, text_[vol_text].y + pf_big.line_h);
                }
            }
        }
        prev_sweep_ = av_.timer_on ? frame_.sweep : -1;
        volume_sig_ = vsig;
        // Sort and merge overlapping or touching spans, then render each.
        for (int i = 1; i < nspans; i++)   // insertion sort (at most 4 spans)
            for (int k = i; k > 0 && spans[k].a < spans[k - 1].a; k--) std::swap(spans[k], spans[k - 1]);
        int merged = 0;
        for (int i = 0; i < nspans; i++) {
            if (merged > 0 && spans[i].a <= spans[merged - 1].b) spans[merged - 1].b = std::max(spans[merged - 1].b, spans[i].b);
            else spans[merged++] = spans[i];
        }
        if (act1 > act0) RenderRows(act0, act1, !full);
        if (!full && card_rect_[2] > card_rect_[0] && card_rect_[3] > card_rect_[1])
            RenderRows(card_rect_[1], card_rect_[3], false, card_rect_[0], card_rect_[2]);
        card_rect_[2] = card_rect_[0] = 0;
        card_sig_ = csig;
        card_arc_drawn_ = carc;
        if (cbox[2] > cbox[0]) std::copy(cbox, cbox + 4, card_box_);
        else card_box_[0] = card_box_[2] = 0;
        for (int i = 0; i < merged; i++) RenderRows(spans[i].a, spans[i].b, false);   // timer sweeps: whole rows
        for (int i = 0; i < nrects; i++) RenderRows(rects[i].y0, rects[i].y1, false, rects[i].x0, rects[i].x1);
        shape_prev_ = shape_;
        static_sig_ = sig;
        overlay_sig_ = osig;
        force_full_ = false;
        dirty_prev_[0] = a0;
        dirty_prev_[1] = a1;

        int64_t frame_end = esp_timer_get_time();
        int64_t frame_us = frame_end - frame_start;
        stat_frames_++;
        stat_full_ += full;
        stat_max_us_ = std::max(stat_max_us_, frame_us);
        if (frame_end - stat_t0_ >= 5000000) {
            float secs = (frame_end - stat_t0_) / 1e6f;
            float avg_ms = (stat_render_us_ + stat_flush_us_) / 1000.0f / std::max(1, stat_frames_);
            ESP_LOGI(TAG, "%.1f fps, render %.1f ms (overlays %.1f) + flush wait %.1f ms per frame (helper wait %.1f ms, max frame %.1f ms), %d rows/frame (%d kpx), %d full, scale %d, state %s, touch errors %d",
                     stat_frames_ / secs, stat_render_us_ / 1000.0f / std::max(1, stat_frames_),
                     stat_overlay_us_ / 1000.0f / std::max(1, stat_frames_),
                     stat_flush_us_ / 1000.0f / std::max(1, stat_frames_),
                     stat_helper_wait_us_ / 1000.0f / std::max(1, stat_frames_), stat_max_us_ / 1000.0f,
                     stat_rows_ / std::max(1, stat_frames_), (int)(stat_px_ / 1000 / std::max(1, stat_frames_)), stat_full_, scale_,
                     avatar_state_name(&av_),
                     stat_touch_errors_);
            if (auto_scale_ && scale_ == 1 && avg_ms > kFullResBudgetMs) {
                ESP_LOGW(TAG, "full-resolution frames average %.1f ms: switching to half resolution", avg_ms);
                // Tripping again soon after a recovery doubles how long frames must stay cheap before the next try.
                recover_need_s_ = frame_end - scale_up_us_ < 30000000 ? std::min(recover_need_s_ * 2, 60) : 3;
                recover_ok_s_ = 0;
                SetScale(2);
            }
            stat_t0_ = frame_end;
            stat_frames_ = stat_rows_ = stat_full_ = stat_touch_errors_ = 0;
            stat_px_ = 0;
            stat_render_us_ = stat_flush_us_ = stat_max_us_ = stat_helper_wait_us_ = stat_overlay_us_ = 0;
        }
        // Back to full resolution once half-resolution frames have stayed cheap (under 40% of the budget,
        // render + flush wait) for recover_need_s_ seconds in a row, judged one second at a time.
        recover_frames_++;
        recover_us_ += frame_us;
        if (frame_end - recover_t0_ >= 1000000) {
            float avg = recover_us_ / 1000.0f / std::max(1, recover_frames_);
            if (auto_scale_ && scale_ == 2) {
                recover_ok_s_ = avg < 0.4f * kFullResBudgetMs ? recover_ok_s_ + 1 : 0;
                if (recover_ok_s_ >= recover_need_s_) {
                    ESP_LOGI(TAG, "half-resolution frames average %.1f ms for %d s: back to full resolution", avg, recover_ok_s_);
                    scale_up_us_ = frame_end;
                    recover_ok_s_ = 0;
                    SetScale(1);
                }
            }
            recover_t0_ = frame_end;
            recover_frames_ = 0;
            recover_us_ = 0;
        }

        // Sleep until the next frame is due (at least 1 ms, so idle tasks and the watchdog run).
        int64_t wait_us = (int64_t)(period_ms * 1000) - (esp_timer_get_time() - frame_start);
        esp_timer_start_once(pace_timer_, std::max<int64_t>(wait_us, 1000));
        xSemaphoreTake(pace_, portMAX_DELAY);
    }
}

// ------------------------------------------------------------------ card client
// Between conversations the bridge's WebSocket is closed, so poll the live card set over HTTP with
// the WebSocket's credentials (avatar-spec PROTOCOL.md "Cards"). During a conversation the bridge
// pushes cards on the WebSocket instead (Application -> ShowCard). Card events are posted.

void AvatarDisplay::CardClientEntry(void* arg) { static_cast<AvatarDisplay*>(arg)->CardClientLoop(); }

bool AvatarDisplay::CardHttp(const char* method, const std::string& path, const std::string& body, std::string* reply) {
    Settings wifi("wifi", false);
    std::string url = wifi.GetString("ota_url");
    if (url.empty()) url = CONFIG_OTA_URL;
    size_t k = url.rfind("/ota");
    if (k == std::string::npos) return false;
    url = url.substr(0, k) + path;   // .../xiaozhi/ota/ -> .../xiaozhi/cards/
    Settings ws("websocket", false);
    std::string token = ws.GetString("token");
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return false;
    auto http = network->CreateHttp(0);
    if (!http) return false;
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!token.empty()) http->SetHeader("Authorization", (token.find(' ') == std::string::npos ? "Bearer " + token : token).c_str());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    if (!body.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::string(body));
    }
    if (!http->Open(method, url)) return false;
    auto status = http->GetStatusCode();
    bool ok = status && *status == 200;
    if (ok && reply) *reply = http->ReadAll();
    http->Close();
    if (!ok) ESP_LOGW(TAG, "cards %s %s: %d", method, path.c_str(), status ? *status : -1);
    return ok;
}

void AvatarDisplay::CardClientLoop() {
    int poll_s = 30;
    int64_t next_poll = esp_timer_get_time() + 20000000;   // let Wi-Fi and the OTA check settle first
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        DeviceState ds;
        bool ps;
        {
            std::lock_guard<std::mutex> lk(in_mutex_);
            ds = device_state_;
            ps = power_save_;
        }
        const bool online = ds == kDeviceStateIdle || ds == kDeviceStateListening || ds == kDeviceStateSpeaking;
        std::vector<std::pair<std::string, std::string>> events;
        {
            std::lock_guard<std::mutex> lk(card_ev_mutex_);
            events.swap(card_events_);
        }
        for (const auto& e : events) {
            if (!online) break;
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "type", "card_event");
            cJSON_AddStringToObject(o, "id", e.first.c_str());
            cJSON_AddStringToObject(o, "event", e.second.c_str());
            char* js = cJSON_PrintUnformatted(o);
            if (js) { CardHttp("POST", "/cards/event", js, nullptr); cJSON_free(js); }
            cJSON_Delete(o);
            next_poll = std::min(next_poll, esp_timer_get_time() + 1500000);   // pick up the bridge's reaction
        }
        int64_t now = esp_timer_get_time();
        if (ds != kDeviceStateIdle || now < next_poll) continue;
        std::string reply;
        if (CardHttp("GET", "/cards/", "", &reply)) {
            cJSON* root = cJSON_Parse(reply.c_str());
            if (root) {
                const cJSON* p = cJSON_GetObjectItem(root, "poll");
                if (cJSON_IsNumber(p)) poll_s = std::clamp(p->valueint, 10, 600);
                cJSON_Delete(root);
                std::lock_guard<std::mutex> lk(in_mutex_);
                card_poll_ = std::move(reply);
            }
        }
        next_poll = esp_timer_get_time() + (int64_t)(ps ? std::max(poll_s, 120) : poll_s) * 1000000;
    }
}

