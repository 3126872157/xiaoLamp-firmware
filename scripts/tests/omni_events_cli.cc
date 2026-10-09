// Production event handler with fake RTOS/transport; no device, credential or cloud access.
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "web_socket.h"
#define private public
#include "omni_realtime_protocol.h"
#undef private
#include "../../main/protocols/omni_realtime_protocol.cc"

void Protocol::OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket>)> f) {
    on_incoming_audio_ = f;
}
void Protocol::OnIncomingJson(std::function<void(const cJSON*)> f) { on_incoming_json_ = f; }
void Protocol::OnAudioChannelOpened(std::function<void()> f) { on_audio_channel_opened_ = f; }
void Protocol::OnAudioChannelClosed(std::function<void()> f) { on_audio_channel_closed_ = f; }
void Protocol::OnNetworkError(std::function<void(const std::string&)> f) { on_network_error_ = f; }
void Protocol::OnConnected(std::function<void()> f) { on_connected_ = f; }
void Protocol::OnDisconnected(std::function<void()> f) { on_disconnected_ = f; }
void Protocol::SetError(const std::string& s) {
    if (on_network_error_)
        on_network_error_(s);
}
bool Protocol::IsTimeout() const { return false; }
void Protocol::SendStartListening(ListeningMode) {}
void Protocol::SendStopListening() {}
void Protocol::SendAbortSpeaking(AbortReason) {}
void Protocol::SendWakeWordDetected(const std::string&) {}
void Protocol::SendMcpMessage(const std::string&) {}

