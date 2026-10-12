#include "application.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "esp32_camera.h"
#include "lamp_bridge.h"
#include "lamp_delivery.h"
#include "lamp_hardware_executor.h"
#if CONFIG_APPLE_LAMP_EDGE_RUNTIME
#include "lamp_edge_runtime.h"
#include "lamp_vision.h"
#include "omni_realtime_protocol.h"
#endif
#include "lamp_local_executor.h"
#include "lamp_twin_web.h"
#include "wifi_board.h"

#include <driver/ledc.h>
#include <esp_log.h>
#include <lvgl.h>

#define TAG "AppleLampOv2640"

class AppleLampOv2640 : public WifiBoard {
private:
    Button boot_button_;
    Esp32Camera* camera_ = nullptr;
    LampActionExecutor* lamp_executor_ = nullptr;

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
        camera_config.pin_sccb_sda = CAMERA_PIN_SIOD;
        camera_config.pin_sccb_scl = CAMERA_PIN_SIOC;
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
        camera_config.pixel_format = PIXFORMAT_JPEG;
        camera_config.frame_size = FRAMESIZE_QVGA;
        camera_config.jpeg_quality = 12;
        camera_config.fb_count = 2;
        camera_config.fb_location = CAMERA_FB_IN_PSRAM;
        camera_config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(camera_config);
        if (esp_camera_sensor_get() == nullptr) {
            delete camera_;
            camera_ = nullptr;
            ESP_LOGE(TAG, "Camera unavailable; continuing with audio and WiFi");
        }
    }

public:
    AppleLampOv2640() : boot_button_(BOOT_BUTTON_GPIO) {
        // The upstream assets loader still constructs LVGL fonts after WiFi
        // connects, even with NoDisplay. Initialize its allocator without
        // creating a panel or starting a display task.
        if (!lv_is_initialized()) {
            lv_init();
        }
        InitializeButtons();
        InitializeCamera();
#if CONFIG_APPLE_LAMP_HARDWARE_MOTION
        auto* hardware = new LampHardwareExecutor();
        lamp_executor_ = hardware;
#if CONFIG_APPLE_LAMP_DELIVERY
        ESP_LOGI(TAG, "Firmware role: XIAOZHI-DELIVERY; local web and UART, no startup motion");
#elif CONFIG_APPLE_LAMP_EDGE_RUNTIME
        auto* runtime = new LampEdgeRuntime(
            *lamp_executor_, [this](const auto& kind, const auto& name, const auto& data) {
                RecordDiagnostic(kind, name, data);
            });
        OmniSetRuntime([runtime](const auto& name, const auto& args,
                                 uint64_t turn) { return runtime->Submit(name, args, turn); },
                       [runtime](uint64_t turn) { runtime->Cancel(turn); },
                       LampEdgeRuntime::Tools());
        auto* vision = new LampVision(camera_, *hardware);
        OmniSetVision(
            [vision] { return vision->Latest(); },
            {},
            [hardware](const OmniImage& frame) {
                CJsonUniquePtr captured(cJSON_Parse(frame.capture_state.c_str()));
                auto* fingerprint =
                    cJSON_GetObjectItem(captured.get(), "configuration_fingerprint");
                if (!cJSON_IsString(fingerprint) ||
                    hardware->ConfigurationFingerprint() != fingerprint->valuestring)
                    return false;
                CJsonUniquePtr current(cJSON_Parse(hardware->State().c_str()));
                auto* before = cJSON_GetObjectItem(captured.get(), "pose_rad");
                auto* after = cJSON_GetObjectItem(current.get(), "pose_rad");
                if (!cJSON_IsObject(before) || !cJSON_IsObject(after) ||
                    !cJSON_GetArraySize(before) || !cJSON_GetArraySize(after))
                    return !cJSON_IsTrue(cJSON_GetObjectItem(captured.get(), "available")) &&
                           !cJSON_IsTrue(cJSON_GetObjectItem(current.get(), "available"));
                for (auto* key : {"baseYaw", "shoulder", "elbow", "wrist", "headRoll"}) {
                    auto* a = cJSON_GetObjectItem(before, key);
                    auto* b = cJSON_GetObjectItem(after, key);
                    if (!cJSON_IsNumber(a) || !cJSON_IsNumber(b) ||
                        std::abs(a->valuedouble - b->valuedouble) > .05)
                        return false;
                }
                return true;
            });
        SetLampVision(vision);
        OmniSetStateReader([runtime] { return runtime->State(); });
        ESP_LOGI(TAG, "Firmware role: EDGE-RUNTIME; semantic tools, operator enable required");
#elif CONFIG_APPLE_LAMP_OMNI_REALTIME
        ESP_LOGI(TAG, "Firmware role: OMNI-POC; direct cloud PCM, read-only cloud tools");
#elif CONFIG_APPLE_LAMP_TWIN_WEB
        ESP_LOGI(TAG, "Firmware role: LOCAL-TWIN; local web test panel, no startup motion");
#else
        ESP_LOGI(TAG, "Firmware role: LOCAL-HARDWARE; operator enable required, no startup motion");
#endif
#elif CONFIG_APPLE_LAMP_LOCAL_EXECUTOR
        lamp_executor_ = new LampLocalExecutor();
        ESP_LOGI(TAG, "Firmware role: LOCAL-READONLY; no browser bridge, no movement writes");
#else
        auto* bridge = new LampBridgeExecutor(*this);
        lamp_executor_ = bridge;
        RegisterLampBridgeConfiguration(*bridge);
        ESP_LOGI(TAG, "Firmware role: SIMULATOR-BRIDGE; servo UART disabled");
#endif
        RegisterLampTools(*lamp_executor_);
#if CONFIG_APPLE_LAMP_DELIVERY
        StartLampDelivery(*static_cast<LampHardwareExecutor*>(lamp_executor_), camera_);
        ESP_LOGI(TAG, "Firmware role: XIAOZHI-DELIVERY; no Edge Runtime, no startup motion");
#endif
        // Board::GetDisplay() provides NoDisplay. No SPI panel/backlight exists.
        ESP_LOGI(
            TAG,
            "No-display board ready; USB console, AEC off; executor role selected at build time");
    }

#if CONFIG_APPLE_LAMP_TWIN_WEB
    void RecordDiagnostic(const std::string& kind, const std::string& name,
                          const std::string& data) override {
        if (lamp_executor_)
            static_cast<LampHardwareExecutor*>(lamp_executor_)->RecordDiagnostic(kind, name, data);
    }
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback(
            [this, callback = std::move(callback)](NetworkEvent event, const std::string& data) {
                if (event == NetworkEvent::Connected)
                    StartLampTwinWeb(*static_cast<LampHardwareExecutor*>(lamp_executor_));
                if (callback)
                    callback(event, data);
            });
    }
#endif

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplexPdm audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(AppleLampOv2640);
