#include "omni_realtime_protocol.h"
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include "application.h"
#include "board.h"
#include "omni_contract.h"
#include "settings.h"

namespace {
std::mutex config_mutex;
std::string workspace, api_key, region = "cn-beijing", status = "unconfigured", error;
std::string last_stop_reason;
std::function<std::string()> state_reader;
std::function<std::future<std::string>(const std::string&, const std::string&, uint64_t)>
    runtime_dispatch;
std::function<void(uint64_t)> runtime_cancel;
std::string runtime_tools;
std::function<std::shared_ptr<const OmniImage>()> image_reader;
bool image_enabled = false;
std::function<void(bool, unsigned)> vision_periodic;
std::function<bool(const OmniImage&)> image_valid;
bool companion_enabled = false, companion_actions = false;
unsigned observation_interval_s = 5;
std::atomic<bool> observation_once{false}, observation_reset{false};
std::string observation_status = "disabled", observation_error, observation_cache;
int64_t observation_at = 0;
uint64_t observation_cached_frame = 0;
unsigned observation_requests = 0, observation_preemptions = 0;
int64_t last_autonomous_action = 0;
std::string image_error;
std::atomic<uint64_t> image_sent_frame{0};
std::atomic<uint64_t> turn_sequence{0};
std::atomic<bool> in_use{false};
std::atomic<uint32_t> playback_sequence{0};
std::atomic<uint32_t> receive_waits{0};
bool configuration_loaded = false;
void LoadConfiguration() {
    if (configuration_loaded)
        return;
    configuration_loaded = true;
    Settings saved("lamp_omni", false);
    auto w = saved.GetString("workspace"), k = saved.GetString("key"),
         r = saved.GetString("region");
    if (omni::Credentials(w, k, r)) {
        workspace = w;
        api_key = k;
        region = r;
        status = "configured";
    }
}
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
void PauseCompanion() {
    std::lock_guard lock(config_mutex);
    companion_enabled = companion_actions = false;
    observation_once = false;
    observation_status = "disabled";
    if (vision_periodic)
        vision_periodic(false, observation_interval_s);
}
void Status(const char* s, const char* e = "", const char* reason = nullptr) {
    std::lock_guard lock(config_mutex);
    status = s;
    error = e;
    if (reason)
        last_stop_reason = reason;
    else if (status == "failed")
        last_stop_reason = e;
}
}  // namespace

void OmniSetVision(std::function<std::shared_ptr<const OmniImage>()> latest,
                   std::function<void(bool, unsigned)> periodic,
                   std::function<bool(const OmniImage&)> valid) {
    std::lock_guard lock(config_mutex);
    image_reader = std::move(latest);
    vision_periodic = std::move(periodic);
    image_valid = std::move(valid);
}
void OmniSetStateReader(std::function<std::string()> reader) {
    std::lock_guard lock(config_mutex);
    state_reader = std::move(reader);
}
void OmniSetRuntime(
    std::function<std::future<std::string>(const std::string&, const std::string&, uint64_t)>
        dispatch,
    std::function<void(uint64_t)> cancel, const std::string& tools) {
    std::lock_guard lock(config_mutex);
    runtime_dispatch = std::move(dispatch);
    runtime_cancel = std::move(cancel);
    runtime_tools = tools;
}

