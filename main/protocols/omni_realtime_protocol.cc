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
    if (op == "configure" || op == "clear") {
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
    if (!ready_ || !recording_ || rest_requested_)
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
                    "动作必须调用工具，不能仅口头答应。站起用lamp_run_sequence的steps=[{action_id:"
                    "set_mode,mode:stand}]。"
                    "明确上力必须调用lamp_set_torque(enabled=true)，不得只口头说已经上力。"
                    "未使能时报告不能运动并请用户明确使能，不自行上力。"
                    "只有execution_completed=true才能说已站起/"
                    "已到位；ok=false或failed必须如实报告原因。"
                    "accepted仅受理，不得声称已完成；运动授权和力矩不同，查询只在需"
                    "要时调用，不反复get_state。"
                    "每轮最多一个动作组，正常对话允许不动。点两下头只用一次A01。靠近"
                    "用A13，不用低头代替；缺标定如实说明。"
                    "用户明确说拜拜、结束聊天、先不聊了等广义告别时调用lamp_end_"
                    "conversation并简短告别；讨论告别或用户说自己休息不结束。"
                    "明确让台灯去休息用lamp_rest，等待成功才告别。显式要求上力/"
                    "别用力才用lamp_set_torque；普通唤醒和聊天不自动上力。"
                    "当前仅语义动作接入，尚无图像、陪伴或随语音同步动作，不得声称看"
                    "见画面或已实现同步。"));
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
    disconnected_ = false;
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
                recording_ = false;
                tool.output =
                    R"({"ok":true,"status":"rest_pending","instruction":"Say one short goodbye; close after playback. No servo or torque change."})";
            } else if (stop_batch &&
                       (tool.name == "lamp_run_sequence" || tool.name == "lamp_rest" ||
                        tool.name == "lamp_record_calibration" || tool.name == "lamp_set_torque")) {
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
            if (tool.name == "lamp_rest") {
                auto j = Parse(tool.output);
                auto* result = cJSON_GetObjectItem(j.get(), "result");
                if (cJSON_IsTrue(cJSON_GetObjectItem(j.get(), "ok")) &&
                    Str(result, "status") == "completed")
                    rest_requested_ = true;
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
void OmniRealtimeProtocol::Run() {
    while (running_) {
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
            if (rest_requested_)
                continue;
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
