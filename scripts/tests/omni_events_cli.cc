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
    // Saturating PCM cannot consume control/cloud slots or terminate the session.
    for (unsigned i = 0; i < 8; ++i)
        assert(p.Enqueue(
            new OmniRealtimeProtocol::Work{std::string(1920, '\0'), false, true, p.epoch_}));
    assert(
        !p.Enqueue(new OmniRealtimeProtocol::Work{std::string(1920, '\0'), false, true, p.epoch_}));
    assert(p.audio_overrun_ && !p.overloaded_);
    p.SendStartListening(kListeningModeManualStop);
    assert(p.listen_requested_ == 1 && p.queue_->items.empty());
    p.socket_ = std::make_unique<WebSocket>();
    p.ready_ = true;
    p.RecoverAudioInput();
    assert(p.audio_recovering_ && !p.close_ && p.ready_ && p.audio_queue_->items.empty());
    assert(p.socket_->sent.back().find("input_audio_buffer.clear") != std::string::npos);
    p.ready_ = false;
    p.audio_recovering_ = false;
    p.socket_ = std::make_unique<WebSocket>();
    std::string raw_pcm(1920, '\0');
    for (size_t i = 0; i < raw_pcm.size(); ++i)
        raw_pcm[i] = char(i % 256);
    assert(p.AppendPcm(raw_pcm));
    assert(p.socket_->sent.size() == 2 && p.socket_->frames == 2 && p.socket_->pending.empty());
    std::string restored_pcm;
    for (const auto& message : p.socket_->sent) {
        auto json = Parse(message);
        auto encoded = Str(json.get(), "audio");
        unsigned char decoded[1024];
        size_t size = 0;
        assert(message.size() < 2048 &&
               mbedtls_base64_decode(decoded, sizeof(decoded), &size,
                                     reinterpret_cast<const unsigned char*>(encoded.data()),
                                     encoded.size()) == 0);
        restored_pcm.append(reinterpret_cast<char*>(decoded), size);
    }
    assert(restored_pcm == raw_pcm);
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
    // The real transport clones masked frames and caps one frame at 65535 bytes.
    // Our fixture rejects >2048-byte fragments and reassembles one complete JSON message.
    OmniRealtimeProtocol fragmented;
    fragmented.socket_ = std::make_unique<WebSocket>();
    fragmented.ready_ = true;
    const std::string large_message =
        "{\"type\":\"session.update\",\"tools\":\"" + std::string(14330, 'x') + "\"}";
    assert(fragmented.Write(large_message));
    assert(fragmented.socket_->sent.size() == 1 && fragmented.socket_->sent[0] == large_message);
    assert(fragmented.socket_->frames > 1 && fragmented.socket_->pending.empty());
    assert(fragmented.Write(
        std::string(90000, 'x')));  // Larger than one vendor frame, still one WS message.
    assert(fragmented.socket_->sent.back().size() == 90000);
    OmniRealtimeProtocol partial;
    partial.socket_ = std::make_unique<WebSocket>();
    partial.ready_ = true;
    partial.socket_->fail_frame = 2;
    assert(!partial.Write(large_message) && partial.close_);
    assert(partial.socket_->frames == 2 && partial.socket_->sent.empty());
    assert(!partial.Write(R"({"type":"input_audio_buffer.clear"})") &&
           partial.socket_->frames == 2);
    // Mode completion guidance is short; accepted/in-progress is never reported completed.
    assert(batch.socket_->sent.front().find("response_instruction") != std::string::npos);
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
            "{\"frame_id\":" + std::to_string(1) + ",\"person\":\"" + person +
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
    assert(observation_cached_frame == photo->frame_id);
    assert(Str(Parse(observation_cache).get(), "person") == "present");
    assert(OmniRuntimeState().find("\"observation_fresh\":true") != std::string::npos);
    assert(OmniRuntimeState().find("\"observation\":{") != std::string::npos);
    auto repeated_state = Parse(OmniRuntimeState());
    auto count_before = cJSON_GetArraySize(cJSON_GetObjectItem(repeated_state.get(), "runtime"));
    AddObservationState(repeated_state.get());
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(repeated_state.get(), "runtime")) ==
           count_before);
    assert(auto_actions == 0);  // Baseline never causes motion.
    observe("absent", "nod");
    assert(auto_actions == 0);  // Unarmed never causes motion.
    hardware_state = R"({"motion_enabled":true,"torque_enabled_confirmed":true,"mode":"work"})";
    observe("present", "nod");
    assert(auto_actions ==
           1);  // Idle work posture permits only head expressions, never active work.
    hardware_state =
        R"({"motion_enabled":true,"torque_enabled_confirmed":true,"mode":"stand","manual_control":true})";
    photo->frame_id++;
    observation_once = true;
    observer.PollObservation();
    assert(observer.observation_phase_ == OmniRealtimeProtocol::ObservationPhase::Idle);
    observation_once = false;
    hardware_state = R"({"motion_enabled":true,"torque_enabled_confirmed":true,"mode":"stand"})";
    omni_test_time += 16000000;
    observe("present", "nod");
    assert(auto_actions == 2);
    observer.PollObservation();  // Consume the asynchronous acceptance, not mechanical completion.
    const auto saved_frame = observation_cached_frame;
    observe("present", "nod", true);
    assert(auto_actions == 2 && observation_cached_frame == saved_frame);
    assert(observer.observation_attempts_ == 0 && companion_enabled);
    assert(speaker_events == 0);
    // Foreground voice invalidates only owned background motion and restores actual VAD config.
    observer.observation_phase_ = OmniRealtimeProtocol::ObservationPhase::Model;
    observer.observation_response_ = "background";
    observer.RestoreForeground("test_voice", true);
    assert(owned_cancel == 1);
    assert(observer.socket_->sent.back().find("\"audio\":{") == std::string::npos);
    // Restore a realistic full tool schema on a transport with a small frame budget.
    runtime_tools = "[{\"type\":\"function\",\"description\":\"" + std::string(12000, 't') + "\"}]";
    observer.RestoreForeground("test_restore", true);
    assert(observer.ready_ && !observer.close_);
    assert(observer.socket_->sent.back().size() > 12000);
    runtime_tools = "[]";
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
    // Voice expression uses production parameter events + asynchronous preparation.
    unsigned prepared_count = 0, released_count = 0;
    hardware_state =
        R"({"mode":"stand","available":true,"motion_enabled":true,"torque_enabled_confirmed":true})";
    OmniSetRuntime(
        [&](const std::string& name, const std::string& args, uint64_t) {
            std::promise<std::string> done;
            auto future = done.get_future();
            if (name == "lamp_prepare_expression") {
                if (args.find("invalid") != std::string::npos) {
                    done.set_value(R"({"ok":false,"error":"Invalid argument"})");
                    return future;
                }
                ++prepared_count;
                if (args.find("null") != std::string::npos) {
                    done.set_value(
                        R"({"ok":true,"status":"prepared","speech":null,"args":{"steps":[{"action_id":"execute_primitive","skill_id":"A01"}]}})");
                    return future;
                }
                done.set_value(
                    R"({"ok":true,"status":"prepared","args":{"steps":[{"action_id":"execute_primitive","skill_id":"B02"}]}})");
            } else {
                assert(name == "lamp_run_sequence");
                ++released_count;
                done.set_value(R"({"ok":true,"result":{"status":"completed"}})");
            }
            return future;
        },
        [](uint64_t) {}, "[]");
    OmniRealtimeProtocol expressive;
    expressive.ready_ = true;
    expressive.socket_ = std::make_unique<WebSocket>();
    expressive.Receive(R"({"type":"response.created","response":{"id":"prepare"}})");
    expressive.Receive(
        R"({"type":"response.function_call_arguments.done","response_id":"prepare","name":"lamp_express","call_id":"bad-expression","arguments":"{\"skill_id\":\"invalid\"}"})");
    expressive.Receive(
        R"({"type":"response.done","response":{"id":"prepare","status":"completed"}})");
    assert(expressive.expression_used_turn_ == 0);
    const auto original_turn = expressive.turn_.load();
    expressive.Receive(R"({"type":"response.created","response":{"id":"prepare"}})");
    assert(expressive.turn_ == original_turn);
    expressive.Receive(
        R"({"type":"response.function_call_arguments.done","response_id":"prepare","name":"lamp_express","call_id":"expression-1","arguments":"{\"skill_id\":\"B02\"}"})");
    expressive.Receive(
        R"({"type":"response.done","response":{"id":"prepare","status":"completed"}})");
    assert(prepared_count == 1 && released_count == 0 && !expressive.expression_.args.empty());
    expressive.Receive(R"({"type":"response.created","response":{"id":"speech"}})");
    expressive.PollExpression();
    assert(released_count == 0);  // Receiving/creating speech is not playback.
    expressive.OnPlaybackStarted(expressive.playback_ + 1);
    expressive.PollExpression();
    assert(released_count == 0);  // Stale/foreign playback cannot release.
    expressive.OnPlaybackStarted(expressive.playback_);
    expressive.PollExpression();
    expressive.PollExpression();
    assert(released_count == 1);
    expressive.PollExpression();
    assert(released_count == 1);  // Once only.
    expressive.expression_ = {"{\"steps\":[]}", "stand", expressive.turn_, expressive.playback_,
                              omni_test_time + 10000000};
    hardware_state =
        R"({"mode":"stand","available":true,"motion_enabled":true,"torque_enabled_confirmed":true,"manual_control":true})";
    expressive.PollExpression();
    assert(expressive.expression_.args.empty() && released_count == 1);
    expressive.expression_.args = "{}";
    expressive.SendAbortSpeaking(kAbortReasonNone);
    expressive.PollExpression();
    assert(expressive.expression_.args.empty() && released_count == 1);
    expressive.Disconnect(false);
    hardware_state =
        R"({"mode":"stand","available":true,"motion_enabled":true,"torque_enabled_confirmed":true})";
    OmniRealtimeProtocol silent;
    silent.ready_ = true;
    silent.socket_ = std::make_unique<WebSocket>();
    silent.Receive(R"({"type":"response.created","response":{"id":"silent"}})");
    silent.Receive(
        R"({"type":"response.function_call_arguments.done","response_id":"silent","name":"lamp_express","call_id":"silent-1","arguments":"{\"speech\":null,\"action\":\"nod\"}"})");
    const auto before = silent.socket_->sent.size();
    silent.Receive(R"({"type":"response.done","response":{"id":"silent","status":"completed"}})");
    assert(silent.expression_.silent && silent.continuing_ && !silent.recording_);
    for (size_t i = before; i < silent.socket_->sent.size(); ++i)
        assert(silent.socket_->sent[i].find("response.create") == std::string::npos);
    silent.PollExpression();
    assert(released_count == 2 && silent.expression_.args.empty());
    silent.PollExpression();
    assert(released_count == 2);
    assert(silent.silent_phase_ == OmniRealtimeProtocol::SilentPhase::Configure);
    silent.Receive(R"({"type":"session.updated","session":{"modalities":["text"]}})");
    assert(silent.socket_->sent.back().find("response.create") != std::string::npos);
    silent.Receive(R"({"type":"response.created","response":{"id":"silent-followup"}})");
    silent.Receive(
        R"({"type":"response.audio.delta","response_id":"silent-followup","delta":"AAAA"})");
    silent.Receive(
        R"({"type":"response.done","response":{"id":"silent-followup","status":"completed"}})");
    assert(silent.silent_phase_ == OmniRealtimeProtocol::SilentPhase::Restore);
    silent.Receive(R"({"type":"session.updated","session":{"modalities":["text","audio"]}})");
    assert(silent.silent_phase_ == OmniRealtimeProtocol::SilentPhase::Idle && !silent.close_);
    // Missing model tools can only be compensated from the matching real ASR item.
    silent.turn_input_item_ = "u1";
    silent.user_transcripts_["u1"] = "一边说话一边点头";
    assert(silent.ExplicitExpression().first == "A01" && !silent.ExplicitExpression().second);
    silent.user_transcripts_["u1"] = "你一边说话边点两下头";
    assert(silent.ExplicitExpression().first == "A01" && silent.ExplicitNodRepetitions() == 0);
    silent.user_transcripts_["u1"] = "一边说话一边点头，头点四次";
    assert(silent.ExplicitExpression().first == "A01" && silent.ExplicitNodRepetitions() == 4);
    silent.user_transcripts_["u1"] = "不要点四下头，只说话";
    assert(silent.ExplicitExpression().first.empty());
    silent.user_transcripts_["u1"] = "点头不要说话";
    assert(silent.ExplicitExpression().second);
    silent.user_transcripts_["u1"] = "只说话不要点头";
    assert(silent.ExplicitExpression().first.empty());
    silent.turn_input_item_ = "new-untranscribed-item";
    assert(silent.ExplicitExpression().first.empty());
    silent.Disconnect(false);
    OmniRealtimeProtocol missed;
    missed.ready_ = true;
    missed.manual_ = false;
    missed.socket_ = std::make_unique<WebSocket>();
    unsigned leaked_pcm = 0;
    missed.OnIncomingAudio([&](auto) { ++leaked_pcm; });
    missed.Receive(R"({"type":"input_audio_buffer.committed","item_id":"actual-user-1"})");
    missed.Receive(
        R"({"type":"conversation.item.input_audio_transcription.completed","item_id":"actual-user-1","transcript":"点头不要说话"})");
    missed.Receive(R"({"type":"response.created","response":{"id":"no-tools"}})");
    missed.Receive(R"({"type":"response.audio.delta","response_id":"no-tools","delta":"AAAA"})");
    missed.PollExpression();
    assert(leaked_pcm == 0 && released_count == 3);
    missed.PollExpression();
    assert(released_count == 3);
    missed.Receive(R"({"type":"response.done","response":{"id":"no-tools","status":"completed"}})");
    assert(missed.recording_ && !missed.close_);
    missed.Disconnect(false);
    OmniSetRuntime({}, {}, "");
    OmniSetStateReader({});
    OmniSetVision({});
    OmniRealtimeProtocol retry;
    retry.ready_ = true;
    retry.manual_ = false;
    retry.conversation_wanted_ = true;
    companion_enabled = companion_actions = true;
    retry.socket_ = std::make_unique<WebSocket>();
    retry.socket_->send_ok = false;
    assert(!retry.Write(R"({"type":"input_audio_buffer.append","audio":"AAAA"})"));
    assert(retry.reconnect_pending_ && retry.reconnect_attempts_ == 1);
    assert(retry.reconnect_companion_ && retry.reconnect_actions_);
    retry.Disconnect(false);
    assert(retry.expression_.args.empty() && retry.call_cache_.empty());
    omni_test_time = retry.reconnect_at_;
    retry.PollReconnect();
    assert(retry.socket_ && retry.socket_->sent.empty() && retry.restoring_preferences_);
    retry.Receive(R"({"type":"session.created","session":{"id":"fresh"}})");
    retry.Receive(R"({"type":"session.updated"})");
    assert(retry.ready_ && !retry.restoring_preferences_);
    assert(retry.ShouldResumeListeningAfterReconnect());
    // Application resumes its original mode only once, after a fresh session ACK.
    retry.SendStartListening(kListeningModeAutoStop);
    assert(retry.listen_requested_ == 2 && !retry.ShouldResumeListeningAfterReconnect());
    retry.ScheduleReconnect();
    assert(retry.reconnect_attempts_ == 2);
    retry.reconnect_pending_ = false;
    retry.ScheduleReconnect();
    assert(retry.reconnect_attempts_ == 3);
    retry.reconnect_pending_ = false;
    retry.ScheduleReconnect();
    assert(!retry.reconnect_pending_);
    retry.reconnect_attempts_ = 0;
    retry.ScheduleReconnect();
    retry.CloseAudioChannel();
    retry.PollReconnect();
    assert(!retry.conversation_wanted_ && !retry.reconnect_pending_);
    retry.resume_after_reconnect_ = true;
    assert(!retry.ShouldResumeListeningAfterReconnect());
    retry.conversation_wanted_ = true;
    retry.rest_requested_ = true;
    retry.ScheduleReconnect();
    assert(!retry.reconnect_pending_ && !retry.ShouldResumeListeningAfterReconnect());
    // Stable background energy should not permanently starve observation; an onset wins.
    OmniRealtimeProtocol ambient;
    std::string noise(1920, '\0');
    for (size_t i = 0; i < noise.size(); i += 2) {
        noise[i] = char(800 & 255);
        noise[i + 1] = char(800 >> 8);
    }
    assert(ambient.BackgroundQuiet(noise));
    std::string loud(1920, '\x7f');
    assert(!ambient.BackgroundQuiet(loud));
    ambient.ready_ = true;
    ambient.manual_ = false;
    ambient.recording_ = true;
    ambient.quiet_since_ = omni_test_time - 3000000;
    companion_enabled = true;
    observation_once = false;
    observation_wait_reason.clear();
    const auto waits_before = Board::GetInstance().observation_wait_events;
    for (int i = 0; i < 100; ++i)
        ambient.PollObservation();
    assert(observation_wait_reason == "waiting_for_fresh_camera_frame");
    assert(Board::GetInstance().observation_wait_events == waits_before + 1);
    auto info = OmniPanel();
    assert(info.find("dummy-secret") == std::string::npos);
    std::cout << "PASS handshake, PCM, stale response, function output/dedup, cancel and corrupt "
                 "payload\n";
}
