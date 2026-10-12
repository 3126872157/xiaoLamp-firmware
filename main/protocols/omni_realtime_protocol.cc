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
#include "omni_lamp_tools.h"
#include "settings.h"

namespace {
std::mutex config_mutex;
std::string workspace, api_key, region = "cn-beijing", status = "unconfigured", error;
std::string last_stop_reason;
std::function<std::string()> state_reader;
std::function<std::future<std::string>(const std::string&, const std::string&, uint64_t)>
    runtime_dispatch;
std::function<void(uint64_t)> runtime_cancel;
std::string runtime_tools, runtime_instructions;
std::function<std::string(const std::string&)> runtime_operations;
std::function<std::shared_ptr<const OmniImage>()> image_reader;
bool image_enabled = false;
std::function<void(bool, unsigned)> vision_periodic;
std::function<bool(const OmniImage&)> image_valid;
bool companion_enabled = false, companion_actions = false;
bool transcription_enabled = false;
unsigned observation_interval_s = 60;
std::string observation_wait_reason;
unsigned reconnect_attempts = 0;
int64_t reconnect_wait_ms = 0;
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
// Audio-uplink instrumentation. Counters only: the queue capacities stay fixed.
std::atomic<unsigned> audio_high_water{0}, audio_dropped_frames{0}, audio_overruns{0},
    audio_recoveries{0}, audio_appends{0}, audio_coalesced_frames{0}, audio_drop_streak{0};
std::atomic<int64_t> slowest_write_us{0};
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
void AddObservationState(cJSON* state) {
    if (!cJSON_IsObject(state))
        return;
    std::lock_guard lock(config_mutex);
    auto* runtime = cJSON_GetObjectItem(state, "runtime");
    if (!cJSON_IsObject(runtime))
        runtime = cJSON_AddObjectToObject(state, "runtime");
    for (auto* key :
         {"vision_enabled", "companion_enabled", "observation_fresh", "observation_age_ms"})
        cJSON_DeleteItemFromObject(runtime, key);
    cJSON_DeleteItemFromObject(state, "observation");
    cJSON_AddBoolToObject(runtime, "vision_enabled",
                          bool(image_reader) && (image_enabled || companion_enabled));
    cJSON_AddBoolToObject(runtime, "companion_enabled", companion_enabled);
    const auto age = observation_at ? (esp_timer_get_time() - observation_at) / 1000 : -1;
    const bool fresh = age >= 0 && age <= observation_interval_s * 2000;
    cJSON_AddBoolToObject(runtime, "observation_fresh", fresh);
    cJSON_AddNumberToObject(runtime, "observation_age_ms", age);
    if (fresh && !observation_cache.empty())
        cJSON_AddItemToObject(state, "observation", cJSON_Parse(observation_cache.c_str()));
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
    std::function<void(uint64_t)> cancel, const std::string& tools,
    const std::string& instructions,
    std::function<std::string(const std::string&)> operations) {
    std::lock_guard lock(config_mutex);
    runtime_dispatch = std::move(dispatch);
    runtime_cancel = std::move(cancel);
    runtime_tools = tools;
    runtime_instructions = instructions;
    runtime_operations = std::move(operations);
    if (runtime_dispatch)
        transcription_enabled = true;
}

void OmniSetRuntimeTools(const std::string& tools, const std::string& instructions) {
    std::lock_guard lock(config_mutex);
    if (!runtime_dispatch)
        return;
    runtime_tools = tools;
    runtime_instructions = instructions;
}

namespace {
// Added tools resolve through the active table; the compiled vocabulary is the fallback
// for callers that install a runtime without one.
std::string ToolOperation(const std::function<std::string(const std::string&)>& operations,
                          const std::string& name) {
    if (operations) {
        if (auto operation = operations(name); !operation.empty())
            return operation;
    }
    return lamp::ToolOperation(name);
}
bool StopTool(const std::function<std::string(const std::string&)>& operations,
              const std::string& name, const std::string& args) {
    const auto operation = ToolOperation(operations, name);
    if (operation == "cancel")
        return true;
    if (operation != "set_torque")
        return false;
    auto parsed = cJSON_ParseWithOpts(args.c_str(), nullptr, true);
    const bool unload = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(parsed, "enabled"));
    cJSON_Delete(parsed);
    return unload;
}
bool MotionTool(const std::function<std::string(const std::string&)>& operations,
                const std::string& name) {
    return lamp::MotionOperation(ToolOperation(operations, name));
}
bool SuppressedByStop(const std::function<std::string(const std::string&)>& operations,
                      const std::string& name) {
    return !lamp::ExpressionTool(name) &&
           lamp::ControlOperation(ToolOperation(operations, name));
}
}  // namespace