int main() {
    auto config = Parse(OmniPanel(
        R"({"op":"configure","workspace":"test-space","region":"cn-beijing","key":"dummy-secret"})"));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(config.get(), "configured")));
    assert(OmniPanel().find("dummy-secret") == std::string::npos);
    configuration_loaded = false;
    api_key.clear();
    workspace.clear();
    assert(cJSON_IsTrue(cJSON_GetObjectItem(Parse(OmniPanel()).get(), "configured")));
    assert(api_key == "dummy-secret");
    OmniPanel(R"({"op":"clear"})");
    configuration_loaded = false;
    assert(!cJSON_IsTrue(cJSON_GetObjectItem(Parse(OmniPanel()).get(), "configured")));
    OmniRealtimeProtocol p;
    // Real Enqueue production path: a slow consumer must pace a burst, not cancel it.
    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (unsigned i = 0; i < 100; i++)
            assert(p.Enqueue(new OmniRealtimeProtocol::Work{"event", true, false, p.epoch_}));
        producer_done = true;
    });
    unsigned consumed = 0;
    while (!producer_done || consumed < 100) {
        OmniRealtimeProtocol::Work* w = nullptr;
        if (xQueueReceive(p.queue_, &w, 0) == pdTRUE) {
            delete w;
            consumed++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    producer.join();
    assert(!p.cancel_ && !p.overloaded_ && consumed == 100);
    p.socket_ = std::make_unique<WebSocket>();
    unsigned opened = 0, audio = 0, starts = 0, stops = 0, errors = 0, reads = 0;
    p.OnAudioChannelOpened([&] { opened++; });
    p.OnNetworkError([&](const std::string&) { errors++; });
    p.OnIncomingJson([&](const cJSON* j) {
        if (Str(j, "state") == "start")
            starts++;
        if (Str(j, "state") == "stop")
            stops++;
    });
    p.OnIncomingAudio([&](auto packet) {
        assert(packet->pcm16);
        assert(packet->sample_rate == 24000);
        audio += packet->payload.size();
    });
    p.Receive(R"({"type":"session.created","session":{"id":"s1"}})");
    assert(!p.ready_ && p.socket_->sent.size() == 1);
    p.Receive(R"({"type":"session.updated"})");
    p.Receive(R"({"type":"session.updated"})");
    assert(p.ready_ && opened == 1);
    p.Receive(R"({"type":"input_audio_buffer.committed"})");
    assert(p.socket_->sent.back() == R"({"type":"response.create"})");
    p.Receive(R"({"type":"response.created","response":{"id":"r1"}})");
    p.Receive(R"({"type":"response.audio.delta","response_id":"old","delta":"AAAA"})");
    assert(audio == 0);
    p.Receive(R"({"type":"response.audio.delta","response_id":"r1","delta":"AAAAAA=="})");
    assert(audio == 4 && starts == 1);
    p.Receive(R"({"type":"response.done","response":{"id":"old","status":"completed"}})");
    assert(stops == 0);
    p.Receive(R"({"type":"response.done","response":{"id":"r1","status":"completed"}})");
    assert(stops == 1);
    OmniSetStateReader([&] {
        reads++;
        return R"({"mode":"sit","pose_rad":{"baseYaw":0},"irrelevant":"hidden"})";
    });
    p.Receive(R"({"type":"response.created","response":{"id":"r2"}})");
    const auto call =
        R"({"type":"response.function_call_arguments.done","response_id":"r2","name":"lamp_get_state","call_id":"call_1","arguments":"{}"})";
    p.Receive(call);
    p.Receive(call);
    assert(reads == 0);  // Complete response, not arguments alone, dispatches tools.
    p.Receive(R"({"type":"response.done","response":{"id":"r2","status":"completed"}})");
    assert(p.socket_->sent.back() == R"({"type":"response.create"})");
    assert(reads == 1);
    auto result = Parse(p.socket_->sent[p.socket_->sent.size() - 2]);
    auto item = cJSON_GetObjectItem(result.get(), "item");
    assert(Str(item, "call_id") == "call_1");
    assert(Str(item, "output").find("irrelevant") == std::string::npos);
    // Model-requested rest waits for the follow-up goodbye AND physical playback drain.
    // Test both audible and text-only replies (no drain event in the latter case).
    for (bool audible : {false, true}) {
        OmniRealtimeProtocol rest;
        rest.socket_ = std::make_unique<WebSocket>();
        rest.ready_ = true;
        in_use = true;
        rest.Receive(R"({"type":"response.created","response":{"id":"rest1"}})");
        const auto request =
            R"({"type":"response.function_call_arguments.done","response_id":"rest1","name":"lamp_end_conversation","call_id":"rest_call","arguments":"{}"})";
        rest.Receive(request);
        rest.Receive(request);
        assert(!rest.WantsConversationEnd() && !rest.close_);
        rest.OnPlaybackDrained();
        assert(!rest.close_);  // Cannot close before the farewell response.
        rest.Receive(R"({"type":"response.done","response":{"id":"rest1","status":"completed"}})");
        assert(rest.WantsConversationEnd());
        assert(rest.socket_->sent.back() == R"({"type":"response.create"})");
        rest.Receive(R"({"type":"response.created","response":{"id":"goodbye"}})");
        if (audible)
            rest.Receive(
                R"({"type":"response.audio.delta","response_id":"goodbye","delta":"AAAAAA=="})");
        rest.Receive(R"({"type":"response.done","response":{"id":"old","status":"completed"}})");
        rest.OnPlaybackDrained();
        assert(!rest.close_);
        rest.Receive(
            R"({"type":"response.done","response":{"id":"goodbye","status":"completed"}})");
        assert(!rest.close_ && in_use);
        rest.OnPlaybackDrained();  // Application calls immediately if playback is already idle.
        assert(rest.close_ && !rest.ready_ && !rest.cancel_);
        rest.Disconnect();
        assert(!in_use && status == "resting" && !rest.WantsConversationEnd());
    }
    p.Receive(R"({"type":"response.created","response":{"id":"r3"}})");
    auto id = p.playback_.load();
    assert(p.AcceptsPlayback(id));
    p.SendAbortSpeaking(kAbortReasonNone);
    assert(!p.AcceptsPlayback(id));
    p.Receive(R"({"type":"response.audio.delta","response_id":"r3","delta":"AAAAAA=="})");
    p.Receive(R"({"type":"response.created","response":{"id":"late"}})");
    assert(audio == 4 && p.playback_ == 0);
    p.cancel_ = false;
    p.Receive(R"({"type":"response.created","response":{"id":"r4"}})");
    p.Receive(R"({"type":"response.audio.delta","response_id":"r4","delta":"@@"})");
    assert(errors == 1 && !p.ready_ && p.close_);
    // Async runtime result must not block event handling, and duplicate IDs do not dispatch twice.
    unsigned dispatched = 0, invalidated = 0;
    std::promise<std::string> confirmation;
    OmniSetRuntime(
        [&](const std::string&, const std::string&, uint64_t) {
            dispatched++;
            return confirmation.get_future();
        },
        [&](uint64_t) { invalidated++; }, "[]");
    OmniRealtimeProtocol async;
    async.socket_ = std::make_unique<WebSocket>();
    async.ready_ = true;
    async.Receive(R"({"type":"response.created","response":{"id":"async"}})");
    const auto control =
        R"({"type":"response.function_call_arguments.done","response_id":"async","name":"lamp_set_torque","call_id":"opaque:call/1","arguments":"{\"enabled\":true}"})";
    async.Receive(control);
    async.Receive(control);
    assert(dispatched == 0);
    async.Receive(R"({"type":"response.done","response":{"id":"async","status":"completed"}})");
    assert(dispatched == 1 && async.awaiting_tools_ && async.socket_->sent.empty());
    confirmation.set_value(R"({"ok":true,"result":{"status":"completed"}})");
    async.CompleteTools();
    assert(!async.awaiting_tools_ && async.socket_->sent.size() == 2);
    async.Receive(R"({"type":"response.created","response":{"id":"continue"}})");
    async.Receive(
        R"({"type":"response.function_call_arguments.done","response_id":"continue","name":"lamp_set_torque","call_id":"opaque:call/1","arguments":"{\"enabled\":true}"})");
    async.Receive(R"({"type":"response.done","response":{"id":"continue","status":"completed"}})");
    assert(dispatched == 1);
    async.SendAbortSpeaking(kAbortReasonNone);
    assert(invalidated > 0);
    // Enabling and standing in one response must not race the asynchronous torque confirmation.
    std::promise<std::string> enabled;
    std::vector<std::string> order;
    OmniSetRuntime(
        [&](const std::string& name, const std::string&, uint64_t) {
            order.push_back(name);
            if (name == "lamp_set_torque")
                return enabled.get_future();
            std::promise<std::string> done;
            auto future = done.get_future();
            done.set_value(R"({"ok":true,"execution_completed":true})");
            return future;
        },
        [](uint64_t) {}, "[]");
    OmniRealtimeProtocol batch;
    batch.socket_ = std::make_unique<WebSocket>();
    batch.ready_ = true;
    batch.Receive(R"({"type":"response.created","response":{"id":"batch"}})");
    batch.Receive(
        R"({"type":"response.function_call_arguments.done","response_id":"batch","name":"lamp_set_torque","call_id":"enable","arguments":"{\"enabled\":true}"})");
    batch.Receive(
        R"({"type":"response.function_call_arguments.done","response_id":"batch","name":"lamp_run_sequence","call_id":"stand","arguments":"{\"steps\":[{\"action_id\":\"set_mode\",\"mode\":\"stand\"}]}"})");
    batch.Receive(R"({"type":"response.done","response":{"id":"batch","status":"completed"}})");
    assert(order.size() == 1 && order[0] == "lamp_set_torque" && batch.awaiting_tools_);
    enabled.set_value(R"({"ok":true,"execution_completed":true})");
    batch.CompleteTools();
    assert(order.size() == 2 && order[1] == "lamp_run_sequence" && !batch.awaiting_tools_);
    OmniSetRuntime({}, {}, "");
    OmniRealtimeProtocol flaky;
    flaky.socket_ = std::make_unique<WebSocket>();
    flaky.ready_ = true;
    flaky.socket_->send_ok = false;
    assert(!flaky.Write(R"({"type":"response.create"})"));
    assert(flaky.close_ && !flaky.ready_);
    auto* stale_work = new OmniRealtimeProtocol::Work{"stale", true, false, flaky.epoch_};
    assert(xQueueSend(flaky.queue_, &stale_work, 0) == pdTRUE);
    flaky.Disconnect(false);
    assert(!flaky.close_ && !flaky.socket_);
    OmniRealtimeProtocol::Work* remaining = nullptr;
    assert(xQueueReceive(flaky.queue_, &remaining, 0) != pdTRUE);
    flaky.Connect();
    flaky.Receive(R"({"type":"session.created","session":{"id":"fresh"}})");
    flaky.Receive(R"({"type":"session.updated"})");
    assert(flaky.ready_ && !flaky.close_);
    assert(flaky.Write(R"({"type":"response.create"})"));
    flaky.Disconnect(false);
    // Image upload is bounded, manual-only, after real audio append and before commit.
    in_use = false;
    auto photo = std::make_shared<OmniImage>();
    photo->jpeg = {0xff, 0xd8, 0xff, 0xd9};
    photo->captured_at_us = esp_timer_get_time();
    photo->frame_id = 1;
    OmniSetVision([&]() -> std::shared_ptr<const OmniImage> { return photo; });
    OmniPanel(R"({"op":"vision_on"})");
    OmniRealtimeProtocol visual;
    visual.socket_ = std::make_unique<WebSocket>();
    visual.ready_ = true;
    assert(!visual.AppendImage() && visual.socket_->sent.empty());
    assert(visual.Write(R"({"type":"input_audio_buffer.append","audio":"AAAA"})"));
    visual.had_audio_ = true;
    assert(visual.AppendImage());
    assert(visual.Write(R"({"type":"input_audio_buffer.commit"})"));
    assert(visual.socket_->sent.size() == 3);
    assert(visual.socket_->sent[1].find("input_image_buffer.append") != std::string::npos);
    assert(visual.socket_->sent[1].find("/9j/2Q==") != std::string::npos);
    photo->captured_at_us = esp_timer_get_time() - 30000001;
    assert(!visual.AppendImage() && visual.socket_->sent.size() == 3);
    photo->captured_at_us = esp_timer_get_time();
    photo->jpeg.resize(65537);
    assert(!visual.AppendImage());
    photo->jpeg = {0xff, 0xd8, 0xff, 0xd9};
    visual.manual_ = false;
    assert(!visual.AppendImage());
    assert(OmniPanel(R"({"op":"vad"})").find("Disable manual image") != std::string::npos);
    OmniPanel(R"({"op":"vision_off"})");
    assert(visual.AppendImage() && visual.socket_->sent.size() == 3);
    // Production same-connection observation state machine: no speaker output, no auto torque.
    omni_test_time = 20000000;
    photo->frame_id = 10;
    photo->captured_at_us = omni_test_time;
    photo->luminance_valid = true;
    photo->luminance.fill(20);
    OmniSetVision([&]() -> std::shared_ptr<const OmniImage> { return photo; });
    unsigned auto_actions = 0, speaker_events = 0, owned_cancel = 0;
    std::string hardware_state =
        R"({"motion_enabled":false,"torque_enabled_confirmed":false,"mode":"stand"})";
    OmniSetStateReader([&] { return hardware_state; });
    OmniSetRuntime(
        [&](const std::string& name, const std::string& args, uint64_t) {
            assert(name == "lamp_run_sequence" &&
                   args.find("enable_if_needed") == std::string::npos);
            ++auto_actions;
            std::promise<std::string> done;
            auto future = done.get_future();
            done.set_value(R"({"ok":true,"status":"started"})");
            return future;
        },
        [&](uint64_t) { ++owned_cancel; }, "[]");
    OmniRealtimeProtocol observer;
    observer.socket_ = std::make_unique<WebSocket>();
    observer.ready_ = true;
    observer.manual_ = false;
    observer.recording_ = true;
    observer.quiet_since_ = omni_test_time - 3000000;
    observer.OnIncomingJson([&](const cJSON*) { ++speaker_events; });
    companion_enabled = companion_actions = true;
    observer.PollObservation();
    assert(observer.observation_phase_ == OmniRealtimeProtocol::ObservationPhase::Configure);
    assert(observer.socket_->sent.back().find("\"modalities\":[\"text\"]") != std::string::npos);
    assert(observer.socket_->sent.back().find("\"audio\":{") == std::string::npos);
    observer.Receive(R"({"type":"session.updated","session":{"modalities":["text","audio"]}})");
    assert(observer.observation_phase_ == OmniRealtimeProtocol::ObservationPhase::Configure);
    observer.Receive(R"({"type":"session.updated","session":{"modalities":["text"]}})");
    assert(observer.observation_phase_ == OmniRealtimeProtocol::ObservationPhase::Sample);
    // No audio => do not commit an empty audio buffer or manufacture a user utterance.
    observer.sample_deadline_ = 0;
    observer.PollObservation();
    assert(observer.observation_phase_ == OmniRealtimeProtocol::ObservationPhase::Restore);
    observer.Receive(R"({"type":"session.updated","session":{"modalities":["text","audio"]}})");
    assert(observer.observation_phase_ == OmniRealtimeProtocol::ObservationPhase::Idle);
    auto observe = [&](const std::string& person, const std::string& action, bool stale = false) {
        photo->frame_id++;
        photo->captured_at_us = omni_test_time;
        observation_once = true;
        observer.PollObservation();
        assert(observer.observation_phase_ == OmniRealtimeProtocol::ObservationPhase::Configure);
        observer.Receive(R"({"type":"session.updated","session":{"modalities":["text"]}})");
        observer.Write(R"({"type":"input_audio_buffer.append","audio":"AAAA"})");
        observer.sample_sent_ = true;
        observer.sample_deadline_ = 0;
        observer.PollObservation();
        assert(observer.socket_->sent.back().find("input_audio_buffer.commit") !=
               std::string::npos);
        assert(observer.socket_->sent[observer.socket_->sent.size() - 2].find(
                   "input_image_buffer.append") != std::string::npos);
        observer.Receive(R"({"type":"input_audio_buffer.committed"})");
        assert(observer.socket_->sent.back().find("response.create") != std::string::npos);
        observer.Receive(R"({"type":"response.created","response":{"id":"background"}})");
        observer.observation_text_ =
            "{\"frame_id\":" + std::to_string(photo->frame_id) + ",\"person\":\"" + person +
            "\",\"book\":\"unknown\",\"activity\":\"looking\",\"confidence\":0.9,\"summary\":"
            "\"observation\",\"action\":\"" +
            action + "\"}";
        if (stale)
            photo->captured_at_us = omni_test_time - 10000001;
        observer.Receive(
            R"({"type":"response.done","response":{"id":"background","status":"completed"}})");
        assert(observer.observation_phase_ == OmniRealtimeProtocol::ObservationPhase::Restore);
        observer.Receive(R"({"type":"session.updated","session":{"modalities":["text","audio"]}})");
    };
    observe("present", "nod");
    assert(auto_actions == 0);  // Baseline never causes motion.
    observe("absent", "nod");
    assert(auto_actions == 0);  // Unarmed never causes motion.
    hardware_state = R"({"motion_enabled":true,"torque_enabled_confirmed":true,"mode":"work"})";
    observe("present", "nod");
    assert(auto_actions == 0);  // Work is not preempted.
    hardware_state =
        R"({"motion_enabled":true,"torque_enabled_confirmed":true,"mode":"stand","manual_control":true})";
    photo->frame_id++;
    observation_once = true;
    observer.PollObservation();
    assert(observer.observation_phase_ == OmniRealtimeProtocol::ObservationPhase::Idle);
    observation_once = false;
    hardware_state = R"({"motion_enabled":true,"torque_enabled_confirmed":true,"mode":"stand"})";
    observe("absent", "nod");
    assert(auto_actions == 1);
    observer.PollObservation();  // Consume the asynchronous acceptance, not mechanical completion.
    const auto saved_frame = observation_cached_frame;
    observe("present", "nod", true);
    assert(auto_actions == 1 && observation_cached_frame == saved_frame);
    assert(speaker_events == 0);
    // Foreground voice invalidates only owned background motion and restores actual VAD config.
    observer.observation_phase_ = OmniRealtimeProtocol::ObservationPhase::Model;
    observer.observation_response_ = "background";
    observer.RestoreForeground("test_voice", true);
    assert(owned_cancel == 1);
    observer.speech_prefill_ = std::string(1920, '\0');
    observer.Receive(R"({"type":"session.updated","session":{"modalities":["text","audio"]}})");
    assert(observer.socket_->sent.back().find("input_audio_buffer.append") != std::string::npos);
    assert(observer.had_audio_ && observer.speech_prefill_.empty());
    assert(observer.QuietPacket(std::string(1920, '\0')));
    assert(!observer.QuietPacket(std::string(1920, '\x7f')));
    // A global illumination shift alone does not spend another cloud request.
    observation_once = false;
    observer.have_luminance_ = true;
    observer.observed_luminance_.fill(20);
    photo->luminance.fill(40);
    photo->captured_at_us = omni_test_time;
    photo->frame_id++;
    observer.PollObservation();
    assert(observer.observation_phase_ == OmniRealtimeProtocol::ObservationPhase::Idle);
    // Two errors pause the experimental lane rather than repeatedly damaging foreground chat.
    observer.observation_attempts_ = 2;
    observer.PollObservation();
    assert(!companion_enabled && observation_status == "paused_after_two_failures");
    observer.Disconnect(false);
    assert(!companion_enabled && observation_cache.empty());
    OmniSetRuntime({}, {}, "");
    OmniSetStateReader({});
    OmniSetVision({});
    auto info = OmniPanel();
    assert(info.find("dummy-secret") == std::string::npos);
    std::cout << "PASS handshake, PCM, stale response, function output/dedup, cancel and corrupt "
                 "payload\n";
}
