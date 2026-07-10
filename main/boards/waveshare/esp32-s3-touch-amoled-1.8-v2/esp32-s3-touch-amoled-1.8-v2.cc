#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"
#include "wifi_board.h"

#include "application.h"
#include "axp2101.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "device_state_machine.h"
#include "i2c_device.h"
#include "led/led.h"
#include "mcp_server.h"
#include "power_save_timer.h"

#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <led_strip.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"

#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>

#define TAG "WaveshareEsp32s3TouchAMOLED1inch8"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110);  // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);   // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);

        WriteReg(0x64, 0x02);  // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02);  // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08);  // set Main battery charger current to 400mA ( 0x08-200mA,
                               // 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01);  // set Main battery term charge current to 25mA
    }

    void EnablePowerButtonShortPressIrq() {
        ClearIrqStatus();
        WriteReg(0x41, ReadReg(0x41) | 0x08);  // Enable AXP2101 PWRON short press IRQ
    }

    bool ConsumePowerButtonShortPressIrq() {
        uint8_t status = ReadReg(0x49);
        if (status != 0) {
            WriteReg(0x49, status);
        }
        return (status & 0x08) != 0;
    }

    void ClearIrqStatus() {
        WriteReg(0x48, 0xff);
        WriteReg(0x49, 0xff);
        WriteReg(0x4a, 0xff);
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600},  // Sleep out

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
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x29, (uint8_t[]){0x00}, 0, 600},
};

// 在waveshare_amoled_1_8类之前添加新的显示类
struct RgbwwColor {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint8_t warm_white = 0;
};

static constexpr uint8_t DEFAULT_LED_BRIGHTNESS = 65;
static constexpr uint8_t MAX_LED_BRIGHTNESS = 80;
static constexpr uint8_t MIN_LED_BRIGHTNESS = 5;

static int ClampLedBrightness(int brightness_percent) {
    return std::clamp(brightness_percent, static_cast<int>(MIN_LED_BRIGHTNESS),
                      static_cast<int>(MAX_LED_BRIGHTNESS));
}

static uint8_t ScaleLedChannel(uint8_t value, uint8_t brightness_percent) {
    brightness_percent = ClampLedBrightness(brightness_percent);
    return static_cast<uint8_t>((value * brightness_percent + 50) / 100);
}

static const RgbwwColor kInvalidRgbwwColor = {0xff, 0xff, 0xff, 0xff};
static const RgbwwColor kWarmWhiteColor = {0, 0, 0, 255};

static bool IsInvalidRgbwwColor(RgbwwColor color);
static std::string NormalizeLedColorName(const std::string& color);
static RgbwwColor ParseRgbwwColor(const std::string& color);

class Sk6812RgbwwStrip : public Led {
public:
    Sk6812RgbwwStrip(gpio_num_t gpio, uint16_t led_count) : led_count_(led_count) {
        led_strip_config_t strip_config = {};
        strip_config.strip_gpio_num = gpio;
        strip_config.max_leds = led_count_;
        strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW;
        strip_config.led_model = LED_MODEL_SK6812;

        led_strip_rmt_config_t rmt_config = {};
        rmt_config.resolution_hz = 10 * 1000 * 1000;

        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_));
        ESP_LOGI(TAG, "[LED] strip initialized gpio=%d count=%d", gpio, led_count_);
        SetDefaultWarmWhite();
        StartDelayedWarmWhiteRefresh();
    }

    ~Sk6812RgbwwStrip() override {
        if (strip_ != nullptr) {
            led_strip_del(strip_);
        }
    }

    void OnStateChanged() override {}

    bool TurnOn() {
        led_on_ = true;
        current_color_ = kWarmWhiteColor;
        current_color_name_ = "warm_white";
        if (current_brightness_ < MIN_LED_BRIGHTNESS) {
            current_brightness_ = DEFAULT_LED_BRIGHTNESS;
        }
        Apply();
        ESP_LOGI(TAG, "[LED] on color=%s brightness=%d", current_color_name_.c_str(),
                 current_brightness_);
        return true;
    }

    void TurnOff() {
        led_on_ = false;
        SetRaw({});
        ESP_LOGI(TAG, "[LED] off");
    }

    bool SetColor(const std::string& color) {
        auto parsed_color = ParseRgbwwColor(color);
        if (IsInvalidRgbwwColor(parsed_color)) {
            return false;
        }
        led_on_ = true;
        current_color_ = parsed_color;
        current_color_name_ = NormalizeLedColorName(color);
        Apply();
        ESP_LOGI(TAG, "[LED] set_color color=%s brightness=%d", current_color_name_.c_str(),
                 current_brightness_);
        return true;
    }

    void SetBrightness(int requested_brightness) {
        auto clamped_brightness = ClampLedBrightness(requested_brightness);
        if (requested_brightness != clamped_brightness) {
            ESP_LOGI(TAG, "[LED] brightness requested=%d clamped=%d", requested_brightness,
                     clamped_brightness);
        }
        current_brightness_ = static_cast<uint8_t>(clamped_brightness);
        led_on_ = true;
        Apply();
    }

    void IncreaseBrightness(int step) {
        SetBrightness(current_brightness_ + std::max(step, 1));
    }

    void DecreaseBrightness(int step) {
        SetBrightness(current_brightness_ - std::max(step, 1));
    }

    const std::string& current_color_name() const { return current_color_name_; }

    uint8_t current_brightness() const { return current_brightness_; }

    bool led_on() const { return led_on_; }

    void Clear() { TurnOff(); }

