// Skipper Porthole: Skipper's avatar (hardware/avatar-render in skipper-ai) on the round AMOLED,
// in place of the stock LVGL emoji/chat UI. Renders in row bands straight to DMA buffers and
// only redraws the rows that changed. See README.md in this directory.
#pragma once

#include "display.h"
#include "device_state.h"
#include "avatar.h"
#include "avatar_dirty.h"
#include "avatar_cards.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class AvatarDisplay : public Display {
public:
    AvatarDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    ~AvatarDisplay() override;

    // Starts the render task. touch may be null.
    void Start(esp_lcd_touch_handle_t touch);

    // Display
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetPowerSaveMode(bool on) override;
    void ShowCard(const std::string& json) override;   // any task

    // Board hooks (any task)
    void OnDeviceState(DeviceState state);
    void SetThinking(bool thinking);
    // Playback PCM as it goes to the codec (audio task); drives the live voice layer.
    void FeedPlayback(const int16_t* pcm, size_t samples, int channels, int sample_rate);
    void ShowVolume(int volume);          // shows the level indicator briefly
    void SetCurrentVolume(int volume) { current_volume_ = volume; }
    void SetBattery(int level, bool charging, bool discharging);
    // Called from the render task when a touch gesture asks for a new volume (0..100).
    void OnVolumeGesture(std::function<void(int volume)> cb) { volume_cb_ = std::move(cb); }
    // Called from the render task on a long press (e.g. to refresh the battery reading).
    void OnLongPress(std::function<void()> cb) { long_press_cb_ = std::move(cb); }
    // Called from the render task when a touch starts (e.g. to wake the power-save timer).
    void OnTouchActivity(std::function<void()> cb) { activity_cb_ = std::move(cb); }

protected:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

