#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <web_socket.h>
#include <atomic>
#include "protocol.h"

// LAN provisioning is RAM-only. Never return the key or persist it in NVS.
std::string OmniPanel(const std::string& request = "");
void OmniSetStateReader(std::function<std::string()> reader);

class OmniRealtimeProtocol final : public Protocol {
public:
    OmniRealtimeProtocol();
    ~OmniRealtimeProtocol() override;
    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool = true) override;
    bool IsAudioChannelOpened() const override { return ready_; }
    bool UsesPcmAudio() const override { return true; }
    bool OpensAsynchronously() const override { return true; }
    bool AcceptsPlayback(uint32_t id) const override { return ready_ && id && id == playback_; }
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendWakeWordDetected(const std::string&) override {}
    void SendMcpMessage(const std::string&) override {}

protected:
    bool SendText(const std::string& text) override;

private:
    struct Work {
        std::string text;
        bool incoming = false;
        bool audio = false;
        uint32_t epoch = 0;
    };
    QueueHandle_t queue_ = nullptr;
    SemaphoreHandle_t stopped_ = nullptr;
    bool task_started_ = false;
    std::unique_ptr<WebSocket> socket_;
    std::atomic<bool> ready_{false}, running_{true}, open_{false}, close_{false}, cancel_{false};
    std::atomic<bool> disconnected_{false}, overloaded_{false}, recording_{false};
    std::atomic<uint32_t> epoch_{1}, playback_{0};
    bool speaking_ = false, had_audio_ = false;
    bool manual_ = true;
    int64_t connecting_since_ = 0;
    int64_t recording_since_ = 0, session_since_ = 0;
    std::string response_, cancelled_response_, transcript_, tool_call_, tool_output_;
    std::string last_call_;
    bool Enqueue(Work* work);
    bool Write(const std::string& text);
    void Run();
    void Connect();
    void Disconnect(bool notify = true);
    void Receive(const std::string& text);
    void Emit(const char* state, const std::string& text = "");
    void Fail(const char* message);
    std::string Session(bool manual);
};
