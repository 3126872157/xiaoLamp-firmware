#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <img_converters.h>
#include <cstdio>
#include <cstring>

#include "board.h"
#include "camera_vision_memory.h"
#include "display.h"
#include "esp32_camera.h"
#include "esp_timer.h"
#include "jpg/image_to_jpeg.h"
#include "lvgl_display.h"
#include "mcp_server.h"
#include "system_info.h"

#define TAG "Esp32Camera"

#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED
#if CONFIG_XIAOZHI_CAMERA_HMIRROR
static constexpr bool kConfiguredHMirror = true;
#else
static constexpr bool kConfiguredHMirror = false;
#endif
#if CONFIG_XIAOZHI_CAMERA_VFLIP
static constexpr bool kConfiguredVFlip = true;
#else
static constexpr bool kConfiguredVFlip = false;
#endif
#endif

Esp32Camera::Esp32Camera(const camera_config_t& config) {
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed with error 0x%x", err);
        return;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 0);  // Control camera mirror: 1 for mirror, 0 for normal
        }
#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED
        s->set_hmirror(s, kConfiguredHMirror ? 1 : 0);
        s->set_vflip(s, kConfiguredVFlip ? 1 : 0);
#endif
        ESP_LOGI(TAG, "Camera initialized: format=%d", config.pixel_format);
    }

#if CONFIG_APPLE_LAMP_DELIVERY
    vision_jpeg_ = static_cast<uint8_t*>(
        heap_caps_malloc(camera_vision::kJpegBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!vision_jpeg_)
        ESP_LOGW(TAG, "Vision PSRAM workspace unavailable; capture will retry allocation");
#endif
    streaming_on_ = true;
}

Esp32Camera::~Esp32Camera() {
#if CONFIG_APPLE_LAMP_DELIVERY
    heap_caps_free(vision_jpeg_);
    vision_jpeg_ = nullptr;
#endif
    if (streaming_on_) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
            current_fb_ = nullptr;
        }
        if (encode_buf_) {
            heap_caps_free(encode_buf_);
            encode_buf_ = nullptr;
            encode_buf_size_ = 0;
        }
        esp_camera_deinit();
        streaming_on_ = false;
    }
}

void Esp32Camera::SetExplainUrl(const std::string& url, const std::string& token) {
    std::lock_guard lock(configuration_mutex_);
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    if (!streaming_on_) {
        return false;
    }

    // Get the latest frame, discard old frames for real-time performance
    for (int i = 0; i < 2; i++) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
        }
        current_fb_ = esp_camera_fb_get();
        if (!current_fb_) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }
    }

    // Prepare encode buffer for RGB565 format (with optional byte swapping)
    if (current_fb_->format == PIXFORMAT_RGB565) {
        size_t pixel_count = current_fb_->width * current_fb_->height;
        size_t data_size = pixel_count * 2;

        // Allocate or reallocate encode buffer if needed
        if (encode_buf_size_ < data_size) {
            if (encode_buf_) {
                heap_caps_free(encode_buf_);
            }
            encode_buf_ =
                (uint8_t*)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (encode_buf_ == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate memory for encode buffer");
                encode_buf_size_ = 0;
                return false;
            }
            encode_buf_size_ = data_size;
        }

        // Copy data to encode buffer with optional byte swapping
        uint16_t* src = (uint16_t*)current_fb_->buf;
        uint16_t* dst = (uint16_t*)encode_buf_;
        if (swap_bytes_enabled_) {
            for (size_t i = 0; i < pixel_count; i++) {
                dst[i] = __builtin_bswap16(src[i]);
            }
        } else {
            memcpy(encode_buf_, current_fb_->buf, data_size);
        }

        // Allocate separate buffer for preview display
        uint8_t* preview_data =
            (uint8_t*)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (preview_data != nullptr) {
            memcpy(preview_data, encode_buf_, data_size);
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(
                    preview_data, data_size, current_fb_->width, current_fb_->height,
                    current_fb_->width * 2, LV_COLOR_FORMAT_RGB565));
            } else {
                heap_caps_free(preview_data);
            }
        }
    } else if (current_fb_->format == PIXFORMAT_JPEG) {
        // JPEG format preview usually requires decoding, skip preview display for now, just log
        ESP_LOGW(TAG, "JPEG capture success, len=%zu, but not supported for preview",
                 current_fb_->len);
    }

    ESP_LOGI(TAG, "Captured frame: %dx%d, len=%zu, format=%d", current_fb_->width,
             current_fb_->height, current_fb_->len, current_fb_->format);

    return true;
}

