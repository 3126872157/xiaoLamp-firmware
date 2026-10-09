#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <web_socket.h>
#include <atomic>
#include <future>
#include <map>
#include "protocol.h"

struct OmniImage {
    std::vector<uint8_t> jpeg;
    uint64_t frame_id = 0;
    int64_t captured_at_us = 0;
    unsigned width = 0, height = 0;
    std::string capture_state;
};
void OmniSetVision(std::function<std::shared_ptr<const OmniImage>()> latest);

// Developer credentials use a separate NVS namespace; never return or log the key.
std::string OmniPanel(const std::string& request = "");
void OmniSetStateReader(std::function<std::string()> reader);
std::string OmniRuntimeState();
void OmniSetRuntime(
    std::function<std::future<std::string>(const std::string&, const std::string&, uint64_t)>
        dispatch,
    std::function<void(uint64_t)> cancel, const std::string& tools);

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
    bool AcceptsPlayback(uint32_t id) const override {
        return running_ && ready_ && id && id == playback_;
    }
    void ReportPlaybackFailure(const std::string& message) override { Fail(message.c_str()); }
    bool WantsConversationEnd() const override { return rest_requested_; }
    void OnPlaybackDrained() override;
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
    std::atomic<AbortReason> abort_reason_{kAbortReasonNone};
    std::atomic<bool> rest_requested_{false}, rest_reply_done_{false};
    std::atomic<uint32_t> epoch_{1}, playback_{0};
    bool speaking_ = false, had_audio_ = false;
    bool manual_ = true;
    int64_t connecting_since_ = 0;
    int64_t recording_since_ = 0, session_since_ = 0;
    std::string response_, cancelled_response_, transcript_;
    struct Tool {
        std::string id, name, arguments, output;
        std::future<std::string> result;
    };
    std::vector<Tool> tools_;
    std::map<std::string, std::pair<std::string, std::string>> call_cache_;
    std::atomic<uint64_t> turn_{0};
    bool continuing_ = false, awaiting_tools_ = false;
    unsigned tool_rounds_ = 0;
    void CompleteTools();
    bool AppendImage();
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
