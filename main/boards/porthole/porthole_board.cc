// Skipper Porthole: Waveshare ESP32-S3-Touch-AMOLED-1.75C (CO5300 466x466 QSPI AMOLED, CST9217 touch,
// AXP2101, ES8311 + ES7210) running the Skipper Pod firmware with Skipper's avatar on the screen.
// Derived from boards/waveshare/esp32-s3-touch-amoled-1.75 (1.75C pins). No LVGL UI: AvatarDisplay
// draws straight to the panel. See README.md.
#include "wifi_board.h"
#include "esp_lcd_co5300.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/led.h"
#include "backlight.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "avatar_display.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_touch_cst9217.h>

#define TAG "Porthole"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off
        WriteReg(0x80, 0x01);  // disable all DCs but DC1
        WriteReg(0x90, 0x00);  // disable all LDOs
        WriteReg(0x91, 0x00);
        WriteReg(0x82, (3300 - 1500) / 100); // DC1 3.3V
        WriteReg(0x92, (3300 - 500) / 100);  // ALDO1 3.3V
        WriteReg(0x90, 0x01);  // enable ALDO1 (mic)
        WriteReg(0x64, 0x02);  // CV charger voltage 4.1V
        WriteReg(0x61, 0x02);  // precharge current 50mA
        WriteReg(0x62, 0x08);  // charger current 200mA
        WriteReg(0x63, 0x01);  // term charge current 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // set display to qspi mode
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

class PortholeBacklight : public Backlight {
public:
    explicit PortholeBacklight(esp_lcd_panel_io_handle_t panel_io) : panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {(uint8_t)((255 * brightness) / 100)};
        int lcd_cmd = (0x51 & 0xff) << 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

// The board has no LEDs; this "LED" forwards device states and the bridge's thinking hint to the avatar.
class AvatarStateLed : public Led {
public:
    explicit AvatarStateLed(AvatarDisplay* display) : display_(display) {}
    void OnStateChanged() override { display_->OnDeviceState(Application::GetInstance().GetDeviceState()); }
    void SetThinking(bool thinking) override { display_->SetThinking(thinking); }

private:
    AvatarDisplay* display_;
};

// Taps the playback PCM for the avatar's live voice layer and shows volume changes from any source
// (touch dial, MCP self.audio_speaker.set_volume).
class PortholeAudioCodec : public BoxAudioCodec {
public:
    using BoxAudioCodec::BoxAudioCodec;
    AvatarDisplay* display = nullptr;

    void OutputData(std::vector<int16_t>& data) override {
        if (display) display->FeedPlayback(data.data(), data.size(), output_channels(), output_sample_rate());
        BoxAudioCodec::OutputData(data);
    }

    void SetOutputVolume(int volume) override {
        bool changed = volume != output_volume();
        BoxAudioCodec::SetOutputVolume(volume);
        if (display) {
            display->SetCurrentVolume(volume);
            if (changed) display->ShowVolume(volume);
        }
    }
};

class SkipperPorthole : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    AvatarDisplay* display_ = nullptr;
    PortholeBacklight* backlight_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() { pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = LCD_PIN_PCLK;
        buscfg.data0_io_num = LCD_PIN_DATA0;
        buscfg.data1_io_num = LCD_PIN_DATA1;
        buscfg.data2_io_num = LCD_PIN_DATA2;
        buscfg.data3_io_num = LCD_PIN_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * 16 * sizeof(uint16_t);   // one avatar band
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_PIN_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = LCD_PIN_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void*)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new AvatarDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        backlight_ = new PortholeBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = TOUCH_PIN_RST,
            .int_gpio_num = TOUCH_PIN_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 1,
                .mirror_y = 1,
            },
        };
        esp_lcd_panel_io_handle_t tp_io = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io));
        if (esp_lcd_touch_new_i2c_cst9217(tp_io, &tp_cfg, &touch_) != ESP_OK) {
            ESP_LOGE(TAG, "Touch controller init failed; continuing without touch");
            touch_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "Touch panel initialized");
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
    }

    void StartAvatar() {
        display_->OnVolumeGesture([this](int volume) {
            // Codec + NVS work happens on the main task, like the MCP volume tool.
            Application::GetInstance().Schedule([this, volume]() { GetAudioCodec()->SetOutputVolume(volume); });
        });
        display_->OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() { display_->UpdateStatusBar(true); });
        });
        display_->OnTouchActivity([this]() {
            Application::GetInstance().Schedule([this]() { power_save_timer_->WakeUp(); });
        });
        display_->Start(touch_);
    }

public:
    SkipperPorthole() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeI2c();
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        StartAvatar();
    }

    AudioCodec* GetAudioCodec() override {
        static PortholeAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        if (audio_codec.display == nullptr && display_ != nullptr) {
            audio_codec.display = display_;
            display_->SetCurrentVolume(audio_codec.output_volume());
        }
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override { return backlight_; }

    Led* GetLed() override {
        static AvatarStateLed led(display_);
        return &led;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = pmic_->GetBatteryLevel();
        return true;
    }

    void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(SkipperPorthole);