private:
    static constexpr int kBandRows = 16;     // rows per DMA band (must be even for the CO5300)
    static constexpr int kBuffers = 3;       // DMA band buffers: rendering runs ahead of the QSPI transfers

    static constexpr int kMaxTextItems = 8;
    struct TextItem {
        std::string text;
        int y = 0;               // top of the line
        bool big = false;        // big digits font (volume)
        bool amber = false;
    };

    esp_lcd_panel_io_handle_t panel_io_;
    esp_lcd_panel_handle_t panel_;
    esp_lcd_touch_handle_t touch_ = nullptr;
    SemaphoreHandle_t lock_ = nullptr;        // recursive: panel access (render loop, backlight)
    SemaphoreHandle_t buf_free_ = nullptr;    // counts DMA buffers not in flight
    uint16_t* buf_[kBuffers] = {};
    int buf_next_ = 0;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t helper_ = nullptr;   // second core: renders the odd rows of each band
    SemaphoreHandle_t pace_ = nullptr;          // frame pacing: given by pace_timer_
    esp_timer_handle_t pace_timer_ = nullptr;   // (vTaskDelay is 10 ms coarse at 100 Hz)
    struct RenderJob { uint16_t* fb; int stride, y0, y1, x0, x1; };
    RenderJob job_ = {};
    std::atomic<int> next_row_{0};    // both cores take the band's rows from here
    void RenderShared();
    // Finished bands go to a small flush task: esp_lcd's draw call waits for the previous band's DMA
    // (and sends the window commands by polling), which would otherwise stall a rendering core.
    struct FlushJob { uint16_t* buf; int x0, y0, x1, y1; };
    QueueHandle_t flush_q_ = nullptr;
    TaskHandle_t flusher_ = nullptr;
    static void FlusherEntry(void* arg);

    // Read by every pixel: keep it in internal RAM (the display object itself may land in PSRAM).
    avatar_t& av_;
    avatar_frame_t& frame_;            // avatar_prepare'd once per frame, read by both cores

    // Inputs from other tasks, guarded by in_mutex_.
    std::mutex in_mutex_;
    DeviceState device_state_ = kDeviceStateStarting;
    bool thinking_ = false;
    bool power_save_ = false;
    std::string mood_;                 // avatar state requested by the bridge (llm emotion)
    std::string status_;
    std::string system_msg_;
    bool error_ = false;
    int64_t error_until_us_ = 0;
    std::string notification_;
    int64_t notification_until_us_ = 0;
    int volume_ = -1;
    std::atomic<int> current_volume_{70};
    int64_t volume_until_us_ = 0;
    int battery_ = -1;
    bool charging_ = false, discharging_ = false;
    int64_t battery_until_us_ = 0;
    int taps_ = 0;
    std::vector<std::string> card_msgs_;   // pushed {"type":"card",…} messages, oldest first
    std::string card_poll_;                // the latest poll reply (the whole live set)

    // Playback level ring: 10 ms windows with the time they reach the speaker.
    static constexpr int kLevelRing = 64;
    struct LevelSample { int64_t at_us; float level; };
    LevelSample levels_[kLevelRing] = {};
    std::atomic<uint32_t> level_head_{0};
    int64_t playback_clock_us_ = 0;   // audio task only

    // Render task state.
    std::string requested_state_;
    uint32_t level_tail_ = 0;
    int dirty_prev_[2] = {0, 0};   // previous frame's active row span
    avatar_dirty_t shape_ = {}, shape_prev_ = {};   // where this / the last frame could change
    float prev_sweep_ = -1;        // previous frame's timer sweep angle, -1 = no timer
    uint32_t static_sig_ = 0;
    uint32_t overlay_sig_ = 0;
    TextItem text_[kMaxTextItems];
    int text_count_ = 0;
    bool volume_arc_ = false;
    int volume_shown_ = 0;
    int volume_sig_ = -1;           // volume level last drawn (-1: no volume arc)
    bool low_battery_ = false;
    int scale_ = 1;                 // 1 = full resolution, 2 = half resolution, pixel-doubled
    bool auto_scale_ = true;
    uint16_t* half_buf_ = nullptr;
    float last_level_ = 0;
    int64_t last_level_at_us_ = 0;
    int64_t last_battery_poll_us_ = 0;
    bool force_full_ = true;
    std::function<void(int)> volume_cb_;
    std::function<void()> long_press_cb_;
    std::function<void()> activity_cb_;

    // Cards (render task).
    CardSet cards_;
    CardLayer card_layer_;
    uint32_t card_sig_ = 0;
    int card_box_[4] = {0, 0, 0, 0};
    int card_rect_[4] = {0, 0, 0, 0};  // this frame's card redraw rectangle (x0, y0, x1, y1)   // last drawn card text box (x0, y0, x1, y1), empty if x1 <= x0
    float card_arc_drawn_ = -1;        // rim progress last drawn (-1: none)
    std::string card_mood_id_;         // card whose mood/params were applied
    bool card_mood_ = false;           // the avatar state comes from a card
    std::vector<CardParam> card_params_;   // that card's `set`
    float card_drain_ = -1;            // a running timer card's tide level (-1: none)
    // Card client task: polls GET <bridge>/xiaozhi/cards/ between conversations, posts card events.
    TaskHandle_t card_task_ = nullptr;
    std::mutex card_ev_mutex_;
    std::vector<std::pair<std::string, std::string>> card_events_;
    static void CardClientEntry(void* arg);
    void CardClientLoop();
    bool CardHttp(const char* method, const std::string& path, const std::string& body, std::string* reply);

    // Touch gesture state.
    bool touching_ = false;
    int64_t touch_t0_ = 0;
    float touch_x0_ = 0, touch_y0_ = 0, touch_x_ = 0, touch_y_ = 0;
    bool gesture_dial_ = false, gesture_swipe_ = false, gesture_long_ = false, gesture_moved_ = false, gesture_card_ = false;
    float dial_angle_ = 0, dial_accum_ = 0;
    int gesture_volume_ = -1, gesture_base_ = 0;
    bool touch_error_logged_ = false;

    // Stats.
    int64_t stat_t0_ = 0;
    int stat_frames_ = 0, stat_rows_ = 0, stat_full_ = 0, stat_touch_errors_ = 0;
    int64_t stat_px_ = 0;
    int64_t stat_render_us_ = 0, stat_flush_us_ = 0, stat_max_us_ = 0, stat_helper_wait_us_ = 0, stat_overlay_us_ = 0;

    // Resolution fallback: full resolution drops to half when 5 s of frames average over the budget, and
    // comes back after recover_need_s_ cheap seconds (3, doubling up to 60 if it trips again within 30 s).
    static constexpr float kFullResBudgetMs = 55;
    int64_t recover_t0_ = 0, recover_us_ = 0, scale_up_us_ = -60000000;
    int recover_frames_ = 0, recover_ok_s_ = 0, recover_need_s_ = 3;

    static bool OnTransDone(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* edata, void* ctx);
    static void TaskEntry(void* arg);
    static void HelperEntry(void* arg);
    void RenderSplit(uint16_t* fb, int stride, int y0, int y1, int x0, int x1);
    void RenderLoop();
    void PollTouch(int64_t now_us);
    void ApplyInputs(int64_t now_us, float* frame_period_ms);
    float PlaybackLevel(int64_t now_us);
    void ActiveRows(int* y0, int* y1);
    // clip: only the column spans avatar_band_spans allows; cx0 >= 0: exactly columns [cx0, cx1).
    void RenderRows(int y0, int y1, bool clip, int cx0 = -1, int cx1 = -1);
    // Overlays over rows [y0, y1), columns [x0, x1) of a band (stride px per row, column x0 at band[0]).
    void DrawOverlays(uint16_t* band, int stride, int x0, int x1, int y0, int y1);
    void DrawText(uint16_t* band, int stride, int x0, int x1, int y0, int y1, const std::string& text, int y_top, bool big,
                  const float rgb[3]);
    void DrawCards(uint16_t* band, int stride, int x0, int x1, int y0, int y1);
    void UpdateCards(int64_t now_us, bool idle, std::string* want);
    void BuildText(int64_t now_us);
    int AddWrapped(const std::string& text, int y, int max_width, int max_lines, bool amber);
    void SetScale(int scale);
};
