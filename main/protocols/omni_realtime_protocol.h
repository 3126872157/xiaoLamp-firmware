#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <web_socket.h>
#include <array>
#include <atomic>
#include <future>
#include <map>
#include "protocol.h"

struct OmniImage {
    std::vector<uint8_t> jpeg;
    std::array<uint8_t, 192> luminance{};
    bool luminance_valid = false;
    uint64_t frame_id = 0;
    int64_t captured_at_us = 0;
    unsigned width = 0, height = 0;
    std::string capture_state;
};
void OmniSetVision(std::function<std::shared_ptr<const OmniImage>()> latest,
                   std::function<void(bool, unsigned)> periodic = {},
                   std::function<bool(const OmniImage&)> valid = {});

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
    enum class ObservationPhase { Idle, Configure, Sample, Model, Restore };
    std::atomic<ObservationPhase> observation_phase_{ObservationPhase::Idle};
    std::shared_ptr<const OmniImage> observation_frame_;
    int64_t observation_deadline_ = 0, sample_deadline_ = 0, quiet_since_ = 0;
    uint64_t observed_frame_ = 0, observation_turn_ = 0;
    std::string observation_response_, observation_text_;
    std::string speech_prefill_;
    std::array<uint8_t, 192> observed_luminance_{};
    bool have_luminance_ = false, sample_sent_ = false, model_commit_pending_ = false;
    bool restoring_for_voice_ = false, observation_is_once_ = false;
    unsigned observation_attempts_ = 0;
    std::future<std::string> background_task_;
    void PollObservation();
    bool ObservationEvent(const cJSON* j, const std::string& type);
    void RestoreForeground(const char* reason, bool voice = false);
    bool SendImageFrame(const OmniImage& frame);
    bool QuietPacket(const std::string& pcm) const;
    bool Enqueue(Work* work);
    bool Write(const std::string& text);
    void Run();
    void Connect();
    void Disconnect(bool notify = true);
    void Receive(const std::string& text);
    void Emit(const char* state, const std::string& text = "");
    void Fail(const char* message);
    std::string Session(bool manual, bool include_audio = true);
};
