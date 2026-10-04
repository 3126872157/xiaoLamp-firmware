#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "custom_io_expander_ch32v003.h"
#include "esp32_camera.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <lvgl.h>

#define TAG "AppleLampOv3660"

class AppleLampOv3660 : public WifiBoard {
private:
    Button boot_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_io_expander_handle_t io_expander_ = nullptr;
    Esp32Camera* camera_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = I2C_NUM_1;
        bus_config.sda_io_num = I2C_SDA_IO;
        bus_config.scl_io_num = I2C_SCL_IO;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus_));
    }

    void InitializeExpander() {
        ESP_ERROR_CHECK(custom_io_expander_new_i2c_ch32v003(i2c_bus_, BSP_IO_EXPANDER_I2C_ADDRESS,
                                                            &io_expander_));

        // Retain the upstream reset/enable sequence. The IO expander also
        // serves camera/audio hardware, so removing the LCD does not remove it.
        constexpr uint32_t reset_pins = IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1;
        constexpr uint32_t enable_pins = IO_EXPANDER_PIN_NUM_4 | IO_EXPANDER_PIN_NUM_6;
        ESP_ERROR_CHECK(
            esp_io_expander_set_dir(io_expander_, reset_pins | enable_pins, IO_EXPANDER_OUTPUT));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_, reset_pins, 1));
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_, reset_pins, 0));
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_, reset_pins, 1));
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_, enable_pins, 1));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            Application::GetInstance().Schedule([this]() {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    EnterWifiConfigMode();
                } else {
                    app.ToggleChatState();
                }
            });
        });
        boot_button_.OnLongPress(
            [this]() { Application::GetInstance().Schedule([this]() { EnterWifiConfigMode(); }); });
    }

    void InitializeCamera() {
        camera_config_t camera_config = {};
        camera_config.pin_pwdn = CAMERA_PIN_PWDN;
        camera_config.pin_reset = CAMERA_PIN_RESET;
        camera_config.pin_xclk = CAMERA_PIN_XCLK;
        camera_config.pin_sccb_sda = -1;
        camera_config.pin_sccb_scl = -1;
        camera_config.pin_d7 = CAMERA_PIN_D7;
        camera_config.pin_d6 = CAMERA_PIN_D6;
        camera_config.pin_d5 = CAMERA_PIN_D5;
        camera_config.pin_d4 = CAMERA_PIN_D4;
        camera_config.pin_d3 = CAMERA_PIN_D3;
        camera_config.pin_d2 = CAMERA_PIN_D2;
        camera_config.pin_d1 = CAMERA_PIN_D1;
        camera_config.pin_d0 = CAMERA_PIN_D0;
        camera_config.pin_vsync = CAMERA_PIN_VSYNC;
        camera_config.pin_href = CAMERA_PIN_HREF;
        camera_config.pin_pclk = CAMERA_PIN_PCLK;
        camera_config.xclk_freq_hz = XCLK_FREQ_HZ;
        camera_config.ledc_timer = LEDC_TIMER_0;
        camera_config.ledc_channel = LEDC_CHANNEL_0;
        camera_config.pixel_format = PIXFORMAT_RGB565;
        camera_config.frame_size = FRAMESIZE_QVGA;
        camera_config.jpeg_quality = 12;
        camera_config.fb_count = 2;
        camera_config.fb_location = CAMERA_FB_IN_PSRAM;
        camera_config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        camera_config.sccb_i2c_port = I2C_NUM_1;

        camera_ = new Esp32Camera(camera_config);
        if (esp_camera_sensor_get() == nullptr) {
            delete camera_;
            camera_ = nullptr;
            ESP_LOGE(TAG, "Camera unavailable; continuing with audio and WiFi");
        }
    }

public:
    AppleLampOv3660() : boot_button_(BOOT_BUTTON_GPIO) {
        // The upstream assets loader still constructs LVGL fonts after WiFi
        // connects, even with NoDisplay. Initialize its allocator without
        // creating a panel or starting a display task.
        if (!lv_is_initialized()) {
            lv_init();
        }
        InitializeI2c();
        InitializeExpander();
        InitializeButtons();
        InitializeCamera();
        // Board::GetDisplay() provides NoDisplay. No SPI panel/backlight exists.
        ESP_LOGI(TAG, "No-display board ready; USB console, AEC off, UART not enabled");
    }

    AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(AppleLampOv3660);