std::string OmniPanel(const std::string& request) {
    auto body = Parse(request);
    auto op = Str(body.get(), "op");
    if (op == "companion" || op == "observe_once") {
        std::lock_guard lock(config_mutex);
        if (!image_reader || !in_use)
            return R"({"ok":false,"error":{"message":"Start VAD conversation first; this experiment reuses that connection"}})";
        if (op == "observe_once") {
            observation_once = true;
            observation_reset = true;
        } else {
            auto* enabled = cJSON_GetObjectItem(body.get(), "enabled");
            auto* interval = cJSON_GetObjectItem(body.get(), "interval_s");
            auto* actions = cJSON_GetObjectItem(body.get(), "actions");
            if (!cJSON_IsBool(enabled) || !cJSON_IsNumber(interval) || interval->valuedouble < 2 ||
                interval->valuedouble > 30 || interval->valuedouble != interval->valueint ||
                !cJSON_IsBool(actions))
                return R"({"ok":false,"error":{"message":"Expected enabled/actions booleans and interval_s 2..30"}})";
            companion_enabled = cJSON_IsTrue(enabled);
            if (companion_enabled)
                observation_reset = true;
            companion_actions = cJSON_IsTrue(actions);
            observation_interval_s = interval->valueint;
            if (vision_periodic)
                vision_periodic(companion_enabled, observation_interval_s);
            if (!companion_enabled)
                observation_status = "disabled";
        }
    } else if (op == "vision_on" || op == "vision_off") {
        std::lock_guard lock(config_mutex);
        if (!image_reader || in_use)
            return R"({"ok":false,"error":{"message":"Close conversation before changing manual image input"}})";
        image_enabled = op == "vision_on";
        image_error.clear();
    } else if (op == "configure" || op == "clear") {
        std::lock_guard lock(config_mutex);
        LoadConfiguration();
        if (in_use)
            return R"({"ok":false,"error":{"message":"Close Omni before changing credentials"}})";
        if (op == "clear") {
            std::fill(api_key.begin(), api_key.end(), '\0');
            api_key.clear();
            workspace.clear();
            status = "unconfigured";
            Settings saved("lamp_omni", true);
            // Settings commits SetString writes; its erase helpers do not mark dirty.
            saved.SetString("workspace", "");
            saved.SetString("key", "");
            saved.SetString("region", "");
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
            Settings saved("lamp_omni", true);
            saved.SetString("workspace", workspace);
            saved.SetString("region", region);
            saved.SetString("key", api_key);
        }
    } else if (op == "start" || op == "vad") {
        {
            std::lock_guard lock(config_mutex);
            if (op == "vad" && image_enabled)
                return R"({"ok":false,"error":{"message":"Disable manual image input before starting VAD"}})";
        }
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
    LoadConfiguration();
    auto j = Json(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddBoolToObject(j.get(), "ok", true);
    cJSON_AddBoolToObject(j.get(), "enabled", true);
    cJSON_AddBoolToObject(j.get(), "configured", !api_key.empty());
    cJSON_AddBoolToObject(j.get(), "in_use", in_use);
    cJSON_AddStringToObject(j.get(), "status", status.c_str());
    cJSON_AddStringToObject(j.get(), "error", error.c_str());
    cJSON_AddStringToObject(j.get(), "last_stop_reason", last_stop_reason.c_str());
    cJSON_AddStringToObject(j.get(), "workspace", workspace.c_str());
    cJSON_AddStringToObject(j.get(), "region", region.c_str());
    cJSON_AddStringToObject(j.get(), "model", "qwen3.8-omni-flash-realtime");
    cJSON_AddBoolToObject(j.get(), "cloud_motion_enabled", !runtime_tools.empty());
    cJSON_AddStringToObject(j.get(), "executor_stage",
                            runtime_tools.empty() ? "poc" : "semantic_tools");
    cJSON_AddBoolToObject(j.get(), "credentials_persistent", true);
    cJSON_AddNumberToObject(j.get(), "receive_backpressure_waits", receive_waits);
    cJSON_AddBoolToObject(j.get(), "manual_image_supported", bool(image_reader));
    cJSON_AddBoolToObject(j.get(), "manual_image_enabled", image_enabled);
    cJSON_AddStringToObject(j.get(), "image_error", image_error.c_str());
    cJSON_AddNumberToObject(j.get(), "image_sent_frame_id", image_sent_frame);
    cJSON_AddBoolToObject(j.get(), "companion_enabled", companion_enabled);
    cJSON_AddBoolToObject(j.get(), "companion_actions", companion_actions);
    cJSON_AddNumberToObject(j.get(), "observation_interval_s", observation_interval_s);
    cJSON_AddStringToObject(j.get(), "observation_status", observation_status.c_str());
    cJSON_AddStringToObject(j.get(), "observation_error", observation_error.c_str());
    cJSON_AddNumberToObject(j.get(), "observation_requests", observation_requests);
    cJSON_AddNumberToObject(j.get(), "observation_preemptions", observation_preemptions);
    cJSON_AddNumberToObject(j.get(), "observation_frame_id", observation_cached_frame);
    const auto age = observation_at ? (esp_timer_get_time() - observation_at) / 1000 : -1;
    cJSON_AddNumberToObject(j.get(), "observation_age_ms", age);
    cJSON_AddBoolToObject(j.get(), "observation_fresh",
                          age >= 0 && age <= observation_interval_s * 2000);
    if (!observation_cache.empty())
        cJSON_AddItemToObject(j.get(), "observation", cJSON_Parse(observation_cache.c_str()));
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
    if (w->incoming && queue_) {
        // Bound memory and let TCP apply backpressure instead of cancelling a normal burst.
        while (running_ && w->epoch == epoch_ && !close_ && !cancel_) {
            if (xQueueSend(queue_, &w, pdMS_TO_TICKS(20)) == pdTRUE)
                return true;
            ++receive_waits;
        }
        delete w;
        return false;  // Teardown invalidated this event; not an overflow failure.
    }
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
    LoadConfiguration();
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
    {
        std::lock_guard lock(config_mutex);
        if (runtime_cancel && turn_)
            runtime_cancel(turn_);
    }
    ready_ = false;
    recording_ = false;
    playback_ = 0;
    ++epoch_;
    close_ = true;
}
void OmniRealtimeProtocol::OnPlaybackDrained() {
    if (rest_requested_ && rest_reply_done_.exchange(false)) {
        Status("resting", "", "model_end_conversation");
        Board::GetInstance().RecordDiagnostic("voice_rest", "omni", "model_end_conversation");
        CloseAudioChannel();
    }
}
bool OmniRealtimeProtocol::SendText(const std::string& s) {
    return Enqueue(new Work{s, false, false, epoch_});
}
bool OmniRealtimeProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> p) {
    if (!ready_ || (!recording_ && observation_phase_ == ObservationPhase::Idle) || rest_requested_)
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
void OmniRealtimeProtocol::SendAbortSpeaking(AbortReason reason) {
    {
        std::lock_guard lock(config_mutex);
        if (runtime_cancel && turn_)
            runtime_cancel(turn_);
    }
    abort_reason_ = reason;
    recording_ = false;
    playback_ = 0;
    ++epoch_;
    cancel_ = true;
}
bool OmniRealtimeProtocol::Write(const std::string& s) {
    if (!socket_ || !socket_->IsConnected() || !socket_->Send(s)) {
        auto event = Parse(s);
        const auto type = Str(event.get(), "type");
        Board::GetInstance().RecordDiagnostic(
            "transport_send", type,
            "bytes=" + std::to_string(s.size()) +
                (!socket_                  ? "; no socket"
                 : !socket_->IsConnected() ? "; disconnected socket"
                                           : "; socket send returned false"));
        Fail("Omni transport send failed; reopen conversation to create a fresh connection");
        return false;
    }
    return true;
}
void OmniRealtimeProtocol::Fail(const char* message) {
    ready_ = false;
    Status("failed", message);
    close_ = true;
    SetError(message);  // Never include provider payloads, headers or keys in diagnostics.
    Board::GetInstance().RecordDiagnostic("voice_error", "omni", message);
}
std::string OmniRealtimeProtocol::Session(bool manual) {
    auto j = Parse(omni::Session());
    {
        std::lock_guard lock(config_mutex);
        if (!runtime_tools.empty()) {
            auto* session = cJSON_GetObjectItem(j.get(), "session");
            cJSON_ReplaceItemInObject(session, "tools", cJSON_Parse(runtime_tools.c_str()));
            cJSON_ReplaceItemInObject(
                session, "instructions",
                cJSON_CreateString(
                    "你是热情自然的桌面台灯，用简短中文对话。通过提供的有限语义工具"
                    "控制台灯，机械参数使用本地默认。"
                    "机械臂在线只由lamp_get_state的available=true判断，runtime_ready仅软件在运行。"
                    "mode是保存的动作参考，不代表实际姿态；pose_rad为空或available="
                    "false时姿态未知。"
                    "你是实际可执行的台灯：用户明确的运动、休息、结束聊天命令必须调用相应工具，纯口"
                    "头承诺不算执行。"
                    "站起来/"
                    "站好=>lamp_set_mode({mode:stand})；坐下=>lamp_set_mode({mode:sit})；开始工作=>"
                    "lamp_set_mode({mode:work})。"
                    "你去休息/休息一下/"
                    "睡吧=>lamp_rest({})，它负责坐下、卸力、关闭会话；绝不能仅说好的我去休息。"
                    "拜拜/先不聊了=>lamp_end_conversation({})。用户自己休息或讨论休息，不控制台灯。"
                    "明确上力必须调用lamp_set_torque(enabled=true)，不得只口头说已经上力。"
                    "明确运动命令已授权必要的使能：lamp_set_mode/"
                    "lamp_rest会自动处理；其他明确运动用lamp_run_sequence的enable_if_needed=true。"
                    "普通聊天、自发表达、后台事件不能自动使能，enable_if_needed默认false。"
                    "只有execution_completed=true才能说已站起/"
                    "已到位；ok=false或failed必须如实报告原因。"
                    "accepted仅受理，不得声称已完成；运动授权和力矩不同，查询只在需"
                    "要时调用，不反复get_state。"
                    "每轮最多一个动作组，正常对话允许不动。点两下头只用一次A01。靠近"
                    "用A13，不用低头代替；缺标定如实说明。"
                    "用户明确说拜拜、结束聊天、先不聊了等广义告别时调用lamp_end_"
                    "conversation并简短告别；讨论告别或用户说自己休息不结束。"
                    "lamp_rest等待真实成功才告别，不成功就说明失败并保留会话。单独明确要求上力/"
                    "别用力调用lamp_set_torque。"
                    "明确的纯动作指令安静执行，不播准备、收到或成功；只有失败才简短提醒。普通问答只"
                    "回答，不乱动。"
                    "图片仅在开启手动附图并且本轮实际收到新图片时可用，不能将旧图当实时画面。"
                    "后台视觉仅明确开启后运行，观察正文不朗读。语音伴随动作尚未开放，不声称已实现"
                    "。"));
        }
    }
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
    const auto generation = epoch_.load();
    socket_->OnData([this, generation](const char* data, size_t length, bool binary) {
        if (generation != epoch_ || !running_)
            return;
        if (binary || length > 32768) {
            overloaded_ = true;
            return;
        }
        Enqueue(new Work{std::string(data, length), true, false, generation});
    });
    socket_->OnDisconnected([this, generation] {
        if (generation == epoch_)
            disconnected_ = true;
    });
    if (!socket_->Connect(url.c_str()))
        Fail("Omni connect failed: check region/workspace/key/network");
}
void OmniRealtimeProtocol::Disconnect(bool notify) {
    {
        std::function<void(uint64_t)> cancel;
        {
            std::lock_guard lock(config_mutex);
            cancel = runtime_cancel;
        }
        if (cancel && turn_)
            cancel(turn_);
    }
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
    observation_phase_ = ObservationPhase::Idle;
    observation_frame_.reset();
    observation_attempts_ = 0;
    observation_response_.clear();
    observation_text_.clear();
    speech_prefill_.clear();
    observation_deadline_ = sample_deadline_ = quiet_since_ = 0;
    observation_turn_ = 0;
    observed_frame_ = 0;
    have_luminance_ = false;
    model_commit_pending_ = sample_sent_ = false;
    background_task_ = {};
    {
        std::lock_guard lock(config_mutex);
        companion_enabled = false;
        companion_actions = false;
        observation_once = false;
        observation_cache.clear();
        observation_at = 0;
        observation_cached_frame = 0;
        observation_status = "disabled";
        if (vision_periodic)
            vision_periodic(false, observation_interval_s);
    }
    disconnected_ = false;
    Work* stale = nullptr;
    while (queue_ && xQueueReceive(queue_, &stale, 0) == pdTRUE)
        delete stale;
    close_ = false;
    response_.clear();
    cancelled_response_.clear();
    transcript_.clear();
    tools_.clear();
    call_cache_.clear();
    continuing_ = awaiting_tools_ = false;
    tool_rounds_ = 0;
    speaking_ = false;
    had_audio_ = false;
    rest_requested_ = false;
    rest_reply_done_ = false;
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
        if (status != "failed" && status != "cancelled" && status != "resting")
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
    if (ObservationEvent(j.get(), type))
        return;
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
        if (awaiting_tools_) {
            std::lock_guard lock(config_mutex);
            if (runtime_cancel && turn_)
                runtime_cancel(turn_);
            tools_.clear();
            awaiting_tools_ = continuing_ = false;
        }
        if (!continuing_) {
            {
                std::lock_guard lock(config_mutex);
                if (runtime_cancel && turn_)
                    runtime_cancel(turn_);
            }
            turn_ = ++turn_sequence;
            tool_rounds_ = 0;
        }
        continuing_ = false;
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
            if (!AcceptsPlayback(playback_))
                break;
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
        if (call.empty() || call.size() > 512 || call.find_first_of("\r\n") != std::string::npos)
            return;
        const auto name = Str(j.get(), "name"), arguments = Str(j.get(), "arguments");
        const auto signature = name + "\n" + arguments;
        for (const auto& tool : tools_) {
            if (tool.id == call) {
                if (tool.name != name || tool.arguments != arguments)
                    Fail("Conflicting duplicate call_id");
                return;
            }
        }
        if (tools_.size() >= 8 || arguments.size() > 24576) {
            Fail("Tool batch or arguments exceed bounded runtime capacity");
            return;
        }
        Tool tool{call, name, arguments, "", {}};
        auto cached = call_cache_.find(call);
        if (cached != call_cache_.end()) {
            if (cached->second.first != signature) {
                Fail("Conflicting reused call_id");
                return;
            }
            tool.output = cached->second.second;
        } else if (call_cache_.size() >= 64) {
            tool.output =
                R"({"ok":false,"error":"Session tool cache full; start a new conversation"})";
        }
        tools_.push_back(std::move(tool));
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
        if (!tools_.empty() && playback_ && Str(resp, "status") == "completed") {
            if (++tool_rounds_ > 4) {
                Fail("Tool continuation limit reached; no automatic retries");
                return;
            }
            awaiting_tools_ = true;
            CompleteTools();
        } else {
            Status(rest_requested_ ? "rest_pending" : "ready");
            rest_reply_done_ = rest_requested_.load();
            if ((speaking_ || rest_requested_) && playback_)
                Emit("stop");
            speaking_ = false;
        }
        response_.clear();
    }
}
void OmniRealtimeProtocol::CompleteTools() {
    std::function<std::future<std::string>(const std::string&, const std::string&, uint64_t)>
        dispatch;
    std::function<std::string()> reader;
    {
        std::lock_guard lock(config_mutex);
        dispatch = runtime_dispatch;
        reader = state_reader;
    }
    // Dispatch emergency controls before motion, independent of their network arrival order.
    std::stable_sort(tools_.begin(), tools_.end(), [](const Tool& a, const Tool& b) {
        auto urgent = [](const Tool& t) {
            auto args = Parse(t.arguments);
            return t.name == "lamp_stop" ||
                   (t.name == "lamp_set_torque" &&
                    cJSON_IsFalse(cJSON_GetObjectItem(args.get(), "enabled")));
        };
        return urgent(a) > urgent(b);
    });
    const bool stop_batch = std::any_of(tools_.begin(), tools_.end(), [](const Tool& t) {
        auto args = Parse(t.arguments);
        return t.name == "lamp_stop" || (t.name == "lamp_set_torque" &&
                                         cJSON_IsFalse(cJSON_GetObjectItem(args.get(), "enabled")));
    });
    bool waiting = false;
    for (auto& tool : tools_) {
        if (tool.output.empty() && !tool.result.valid()) {
            if (tool.name == "lamp_end_conversation" &&
                omni::ToolArguments(tool.name, tool.arguments)) {
                rest_requested_ = true;
                PauseCompanion();
                recording_ = false;
                tool.output =
                    R"({"ok":true,"status":"rest_pending","instruction":"Say one short goodbye; close after playback. No servo or torque change."})";
            } else if (stop_batch &&
                       (tool.name == "lamp_run_sequence" || tool.name == "lamp_set_mode" ||
                        tool.name == "lamp_rest" || tool.name == "lamp_record_calibration" ||
                        tool.name == "lamp_set_torque")) {
                auto args = Parse(tool.arguments);
                if (tool.name == "lamp_set_torque" &&
                    cJSON_IsFalse(cJSON_GetObjectItem(args.get(), "enabled")) && dispatch)
                    tool.result = dispatch(tool.name, tool.arguments, turn_);
                else
                    tool.output =
                        R"({"ok":false,"error":"Motion suppressed by stop/unload in the same response"})";
            } else if (dispatch) {
                tool.result = dispatch(tool.name, tool.arguments, turn_);
            } else if (tool.name == "lamp_get_state" &&
                       omni::ToolArguments(tool.name, tool.arguments)) {
                auto full = Parse(reader ? reader() : "{}");
                auto compact = Json(cJSON_CreateObject(), cJSON_Delete);
                for (auto key : {"mode", "busy", "motion_enabled", "manual_control", "pose_rad",
                                 "last_task"}) {
                    auto* value = cJSON_GetObjectItem(full.get(), key);
                    if (value)
                        cJSON_AddItemToObject(compact.get(), key, cJSON_Duplicate(value, true));
                }
                tool.output = Dump(compact.get());
            } else
                tool.output = R"({"ok":false,"error":"Unsupported tool or arguments"})";
        }
        if (tool.output.empty() && tool.result.valid()) {
            if (tool.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                waiting = true;
                break;  // Preserve dependencies: explicit enable must finish before a later motion.
            }
            tool.output = tool.result.get();
            auto confirmed = Parse(tool.output);
            auto args = Parse(tool.arguments);
            if (cJSON_IsTrue(cJSON_GetObjectItem(confirmed.get(), "ok")) &&
                (tool.name == "lamp_stop" ||
                 (tool.name == "lamp_set_torque" &&
                  cJSON_IsFalse(cJSON_GetObjectItem(args.get(), "enabled")))))
                PauseCompanion();
            if (tool.name == "lamp_rest") {
                auto j = Parse(tool.output);
                auto* result = cJSON_GetObjectItem(j.get(), "result");
                if (cJSON_IsTrue(cJSON_GetObjectItem(j.get(), "ok")) &&
                    Str(result, "status") == "completed") {
                    rest_requested_ = true;
                    PauseCompanion();
                }
            }
        }
    }
    if (waiting)
        return;  // Worker keeps processing cancel/network/audio while controls run.
    for (auto& tool : tools_) {
        auto out =
            Parse(R"({"type":"conversation.item.create","item":{"type":"function_call_output"}})");
        auto* item = cJSON_GetObjectItem(out.get(), "item");
        cJSON_AddStringToObject(item, "call_id", tool.id.c_str());
        cJSON_AddStringToObject(item, "output", tool.output.c_str());
        if (!Write(Dump(out.get())))
            return;
        if (call_cache_.size() < 64)
            call_cache_.try_emplace(tool.id, tool.name + "\n" + tool.arguments, tool.output);
        Board::GetInstance().RecordDiagnostic("tool", tool.name, tool.output);
    }
    tools_.clear();
    awaiting_tools_ = false;
    continuing_ = true;
    Write(R"({"type":"response.create"})");
}
std::string OmniRuntimeState() {
    std::function<std::string()> reader;
    {
        std::lock_guard lock(config_mutex);
        reader = state_reader;
    }
    return reader ? reader() : "{}";
}
bool OmniRealtimeProtocol::AppendImage() {
    std::function<std::shared_ptr<const OmniImage>()> reader;
    {
        std::lock_guard lock(config_mutex);
        if (!image_enabled)
            return true;
        reader = image_reader;
        image_error.clear();
    }
    auto frame = reader ? reader() : nullptr;
    const auto age = frame ? esp_timer_get_time() - frame->captured_at_us : -1;
    if (!manual_ || !had_audio_ || !frame || age < 0 || age > 30000000 || frame->jpeg.size() < 4 ||
        frame->jpeg.size() > 65536 || frame->jpeg[0] != 0xff || frame->jpeg[1] != 0xd8 ||
        frame->jpeg[frame->jpeg.size() - 2] != 0xff || frame->jpeg.back() != 0xd9) {
        std::lock_guard lock(config_mutex);
        image_error =
            "Need a fresh (30 seconds) JPEG and real audio in manual mode; recapture then finish "
            "again";
        return false;
    }
    return SendImageFrame(*frame);
}
bool OmniRealtimeProtocol::SendImageFrame(const OmniImage& value) {
    const auto* frame = &value;
    const auto age = esp_timer_get_time() - frame->captured_at_us;
    size_t size = 0;
    const std::string prefix = "{\"type\":\"input_image_buffer.append\",\"image\":\"";
    std::string data(prefix.size() + 4 * ((frame->jpeg.size() + 2) / 3) + 3, '\0');
    std::copy(prefix.begin(), prefix.end(), data.begin());
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(data.data() + prefix.size()),
                              data.size() - prefix.size(), &size, frame->jpeg.data(),
                              frame->jpeg.size()) != 0)
        return false;
    data.resize(prefix.size() + size);
    data += "\"}";
    if (!Write(data))
        return false;
    image_sent_frame = frame->frame_id;
    Board::GetInstance().RecordDiagnostic(
        "vision_upload",
        observation_phase_ == ObservationPhase::Idle ? "manual_audio" : "ambient_audio",
        "frame=" + std::to_string(frame->frame_id) + "; bytes=" +
            std::to_string(frame->jpeg.size()) + "; age_ms=" + std::to_string(age / 1000));
    return true;
}
bool OmniRealtimeProtocol::QuietPacket(const std::string& pcm) const {
    if (pcm.empty() || pcm.size() % 2)
        return true;
    uint64_t sum = 0;
    for (size_t i = 0; i < pcm.size(); i += 2) {
        const int value = int16_t(uint16_t(uint8_t(pcm[i])) | uint16_t(uint8_t(pcm[i + 1])) << 8);
        sum += std::abs(value);
    }
    return sum / (pcm.size() / 2) <
           500;  // Conservative foreground priority, not speaker/AEC interruption.
}
void OmniRealtimeProtocol::RestoreForeground(const char* reason, bool voice) {
    if (observation_phase_ == ObservationPhase::Idle)
        return;
    if (observation_phase_ == ObservationPhase::Model) {
        if (!observation_response_.empty())
            cancelled_response_ = observation_response_;
        Write(R"({"type":"response.cancel"})");
    }
    recording_ = false;
    observation_phase_ = ObservationPhase::Restore;
    restoring_for_voice_ = voice;
    model_commit_pending_ = false;
    if (!voice && std::string(reason) != "completed" &&
        std::string(reason) != "companion_disabled") {
        std::lock_guard lock(config_mutex);
        if (observation_error.empty())
            observation_error = reason;
    }
    if (voice) {
        std::function<void(uint64_t)> cancel;
        {
            std::lock_guard lock(config_mutex);
            ++observation_preemptions;
            cancel = runtime_cancel;
        }
        if (cancel && observation_turn_)
            cancel(observation_turn_);
    }
    Write(R"({"type":"input_audio_buffer.clear"})");
    auto foreground = Parse(Session(manual_));
    cJSON_DeleteItemFromObject(cJSON_GetObjectItem(foreground.get(), "session"), "audio");
    Write(Dump(foreground.get()));
    observation_deadline_ = esp_timer_get_time() + 5000000;
    Board::GetInstance().RecordDiagnostic("observation_restore", "omni", reason);
}
void OmniRealtimeProtocol::PollObservation() {
    const auto now = esp_timer_get_time();
    if (observation_reset.exchange(false))
        observation_attempts_ = 0;
    if (!ready_ || close_ || cancel_ || rest_requested_)
        return;
    if (background_task_.valid() &&
        background_task_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Board::GetInstance().RecordDiagnostic("companion_task", "result", background_task_.get());
    }
    if (observation_attempts_ >= 2) {
        std::lock_guard lock(config_mutex);
        companion_enabled = companion_actions = false;
        observation_once = false;
        observation_status = "paused_after_two_failures";
        if (vision_periodic)
            vision_periodic(false, observation_interval_s);
    }
    if (observation_phase_ != ObservationPhase::Idle) {
        bool enabled;
        {
            std::lock_guard lock(config_mutex);
            enabled = companion_enabled;
        }
        if (!enabled && !observation_is_once_ && observation_phase_ != ObservationPhase::Restore) {
            RestoreForeground("companion_disabled");
            return;
        }
        if (observation_deadline_ && now > observation_deadline_) {
            if (observation_phase_ == ObservationPhase::Restore) {
                Fail("Observation config restore timed out; reopen foreground");
                return;
            }
            {
                std::lock_guard lock(config_mutex);
                observation_error = "Observation timed out; no capability conclusion";
            }
            ++observation_attempts_;
            RestoreForeground("timeout");
            return;
        }
        if (observation_phase_ == ObservationPhase::Sample && now >= sample_deadline_) {
            if (!sample_sent_) {
                ++observation_attempts_;
                RestoreForeground("No real microphone samples; no placeholder audio");
                return;
            }
            if (!observation_frame_ || now - observation_frame_->captured_at_us > 30000000) {
                ++observation_attempts_;
                RestoreForeground("expired_frame");
                return;
            }
            if (!SendImageFrame(*observation_frame_)) {
                RestoreForeground("image_send_failed");
                return;
            }
            observation_phase_ = ObservationPhase::Model;
            model_commit_pending_ = true;
            recording_ = false;
            Write(R"({"type":"input_audio_buffer.commit"})");
        }
        return;
    }
    std::function<std::shared_ptr<const OmniImage>()> reader;
    std::function<std::string()> state;
    bool enabled;
    unsigned interval;
    {
        std::lock_guard lock(config_mutex);
        reader = image_reader;
        state = state_reader;
        enabled = companion_enabled;
        interval = observation_interval_s;
    }
    const bool once = observation_once.load();
    if (observation_attempts_ >= 2 || (!enabled && !once) || manual_ || !recording_ ||
        !response_.empty() || awaiting_tools_ || speaking_ || !quiet_since_ ||
        now - quiet_since_ < 2500000 || background_task_.valid())
        return;
    auto frame = reader ? reader() : nullptr;
    if (!frame || frame->frame_id == observed_frame_ ||
        now - frame->captured_at_us > int64_t(interval) * 2000000)
        return;
    auto hardware = Parse(state ? state() : "{}");
    if (cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "busy")) ||
        cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "manual_control")))
        return;
    if (!once && have_luminance_ && frame->luminance_valid) {
        int mean = 0;
        for (size_t i = 0; i < 192; i++)
            mean += int(frame->luminance[i]) - int(observed_luminance_[i]);
        mean /= 192;
        unsigned change = 0;
        for (size_t i = 0; i < 192; i++)
            change += std::abs(int(frame->luminance[i]) - int(observed_luminance_[i]) - mean);
        if (change / 192 < 12) {
            observed_frame_ = frame->frame_id;
            return;
        }
    }
    observation_once = false;
    observation_is_once_ = once;
    observation_frame_ = frame;
    observed_frame_ = frame->frame_id;
    observed_luminance_ = frame->luminance;
    have_luminance_ = frame->luminance_valid;
    observation_response_.clear();
    observation_text_.clear();
    sample_sent_ = model_commit_pending_ = false;
    restoring_for_voice_ = false;
    observation_phase_ = ObservationPhase::Configure;
    observation_deadline_ = now + 20000000;
    auto config = Parse(Session(true));
    auto* session = cJSON_GetObjectItem(config.get(), "session");
    cJSON_DeleteItemFromObject(session, "audio");
    cJSON_ReplaceItemInObject(session, "modalities", cJSON_Parse("[\"text\"]"));
    cJSON_ReplaceItemInObject(session, "tools", cJSON_CreateArray());
    const auto instruction =
        std::string(
            "这是独立后台视觉观察轮次，不是用户说话。真实麦克风片段只作环境采样，不把噪声解释成命令"
            "，不回复历史用户发言。只观察这次新图片，只输出一个JSON对象，禁止语音和工具调用。schema"
            "={frame_id:整数,person:present/absent/unknown,book:present/absent/"
            "unknown,activity:reading/looking/away/"
            "unknown,confidence:0到1,summary:简短客观中文,action:none/nod/tilt/gaze_user/"
            "gaze_book}。看不清用unknown，不能猜坐标。默认action="
            "none，仅有明显有意义事件才建议轻量动作。frame_id=") +
        std::to_string(frame->frame_id);
    cJSON_ReplaceItemInObject(session, "instructions", cJSON_CreateString(instruction.c_str()));
    recording_ = false;
    Write(R"({"type":"input_audio_buffer.clear"})");
    Write(Dump(config.get()));
    {
        std::lock_guard lock(config_mutex);
        observation_status = "configuring";
        observation_error.clear();
        ++observation_requests;
    }
    Board::GetInstance().RecordDiagnostic("observation_request", "ambient_real_audio",
                                          "frame=" + std::to_string(frame->frame_id));
}
bool OmniRealtimeProtocol::ObservationEvent(const cJSON* j, const std::string& type) {
    if (observation_phase_ == ObservationPhase::Idle) {
        if (type == "input_audio_buffer.speech_started")
            quiet_since_ = esp_timer_get_time();
        return false;
    }
    if (type == "error") {
        if (Str(cJSON_GetObjectItem(j, "error"), "code") == "response_cancel_not_active")
            return true;
        {
            std::lock_guard lock(config_mutex);
            observation_error = "Omni rejected observation event (" +
                                Str(cJSON_GetObjectItem(j, "error"), "code") +
                                "); inspect protocol/quota/network before fallback";
        }
        if (observation_phase_ != ObservationPhase::Restore) {
            ++observation_attempts_;
            RestoreForeground("api_error");
        }
        return true;
    }
    if (type == "session.updated") {
        if (observation_phase_ == ObservationPhase::Configure) {
            // Require the actual text-only configuration, not an unrelated late foreground ACK.
            auto* session = cJSON_GetObjectItem(j, "session");
            auto* modalities = cJSON_GetObjectItem(session, "modalities");
            if (cJSON_GetArraySize(modalities) != 1 ||
                !cJSON_IsString(cJSON_GetArrayItem(modalities, 0)) ||
                std::string(cJSON_GetArrayItem(modalities, 0)->valuestring) != "text")
                return true;
            observation_phase_ = ObservationPhase::Sample;
            sample_deadline_ = esp_timer_get_time() + 600000;
            recording_ = true;
            std::lock_guard lock(config_mutex);
            observation_status = "sampling_real_audio";
        } else if (observation_phase_ == ObservationPhase::Restore) {
            auto* modalities = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "session"), "modalities");
            if (cJSON_GetArraySize(modalities) != 2 ||
                !cJSON_IsString(cJSON_GetArrayItem(modalities, 0)) ||
                !cJSON_IsString(cJSON_GetArrayItem(modalities, 1)) ||
                std::string(cJSON_GetArrayItem(modalities, 0)->valuestring) != "text" ||
                std::string(cJSON_GetArrayItem(modalities, 1)->valuestring) != "audio")
                return true;
            observation_phase_ = ObservationPhase::Idle;
            observation_deadline_ = 0;
            recording_ = true;
            had_audio_ = false;
            observation_frame_.reset();
            if (!speech_prefill_.empty()) {
                size_t n = 0;
                std::string encoded(4 * ((speech_prefill_.size() + 2) / 3) + 1, '\0');
                if (mbedtls_base64_encode(
                        reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(), &n,
                        reinterpret_cast<const unsigned char*>(speech_prefill_.data()),
                        speech_prefill_.size()) == 0) {
                    encoded.resize(n);
                    had_audio_ = true;
                    Write("{\"type\":\"input_audio_buffer.append\",\"audio\":\"" + encoded + "\"}");
                }
                speech_prefill_.clear();
            }
            Status("listening_vad");
        }
        return true;
    }
    if (type == "input_audio_buffer.speech_started") {
        RestoreForeground("server_voice", true);
        return true;
    }
    if (type == "input_audio_buffer.committed") {
        if (observation_phase_ == ObservationPhase::Model && model_commit_pending_) {
            model_commit_pending_ = false;
            Write(R"({"type":"response.create"})");
        }
        return true;
    }
    if (type == "response.created") {
        const auto id = Str(cJSON_GetObjectItem(j, "response"), "id");
        if (observation_phase_ == ObservationPhase::Restore)
            cancelled_response_ = id;
        else if (observation_phase_ == ObservationPhase::Model)
            observation_response_ = id;
        return true;
    }
    if (type == "response.text.delta" || type == "response.audio_transcript.delta") {
        if (observation_phase_ == ObservationPhase::Model &&
            Str(j, "response_id") == observation_response_ &&
            observation_text_.size() + Str(j, "delta").size() <= 4096)
            observation_text_ += Str(j, "delta");
        return true;
    }
    if (type == "response.audio.delta" || type == "response.function_call_arguments.done") {
        {
            std::lock_guard lock(config_mutex);
            observation_error = "Observation unexpectedly returned audio/tool; suppressed";
        }
        ++observation_attempts_;
        RestoreForeground("unexpected_output");
        return true;
    }
    if (type == "response.done") {
        const auto* response = cJSON_GetObjectItem(j, "response");
        if (observation_phase_ != ObservationPhase::Model ||
            Str(response, "id") != observation_response_)
            return true;
        auto observed = Parse(observation_text_);
        const auto* id = cJSON_GetObjectItem(observed.get(), "frame_id");
        const auto* confidence = cJSON_GetObjectItem(observed.get(), "confidence");
        const auto person = Str(observed.get(), "person"), book = Str(observed.get(), "book"),
                   activity = Str(observed.get(), "activity"),
                   action = Str(observed.get(), "action");
        unsigned interval;
        std::function<bool(const OmniImage&)> validate;
        {
            std::lock_guard lock(config_mutex);
            interval = observation_interval_s;
            validate = image_valid;
        }
        const auto result_age =
            observation_frame_ ? esp_timer_get_time() - observation_frame_->captured_at_us : -1;
        const bool valid =
            result_age >= 0 && result_age <= int64_t(interval) * 2000000 &&
            (!validate || (observation_frame_ && validate(*observation_frame_))) &&
            Str(response, "status") == "completed" && cJSON_IsObject(observed.get()) &&
            cJSON_GetArraySize(observed.get()) == 7 && cJSON_IsNumber(id) && observation_frame_ &&
            id->valuedouble == double(observation_frame_->frame_id) && cJSON_IsNumber(confidence) &&
            confidence->valuedouble >= 0 && confidence->valuedouble <= 1 &&
            (person == "present" || person == "absent" || person == "unknown") &&
            (book == "present" || book == "absent" || book == "unknown") &&
            (activity == "reading" || activity == "looking" || activity == "away" ||
             activity == "unknown") &&
            cJSON_IsString(cJSON_GetObjectItem(observed.get(), "summary")) &&
            Str(observed.get(), "summary").size() <= 640 &&
            (action == "none" || action == "nod" || action == "tilt" || action == "gaze_user" ||
             action == "gaze_book");
        if (valid) {
            observation_attempts_ = 0;
            bool changed = false, actions = false;
            {
                std::lock_guard lock(config_mutex);
                auto old = Parse(observation_cache);
                changed =
                    old && (Str(old.get(), "person") != person || Str(old.get(), "book") != book ||
                            Str(old.get(), "activity") != activity);
                observation_cache = Dump(observed.get());
                observation_at = observation_frame_->captured_at_us;
                observation_cached_frame = observation_frame_->frame_id;
                observation_status = "observed";
                observation_error.clear();
                actions = companion_enabled && companion_actions;
            }
            Board::GetInstance().RecordDiagnostic("observation_result", "omni",
                                                  Dump(observed.get()));
            std::function<std::string()> state;
            decltype(runtime_dispatch) dispatch;
            {
                std::lock_guard lock(config_mutex);
                state = state_reader;
                dispatch = runtime_dispatch;
            }
            auto hardware = Parse(state ? state() : "{}");
            const auto now = esp_timer_get_time();
            if (changed && actions && confidence->valuedouble >= .7 && dispatch &&
                now - last_autonomous_action >= 15000000 &&
                cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "motion_enabled")) &&
                cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "torque_enabled_confirmed")) &&
                !cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "busy")) &&
                !cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "manual_control")) &&
                Str(hardware.get(), "mode") != "work") {
                std::string args;
                if (action == "nod" || action == "tilt")
                    args = "{\"steps\":[{\"action_id\":\"execute_primitive\",\"skill_id\":\"" +
                           std::string(action == "nod" ? "A01" : "B02") +
                           "\",\"options\":{\"end_mode\":\"return_to_reference\"}}]}";
                else if (action == "gaze_user" || action == "gaze_book")
                    args = "{\"steps\":[{\"action_id\":\"gaze_calibrated\",\"target_id\":\"" +
                           std::string(action == "gaze_user" ? "user" : "book") + "\"}]}";
                if (!args.empty()) {
                    turn_ = observation_turn_ = ++turn_sequence;
                    background_task_ = dispatch("lamp_run_sequence", args, turn_);
                    last_autonomous_action = now;
                }
            }
        } else {
            ++observation_attempts_;
            std::lock_guard lock(config_mutex);
            observation_status = "invalid_result";
            observation_error =
                "Invalid/stale structured observation; not sent to speaker or hardware";
        }
        observation_response_.clear();  // Already completed: no redundant cancellation.
        observation_phase_ =
            ObservationPhase::Sample;  // Restore helper without cancelling a finished response.
        RestoreForeground(valid ? "completed" : "invalid_result");
        return true;
    }
    return type.starts_with("response.");
}
void OmniRealtimeProtocol::Run() {
    while (running_) {
        PollObservation();
        if (awaiting_tools_ && ready_ && !close_ && !cancel_)
            CompleteTools();
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
            transcript_.clear();
            speaking_ = false;
            // Close the session on cancel: late response.created cannot start an old turn again.
            if (ready_ && !cancelled_response_.empty())
                Write(R"({"type":"response.cancel"})");
            if (ready_)
                Write(R"({"type":"input_audio_buffer.clear"})");
            close_ = true;
            ready_ = false;
            const char* reason =
                abort_reason_ == kAbortReasonWakeWordDetected ? "wake_word" : "operator_or_button";
            Status("cancelled", "", reason);
            Board::GetInstance().RecordDiagnostic("voice_cancel", "omni", reason);
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
            const bool quiet = QuietPacket(w->text);
            if (!quiet) {
                quiet_since_ = esp_timer_get_time();
                if (observation_phase_ == ObservationPhase::Restore)
                    restoring_for_voice_ = true;
                if (observation_phase_ != ObservationPhase::Idle &&
                    observation_phase_ != ObservationPhase::Restore)
                    RestoreForeground("foreground_voice", true);
            } else if (!quiet_since_)
                quiet_since_ = esp_timer_get_time();
            if (observation_phase_ == ObservationPhase::Restore) {
                if (restoring_for_voice_) {
                    if (speech_prefill_.size() + w->text.size() > 32768) {
                        Fail("Foreground audio exceeded bounded restore buffer; restart this turn");
                    } else
                        speech_prefill_ += w->text;
                }
                continue;
            }
            if (observation_phase_ == ObservationPhase::Configure ||
                observation_phase_ == ObservationPhase::Model)
                continue;
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
            if (observation_phase_ == ObservationPhase::Sample)
                sample_sent_ = true;
        } else if (w->text == "@manual" || w->text == "@vad") {
            if (rest_requested_)
                continue;
            manual_ = w->text == "@manual";
            if (!manual_) {
                std::lock_guard lock(config_mutex);
                if (image_enabled) {
                    image_error = "Image input requires manual recording; disable it before VAD";
                    continue;
                }
            }
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
            if (manual_ && had_audio_) {
                if (!AppendImage()) {
                    had_audio_ = false;
                    Write(R"({"type":"input_audio_buffer.clear"})");
                    continue;
                }
                Write(R"({"type":"input_audio_buffer.commit"})");
            }
            had_audio_ = false;
        } else
            Write(w->text);
    }
    Disconnect();
    xSemaphoreGive(stopped_);
    vTaskDelete(nullptr);
}