private:
    static void DelayedWarmWhiteRefreshTask(void* arg) {
        auto* self = static_cast<Sk6812RgbwwStrip*>(arg);
        const TickType_t delays[] = {
            pdMS_TO_TICKS(300),
            pdMS_TO_TICKS(700),
            pdMS_TO_TICKS(1500),
        };

        for (int i = 0; i < 3; ++i) {
            vTaskDelay(delays[i]);
            ESP_LOGI(TAG, "[LED] delayed refresh warm_white attempt=%d", i + 1);
            if (self->led_on_) {
                self->ApplyCurrentState();
            }
        }

        vTaskDelete(nullptr);
    }

    void StartDelayedWarmWhiteRefresh() {
        xTaskCreate(DelayedWarmWhiteRefreshTask, "led_warm_refresh", 3072, this, 2, nullptr);
    }

    void SetDefaultWarmWhite() {
        led_on_ = true;
        current_color_ = kWarmWhiteColor;
        current_color_name_ = "warm_white";
        current_brightness_ = DEFAULT_LED_BRIGHTNESS;
        ApplyCurrentState();
        ESP_LOGI(TAG, "[LED] default warm_white brightness=%d", current_brightness_);
    }

    void SetRaw(RgbwwColor color) {
        if (strip_ == nullptr) {
            return;
        }
        for (uint16_t i = 0; i < led_count_; ++i) {
            ESP_ERROR_CHECK(led_strip_set_pixel_rgbw(strip_, i, color.red, color.green,
                                                     color.blue, color.warm_white));
        }
        ESP_ERROR_CHECK(led_strip_refresh(strip_));
    }

    RgbwwColor ScaleCurrentColor() const {
        return {ScaleLedChannel(current_color_.red, current_brightness_),
                ScaleLedChannel(current_color_.green, current_brightness_),
                ScaleLedChannel(current_color_.blue, current_brightness_),
                ScaleLedChannel(current_color_.warm_white, current_brightness_)};
    }

    void ApplyCurrentState() {
        if (!led_on_) {
            SetRaw({});
            return;
        }
        SetRaw(ScaleCurrentColor());
    }

    void Apply() { ApplyCurrentState(); }

    led_strip_handle_t strip_ = nullptr;
    uint16_t led_count_ = 0;
    bool led_on_ = true;
    RgbwwColor current_color_ = kWarmWhiteColor;
    uint8_t current_brightness_ = DEFAULT_LED_BRIGHTNESS;
    std::string current_color_name_ = "warm_white";
};

static std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool IsHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint8_t HexByte(const std::string& value, size_t offset) {
    return static_cast<uint8_t>(std::stoi(value.substr(offset, 2), nullptr, 16));
}

static bool IsInvalidRgbwwColor(RgbwwColor color) {
    return color.red == 0xff && color.green == 0xff && color.blue == 0xff &&
           color.warm_white == 0xff;
}

static std::string NormalizeLedColorName(const std::string& color) {
    auto normalized_color = ToLower(color);
    if (normalized_color == "white" || normalized_color == "ww") {
        return "warm_white";
    }
    return normalized_color;
}