bool Esp32Camera::SetHMirror(bool enabled) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_hmirror(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_vflip(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetSwapBytes(bool enabled) {
    swap_bytes_enabled_ = enabled;
    return true;
}

std::expected<std::string, std::string> Esp32Camera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        return std::unexpected("Image explain URL or token is not set");
    }

    if (current_fb_ == nullptr) {
        return std::unexpected("No camera frame captured");
    }

    // Create local JPEG queue
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        return std::unexpected("Failed to create JPEG queue");
    }

    // Start encoding thread
    encoder_thread_ = std::thread([this, jpeg_queue]() {
        int64_t start_time = esp_timer_get_time();
        uint16_t w = current_fb_->width;
        uint16_t h = current_fb_->height;
        v4l2_pix_fmt_t enc_fmt;
        switch (current_fb_->format) {
            case PIXFORMAT_RGB565:
                enc_fmt = V4L2_PIX_FMT_RGB565;
                break;
            case PIXFORMAT_YUV422:
                enc_fmt = V4L2_PIX_FMT_YUYV;  // YUV422 is actually YUYV format
                break;
            case PIXFORMAT_YUV420:
                enc_fmt = V4L2_PIX_FMT_YUV420;
                break;
            case PIXFORMAT_GRAYSCALE:
                enc_fmt = V4L2_PIX_FMT_GREY;
                break;
            case PIXFORMAT_JPEG:
                enc_fmt = V4L2_PIX_FMT_JPEG;
                break;
            case PIXFORMAT_RGB888:
                enc_fmt = V4L2_PIX_FMT_RGB24;
                break;
            default:
                ESP_LOGE(TAG, "Unsupported pixel format: %d", current_fb_->format);
                JpegChunk chunk = {.data = nullptr, .len = 0};
                xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
                return;
        }

        // Use encode buffer for RGB565, otherwise use original frame buffer
        uint8_t* jpeg_src_buf = current_fb_->buf;
        size_t jpeg_src_len = current_fb_->len;
        if (current_fb_->format == PIXFORMAT_RGB565 && encode_buf_ != nullptr) {
            jpeg_src_buf = encode_buf_;
            jpeg_src_len = encode_buf_size_;
        }

        bool ok = image_to_jpeg_cb(
            jpeg_src_buf, jpeg_src_len, w, h, enc_fmt, 80,
            [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                auto jpeg_queue = static_cast<QueueHandle_t>(arg);
                JpegChunk chunk = {.data = nullptr, .len = len};
                if (index == 0 && data != nullptr && len > 0) {
                    chunk.data = (uint8_t*)heap_caps_aligned_alloc(
                        16, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (chunk.data == nullptr) {
                        ESP_LOGE(TAG, "Failed to allocate %zu bytes for JPEG chunk", len);
                        chunk.len = 0;
                    } else {
                        memcpy(chunk.data, data, len);
                    }
                } else {
                    chunk.len = 0;  // Sentinel or error
                }
                xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
                return len;
            },
            jpeg_queue);

        if (!ok) {
            JpegChunk chunk = {.data = nullptr, .len = 0};
            xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
        }
        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "JPEG encoding time: %ld ms", int((end_time - start_time) / 1000));
    });

    auto drain_jpeg_queue = [this, jpeg_queue]() {
        JpegChunk chunk;
        while (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) == pdPASS) {
            if (chunk.data != nullptr) {
                heap_caps_free(chunk.data);
            } else {
                break;
            }
        }
        encoder_thread_.join();
        vQueueDelete(jpeg_queue);
    };

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (auto opened = http->Open("POST", explain_url_); !opened) {
        ESP_LOGE(TAG, "Failed to connect to explain URL: %s", opened.error().ToString().c_str());
        drain_jpeg_queue();
        return std::unexpected("Failed to connect to explain URL");
    }

    auto write_or_fail = [&http](const char* data, size_t size) -> bool {
        if (auto written = http->Write(data, size); !written) {
            ESP_LOGE(TAG, "Failed to upload photo: %s", written.error().ToString().c_str());
            return true;
        }
        return false;
    };

    {
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        if (write_or_fail(question_field.c_str(), question_field.size())) {
            drain_jpeg_queue();
            http->Close();
            return std::unexpected("Failed to upload photo");
        }
    }
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        if (write_or_fail(file_header.c_str(), file_header.size())) {
            drain_jpeg_queue();
            http->Close();
            return std::unexpected("Failed to upload photo");
        }
    }

    size_t total_sent = 0;
    bool saw_terminator = false;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            saw_terminator = true;
            break;
        }
        const bool write_failed = write_or_fail((const char*)chunk.data, chunk.len);
        heap_caps_free(chunk.data);
        if (write_failed) {
            drain_jpeg_queue();
            http->Close();
            return std::unexpected("Failed to upload photo");
        }
        total_sent += chunk.len;
    }
    encoder_thread_.join();
    vQueueDelete(jpeg_queue);

    if (!saw_terminator || total_sent == 0) {
        ESP_LOGE(TAG, "JPEG encoder failed or produced empty output");
        return std::unexpected("Failed to encode image to JPEG");
    }

    {
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        if (write_or_fail(multipart_footer.c_str(), multipart_footer.size())) {
            http->Close();
            return std::unexpected("Failed to upload photo");
        }
    }
    if (write_or_fail("", 0)) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        http->Close();
        return std::unexpected("Failed to upload photo");
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", *status_code);
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG,
             "Explain image size=%dx%d, compressed size=%d, remain stack size=%d, question=%s\n%s",
             current_fb_->width, current_fb_->height, (int)total_sent, (int)remain_stack_size,
             question.c_str(), result.c_str());
    return result;
}

