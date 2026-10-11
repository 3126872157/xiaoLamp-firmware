#ifndef CAMERA_H
#define CAMERA_H

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>

class Camera {
protected:
    std::mutex observation_mutex_;
    std::atomic<bool> observation_paused_{false};

public:
    virtual ~Camera() = default;

    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    void SetObservationPaused(bool paused) { observation_paused_ = paused; }
    using Preview = std::function<void(const uint8_t*, size_t)>;
    virtual std::expected<std::string, std::string> CaptureAndExplain(const std::string& question,
                                                                      Preview preview = {}) {
        std::lock_guard lock(observation_mutex_);
        if (observation_paused_)
            return std::unexpected("Camera paused by operator teaching");
        if (!Capture())
            return std::unexpected("Camera capture failed");
        return Explain(question);
    }
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::expected<std::string, std::string> Explain(const std::string& question) = 0;
};

#endif  // CAMERA_H