static RgbwwColor ParseRgbwwColor(const std::string& color) {
    if (color.length() == 7 && color[0] == '#') {
        for (size_t i = 1; i < color.length(); ++i) {
            if (!IsHexDigit(color[i])) {
                return kInvalidRgbwwColor;
            }
        }
        return {HexByte(color, 1), HexByte(color, 3), HexByte(color, 5), 0};
    }

    auto normalized_color = NormalizeLedColorName(color);
    if (normalized_color == "trucchi_warm") {
        return {255, 180, 80, 200};
    }
    if (normalized_color == "error_color") {
        return {220, 60, 20, 0};
    }
    if (normalized_color == "warm_white") {
        return kWarmWhiteColor;
    }

    if (normalized_color == "red") {
        return {255, 0, 0, 0};
    }
    if (normalized_color == "green") {
        return {0, 255, 0, 0};
    }
    if (normalized_color == "blue") {
        return {0, 0, 255, 0};
    }
    if (normalized_color == "yellow") {
        return {255, 255, 0, 0};
    }
    if (normalized_color == "purple") {
        return {255, 0, 255, 0};
    }
    if (normalized_color == "cyan") {
        return {0, 255, 255, 0};
    }
    return kInvalidRgbwwColor;
}

class CustomLcdDisplay : public SpiLcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                     int width, int height, int offset_x, int offset_y, bool mirror_x,
                     bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x,
                        mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.1, 0);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255 * brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED1inch8 : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    Sk6812RgbwwStrip* led_strip_ = nullptr;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;
    TaskHandle_t power_button_task_handle_ = nullptr;
    TaskHandle_t auto_start_task_handle_ = nullptr;
    bool auto_listen_enabled_ = true;
    bool auto_start_in_progress_ = false;
    bool boot_auto_start_done_ = false;
    bool user_manual_stop_ = false;
    int64_t last_auto_start_us_ = 0;
    bool returning_to_brookesia_ = false;

    static void PowerButtonTask(void* arg) {
        auto* self = static_cast<WaveshareEsp32s3TouchAMOLED1inch8*>(arg);
        while (true) {
            if (self->pmic_ != nullptr && self->pmic_->ConsumePowerButtonShortPressIrq()) {
                self->ReturnToBrookesia();
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        self->power_button_task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }

    static void AutoStartListeningTask(void* arg) {
        auto* self = static_cast<WaveshareEsp32s3TouchAMOLED1inch8*>(arg);
        self->RunAutoStartListeningLoop();
        self->auto_start_task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }

    bool TryAutoStartListening(const char* start_log) {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (state != kDeviceStateIdle) {
            ESP_LOGI(TAG, "[AUTO] skip auto-start because state=%s",
                     DeviceStateMachine::GetStateName(state));
            return false;
        }

        int64_t now_us = esp_timer_get_time();
        if (auto_start_in_progress_ && now_us - last_auto_start_us_ < 1500 * 1000) {
            return false;
        }
        if (last_auto_start_us_ != 0 && now_us - last_auto_start_us_ < 1500 * 1000) {
            return false;
        }

        auto_start_in_progress_ = true;
        last_auto_start_us_ = now_us;
        if (start_log != nullptr) {
            ESP_LOGI(TAG, "%s", start_log);
        }
        app.StartListening(kListeningModeAutoStop);
        return true;
    }

    void RunAutoStartListeningLoop() {
        auto& app = Application::GetInstance();

        vTaskDelay(pdMS_TO_TICKS(2000));
        if (auto_listen_enabled_ && !user_manual_stop_) {
            boot_auto_start_done_ =
                TryAutoStartListening("[AUTO] boot auto-start listening") || boot_auto_start_done_;
        }

        DeviceState last_state = app.GetDeviceState();
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(250));

            auto state = app.GetDeviceState();
            if (state != kDeviceStateIdle) {
                auto_start_in_progress_ = false;
            }

            if (state != last_state) {
                if (state == kDeviceStateIdle && auto_listen_enabled_ && !user_manual_stop_) {
                    if (boot_auto_start_done_) {
                        ESP_LOGI(TAG, "[AUTO] state idle, restart listening after goodbye");
                    }
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    if (auto_listen_enabled_ && !user_manual_stop_) {
                        const char* start_log =
                            boot_auto_start_done_ ? nullptr : "[AUTO] boot auto-start listening";
                        boot_auto_start_done_ =
                            TryAutoStartListening(start_log) || boot_auto_start_done_;
                    }
                    state = app.GetDeviceState();
                }
                last_state = state;
            }
        }
    }

    void ReturnToBrookesia() {
        if (returning_to_brookesia_) {
            return;
        }
        returning_to_brookesia_ = true;

        const esp_partition_t* factory = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
        if (factory == nullptr) {
            ESP_LOGE(TAG, "Factory partition not found");
            returning_to_brookesia_ = false;
            return;
        }

        esp_err_t err = esp_ota_set_boot_partition(factory);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
            returning_to_brookesia_ = false;
            return;
        }

        ESP_LOGI(TAG, "PWR short press: return to Brookesia factory partition");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }

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
        // power_save_timer_->OnShutdownRequest([this]() { pmic_->PowerOff(); });
        power_save_timer_->OnShutdownRequest([this]() {
            if (pmic_ != nullptr) {
                pmic_->PowerOff();
            }
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(codec_i2c_bus_, I2C_ADDRESS, &io_expander);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(
            io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2,
            IO_EXPANDER_OUTPUT);
        ret |= esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        ESP_ERROR_CHECK(ret);
        ret = esp_io_expander_set_level(
            io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, 1);
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = esp_io_expander_set_level(
            io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, 0);
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(
            io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, 1);
        ESP_ERROR_CHECK(ret);
    }

    // void InitializeAxp2101() {
    //     ESP_LOGI(TAG, "Init AXP2101");
    //     pmic_ = new Pmic(codec_i2c_bus_, 0x34);
    //     pmic_->EnablePowerButtonShortPressIrq();
    // }
    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");

        vTaskDelay(pdMS_TO_TICKS(50));

        esp_err_t ret = ESP_FAIL;

        for (int i = 0; i < 3; ++i) {
            ret = i2c_master_probe(codec_i2c_bus_, 0x34, 200);
            if (ret == ESP_OK) {
                break;
            }

            ESP_LOGW(TAG, "AXP2101 probe retry %d failed: %s", i + 1, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "AXP2101 not detected at 0x34, skip PMIC init to avoid bootloop");
            pmic_ = nullptr;
            return;
        }

        pmic_ = new Pmic(codec_i2c_bus_, 0x34);

        if (pmic_ == nullptr) {
            ESP_LOGE(TAG, "Create PMIC object failed");
            return;
        }

        pmic_->EnablePowerButtonShortPressIrq();

        ESP_LOGI(TAG, "AXP2101 initialized");
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = GPIO_NUM_11;
        buscfg.data0_io_num = GPIO_NUM_4;
        buscfg.data1_io_num = GPIO_NUM_5;
        buscfg.data2_io_num = GPIO_NUM_6;
        buscfg.data3_io_num = GPIO_NUM_7;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }

            if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                state == kDeviceStateSpeaking) {
                user_manual_stop_ = true;
                ESP_LOGI(TAG, "[AUTO] user manual stop, auto-listen paused");
            } else if (state == kDeviceStateIdle) {
                user_manual_stop_ = false;
            }

            app.ToggleChatState();
        });

        xTaskCreate(PowerButtonTask, "pwr_button", 4096, this, 5, &power_button_task_handle_);
    }

    void InitializeAutoStartChat() {
        xTaskCreate(AutoStartListeningTask, "auto_listen", 4096, this, 2, &auto_start_task_handle_);
    }

    void InitializeLedStrip() {
        led_strip_ = new Sk6812RgbwwStrip(LUMI_LED_DATA_PIN, LUMI_LED_COUNT);
    }

    ReturnValue HandleLedControl(const PropertyList& properties) {
        if (led_strip_ == nullptr) {
            return std::string("Error: LED strip is not initialized");
        }

        auto action = ToLower(properties["action"].value<std::string>());
        auto status = ToLower(properties["status"].value<std::string>());
        auto color = ToLower(properties["color"].value<std::string>());
        auto requested_brightness = properties["brightness"].value<int>();
        auto step = properties["step"].value<int>();

        if (status == "off") {
            action = "off";
        } else if (status != "on") {
            return std::string("Lỗi: status '") + status + "' không hợp lệ. Dùng: on/off";
        }

        if (action == "off") {
            led_strip_->TurnOff();
            return std::string("LED đã tắt");
        }

        if (action == "on") {
            led_strip_->TurnOn();
            return std::string("LED bật màu ") + led_strip_->current_color_name() +
                   ", độ sáng " + std::to_string(led_strip_->current_brightness());
        }

        if (action == "set_color") {
            if (!led_strip_->SetColor(color)) {
                return std::string("Lỗi: màu '") + color +
                       "' không hợp lệ. Dùng: "
                       "warm_white/ww/red/green/blue/yellow/purple/cyan/#RRGGBB";
            }
            return std::string("LED bật màu ") + led_strip_->current_color_name() +
                   ", độ sáng " + std::to_string(led_strip_->current_brightness());
        }

        if (action == "set_brightness") {
            led_strip_->SetBrightness(requested_brightness);
            return std::string("LED bật màu ") + led_strip_->current_color_name() +
                   ", độ sáng " + std::to_string(led_strip_->current_brightness());
        }

        if (action == "increase_brightness") {
            led_strip_->IncreaseBrightness(step);
            return std::string("LED bật màu ") + led_strip_->current_color_name() +
                   ", độ sáng " + std::to_string(led_strip_->current_brightness());
        }

        if (action == "decrease_brightness") {
            led_strip_->DecreaseBrightness(step);
            return std::string("LED bật màu ") + led_strip_->current_color_name() +
                   ", độ sáng " + std::to_string(led_strip_->current_brightness());
        }

        return std::string("Lỗi: action '") + action +
               "' không hợp lệ. Dùng: "
               "on/off/set_color/set_brightness/increase_brightness/decrease_brightness";
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config =
            CO5300_PANEL_IO_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_CS, nullptr, nullptr);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void*)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                        DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = GPIO_NUM_39,
            .int_gpio_num = GPIO_NUM_13,
            .levels =
                {
                    .reset = 0,
                    .interrupt = 0,
                },
            .flags =
                {
                    .swap_xy = 0,
                    .mirror_x = 0,
                    .mirror_y = 0,
                },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
            .on_color_trans_done = 0,
            .user_ctx = 0,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 0,
            .flags =
                {
                    .dc_low_on_data = 0,
                    .disable_control_phase = 1,
                },
        };
        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(codec_i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 初始化工具
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
                           "End this conversation and enter WiFi configuration mode.\n"
                           "**CAUTION** You must ask the user to confirm this action.",
                           PropertyList(), [this](const PropertyList& properties) {
                               EnterWifiConfigMode();
                               return true;
                           });
        mcp_server.AddTool("self.led.control",
                           "Control the external SK6812 RGBWW LED strip. Use it for phrases like: "
                           "bật đèn=>action on, tắt đèn=>off, đổi màu đỏ=>set_color red, "
                           "trắng ấm=>set_color warm_white, tăng sáng=>increase_brightness, "
                           "giảm sáng=>decrease_brightness, độ sáng 80%=>set_brightness 80. "
                           "action: on/off/set_color/set_brightness/increase_brightness/"
                           "decrease_brightness. status: on/off. color: "
                           "warm_white/ww/red/green/blue/yellow/purple/cyan/#RRGGBB. "
                           "brightness: 0-100 (clamped to 5..80). step: brightness delta.",
                           PropertyList({
                               Property("action", kPropertyTypeString, std::string("set_color")),
                               Property("status", kPropertyTypeString, std::string("on")),
                               Property("color", kPropertyTypeString, std::string("warm_white")),
                               Property("brightness", kPropertyTypeInteger, 65, 0, 100),
                               Property("step", kPropertyTypeInteger, 10, 1, 100),
                           }),
                           [this](const PropertyList& properties) -> ReturnValue {
                               return HandleLedControl(properties);
                           });
    }

public:
    WaveshareEsp32s3TouchAMOLED1inch8() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeAxp2101();

        InitializeTca9554();

        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeLedStrip();
        InitializeTools();
        InitializeAutoStartChat();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override { return backlight_; }

    virtual Led* GetLed() override { return led_strip_; }

    // virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
    //     static bool last_discharging = false;
    //     charging = pmic_->IsCharging();
    //     discharging = pmic_->IsDischarging();
    //     if (discharging != last_discharging) {
    //         power_save_timer_->SetEnabled(discharging);
    //         last_discharging = discharging;
    //     }

    //     level = pmic_->GetBatteryLevel();
    //     return true;
    // }
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (pmic_ == nullptr) {
            level = 100;
            charging = false;
            discharging = false;
            return false;
        }

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

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED1inch8);
