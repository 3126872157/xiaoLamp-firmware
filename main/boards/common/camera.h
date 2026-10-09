#ifndef CAMERA_H
#define CAMERA_H

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

struct CameraJpegFrame {
    std::vector<uint8_t> jpeg;
    unsigned width = 0, height = 0;
};

class Camera {
public:
    virtual ~Camera() = default;

    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    // Optional local JPEG capture, with no Explain service/network dependency.
    virtual std::expected<CameraJpegFrame, std::string> CaptureJpeg(size_t max_bytes = 65536) {
        return std::unexpected("Local JPEG capture unsupported");
    }
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::expected<std::string, std::string> Explain(const std::string& question) = 0;
};

#endif  // CAMERA_H
