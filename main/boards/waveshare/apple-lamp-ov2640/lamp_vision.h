#pragma once
#include "sdkconfig.h"
#if CONFIG_APPLE_LAMP_EDGE_RUNTIME
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <algorithm>
#include <mutex>
#include "camera.h"
#include "lamp_hardware_executor.h"
#include "lamp_protocol.h"
#include "omni_realtime_protocol.h"

// Camera IO owner, independent from audio, model callbacks and the UART worker.
// At most one request/frame; latest immutable media handle is shared with HTTP/Omni.
class LampVision {
public:
    LampVision(Camera* camera, LampHardwareExecutor& executor)
        : camera_(camera), executor_(executor) {
        signal_ = xSemaphoreCreateBinary();
        TaskHandle_t task = nullptr;
        ready_ =
            camera_ && signal_ &&
            xTaskCreate(
                [](void* p) {
                    auto* self = static_cast<LampVision*>(p);
                    for (;;) {
                        // Only manual capture requests (preview button / manual
                        // image) drive the camera; periodic observation is removed.
                        if (xSemaphoreTake(self->signal_, pdMS_TO_TICKS(250)) == pdTRUE)
                            self->Capture();
                    }
                },
                "lamp_camera", 16384, this, 1, &task) == pdPASS;
    }
    bool Request() {
        std::lock_guard lock(mutex_);
        const auto now = esp_timer_get_time();
        if (!ready_ || busy_ || (requested_at_ && now - requested_at_ < 2000000))
            return false;
        busy_ = true;
        requested_at_ = now;
        error_.clear();
        return xSemaphoreGive(signal_) == pdTRUE;
    }
    std::shared_ptr<const OmniImage> Latest() {
        std::lock_guard lock(mutex_);
        return frame_;
    }
    std::string State() {
        std::lock_guard lock(mutex_);
        CJsonUniquePtr j(cJSON_CreateObject());
        cJSON_AddBoolToObject(j.get(), "available", ready_);
        cJSON_AddBoolToObject(j.get(), "busy", busy_);
        cJSON_AddStringToObject(j.get(), "error", error_.c_str());
        cJSON_AddNumberToObject(j.get(), "max_jpeg_bytes", 65536);
        cJSON_AddNumberToObject(j.get(), "required_internal_free_bytes", 24576);
        cJSON_AddNumberToObject(j.get(), "required_internal_largest_block", 4096);
        cJSON_AddNumberToObject(j.get(), "capture_duration_ms", duration_ms_);
        cJSON_AddNumberToObject(j.get(), "capture_min_internal_bytes", capture_min_internal_);
        cJSON_AddNumberToObject(j.get(), "internal_free_bytes",
                                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(j.get(), "internal_min_free_bytes",
                                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(j.get(), "internal_largest_block",
                                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(j.get(), "psram_free_bytes",
                                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (frame_) {
            cJSON_AddNumberToObject(j.get(), "frame_id", frame_->frame_id);
            cJSON_AddNumberToObject(j.get(), "age_ms",
                                    (esp_timer_get_time() - frame_->captured_at_us) / 1000);
            cJSON_AddNumberToObject(j.get(), "jpeg_bytes", frame_->jpeg.size());
            cJSON_AddNumberToObject(j.get(), "width", frame_->width);
            cJSON_AddNumberToObject(j.get(), "height", frame_->height);
            cJSON_AddItemToObject(j.get(), "capture_state",
                                  cJSON_Parse(frame_->capture_state.c_str()));
        }
        return lamp::Encode(j.get());
    }

private:
    Camera* camera_;
    LampHardwareExecutor& executor_;
    SemaphoreHandle_t signal_ = nullptr;
    bool ready_ = false, busy_ = false;
    std::mutex mutex_;
    std::shared_ptr<const OmniImage> frame_;
    std::string error_;
    int64_t requested_at_ = 0, duration_ms_ = 0;
    int64_t capture_min_internal_ = -1;
    uint64_t sequence_ = 0;
    void Capture() {
        const auto start = esp_timer_get_time();
        const bool monitored = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
        CJsonUniquePtr full(cJSON_Parse(executor_.State().c_str()));
        CJsonUniquePtr pose(cJSON_CreateObject());
        for (auto* key : {"pose_rad", "model_version", "available", "busy"}) {
            auto* item = cJSON_GetObjectItemCaseSensitive(full.get(), key);
            if (item)
                cJSON_AddItemToObject(pose.get(), key, cJSON_Duplicate(item, true));
        }
        cJSON_AddStringToObject(pose.get(), "configuration_fingerprint",
                                executor_.ConfigurationFingerprint().c_str());
        auto result = camera_->CaptureJpeg(65536);
        const auto capture_min =
            monitored ? int64_t(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)) : -1;
        if (monitored)
            heap_caps_monitor_local_minimum_free_size_stop();
        std::lock_guard lock(mutex_);
        duration_ms_ = (esp_timer_get_time() - start) / 1000;
        capture_min_internal_ = capture_min;
        busy_ = false;
        if (!result) {
            error_ = result.error();
            frame_.reset();
            return;
        }
        auto frame = std::make_shared<OmniImage>();
        frame->jpeg = std::move(result->jpeg);
        frame->luminance = result->luminance;
        frame->luminance_valid = result->luminance_valid;
        frame->width = result->width;
        frame->height = result->height;
        frame->captured_at_us = start;
        frame->frame_id = ++sequence_;
        frame->capture_state = lamp::Encode(pose.get());
        frame_ = std::move(frame);
    }
};
#endif