std::string OmniPanel(const std::string& request) {
    auto body = Parse(request);
    auto op = Str(body.get(), "op");
    if (op == "transcription") {
        std::lock_guard lock(config_mutex);
        auto* enabled = cJSON_GetObjectItem(body.get(), "enabled");
        if (in_use || !cJSON_IsBool(enabled))
            return R"({"ok":false,"error":{"message":"Close conversation before changing transcription"}})";
        transcription_enabled = cJSON_IsTrue(enabled);
    } else if (op == "companion" || op == "observe_once") {
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
                interval->valuedouble > 3600 || interval->valuedouble != interval->valueint ||
                !cJSON_IsBool(actions))
                return R"({"ok":false,"error":{"message":"Expected enabled/actions booleans and interval_s 2..3600"}})";
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
    cJSON_AddNumberToObject(j.get(), "audio_queue_high_water", audio_high_water);
    cJSON_AddNumberToObject(j.get(), "audio_queue_capacity", 8);
    cJSON_AddNumberToObject(j.get(), "audio_dropped_frames", audio_dropped_frames);
    cJSON_AddNumberToObject(j.get(), "audio_overruns", audio_overruns);
    cJSON_AddNumberToObject(j.get(), "audio_recoveries", audio_recoveries);
    cJSON_AddNumberToObject(j.get(), "audio_appends", audio_appends);
    cJSON_AddNumberToObject(j.get(), "audio_coalesced_frames", audio_coalesced_frames);
    cJSON_AddNumberToObject(j.get(), "audio_worst_write_ms", double(slowest_write_us) / 1000);
    cJSON_AddBoolToObject(j.get(), "manual_image_supported", bool(image_reader));
    cJSON_AddBoolToObject(j.get(), "manual_image_enabled", image_enabled);
    cJSON_AddStringToObject(j.get(), "image_error", image_error.c_str());
    cJSON_AddNumberToObject(j.get(), "image_sent_frame_id", image_sent_frame);
    cJSON_AddBoolToObject(j.get(), "companion_enabled", companion_enabled);
    cJSON_AddBoolToObject(j.get(), "companion_actions", companion_actions);
    cJSON_AddBoolToObject(j.get(), "transcription_enabled", transcription_enabled);
    cJSON_AddStringToObject(j.get(), "observation_wait_reason", observation_wait_reason.c_str());
    cJSON_AddNumberToObject(j.get(), "reconnect_attempts", reconnect_attempts);
    cJSON_AddNumberToObject(j.get(), "reconnect_wait_ms", reconnect_wait_ms);
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
    audio_queue_ = xQueueCreate(8, sizeof(Work*));
    stopped_ = xSemaphoreCreateBinary();
}
OmniRealtimeProtocol::~OmniRealtimeProtocol() {
    running_ = false;
    if (task_started_)
        xSemaphoreTake(stopped_, portMAX_DELAY);
    Work* w;
    while (queue_ && xQueueReceive(queue_, &w, 0) == pdTRUE)
        delete w;
    while (audio_queue_ && xQueueReceive(audio_queue_, &w, 0) == pdTRUE)
        delete w;
    if (audio_queue_)
        vQueueDelete(audio_queue_);
    if (queue_)
        vQueueDelete(queue_);
    if (stopped_)
        vSemaphoreDelete(stopped_);
}
bool OmniRealtimeProtocol::Start() {
    if (!queue_ || !audio_queue_ || !stopped_) {
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
    if (w->audio) {
        // Microphone audio is realtime: when the bounded queue is full, drop the oldest
        // frame and keep the turn instead of rejecting the whole utterance. Only a
        // sustained streak of drops falls back to the bounded quiet-recovery path.
        constexpr unsigned kSustainedDropStreak = 4;
        bool accepted = false, dropped = false, sustained = false;
        if (running_ && audio_queue_) {
            if (xQueueSend(audio_queue_, &w, 0) == pdTRUE) {
                accepted = true;
            } else {
                Work* oldest = nullptr;
                if (xQueueReceive(audio_queue_, &oldest, 0) == pdTRUE) {
                    delete oldest;
                    dropped = true;
                    if (xQueueSend(audio_queue_, &w, 0) == pdTRUE)
                        accepted = true;
                }
            }
        }
        if (accepted) {
            const auto depth = uxQueueMessagesWaiting(audio_queue_);
            auto high = audio_high_water.load();
            while (depth > high && !audio_high_water.compare_exchange_weak(high, depth)) {
            }
            if (dropped) {
                ++audio_dropped_frames;
                sustained = ++audio_drop_streak >= kSustainedDropStreak;
            } else {
                audio_drop_streak = 0;
            }
            if (!sustained)
                return true;
        } else {
            delete w;
        }
        audio_overrun_ = true;
        ++audio_overruns;
        return false;
    }
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
    conversation_wanted_ = true;
    resume_after_reconnect_ = false;
    reconnect_pending_ = false;
    reconnect_attempts_ = 0;
    reconnect_preferences_valid_ = false;
    in_use = true;
    open_ = true;
    status = "connecting";
    return true;
}
void OmniRealtimeProtocol::CloseAudioChannel(bool) {
    conversation_wanted_ = false;
    resume_after_reconnect_ = false;
    reconnect_pending_ = false;
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
    output_drained_ = true;
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
    if (!ready_ || silent_phase_ != SilentPhase::Idle || (!recording_ && !audio_recovering_) ||
        rest_requested_)
        return true;
    if (!p->pcm16 || p->sample_rate != 16000 || p->payload.empty() || p->payload.size() > 8192)
        return false;
    // Queue raw PCM: the audio/main tasks never perform TLS writes or JSON/base64 work.
    return Enqueue(
        new Work{std::string(reinterpret_cast<char*>(p->payload.data()), p->payload.size()), false,
                 true, epoch_});
}
void OmniRealtimeProtocol::SendStartListening(ListeningMode mode) {
    resume_after_reconnect_ = false;
    listen_requested_ = mode == kListeningModeManualStop ? 1 : 2;
}
void OmniRealtimeProtocol::SendStopListening() {
    recording_ = false;
    finish_requested_ = true;
}
void OmniRealtimeProtocol::SendAbortSpeaking(AbortReason reason) {
    conversation_wanted_ = false;
    resume_after_reconnect_ = false;
    reconnect_pending_ = false;
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
bool OmniRealtimeProtocol::AppendPcm(const std::string& pcm) {
    // One append per captured frame. A 60 ms mono PCM16 frame is 1920 bytes and becomes a
    // ~2.6 KB JSON message, which Write() carries in a few <= 2048-byte continuation
    // fragments of *one* message. Only oversized input (e.g. a coalesced backlog) is split,
    // to keep each message bounded without dropping samples.
    constexpr size_t kMaxAppendBytes = 960 * 4;
    for (size_t offset = 0; offset < pcm.size(); offset += kMaxAppendBytes) {
        const auto bytes = std::min(kMaxAppendBytes, pcm.size() - offset);
        size_t length = 0;
        std::string encoded(4 * ((bytes + 2) / 3) + 1, '\0');
        if (mbedtls_base64_encode(
                reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(), &length,
                reinterpret_cast<const unsigned char*>(pcm.data() + offset), bytes) != 0) {
            Fail("PCM encoding failed");
            return false;
        }
        encoded.resize(length);
        if (!Write("{\"type\":\"input_audio_buffer.append\",\"audio\":\"" + encoded + "\"}"))
            return false;
        ++audio_appends;
    }
    return true;
}
OmniRealtimeProtocol::Work* OmniRealtimeProtocol::TakeQueuedAudio() {
    // Microphone audio is taken first each iteration, and when the worker is behind, frames
    // that are already queued are merged into one append. Never waits for new audio and adds
    // no buffering beyond the frames already in the fixed-capacity queue.
    Work* raw = nullptr;
    if (!audio_queue_ || xQueueReceive(audio_queue_, &raw, 0) != pdTRUE)
        return nullptr;
    constexpr unsigned kMaxCoalescedFrames = 3;
    unsigned frames = 1;
    while (frames < kMaxCoalescedFrames && uxQueueMessagesWaiting(audio_queue_) > 0) {
        Work* next = nullptr;
        if (xQueueReceive(audio_queue_, &next, 0) != pdTRUE)
            break;
        if (next->epoch != epoch_ || !next->audio) {
            delete next;
            break;
        }
        raw->text += next->text;
        ++frames;
        ++audio_coalesced_frames;
        delete next;
    }
    return raw;
}
void OmniRealtimeProtocol::DrainQueuedAudioBeforeLongWrite() {
    // Only in the exact state where the run loop would also append microphone PCM, and never
    // re-entered from a nested Write, so message order and turn bookkeeping stay intact.
    // This waits for nothing: it only sends frames that are already queued.
    if (draining_write_ || !audio_queue_ || !recording_ || audio_recovering_ ||
        silent_phase_ != SilentPhase::Idle || observation_phase_ != ObservationPhase::Idle)
        return;
    draining_write_ = true;
    constexpr unsigned kMaxDrainedFrames = 4;
    for (unsigned frames = 0; frames < kMaxDrainedFrames && uxQueueMessagesWaiting(audio_queue_) > 0;
         ++frames) {
        Work* raw = nullptr;
        if (xQueueReceive(audio_queue_, &raw, 0) != pdTRUE)
            break;
        std::unique_ptr<Work> frame(raw);
        if (frame->epoch != epoch_ || !frame->audio || frame->text.empty())
            continue;
        if (BackgroundQuiet(frame->text)) {
            if (!quiet_since_)
                quiet_since_ = esp_timer_get_time();
        } else {
            quiet_since_ = esp_timer_get_time();
        }
        if (AppendPcm(frame->text))
            had_audio_ = true;
    }
    draining_write_ = false;
}
namespace {
// Longest single outbound write, so the panel can show the worst-case worker stall.
// Counters only: nothing here buffers or resequences audio.
struct WriteWatch {
    std::atomic<int64_t>* worst;
    int64_t started_at;
    explicit WriteWatch(std::atomic<int64_t>& value)
        : worst(&value), started_at(esp_timer_get_time()) {}
    ~WriteWatch() {
        const auto elapsed = esp_timer_get_time() - started_at;
        auto current = worst->load();
        while (elapsed > current && !worst->compare_exchange_weak(current, elapsed)) {
        }
    }
};
}  // namespace

bool OmniRealtimeProtocol::Write(const std::string& s) {
    const WriteWatch watch{slowest_write_us};
    if (close_ || disconnected_)
        return false;
    // One JSON message, bounded WebSocket continuation frames. The vendor transport clones
    // each masked frame; keeping it below the 4 KiB TLS record avoids another large copy.
    // Never resend a failed fragment: the underlying TCP write may already be partial.
    constexpr size_t kFragmentBytes = 2048;
    // A multi-fragment message cannot be interleaved with any other message (continuation
    // frames must stay contiguous), so push out the microphone frames that are already
    // queued before monopolising the worker for its whole length.
    if (s.size() > kFragmentBytes)
        DrainQueuedAudioBeforeLongWrite();
    size_t offset = 0;
    if (socket_ && socket_->IsConnected()) {
        do {
            const auto length = std::min(kFragmentBytes, s.size() - offset);
            const bool final = offset + length == s.size();
            if (!socket_->IsConnected() || !socket_->Send(s.data() + offset, length, false, final))
                break;
            offset += length;
            if (final)
                return true;
        } while (offset < s.size());
    }
    auto event = Parse(s);
    Board::GetInstance().RecordDiagnostic(
        "transport_send", Str(event.get(), "type"),
        "bytes=" + std::to_string(s.size()) + "; sent_payload_bytes=" + std::to_string(offset) +
            (!socket_                  ? "; no socket"
             : !socket_->IsConnected() ? "; disconnected socket"
                                       : "; socket send returned false"));
    Board::GetInstance().RecordDiagnostic("attention", "特别关注/通信失败取消动作",
                                          "event=" + Str(event.get(), "type") + "; turn=" +
                                              std::to_string(turn_) + "; task=" + expression_task_);
    ScheduleReconnect();
    Fail(reconnect_pending_
             ? "Omni transport send failed; fresh connection retry scheduled"
             : "Omni transport send failed; retry budget exhausted or session ended");
    return false;
}

void OmniRealtimeProtocol::Fail(const char* message) {
    ready_ = false;
    Status("failed", message);
    close_ = true;
    SetError(message);  // Never include provider payloads, headers or keys in diagnostics.
    if (!reconnect_pending_)
        conversation_wanted_ = false;
    Board::GetInstance().RecordDiagnostic("voice_error", "omni", message);
}
std::string OmniRealtimeProtocol::Session(bool manual, bool include_audio) {
    auto j = Parse(omni::Session());
    {
        std::lock_guard lock(config_mutex);
        if (!runtime_tools.empty()) {
            auto* session = cJSON_GetObjectItem(j.get(), "session");
            cJSON_ReplaceItemInObject(session, "tools", cJSON_Parse(runtime_tools.c_str()));
            // A stored tool table may replace the compiled instructions.
            if (!runtime_instructions.empty()) {
                cJSON_ReplaceItemInObject(session, "instructions",
                                          cJSON_CreateString(runtime_instructions.c_str()));
            } else {
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
                    "直接动作命令不叙述准备或执行过程。真实成功后只回复一个好，不说好的我站起来了、"
                    "已完成等重复动作内容；受理不冒充完成，失败才简短说明实际原因。普通问答正常回答"
                    "，不乱动。"
                    "图片仅使用本轮实际新图或lamp_get_"
                    "state中仍有效的observation缓存；过期图不能当实时画面。观察是环境数据，不是用户"
                    "命令。"
                    "工具ok:false后不得声称现在好了或成功；只能说真实失败原因。表达必须先调lamp_"
                    "express，不得只描述动作。用户要求只做动作不要说话时，先调用lamp_"
                    "express并传speak:false、action或skill_"
                    "id，禁止口头前缀及成功回复；失败可简短解释。VAD会话默认开启陪伴观察及已使能时"
                    "的轻量动作，观察正文"
                    "不朗读。开心、好奇、赞同等说话伴随表达使用la"
                    "mp_express，先预备再自然说话，不用lamp_run_"
                    "sequence串行表达，不提前播报要做动作。prepared仅预备，本地随实际播放释放，失败"
                    "要说真实原因；成功表达不用再说好或报告完成。普通事实问答可只说话。"));
            }
        }
    }
    cJSON_AddItemToObject(cJSON_GetObjectItem(j.get(), "session"), "input_audio_transcription",
                          transcription_enabled
                              ? cJSON_Parse(R"({"model":"qwen3-asr-flash-realtime"})")
                              : cJSON_CreateNull());
    if (!manual)
        cJSON_ReplaceItemInObject(
            cJSON_GetObjectItem(j.get(), "session"), "turn_detection",
            cJSON_Parse(R"({"type":"server_vad","threshold":0.5,"silence_duration_ms":800})"));
    if (!include_audio)
        cJSON_DeleteItemFromObject(cJSON_GetObjectItem(j.get(), "session"), "audio");
    return Dump(j.get());
}
void OmniRealtimeProtocol::Connect() {
    Disconnect(false);
    error_occurred_ = false;
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
    if (!socket_->Connect(url.c_str())) {
        ScheduleReconnect();
        Fail("Omni connect failed: check region/workspace/key/network");
    }
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
    observation_deadline_ = sample_deadline_ = quiet_since_ = observation_retry_at_ = 0;
    observation_turn_ = 0;
    server_speech_active_ = false;
    noise_mean_ = 0;
    observed_frame_ = 0;
    have_luminance_ = false;
    model_commit_pending_ = sample_sent_ = false;
    background_task_ = {};
    expression_ = {};
    expression_result_ = {};
    expression_task_.clear();
    expression_failure_.clear();
    output_started_ = 0;
    output_drained_ = true;
    expression_used_turn_ = 0;
    failure_notification_ = false;
    silent_phase_ = SilentPhase::Idle;
    companion_default_applied_ = false;
    response_motion_tool_ = false;
    user_transcripts_.clear();
    latest_input_item_.clear();
    turn_input_item_.clear();
    fallback_prepare_ = {};
    fallback_skill_.clear();
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
    while (audio_queue_ && xQueueReceive(audio_queue_, &stale, 0) == pdTRUE)
        delete stale;
    audio_overrun_ = audio_recovering_ = false;
    listen_requested_ = 0;
    finish_requested_ = false;
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
        if (status != "failed" && status != "cancelled" && status != "resting" &&
            status != "reconnecting")
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
    if (type == "conversation.item.input_audio_transcription.completed") {
        const auto item = Str(j.get(), "item_id"), text = Str(j.get(), "transcript");
        if (observation_phase_ == ObservationPhase::Idle && silent_phase_ == SilentPhase::Idle &&
            !item.empty() && text.size() <= 4096) {
            if (user_transcripts_.size() >= 8)
                user_transcripts_.clear();
            user_transcripts_[item] = text;
            Board::GetInstance().RecordDiagnostic("user", item, text);
        }
        return;
    }
    if (type == "conversation.item.input_audio_transcription.failed") {
        Board::GetInstance().RecordDiagnostic("attention", "特别关注/用户转写失败",
                                              Str(cJSON_GetObjectItem(j.get(), "error"), "code"));
        return;
    }
    if (type == "input_audio_buffer.speech_started")
        server_speech_active_ = true;
    if (type == "input_audio_buffer.speech_stopped") {
        server_speech_active_ = false;
        quiet_since_ = esp_timer_get_time();
    }
    if (SilentEvent(j.get(), type))
        return;
    if (ObservationEvent(j.get(), type))
        return;
    if (type == "input_audio_buffer.committed")
        latest_input_item_ = Str(j.get(), "item_id");
    if (type == "session.created") {
        session_id_ = Str(cJSON_GetObjectItem(j.get(), "session"), "id");
        Write(Session(true));
    } else if (type == "session.updated" && !ready_) {
        ready_ = true;
        ready_since_ = esp_timer_get_time();
        if (restoring_preferences_) {
            restoring_preferences_ = false;
            resume_after_reconnect_ = true;
        }
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
        const auto param = Str(cJSON_GetObjectItem(j.get(), "error"), "param");
        Board::GetInstance().RecordDiagnostic("omni_api_error", code.substr(0, 96),
                                              param.substr(0, 96));
        const auto message = "Omni API rejected event: code=" + code.substr(0, 96) +
                             "; param=" + param.substr(0, 96);
        Fail(message.c_str());
    } else if (type == "input_audio_buffer.committed" && manual_) {
        Write(R"({"type":"response.create"})");
    } else if (type == "response.created") {
        auto id = Str(cJSON_GetObjectItem(j.get(), "response"), "id");
        if (id.empty() || id == cancelled_response_)
            return;
        response_ = id;
        server_speech_active_ = false;
        if (awaiting_tools_) {
            std::lock_guard lock(config_mutex);
            if (runtime_cancel && turn_)
                runtime_cancel(turn_);
            tools_.clear();
            awaiting_tools_ = continuing_ = false;
        }
        if (!continuing_) {
            response_motion_tool_ = false;
            turn_input_item_ = latest_input_item_;
            fallback_prepare_ = {};
            fallback_skill_.clear();
            expression_ = {};
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
        response_audio_suppressed_ = false;
        playback_ = ++playback_sequence;
        if (!playback_)
            playback_ = ++playback_sequence;
        if (!expression_.args.empty() && expression_.turn == turn_)
            expression_.playback = playback_;
        Board::GetInstance().RecordDiagnostic(
            "response", "start", "turn=" + std::to_string(turn_) + "; response=" + response_);
        Status("responding");
    } else if (type == "response.audio.delta") {
        if (response_.empty() || !playback_ || Str(j.get(), "response_id") != response_)
            return;
        if (!ExplicitExpression().first.empty())
            PrepareFallback();
        if (ExplicitExpression().second) {
            if (!response_audio_suppressed_) {
                response_audio_suppressed_ = true;
                Board::GetInstance().RecordDiagnostic(
                    "silent_output", "suppressed",
                    "Explicit user requested no speech; model PCM not played");
            }
            return;
        }
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
            output_drained_ = false;
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
        std::function<std::string(const std::string&)> operations;
        {
            std::lock_guard lock(config_mutex);
            operations = runtime_operations;
        }
        if (MotionTool(operations, name))
            response_motion_tool_ = true;
        auto call_info = Parse("{}");
        cJSON_AddStringToObject(call_info.get(), "call_id", call.c_str());
        cJSON_AddStringToObject(call_info.get(), "response_id", response_.c_str());
        cJSON_AddNumberToObject(call_info.get(), "turn", double(turn_.load()));
        cJSON_AddStringToObject(call_info.get(), "arguments", arguments.substr(0, 1024).c_str());
        Board::GetInstance().RecordDiagnostic("function_call", name, Dump(call_info.get()));
        if (tool.name == "lamp_express")
            Board::GetInstance().RecordDiagnostic("expression_arguments", tool.id,
                                                  tool.arguments.substr(0, 1024));
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
        PrepareFallback();
        if (!transcript_.empty()) {
            auto output = Parse("{}");
            cJSON_AddStringToObject(output.get(), "response_id", response_.c_str());
            cJSON_AddNumberToObject(output.get(), "turn", double(turn_.load()));
            cJSON_AddStringToObject(output.get(), "text", transcript_.c_str());
            cJSON_AddBoolToObject(output.get(), "audio_received", speaking_);
            cJSON_AddBoolToObject(output.get(), "audio_suppressed", response_audio_suppressed_);
            Board::GetInstance().RecordDiagnostic("assistant_output", "model", Dump(output.get()));
        }
        DiagnoseResponse();
        if (!transcript_.empty() && playback_ && speaking_)
            Emit("sentence_start", transcript_);
        if (!tools_.empty() && playback_ && Str(resp, "status") == "completed") {
            if (++tool_rounds_ > 4) {
                Fail("Tool continuation limit reached; no automatic retries");
                return;
            }
            awaiting_tools_ = true;
            CompleteTools();
        } else {
            if (ExplicitExpression().second && !speaking_ && !rest_requested_)
                recording_ = !manual_;
            Status(rest_requested_ ? "rest_pending" : "ready");
            rest_reply_done_ = rest_requested_.load();
            if ((speaking_ || rest_requested_) && playback_)
                Emit("stop");
            speaking_ = false;
        }
        response_.clear();
        if (failure_notification_) {
            failure_notification_ = false;
            Write(Session(manual_, false));
        }
    }
}
void OmniRealtimeProtocol::CompleteTools() {
    std::function<std::future<std::string>(const std::string&, const std::string&, uint64_t)>
        dispatch;
    std::function<std::string()> reader;
    std::function<std::string(const std::string&)> operations;
    {
        std::lock_guard lock(config_mutex);
        dispatch = runtime_dispatch;
        reader = state_reader;
        operations = runtime_operations;
    }
    // Dispatch emergency controls before motion, independent of their network arrival order.
    std::stable_sort(tools_.begin(), tools_.end(), [&operations](const Tool& a, const Tool& b) {
        auto urgent = [&operations](const Tool& t) {
            return StopTool(operations, t.name, t.arguments);
        };
        return urgent(a) > urgent(b);
    });
    const bool stop_batch = std::any_of(tools_.begin(), tools_.end(), [&operations](const Tool& t) {
        return StopTool(operations, t.name, t.arguments);
    });
    const bool explicit_motion =
        std::any_of(tools_.begin(), tools_.end(), [&operations](const Tool& t) {
            return MotionTool(operations, t.name) && !lamp::ExpressionTool(t.name);
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
            } else if (stop_batch && SuppressedByStop(operations, tool.name)) {
                auto args = Parse(tool.arguments);
                if (ToolOperation(operations, tool.name) == "set_torque" &&
                    cJSON_IsFalse(cJSON_GetObjectItem(args.get(), "enabled")) && dispatch)
                    tool.result = dispatch(tool.name, tool.arguments, turn_);
                else
                    tool.output =
                        R"({"ok":false,"error":"Motion suppressed by stop/unload in the same response"})";
            } else if (tool.name == "lamp_express") {
                auto incoming = Parse(tool.arguments);
                if (!fallback_skill_.empty() &&
                    (expression_used_turn_ == turn_ || fallback_prepare_.valid()) &&
                    Str(incoming.get(), "skill_id") == fallback_skill_) {
                    tool.output =
                        R"({"ok":true,"status":"already_handled","instruction":"Same explicit user expression already scheduled locally; do not repeat or claim physical completion."})";
                } else if (stop_batch || explicit_motion || expression_used_turn_ == turn_ ||
                           !expression_.args.empty() || expression_result_.valid() ||
                           !expression_task_.empty() ||
                           (expression_last_release_ &&
                            esp_timer_get_time() - expression_last_release_ < 2500000))
                    tool.output =
                        R"({"ok":false,"error":"Expression skipped: busy/cooldown/control or one motion per turn"})";
                else if (dispatch) {
                    tool.result = dispatch("lamp_prepare_expression", tool.arguments, turn_);
                } else
                    tool.output = R"({"ok":false,"error":"Expression is unsupported in PoC"})";
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
            if (tool.name == "lamp_express") {
                auto prepared = Parse(tool.output);
                if (cJSON_IsTrue(cJSON_GetObjectItem(prepared.get(), "ok")) &&
                    Str(prepared.get(), "status") == "prepared") {
                    expression_used_turn_ = turn_;
                    expression_.args = Dump(cJSON_GetObjectItem(prepared.get(), "args"));
                    expression_.silent =
                        cJSON_IsNull(cJSON_GetObjectItem(prepared.get(), "speech"));
                    expression_.turn = turn_;
                    expression_.deadline = esp_timer_get_time() + 10000000;
                    auto current = Parse(reader ? reader() : "{}");
                    expression_.mode = Str(current.get(), "mode");
                    Board::GetInstance().RecordDiagnostic(
                        "expression_prepared", tool.id,
                        expression_.silent ? "speech_null" : "waiting_for_actual_playback");
                }
            }
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
        if (tool.name == "lamp_get_state") {
            auto state = Parse(tool.output);
            AddObservationState(state.get());
            tool.output = Dump(state.get());
        }
        if (tool.name == "lamp_set_mode" || tool.name == "lamp_run_sequence" ||
            tool.name == "lamp_set_torque") {
            auto result = Parse(tool.output);
            if (cJSON_IsObject(result.get())) {
                cJSON_DeleteItemFromObject(result.get(), "response_instruction");
                auto* task = cJSON_GetObjectItem(result.get(), "result");
                if (cJSON_IsFalse(cJSON_GetObjectItem(result.get(), "ok"))) {
                    cJSON_AddStringToObject(result.get(), "response_instruction",
                                            "动作失败，只简短说明实际错误原因，不能说好或成功。");
                } else if (cJSON_IsTrue(cJSON_GetObjectItem(result.get(), "execution_completed")) ||
                           Str(task, "status") == "completed") {
                    cJSON_AddStringToObject(
                        result.get(), "response_instruction",
                        "动作已真实完成，只回复一个好，不复述动作或宣告已完成。");
                }
                tool.output = Dump(result.get());
            }
        }
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
    bool silent_only = !tools_.empty();
    for (const auto& tool : tools_) {
        auto args = Parse(tool.arguments);
        auto result = Parse(tool.output);
        silent_only = silent_only && tool.name == "lamp_express" &&
                      cJSON_IsNull(cJSON_GetObjectItem(result.get(), "speech")) &&
                      cJSON_IsTrue(cJSON_GetObjectItem(result.get(), "ok")) &&
                      Str(result.get(), "status") == "prepared";
    }
    tools_.clear();
    awaiting_tools_ = false;
    if (silent_only) {
        if (speaking_ && playback_)
            Emit("stop");
        speaking_ = false;
        continuing_ = true;
        recording_ = false;
        silent_phase_ = SilentPhase::Configure;
        silent_deadline_ = esp_timer_get_time() + 10000000;
        Work* stale = nullptr;
        while (audio_queue_ && xQueueReceive(audio_queue_, &stale, 0) == pdTRUE)
            delete stale;
        Status("finishing_silent_turn");
        Write(
            R"({"type":"session.update","session":{"modalities":["text"],"tools":[],"turn_detection":null}})");
        return;
    }
    continuing_ = true;
    Write(R"({"type":"response.create"})");
}
std::string OmniRuntimeState() {
    std::function<std::string()> reader;
    {
        std::lock_guard lock(config_mutex);
        reader = state_reader;
    }
    auto state = Parse(reader ? reader() : "{}");
    AddObservationState(state.get());
    // Audio-uplink health: shows whether the bounded input queue is actually saturating.
    auto* audio = cJSON_AddObjectToObject(state.get(), "audio_uplink");
    if (audio) {
        cJSON_AddNumberToObject(audio, "queue_capacity", 8);
        cJSON_AddNumberToObject(audio, "queue_high_water", audio_high_water.load());
        cJSON_AddNumberToObject(audio, "dropped_frames", audio_dropped_frames.load());
        cJSON_AddNumberToObject(audio, "overruns", audio_overruns.load());
        cJSON_AddNumberToObject(audio, "recoveries", audio_recoveries.load());
        cJSON_AddNumberToObject(audio, "appends", audio_appends.load());
        cJSON_AddNumberToObject(audio, "coalesced_frames", audio_coalesced_frames.load());
        cJSON_AddNumberToObject(audio, "worst_write_ms", double(slowest_write_us.load()) / 1000);
    }
    return Dump(state.get());
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
void OmniRealtimeProtocol::PollExpression() {
    const auto now = esp_timer_get_time();
    if (fallback_prepare_.valid() &&
        fallback_prepare_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = Parse(fallback_prepare_.get());
        if (cJSON_IsTrue(cJSON_GetObjectItem(result.get(), "ok")) &&
            Str(result.get(), "status") == "prepared") {
            expression_used_turn_ = turn_;
            expression_.args = Dump(cJSON_GetObjectItem(result.get(), "args"));
            expression_.silent = fallback_silent_;
            expression_.turn = turn_;
            expression_.playback = playback_;
            expression_.deadline = now + 10000000;
            std::function<std::string()> read;
            {
                std::lock_guard lock(config_mutex);
                read = state_reader;
            }
            expression_.mode = Str(Parse(read ? read() : "{}").get(), "mode");
            Board::GetInstance().RecordDiagnostic("expression_prepared", "verified_user_fallback",
                                                  fallback_skill_);
        } else {
            expression_failure_ = Str(result.get(), "error");
            Board::GetInstance().RecordDiagnostic("attention", "特别关注/用户动作补偿失败",
                                                  expression_failure_);
        }
    }
    if (!ready_ || close_ || cancel_ || rest_requested_) {
        fallback_prepare_ = {};
        expression_ = {};
        return;
    }
    decltype(runtime_dispatch) dispatch;
    std::function<std::string()> reader;
    {
        std::lock_guard lock(config_mutex);
        dispatch = runtime_dispatch;
        reader = state_reader;
    }
    if (!expression_.args.empty()) {
        auto state = Parse(reader ? reader() : "{}");
        const bool authorized =
            expression_.turn == turn_ && Str(state.get(), "mode") == expression_.mode &&
            cJSON_IsTrue(cJSON_GetObjectItem(state.get(), "motion_enabled")) &&
            cJSON_IsTrue(cJSON_GetObjectItem(state.get(), "torque_enabled_confirmed")) &&
            cJSON_IsTrue(cJSON_GetObjectItem(state.get(), "available")) &&
            !cJSON_IsTrue(cJSON_GetObjectItem(state.get(), "manual_control")) &&
            !cJSON_IsTrue(cJSON_GetObjectItem(state.get(), "busy"));
        if (!authorized || now > expression_.deadline) {
            Board::GetInstance().RecordDiagnostic("expression_discarded", "state_or_deadline",
                                                  "no_motion_dispatched");
            if (expression_.turn == turn_)
                expression_failure_ = "伴随动作未执行：控制权、使能或播放等待条件改变。";
            expression_ = {};
        } else if (dispatch && (expression_.silent || (expression_.playback &&
                                                       output_started_ == expression_.playback))) {
            expression_motion_turn_ = expression_.turn;
            expression_result_ = dispatch("lamp_run_sequence", expression_.args, expression_.turn);
            expression_last_release_ = now;
            Board::GetInstance().RecordDiagnostic(
                "expression_released", expression_.silent ? "speech_null" : "actual_pcm_playback",
                "playback=" + std::to_string(expression_.playback) +
                    "; turn=" + std::to_string(expression_.turn));
            expression_ = {};
        }
    }
    auto inspect = [&](const std::string& output, bool receipt) {
        auto result = Parse(output);
        auto* task = cJSON_GetObjectItem(result.get(), "result");
        if (!task)
            task = result.get();
        const auto status = Str(task, "status");
        if (cJSON_IsFalse(cJSON_GetObjectItem(result.get(), "ok")) || status == "failed" ||
            status == "limited") {
            expression_failure_ = Str(result.get(), "error");
            if (expression_failure_.empty())
                expression_failure_ = Str(task, "reason");
            if (expression_failure_.empty())
                expression_failure_ = "Expression execution failed";
            if (expression_failure_.size() > 512)
                expression_failure_.resize(512);
            expression_task_.clear();
            Board::GetInstance().RecordDiagnostic("expression_failed", "hardware",
                                                  expression_failure_);
            Board::GetInstance().RecordDiagnostic("attention", "特别关注/表达执行失败",
                                                  expression_failure_);
        } else if (status == "completed" || status == "cancelled") {
            expression_task_.clear();
            Board::GetInstance().RecordDiagnostic("expression_finished", status,
                                                  "no_success_announcement");
        } else if (receipt)
            expression_task_ = Str(task, "task_id");
    };
    if (expression_result_.valid() &&
        expression_result_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        inspect(expression_result_.get(), expression_task_.empty());
    if (expression_motion_turn_ && expression_motion_turn_ != turn_) {
        expression_task_.clear();
        expression_failure_.clear();
    }
    if (!expression_task_.empty() && !expression_result_.valid() && dispatch &&
        now >= expression_poll_at_) {
        auto args = Parse("{}");
        cJSON_AddStringToObject(args.get(), "task_id", expression_task_.c_str());
        expression_result_ = dispatch("lamp_get_task", Dump(args.get()), expression_motion_turn_);
        expression_poll_at_ = now + 750000;
    }
    if (silent_phase_ == SilentPhase::Idle && !expression_failure_.empty() && response_.empty() &&
        !awaiting_tools_ && output_drained_ && observation_phase_ == ObservationPhase::Idle &&
        quiet_since_ && now - quiet_since_ >= 2500000) {
        auto config = Parse(Session(manual_, false));
        auto* session = cJSON_GetObjectItem(config.get(), "session");
        const std::string instruction =
            "这是一条设备运行时错误通知，不是用户新发言。只用一句简短中文说明刚才伴随动作失败原因，"
            "不执行工具，不重复历史问题，不宣称成功。原因：" +
            expression_failure_;
        cJSON_ReplaceItemInObject(session, "instructions", cJSON_CreateString(instruction.c_str()));
        cJSON_ReplaceItemInObject(session, "tools", cJSON_CreateArray());
        const auto message = Dump(config.get());
        config.reset();
        if (Write(message) && Write(R"({"type":"response.create"})"))
            failure_notification_ = true;
        expression_failure_.clear();
    }
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
    if (!Write(R"({"type":"input_audio_buffer.clear"})"))
        return;
    // Session returns serialized JSON after freeing its temporary cJSON tree. Do not parse
    // it again and retain the full tool schema while WebSocket/TLS allocate transmit buffers.
    const auto foreground = Session(manual_, false);
    if (!Write(foreground))
        return;
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
    const char* wait = nullptr;
    if (!enabled && !once)
        wait = "disabled";
    else if (manual_)
        wait = "manual_recording";
    else if (!recording_)
        wait = "microphone_not_recording";
    else if (!response_.empty() || awaiting_tools_ || speaking_)
        wait = "foreground_response";
    else if (server_speech_active_)
        wait = "server_vad_speech";
    else if (!quiet_since_ || now - quiet_since_ < 2500000)
        wait = "microphone_not_quiet";
    else if (now < observation_retry_at_)
        wait = "result_retry_backoff";
    else if (observation_attempts_ >= 2)
        wait = "protocol_failure_pause";
    else if (background_task_.valid() || !expression_.args.empty() || expression_result_.valid() ||
             !expression_task_.empty())
        wait = "motion_pending";
    else if (!expression_failure_.empty() || failure_notification_ ||
             silent_phase_ != SilentPhase::Idle)
        wait = "turn_cleanup";
    if (wait) {
        ObservationWait(wait);
        return;
    }
    auto frame = reader ? reader() : nullptr;
    if (!frame || frame->frame_id == observed_frame_ ||
        now - frame->captured_at_us > int64_t(interval) * 2000000) {
        ObservationWait("waiting_for_fresh_camera_frame");
        return;
    }
    auto hardware = Parse(state ? state() : "{}");
    if (cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "busy")) ||
        cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "manual_control"))) {
        ObservationWait("hardware_busy_or_manual_control");
        return;
    }
    if (!once && have_luminance_ && frame->luminance_valid) {
        int mean = 0;
        for (size_t i = 0; i < 192; i++)
            mean += int(frame->luminance[i]) - int(observed_luminance_[i]);
        mean /= 192;
        unsigned change = 0;
        for (size_t i = 0; i < 192; i++)
            change += std::abs(int(frame->luminance[i]) - int(observed_luminance_[i]) - mean);
        if (change / 192 < 12 && observation_at &&
            now - observation_at < int64_t(interval) * 2000000) {
            observed_frame_ = frame->frame_id;
            ObservationWait("unchanged_scene_cache_still_fresh");
            return;
        }
    }
    ObservationWait("eligible");
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
    auto config = Parse(
        R"({"type":"session.update","session":{"modalities":["text"],"tools":[],"turn_detection":null}})");
    auto* session = cJSON_GetObjectItem(config.get(), "session");
    cJSON_AddNullToObject(session, "input_audio_transcription");
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
    cJSON_AddStringToObject(session, "instructions", instruction.c_str());
    recording_ = false;
    const auto config_message = Dump(config.get());
    config.reset();
    if (!Write(R"({"type":"input_audio_buffer.clear"})") || !Write(config_message))
        return;
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
                if (AppendPcm(speech_prefill_))
                    had_audio_ = true;
                speech_prefill_.clear();
            }
            Status("listening_vad");
            Board::GetInstance().RecordDiagnostic("observation_restored", "vad",
                                                  "foreground_config_confirmed");
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
        auto structured = observation_text_;
        if (structured.starts_with("```")) {
            const auto start = structured.find('{'), end = structured.rfind('}');
            if (start != std::string::npos && end != std::string::npos && end >= start)
                structured = structured.substr(start, end - start + 1);
        }
        auto observed = Parse(structured);
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
        const bool valid = result_age >= 0 && result_age <= int64_t(interval) * 2000000 &&
                           (!validate || (observation_frame_ && validate(*observation_frame_))) &&
                           Str(response, "status") == "completed" &&
                           cJSON_IsObject(observed.get()) && cJSON_IsNumber(id) &&
                           observation_frame_ && cJSON_IsNumber(confidence) &&
                           confidence->valuedouble >= 0 && confidence->valuedouble <= 1 &&
                           (person == "present" || person == "absent" || person == "unknown") &&
                           (book == "present" || book == "absent" || book == "unknown") &&
                           (activity == "reading" || activity == "looking" || activity == "away" ||
                            activity == "unknown") &&
                           cJSON_IsString(cJSON_GetObjectItem(observed.get(), "summary")) &&
                           Str(observed.get(), "summary").size() <= 640 &&
                           (action == "none" || action == "nod" || action == "tilt" ||
                            action == "gaze_user" || action == "gaze_book");
        if (valid) {
            if (id->valuedouble != double(observation_frame_->frame_id))
                Board::GetInstance().RecordDiagnostic(
                    "observation_frame_normalized", "response_bound",
                    "local=" + std::to_string(observation_frame_->frame_id) +
                        "; model=" + std::to_string(id->valuedouble));
            cJSON_SetNumberValue(cJSON_GetObjectItem(observed.get(), "frame_id"),
                                 double(observation_frame_->frame_id));
            observation_attempts_ = 0;
            bool actions = false;
            {
                std::lock_guard lock(config_mutex);
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
            if (actions && (person == "present" || book == "present") &&
                confidence->valuedouble >= .7 && dispatch &&
                now - last_autonomous_action >= 15000000 &&
                cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "motion_enabled")) &&
                cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "torque_enabled_confirmed")) &&
                !cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "busy")) &&
                !cJSON_IsTrue(cJSON_GetObjectItem(hardware.get(), "manual_control"))) {
                // A visual suggestion is optional. Relative expressions require no target
                // calibration. A static work posture may tilt/nod; an active work task remains
                // protected by busy.
                static constexpr const char* expressions[] = {"B02", "A01", "B01",
                                                              "A02", "A03", "A16"};
                const auto index = companion_expression_sequence_++;
                const auto skill = Str(hardware.get(), "mode") == "work"
                                       ? (index % 2 ? "A01" : "B02")
                                       : expressions[index % 6];
                const std::string args =
                    "{\"steps\":[{\"action_id\":\"execute_primitive\",\"skill_id\":\"" +
                    std::string(skill) + "\",\"options\":{\"end_mode\":\"return_to_reference\"}}]}";
                if (!args.empty()) {
                    turn_ = observation_turn_ = ++turn_sequence;
                    background_task_ = dispatch("lamp_run_sequence", args, turn_);
                    last_autonomous_action = now;
                }
            }
        } else {
            observation_retry_at_ = esp_timer_get_time() + 10000000;
            std::lock_guard lock(config_mutex);
            observation_status = "invalid_result_retry";
            observation_error =
                "Invalid/stale observation; dropped, retry after 10s; frame/status/schema/pose "
                "check";
            Board::GetInstance().RecordDiagnostic(
                "observation_rejected", "validation",
                "frame=" + std::to_string(observation_frame_ ? observation_frame_->frame_id : 0) +
                    "; age_ms=" + std::to_string(result_age / 1000) +
                    "; echoed_frame=" + std::to_string(cJSON_IsNumber(id) ? id->valuedouble : -1) +
                    "; fields=" + std::to_string(cJSON_GetArraySize(observed.get())));
        }
        observation_response_.clear();  // Already completed: no redundant cancellation.
        observation_phase_ =
            ObservationPhase::Sample;  // Restore helper without cancelling a finished response.
        RestoreForeground(valid ? "completed" : "invalid_result");
        return true;
    }
    return type.starts_with("response.");
}
std::pair<std::string, bool> OmniRealtimeProtocol::ExplicitExpression() const {
    auto found = user_transcripts_.find(turn_input_item_);
    if (turn_input_item_.empty() || found == user_transcripts_.end())
        return {};
    const auto& text = found->second;
    for (const auto* discussion : {"什么意思", "是什么", "为什么", "原理", "介绍", "解释"})
        if (text.find(discussion) != std::string::npos)
            return {};
    for (const auto* denied :
         {"不要点", "不点头", "别点", "不用点", "不要摇", "别摇", "只说话", "不要动", "别动",
          "不做动作", "不要低头", "不低头", "别低头", "不要歪", "别歪", "不歪头", "不要摆", "别摆"})
        if (text.find(denied) != std::string::npos)
            return {};
    const bool silent =
        text.find("不要说话") != std::string::npos || text.find("不说话") != std::string::npos ||
        text.find("只动") != std::string::npos || text.find("安静") != std::string::npos;
    const bool combined =
        text.find("边说") != std::string::npos || text.find("一边") != std::string::npos ||
        text.find("同时") != std::string::npos ||
        (text.find("边") != std::string::npos && text.find("话") != std::string::npos);
    const bool command = text.starts_with("来吧") || text.starts_with("请") ||
                         text.starts_with("帮我") || text.starts_with("给我") ||
                         text.starts_with("我希望你") || text.starts_with("我想让你") ||
                         text.starts_with("调用工具");
    for (const auto& entry : {std::pair{"点头", "A01"},
                              {"点点头", "A01"},
                              {"点一下头", "A01"},
                              {"点两下头", "A01"},
                              {"点三下头", "A01"},
                              {"点四下头", "A01"},
                              {"点五下头", "A01"},
                              {"点两次头", "A01"},
                              {"点三次头", "A01"},
                              {"点四次头", "A01"},
                              {"点五次头", "A01"},
                              {"摇头", "B01"},
                              {"摇下头", "B01"},
                              {"摇一下头", "B01"},
                              {"歪头", "B02"},
                              {"低头", "A03"},
                              {"摇摆", "A02"},
                              {"摆动", "A02"}}) {
        if (text.find(entry.first) == std::string::npos)
            continue;
        if (silent || combined || command || text.starts_with(entry.first) ||
            text.starts_with(std::string("请") + entry.first) ||
            text.starts_with(std::string("你") + entry.first))
            return {entry.second, silent};
    }
    return {};
}
unsigned OmniRealtimeProtocol::ExplicitNodRepetitions() const {
    const auto found = user_transcripts_.find(turn_input_item_);
    if (found == user_transcripts_.end())
        return 0;
    static constexpr const char* counts[] = {"三", "四", "五", "六", "七", "八", "九"};
    for (unsigned n = 3; n <= 9; ++n)
        for (const auto& count : {std::string(counts[n - 3]), std::to_string(n)})
            if (found->second.find("点" + count + "下") != std::string::npos ||
                found->second.find("点" + count + "次") != std::string::npos ||
                found->second.find("点头" + count + "下") != std::string::npos ||
                found->second.find("点头" + count + "次") != std::string::npos)
                return n;  // Runtime rejects >5; never silently substitutes two for six.
    return 0;              // One/two retain the established local default of two.
}
void OmniRealtimeProtocol::PrepareFallback() {
    const auto intent = ExplicitExpression();
    if (intent.first.empty() || response_motion_tool_ || expression_used_turn_ == turn_ ||
        fallback_prepare_.valid() || !expression_.args.empty() || expression_result_.valid() ||
        !expression_task_.empty())
        return;
    decltype(runtime_dispatch) dispatch;
    {
        std::lock_guard lock(config_mutex);
        dispatch = runtime_dispatch;
    }
    if (!dispatch)
        return;
    fallback_skill_ = intent.first;
    fallback_silent_ = intent.second;
    const auto repetitions = intent.first == "A01" ? ExplicitNodRepetitions() : 0;
    fallback_prepare_ = dispatch(
        "lamp_prepare_expression",
        "{\"skill_id\":\"" + intent.first + "\",\"speak\":" + (intent.second ? "false" : "true") +
            (repetitions ? ",\"repetitions\":" + std::to_string(repetitions) : "") + "}",
        turn_);
    response_motion_tool_ = true;
    Board::GetInstance().RecordDiagnostic("attention", "特别关注/模型漏工具已补偿",
                                          "Verified user item=" + turn_input_item_ +
                                              "; skill=" + intent.first +
                                              "; no automatic torque enable; awaiting validation");
}
void OmniRealtimeProtocol::DiagnoseResponse() {
    if (!response_motion_tool_ && (transcript_.find("边说边点头") != std::string::npos ||
                                   transcript_.find("点了") != std::string::npos ||
                                   transcript_.find("点一下头") != std::string::npos ||
                                   transcript_.find("摇一下头") != std::string::npos))
        Board::GetInstance().RecordDiagnostic("attention", "特别关注/疑似只说未动",
                                              "No motion tool in turn=" + std::to_string(turn_) +
                                                  "; response=" + response_ +
                                                  "; claim=" + transcript_.substr(0, 256));
}
bool OmniRealtimeProtocol::SilentEvent(const cJSON* j, const std::string& type) {
    const auto phase = silent_phase_.load();
    if (phase == SilentPhase::Idle)
        return false;
    if (type == "error")
        return false;  // Existing error diagnostics still own failures.
    if (type == "session.updated") {
        auto* modalities = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "session"), "modalities");
        if (phase == SilentPhase::Configure && cJSON_GetArraySize(modalities) == 1 &&
            cJSON_IsString(cJSON_GetArrayItem(modalities, 0)) &&
            std::string(cJSON_GetArrayItem(modalities, 0)->valuestring) == "text") {
            silent_phase_ = SilentPhase::Model;
            Write(R"({"type":"response.create"})");
        } else if (phase == SilentPhase::Restore && cJSON_GetArraySize(modalities) == 2 &&
                   cJSON_IsString(cJSON_GetArrayItem(modalities, 1)) &&
                   std::string(cJSON_GetArrayItem(modalities, 1)->valuestring) == "audio") {
            silent_phase_ = SilentPhase::Idle;
            recording_ = !manual_;
            had_audio_ = false;
            Status(manual_ ? "ready" : "listening_vad");
            Board::GetInstance().RecordDiagnostic(
                "silent_turn", "completed",
                "text-only continuation completed; foreground config confirmed");
        }
        return true;
    }
    if (phase == SilentPhase::Model && type == "response.created") {
        response_ = Str(cJSON_GetObjectItem(j, "response"), "id");
        continuing_ = false;
        playback_ = 0;
        return true;
    }
    if (phase == SilentPhase::Model && type == "response.done" &&
        Str(cJSON_GetObjectItem(j, "response"), "id") == response_) {
        const auto status = Str(cJSON_GetObjectItem(j, "response"), "status");
        if (status != "completed") {
            Fail("Silent tool continuation failed");
            return true;
        }
        response_.clear();
        silent_phase_ = SilentPhase::Restore;
        silent_deadline_ = esp_timer_get_time() + 5000000;
        Write(Session(manual_, false));
        return true;
    }
    if (type == "response.audio.delta") {
        Board::GetInstance().RecordDiagnostic("attention", "特别关注/静默轮次出现音频",
                                              "Suppressed; no PCM sent to speaker");
        return true;
    }
    return type.starts_with("response.") || type == "input_audio_buffer.committed" ||
           type == "input_audio_buffer.speech_started";
}
void OmniRealtimeProtocol::ObservationWait(const char* reason) {
    std::lock_guard lock(config_mutex);
    if (observation_wait_reason != reason) {
        observation_wait_reason = reason;
        Board::GetInstance().RecordDiagnostic("observation_wait", reason,
                                              "No hidden mode/torque change");
    }
}
bool OmniRealtimeProtocol::BackgroundQuiet(const std::string& pcm) {
    if (pcm.empty() || pcm.size() % 2)
        return true;
    double mean = 0;
    for (size_t i = 0; i < pcm.size(); i += 2)
        mean +=
            std::abs(int(int16_t(uint16_t(uint8_t(pcm[i])) | uint16_t(uint8_t(pcm[i + 1])) << 8)));
    mean /= pcm.size() / 2;
    if (!noise_mean_)
        noise_mean_ = std::min(mean, 2000.);
    const bool quiet = mean < std::max(500., noise_mean_ * 1.8 + 200.);
    if (!server_speech_active_ && response_.empty() && quiet)
        noise_mean_ = .98 * noise_mean_ + .02 * std::min(mean, noise_mean_ * 1.01 + 50.);
    return quiet && !server_speech_active_;
}
void OmniRealtimeProtocol::ScheduleReconnect() {
    if (!conversation_wanted_ || rest_requested_ || cancel_ || !running_ || reconnect_pending_)
        return;
    if (reconnect_attempts_ >= 3)
        return;
    static constexpr int delays[] = {2, 5, 10};
    reconnect_at_ = esp_timer_get_time() + int64_t(delays[reconnect_attempts_++]) * 1000000;
    reconnect_pending_ = true;
    {
        std::lock_guard lock(config_mutex);
        if (!restoring_preferences_) {
            reconnect_companion_ = companion_enabled;
            reconnect_actions_ = companion_actions;
            reconnect_preferences_valid_ = companion_default_applied_;
        }
        reconnect_attempts = reconnect_attempts_;
    }
    Board::GetInstance().RecordDiagnostic(
        "connection_retry", "scheduled",
        "attempt=" + std::to_string(reconnect_attempts_) +
            "; fresh socket; old utterance/motion never replayed");
}
void OmniRealtimeProtocol::PollReconnect() {
    const auto now = esp_timer_get_time();
    if (ready_ && ready_since_ && now - ready_since_ >= 60000000) {
        reconnect_attempts_ = 0;
        ready_since_ = 0;
    }
    if (!reconnect_pending_)
        return;
    if (!conversation_wanted_ || rest_requested_ || cancel_ || !running_) {
        reconnect_pending_ = false;
        std::lock_guard lock(config_mutex);
        reconnect_wait_ms = 0;
        return;
    }
    {
        std::lock_guard lock(config_mutex);
        reconnect_wait_ms = std::max(int64_t(0), (reconnect_at_ - now) / 1000);
        status = "reconnecting";
    }
    if (now < reconnect_at_)
        return;
    reconnect_pending_ = false;
    restoring_preferences_ = true;
    Connect();
}
void OmniRealtimeProtocol::RecoverAudioInput() {
    if (!audio_overrun_.exchange(false))
        return;
    audio_recovering_ = true;
    recovery_quiet_since_ = 0;
    Work* stale = nullptr;
    while (audio_queue_ && xQueueReceive(audio_queue_, &stale, 0) == pdTRUE)
        delete stale;
    if (ready_) {
        if (observation_phase_ != ObservationPhase::Idle &&
            observation_phase_ != ObservationPhase::Restore)
            RestoreForeground("audio_input_overrun");
        else
            Write(R"({"type":"input_audio_buffer.clear"})");
    }
    sample_sent_ = false;
    had_audio_ = false;
    ++audio_recoveries;
    Board::GetInstance().RecordDiagnostic(
        "audio_input_overrun", "utterance_rejected",
        "Input buffer full (high_water=" + std::to_string(audio_high_water.load()) +
            "/8, dropped=" + std::to_string(audio_dropped_frames.load()) +
            ", worst_write_ms=" + std::to_string(slowest_write_us.load() / 1000) +
            "); discard incomplete input until 600ms quiet; connection preserved");
}
void OmniRealtimeProtocol::Run() {
    while (running_) {
        RecoverAudioInput();
        // Realtime PCM has strict priority: while the fixed input queue holds a backlog,
        // defer the optional (but deadline-checked) work below to the next iteration.
        const bool audio_backlog = audio_queue_ && uxQueueMessagesWaiting(audio_queue_) > 0;
        if (!audio_backlog) {
            PollExpression();
            PollObservation();
            if (awaiting_tools_ && ready_ && !close_ && !cancel_)
                CompleteTools();
        }
        const bool peer_closed = disconnected_.exchange(false);
        if (peer_closed && !close_ && !rest_requested_ && conversation_wanted_) {
            ScheduleReconnect();
            Fail("Omni connection closed unexpectedly");
        }
        if (close_.exchange(false) || peer_closed)
            Disconnect();
        PollReconnect();
        if (overloaded_.exchange(false))
            Fail("Omni queue overflow; session stopped");
        if (open_.exchange(false))
            Connect();
        if (connecting_since_ && esp_timer_get_time() - connecting_since_ > 20000000) {
            ScheduleReconnect();
            Fail("Omni session handshake timed out");
        }
        if (manual_ && recording_ && recording_since_ &&
            esp_timer_get_time() - recording_since_ > 30000000)
            Fail("Manual recording exceeded 30 seconds; start a new turn");
        if (silent_phase_ != SilentPhase::Idle && esp_timer_get_time() > silent_deadline_)
            Fail("Silent tool turn completion timed out; inspect cloud ACK");
        // No arbitrary five-minute failure; provider disconnects remain explicit errors.
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
        Work* raw = TakeQueuedAudio();
        if (!raw) {
            const auto listen = listen_requested_.exchange(0);
            if (listen)
                raw = new Work{listen == 1 ? "@manual" : "@vad", false, false, epoch_};
            else if (finish_requested_.exchange(false))
                raw = new Work{"@finish", false, false, epoch_};
            else if (xQueueReceive(queue_, &raw, 0) != pdTRUE) {
                if (xQueueReceive(audio_queue_, &raw, pdMS_TO_TICKS(20)) != pdTRUE)
                    continue;
            }
        }
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
            const bool quiet = BackgroundQuiet(w->text);
            if (audio_recovering_) {
                const auto now = esp_timer_get_time();
                if (!quiet)
                    recovery_quiet_since_ = 0;
                else if (!recovery_quiet_since_)
                    recovery_quiet_since_ = now;
                else if (now - recovery_quiet_since_ >= 600000) {
                    audio_recovering_ = false;
                    Board::GetInstance().RecordDiagnostic("audio_input_recovered", "quiet",
                                                          "Next complete utterance accepted");
                }
                continue;
            }
            if (!recording_ || silent_phase_ != SilentPhase::Idle)
                continue;
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
            if (AppendPcm(w->text)) {
                had_audio_ = true;
                if (observation_phase_ == ObservationPhase::Sample)
                    sample_sent_ = true;
            }
        } else if (w->text == "@manual" || w->text == "@vad") {
            if (rest_requested_ || silent_phase_ != SilentPhase::Idle)
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
            if (!manual_ && !companion_default_applied_) {
                std::lock_guard lock(config_mutex);
                companion_default_applied_ = true;
                companion_enabled = bool(image_reader) && bool(runtime_dispatch) &&
                                    (!reconnect_preferences_valid_ || reconnect_companion_);
                companion_actions =
                    companion_enabled && (!reconnect_preferences_valid_ || reconnect_actions_);
                if (vision_periodic)
                    vision_periodic(companion_enabled, observation_interval_s);
                Board::GetInstance().RecordDiagnostic(
                    "companion_defaults", "vad",
                    companion_enabled ? "observation/actions enabled; torque unchanged"
                                      : "unavailable");
            }
            Status(manual_ ? "listening_manual" : "listening_vad");
        } else if (w->text == "@finish") {
            if (manual_ && had_audio_) {
                if (!AppendImage()) {
                    had_audio_ = false;
                    Write(R"({"type":"input_audio_buffer.clear"})");
                    continue;
                }
                Write(R"({"type":"input_audio_buffer.commit"})");
                // Manual image input is single-shot: after a successful turn,
                // clear the gate so a later VAD conversation is not blocked.
                std::lock_guard lock(config_mutex);
                if (image_enabled) {
                    image_enabled = false;
                    image_error.clear();
                }
            }
            had_audio_ = false;
        } else
            Write(w->text);
    }
    Disconnect();
    xSemaphoreGive(stopped_);
    vTaskDelete(nullptr);
}