#if CONFIG_APPLE_LAMP_DELIVERY
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <algorithm>
std::expected<std::string, std::string> Esp32Camera::CaptureAndExplain(const std::string& question,
                                                                       Preview preview) {
    // Serialize a complete capture/upload with the ordinary MCP photo tool.
    std::lock_guard observation_lock(observation_mutex_);
    if (observation_paused_)
        return std::unexpected("Camera paused by operator teaching");
    std::string url, token;
    {
        std::lock_guard lock(configuration_mutex_);
        url = explain_url_;
        token = explain_token_;
    }
    if (url.empty())
        return std::unexpected("Vision URL/token not initialized: open Xiaozhi conversation first");
    if (!url.starts_with("https://") && !url.starts_with("http://"))
        return std::unexpected("Unsupported vision URL");
    if (question.empty() || question.size() > 1024)
        return std::unexpected("Vision question too long");
    struct HeapTrace {
        size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ~HeapTrace() {
            ESP_LOGI(
                TAG, "Vision heap internal=%u->%u largest=%u->%u PSRAM=%u->%u", unsigned(internal),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                unsigned(largest),
                unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                unsigned(psram), unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        }
    } trace;
    if (!camera_vision::CanStart(trace.internal, trace.largest))
        return std::unexpected("Vision deferred: internal_free=" + std::to_string(trace.internal) +
                               "; largest_block=" + std::to_string(trace.largest) +
                               "; need reserve=" + std::to_string(camera_vision::kInternalReserve) +
                               ", block=" + std::to_string(camera_vision::kLargestControlBlock) +
                               "; audio idle does not guarantee memory");
    if (!Capture())
        return std::unexpected("Camera capture failed");
    constexpr size_t limit = camera_vision::kJpegBytes;
    if (!vision_jpeg_)
        vision_jpeg_ =
            static_cast<uint8_t*>(heap_caps_malloc(limit, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* jpeg = vision_jpeg_;
    if (!jpeg)
        return std::unexpected("JPEG PSRAM workspace allocation failed");
    struct Jpeg {
        uint8_t* bytes;
        size_t size = 0;
        bool overflow = false;
    } data{jpeg};
    bool encoded = false;
    if (current_fb_->format == PIXFORMAT_JPEG) {
        if (current_fb_->len <= limit) {
            memcpy(jpeg, current_fb_->buf, current_fb_->len);
            data.size = current_fb_->len;
            encoded = true;
        }
    } else if (current_fb_->format == PIXFORMAT_RGB565 && encode_buf_) {
        encoded = image_to_jpeg_cb(
            encode_buf_, encode_buf_size_, current_fb_->width, current_fb_->height,
            V4L2_PIX_FMT_RGB565, 65,
            [](void* arg, size_t, const void* bytes, size_t len) -> size_t {
                auto* j = static_cast<Jpeg*>(arg);
                if (!bytes || !len)
                    return len;
                if (j->size + len > limit) {
                    j->overflow = true;
                    return 0;
                }
                memcpy(j->bytes + j->size, bytes, len);
                j->size += len;
                return len;
            },
            &data);
    }
    if (!encoded || data.overflow || data.size < 4) {
        return std::unexpected("JPEG encode failed/over 64 KiB");
    }
    if (preview)
        preview(jpeg, data.size);
    const std::string boundary = "----LAMP_VISION_REQUEST";
    const std::string head = "--" + boundary +
                             "\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n" +
                             question + "\r\n--" + boundary +
                             "\r\nContent-Disposition: form-data; name=\"file\"; "
                             "filename=\"camera.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    const std::string tail = "\r\n--" + boundary + "--\r\n";
    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.timeout_ms = 5000;
    // IDF uses malloc for these buffers. In the delivery variants allocations
    // above ALWAYSINTERNAL go to PSRAM, unlike its default 512-byte buffers.
#if CONFIG_SPIRAM_USE_MALLOC
    cfg.buffer_size = std::max(size_t(2048), size_t(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) + 1);
    cfg.buffer_size_tx = cfg.buffer_size;
#endif
    cfg.disable_auto_redirect = true;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    auto http = esp_http_client_init(&cfg);
    if (!http) {
        return std::unexpected("Vision HTTP allocation failed");
    }
    const auto deadline = esp_timer_get_time() + 20000000;
    std::string error, response;
    response.reserve(camera_vision::kResponseBytes);
    esp_http_client_set_method(http, HTTP_METHOD_POST);
    esp_http_client_set_header(http, "Device-Id", SystemInfo::GetMacAddress().c_str());
    esp_http_client_set_header(http, "Client-Id", Board::GetInstance().GetUuid().c_str());
    const auto auth = "Bearer " + token;
    if (!token.empty())
        esp_http_client_set_header(http, "Authorization", auth.c_str());
    esp_http_client_set_header(http, "Content-Type",
                               ("multipart/form-data; boundary=" + boundary).c_str());
    auto write = [&](const void* ptr, size_t len) {
        const auto* bytes = static_cast<const char*>(ptr);
        while (len) {
            if (esp_timer_get_time() > deadline)
                return false;
            const int n = esp_http_client_write(http, bytes, std::min(len, size_t(4096)));
            if (n <= 0)
                return false;
            bytes += n;
            len -= n;
        }
        return true;
    };
    if (esp_http_client_open(http, head.size() + data.size + tail.size()) != ESP_OK)
        error = "Vision connection failed";
    else if (!write(head.data(), head.size()) || !write(jpeg, data.size) ||
             !write(tail.data(), tail.size()))
        error = "Vision upload failed/timeout";
    if (error.empty()) {
        const auto headers = esp_http_client_fetch_headers(http);
        const int status = esp_http_client_get_status_code(http);
        if (headers < 0 || status != 200)
            error = "Vision HTTP " + std::to_string(status) + "; verify service credentials/quota";
        else {
            char buffer[768];
            while (esp_timer_get_time() < deadline) {
                const int n = esp_http_client_read(http, buffer, sizeof(buffer));
                if (n < 0) {
                    error = "Vision response read failed";
                    break;
                }
                if (!n) {
                    if (!esp_http_client_is_complete_data_received(http))
                        error = "Incomplete vision response";
                    break;
                }
                if (response.size() + n > camera_vision::kResponseBytes) {
                    error = "Vision response exceeds 8 KiB";
                    break;
                }
                response.append(buffer, n);
            }
            if (esp_timer_get_time() >= deadline)
                error = "Vision request timed out";
        }
    }
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    return error.empty() ? std::expected<std::string, std::string>(std::move(response))
                         : std::unexpected(error);
}
#endif
