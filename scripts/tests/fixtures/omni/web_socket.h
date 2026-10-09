#pragma once
#include <functional>
#include <string>
#include <vector>
class WebSocket {
public:
    std::vector<std::string> sent;
    bool connected = true, send_ok = true;
    size_t max_frame = 2048, frames = 0, fail_frame = 0;
    std::string pending;
    bool IsConnected() const { return connected; }
    bool Send(const std::string& s) { return Send(s.data(), s.size(), false, true); }
    bool Send(const void* data, size_t n, bool binary = false, bool final = true) {
        ++frames;
        if (binary || n > max_frame || !send_ok || (fail_frame && frames == fail_frame))
            return false;
        pending.append(static_cast<const char*>(data), n);
        if (final) {
            sent.push_back(pending);
            pending.clear();
        }
        return true;
    }
    bool Connect(const char*) { return true; }
    void SetHeader(const char*, const char*) {}
    void OnData(std::function<void(const char*, size_t, bool)>) {}
    void OnDisconnected(std::function<void()>) {}
};
