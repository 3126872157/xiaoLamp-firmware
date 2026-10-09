#pragma once
#include <functional>
#include <string>
#include <vector>
class WebSocket {
public:
    std::vector<std::string> sent;
    bool connected = true, send_ok = true;
    bool IsConnected() const { return connected; }
    bool Send(const std::string& s) {
        sent.push_back(s);
        return send_ok;
    }
    bool Connect(const char*) { return true; }
    void SetHeader(const char*, const char*) {}
    void OnData(std::function<void(const char*, size_t, bool)>) {}
    void OnDisconnected(std::function<void()>) {}
};
