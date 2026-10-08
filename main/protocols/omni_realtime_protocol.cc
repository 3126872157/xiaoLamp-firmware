#include "omni_realtime_protocol.h"
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <cctype>
#include <cstring>
#include <mutex>
#include "application.h"
#include "board.h"
#include "omni_contract.h"

namespace {
std::mutex config_mutex;
std::string workspace, api_key, region = "cn-beijing", status = "unconfigured", error;
std::function<std::string()> state_reader;
std::atomic<bool> in_use{false};
std::atomic<uint32_t> playback_sequence{0};
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json Parse(const std::string& s) { return Json(cJSON_Parse(s.c_str()), cJSON_Delete); }
std::string Str(const cJSON* j, const char* key) {
    auto* v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
std::string Dump(const cJSON* j) {
    char* p = cJSON_PrintUnformatted(j);
    std::string s = p ? p : "{}";
    cJSON_free(p);
    return s;
}
void Status(const char* s, const char* e = "") {
    std::lock_guard lock(config_mutex);
    status = s;
    error = e;
}
bool Identifier(const std::string& s) { return omni::Identifier(s); }
}  // namespace

void OmniSetStateReader(std::function<std::string()> reader) {
    std::lock_guard lock(config_mutex);
    state_reader = std::move(reader);
}

std::string OmniPanel(const std::string& request) {
    auto body = Parse(request);
    auto op = Str(body.get(), "op");
    if (op == "configure" || op == "clear") {
        std::lock_guard lock(config_mutex);
        if (in_use)
            return R"({"ok":false,"error":{"message":"Close Omni before changing credentials"}})";
        if (op == "clear") {
            std::fill(api_key.begin(), api_key.end(), '\0');
            api_key.clear();
            workspace.clear();
            status = "unconfigured";
        } else {
            auto w = Str(body.get(), "workspace"), k = Str(body.get(), "key"),
                 r = Str(body.get(), "region");
            if (!omni::Credentials(w, k, r))
                return R"({"ok":false,"error":{"message":"Invalid workspace, region or key"}})";
            std::fill(api_key.begin(), api_key.end(), '\0');
            workspace = w;
            api_key = k;
            region = r;
            status = "configured";
            error.clear();
        }
    } else if (op == "start" || op == "vad") {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
            return R"({"ok":false,"error":{"message":"Cancel/close the current turn first"}})";
        if (op == "start")
            Application::GetInstance().StartListening();
        else
            Application::GetInstance().ToggleChatState();
    } else if (op == "finish") {
        Application::GetInstance().StopListening();
    } else if (op == "close") {
        Application::GetInstance().CloseConversation();
    } else if (op == "cancel") {
        Application::GetInstance().CloseConversation(true);
    } else if (!request.empty()) {
        return R"({"ok":false,"error":{"message":"Unknown Omni operation"}})";
    }
    std::lock_guard lock(config_mutex);
    auto j = Json(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddBoolToObject(j.get(), "ok", true);
    cJSON_AddBoolToObject(j.get(), "enabled", true);
    cJSON_AddBoolToObject(j.get(), "configured", !api_key.empty());
    cJSON_AddBoolToObject(j.get(), "in_use", in_use);
    cJSON_AddStringToObject(j.get(), "status", status.c_str());
    cJSON_AddStringToObject(j.get(), "error", error.c_str());
    cJSON_AddStringToObject(j.get(), "workspace", workspace.c_str());
    cJSON_AddStringToObject(j.get(), "region", region.c_str());
    cJSON_AddStringToObject(j.get(), "model", "qwen3.8-omni-flash-realtime");
    cJSON_AddBoolToObject(j.get(), "cloud_motion_enabled", false);
    return Dump(j.get());
}

OmniRealtimeProtocol::OmniRealtimeProtocol() {
    queue_ = xQueueCreate(24, sizeof(Work*));
    stopped_ = xSemaphoreCreateBinary();
}
OmniRealtimeProtocol::~OmniRealtimeProtocol() {
    running_ = false;
    if (task_started_)
        xSemaphoreTake(stopped_, portMAX_DELAY);
    Work* w;
    while (queue_ && xQueueReceive(queue_, &w, 0) == pdTRUE)
        delete w;
    if (queue_)
        vQueueDelete(queue_);
    if (stopped_)
        vSemaphoreDelete(stopped_);
}
bool OmniRealtimeProtocol::Start() {
    if (!queue_ || !stopped_) {
        SetError("Cannot allocate Omni worker");
        return false;
    }
    TaskHandle_t task = nullptr;
    if (xTaskCreate([](void* p) { static_cast<OmniRealtimeProtocol*>(p)->Run(); }, "omni_ws", 12288,
                    this, 3, &task) != pdPASS) {
        running_ = false;
        SetError("Cannot allocate Omni worker task");
        return false;
    }
    task_started_ = true;
    return true;
}
bool OmniRealtimeProtocol::Enqueue(Work* w) {
    if (!running_ || !queue_ || xQueueSend(queue_, &w, 0) != pdTRUE) {
        delete w;
        overloaded_ = true;
        return false;
    }
    return true;
}
bool OmniRealtimeProtocol::OpenAudioChannel() {
    if (!task_started_) {
        SetError("Omni worker is unavailable");
        return false;
    }
    std::lock_guard lock(config_mutex);
    if (api_key.empty()) {
        SetError("Configure Bailian credentials in the device web panel");
        return false;
    }
    in_use = true;
    open_ = true;
    status = "connecting";
    return true;
}
void OmniRealtimeProtocol::CloseAudioChannel(bool) {
    ready_ = false;
    recording_ = false;
    playback_ = 0;
    ++epoch_;
    close_ = true;
}
bool OmniRealtimeProtocol::SendText(const std::string& s) {
    return Enqueue(new Work{s, false, false, epoch_});
}
bool OmniRealtimeProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> p) {
    if (!ready_ || !recording_)
        return true;
    if (!p->pcm16 || p->sample_rate != 16000 || p->payload.empty() || p->payload.size() > 8192)
        return false;
    // Queue raw PCM: the audio/main tasks never perform TLS writes or JSON/base64 work.
    return Enqueue(
        new Work{std::string(reinterpret_cast<char*>(p->payload.data()), p->payload.size()), false,
                 true, epoch_});
}
void OmniRealtimeProtocol::SendStartListening(ListeningMode mode) {
    SendText(mode == kListeningModeManualStop ? "@manual" : "@vad");
}
void OmniRealtimeProtocol::SendStopListening() {
    recording_ = false;
    SendText("@finish");
}
void OmniRealtimeProtocol::SendAbortSpeaking(AbortReason) {
    recording_ = false;
    playback_ = 0;
    ++epoch_;
    cancel_ = true;
}
bool OmniRealtimeProtocol::Write(const std::string& s) {
    if (!socket_ || !socket_->IsConnected() || !socket_->Send(s)) {
        Fail("Omni transport send failed");
        return false;
    }
    return true;
}
void OmniRealtimeProtocol::Fail(const char* message) {
    ready_ = false;
    Status("failed", message);
    close_ = true;
    SetError(message);  // Never include provider payloads, headers or keys in diagnostics.
}
std::string OmniRealtimeProtocol::Session(bool manual) {
    auto j = Parse(omni::Session());
    if (!manual)
        cJSON_ReplaceItemInObject(
            cJSON_GetObjectItem(j.get(), "session"), "turn_detection",
            cJSON_Parse(R"({"type":"server_vad","threshold":0.5,"silence_duration_ms":800})"));
    return Dump(j.get());
}
void OmniRealtimeProtocol::Connect() {
    Disconnect(false);
    in_use = true;
    std::string url, key;
    {
        std::lock_guard lock(config_mutex);
        url = "wss://" + workspace + "." + region +
              ".maas.aliyuncs.com/api-ws/v1/realtime?model=qwen3.8-omni-flash-realtime";
        key = "Bearer " + api_key;
    }
    Status("connecting");
    connecting_since_ = esp_timer_get_time();
    session_since_ = connecting_since_;
    socket_ = Board::GetInstance().GetNetwork()->CreateWebSocket(1);
    if (!socket_) {
        Fail("Cannot allocate Omni transport");
        return;
    }
    socket_->SetHeader("Authorization", key.c_str());
    std::fill(key.begin(), key.end(), '\0');
    socket_->OnData([this](const char* data, size_t length, bool binary) {
        if (binary || length > 32768) {
            overloaded_ = true;
            return;
        }
        Enqueue(new Work{std::string(data, length), true, false, epoch_});
    });
    socket_->OnDisconnected([this] { disconnected_ = true; });
    if (!socket_->Connect(url.c_str()))
        Fail("Omni connect failed: check region/workspace/key/network");
}
void OmniRealtimeProtocol::Disconnect(bool notify) {
    const bool was_in_use = in_use;
    ready_ = false;
    recording_ = false;
    playback_ = 0;
    ++epoch_;
    if (socket_) {
        socket_.reset();
    }
    // The TLS destructor waits for its receive task. Invalidate anything queued during shutdown.
    ++epoch_;
    disconnected_ = false;
    response_.clear();
    cancelled_response_.clear();
    transcript_.clear();
    tool_call_.clear();
    tool_output_.clear();
    last_call_.clear();
    speaking_ = false;
    had_audio_ = false;
    connecting_since_ = 0;
    recording_since_ = 0;
    session_since_ = 0;
    if (notify)
        in_use = false;
    if (notify && was_in_use && on_audio_channel_closed_)
        on_audio_channel_closed_();
    if (notify && on_disconnected_)
        on_disconnected_();
    if (notify) {
        std::lock_guard lock(config_mutex);
        if (status != "failed" && status != "cancelled")
            status = "disconnected";
    }
}
void OmniRealtimeProtocol::Emit(const char* state, const std::string& text) {
    auto j = Parse(R"({"type":"tts"})");
    cJSON_AddStringToObject(j.get(), "state", state);
    cJSON_AddNumberToObject(j.get(), "playback_id", playback_);
    if (!text.empty())
        cJSON_AddStringToObject(j.get(), "text", text.c_str());
    if (on_incoming_json_)
        on_incoming_json_(j.get());
}
void OmniRealtimeProtocol::Receive(const std::string& text) {
    if (close_ || cancel_)
        return;
    auto j = Parse(text);
    if (!j) {
        Fail("Invalid Omni JSON event");
        return;
    }
    auto type = Str(j.get(), "type");
    if (type == "session.created") {
        session_id_ = Str(cJSON_GetObjectItem(j.get(), "session"), "id");
        Write(Session(true));
    } else if (type == "session.updated" && !ready_) {
        ready_ = true;
        connecting_since_ = 0;
        Status("ready");
        if (on_connected_)
            on_connected_();
        if (on_audio_channel_opened_)
            on_audio_channel_opened_();
    } else if (type == "error") {
        auto code = Str(cJSON_GetObjectItem(j.get(), "error"), "code");
        if (code == "response_cancel_not_active")
            return;
        Fail("Omni API rejected an event: inspect credentials/session format/quota");
    } else if (type == "input_audio_buffer.committed" && manual_) {
        Write(R"({"type":"response.create"})");
    } else if (type == "response.created") {
        auto id = Str(cJSON_GetObjectItem(j.get(), "response"), "id");
        if (id.empty() || id == cancelled_response_)
            return;
        response_ = id;
        recording_ = false;
        transcript_.clear();
        speaking_ = false;
        playback_ = ++playback_sequence;
        if (!playback_)
            playback_ = ++playback_sequence;
        Status("responding");
    } else if (type == "response.audio.delta") {
        if (response_.empty() || !playback_ || Str(j.get(), "response_id") != response_)
            return;
        auto encoded = Str(j.get(), "delta");
        if (encoded.empty() || encoded.size() > 24576) {
            Fail("Oversized Omni audio event");
            return;
        }
        std::vector<uint8_t> data(encoded.size() * 3 / 4 + 4);
        size_t length = 0;
        if (mbedtls_base64_decode(data.data(), data.size(), &length,
                                  reinterpret_cast<const unsigned char*>(encoded.data()),
                                  encoded.size()) != 0 ||
            length % 2) {
            Fail("Invalid Omni PCM payload");
            return;
        }
        if (!speaking_) {
            speaking_ = true;
            recording_ = false;
            Emit("start");
        }
        for (size_t offset = 0; offset < length; offset += 2880) {
            auto p = std::make_unique<AudioStreamPacket>();
            p->sample_rate = 24000;
            p->frame_duration = 60;
            p->pcm16 = true;
            p->playback_id = playback_;
            p->payload.assign(data.begin() + offset,
                              data.begin() + std::min(length, offset + 2880));
            if (on_incoming_audio_)
                on_incoming_audio_(std::move(p));
        }
    } else if (type == "response.audio_transcript.delta" || type == "response.text.delta") {
        if (Str(j.get(), "response_id") == response_ && playback_) {
            auto delta = Str(j.get(), "delta");
            if (transcript_.size() + delta.size() <= 4096)
                transcript_ += delta;
        }
    } else if (type == "response.function_call_arguments.done") {
        if (Str(j.get(), "response_id") != response_ || !playback_)
            return;
        auto call = Str(j.get(), "call_id");
        if (!Identifier(call) || call == last_call_)
            return;
        if (!tool_call_.empty()) {
            Fail("Multiple function calls in one response are not supported in this PoC");
            return;
        }
        last_call_ = call;
        tool_call_ = call;
        if (!omni::StateArguments(Str(j.get(), "name"), Str(j.get(), "arguments"))) {
            tool_output_ = R"({"ok":false,"error":"Only lamp_get_state({}) is allowed"})";
        } else {
            std::function<std::string()> reader;
            {
                std::lock_guard lock(config_mutex);
                reader = state_reader;
            }
            auto full = Parse(reader ? reader() : "{}");
            auto compact = Json(cJSON_CreateObject(), cJSON_Delete);
            for (auto key :
                 {"mode", "busy", "motion_enabled", "manual_control", "pose_rad", "last_task"}) {
                auto value = cJSON_GetObjectItem(full.get(), key);
                if (value)
                    cJSON_AddItemToObject(compact.get(), key, cJSON_Duplicate(value, true));
            }
            tool_output_ = Dump(compact.get());
            Board::GetInstance().RecordDiagnostic("tool", "lamp_get_state", tool_output_);
        }
    } else if (type == "response.done") {
        auto resp = cJSON_GetObjectItem(j.get(), "response");
        if (Str(resp, "id") != response_)
            return;
        if (Str(resp, "status") == "cancelled") {
            playback_ = 0;
            close_ = true;
            return;
        }
        if (Str(resp, "status") == "failed" || Str(resp, "status") == "incomplete") {
            Fail("Omni response failed or exceeded its output limit");
            return;
        }
        if (!transcript_.empty() && playback_)
            Emit("sentence_start", transcript_);
        if (!tool_call_.empty() && playback_ && Str(resp, "status") == "completed") {
            auto out = Parse(
                R"({"type":"conversation.item.create","item":{"type":"function_call_output"}})");
            auto item = cJSON_GetObjectItem(out.get(), "item");
            cJSON_AddStringToObject(item, "call_id", tool_call_.c_str());
            cJSON_AddStringToObject(item, "output", tool_output_.c_str());
            Write(Dump(out.get()));
            Write(R"({"type":"response.create"})");
            tool_call_.clear();
            tool_output_.clear();
        } else {
            if (speaking_ && playback_)
                Emit("stop");
            speaking_ = false;
            Status("ready");
        }
        response_.clear();
    }
}
void OmniRealtimeProtocol::Run() {
    while (running_) {
        if (close_.exchange(false) || disconnected_.exchange(false))
            Disconnect();
        if (overloaded_.exchange(false))
            Fail("Omni queue overflow; session stopped");
        if (open_.exchange(false))
            Connect();
        if (connecting_since_ && esp_timer_get_time() - connecting_since_ > 20000000)
            Fail("Omni session handshake timed out");
        if (manual_ && recording_ && recording_since_ &&
            esp_timer_get_time() - recording_since_ > 30000000)
            Fail("Manual recording exceeded 30 seconds; start a new turn");
        if (session_since_ && esp_timer_get_time() - session_since_ > 300000000)
            Fail("PoC session reached its five-minute limit; reconnect explicitly");
        if (cancel_.exchange(false)) {
            cancelled_response_ = response_;
            response_.clear();
            tool_call_.clear();
            tool_output_.clear();
            transcript_.clear();
            speaking_ = false;
            // Close the session on cancel: late response.created cannot start an old turn again.
            if (ready_ && !cancelled_response_.empty())
                Write(R"({"type":"response.cancel"})");
            if (ready_)
                Write(R"({"type":"input_audio_buffer.clear"})");
            close_ = true;
            ready_ = false;
            Status("cancelled");
        }
        Work* raw = nullptr;
        if (xQueueReceive(queue_, &raw, pdMS_TO_TICKS(20)) != pdTRUE)
            continue;
        std::unique_ptr<Work> w(raw);
        if (w->epoch != epoch_)
            continue;
        if (w->incoming) {
            Receive(w->text);
            continue;
        }
        if (!ready_)
            continue;
        if (w->audio) {
            size_t length = 0;
            std::string encoded(4 * ((w->text.size() + 2) / 3) + 1, '\0');
            if (mbedtls_base64_encode(
                    reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(), &length,
                    reinterpret_cast<const unsigned char*>(w->text.data()), w->text.size()) != 0) {
                Fail("PCM encoding failed");
                continue;
            }
            encoded.resize(length);
            if (Write("{\"type\":\"input_audio_buffer.append\",\"audio\":\"" + encoded + "\"}"))
                had_audio_ = true;
        } else if (w->text == "@manual" || w->text == "@vad") {
            manual_ = w->text == "@manual";
            // Audio format is immutable after first append; change only turn detection.
            Write(
                manual_
                    ? R"({"type":"session.update","session":{"turn_detection":null}})"
                    : R"({"type":"session.update","session":{"turn_detection":{"type":"server_vad","threshold":0.5,"silence_duration_ms":800}}})");
            Write(R"({"type":"input_audio_buffer.clear"})");
            had_audio_ = false;
            recording_ = true;
            recording_since_ = esp_timer_get_time();
            Status(manual_ ? "listening_manual" : "listening_vad");
        } else if (w->text == "@finish") {
            if (manual_ && had_audio_)
                Write(R"({"type":"input_audio_buffer.commit"})");
            had_audio_ = false;
        } else
            Write(w->text);
    }
    Disconnect();
    xSemaphoreGive(stopped_);
    vTaskDelete(nullptr);
}
