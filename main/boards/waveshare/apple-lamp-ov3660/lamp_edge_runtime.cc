#include "lamp_edge_runtime.h"
#include <esp_timer.h>
#include <chrono>
#include <memory>
#include "lamp_catalog.h"
#include "lamp_protocol.h"

namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json Parse(const std::string& s) {
    return Json(cJSON_ParseWithOpts(s.c_str(), nullptr, true), cJSON_Delete);
}
std::string Error(const char* reason) {
    Json j(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddBoolToObject(j.get(), "ok", false);
    cJSON_AddStringToObject(j.get(), "error", reason);
    return lamp::Encode(j.get());
}
const cJSON* Result(const cJSON* j) {
    auto* result = cJSON_GetObjectItemCaseSensitive(j, "result");
    return result ? result : j;
}
bool Emergency(const std::string& name, const std::string& args) {
    auto j = Parse(args);
    return name == "lamp_stop" ||
           (name == "lamp_set_torque" &&
            cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j.get(), "enabled")));
}
std::string Operation(const std::string& name) {
    if (name == "lamp_run_sequence")
        return "execute_group";
    if (name == "lamp_stop")
        return "cancel";
    if (name == "lamp_get_task")
        return "get_task";
    if (name == "lamp_set_torque")
        return "set_torque";
    if (name == "lamp_record_calibration")
        return "record_calibration";
    if (name == "lamp_rest")
        return "rest";
    if (name == "lamp_get_state")
        return "get_state";
    return "";
}
}  // namespace

LampEdgeRuntime::LampEdgeRuntime(
    LampActionExecutor& executor,
    std::function<void(const std::string&, const std::string&, const std::string&)> diagnostic)
    : executor_(executor), diagnostic_(std::move(diagnostic)) {
    queue_ = xQueueCreate(64, sizeof(Work*));
    urgent_ = xQueueCreate(4, sizeof(Work*));
    TaskHandle_t worker = nullptr;
    if (queue_ && urgent_)
        started_ = xTaskCreate(
                       [](void* p) {
                           auto* runtime = static_cast<LampEdgeRuntime*>(p);
                           for (;;) {
                               runtime->Tick();
                               vTaskDelay(pdMS_TO_TICKS(20));
                           }
                       },
                       "lamp_runtime", 8192, this, 2, &worker) == pdPASS;
}
std::future<std::string> LampEdgeRuntime::Submit(const std::string& name, const std::string& args,
                                                 uint64_t turn) {
    auto* work = new Work{name, args, turn, {}};
    auto future = work->result.get_future();
    const auto bytes = name.size() + args.size();
    const bool urgent = args.size() <= lamp::kMaxCommandBytes && Emergency(name, args);
    const auto total = queued_bytes_.fetch_add(bytes) + bytes;
    if (!started_ || args.size() > lamp::kMaxCommandBytes || (!urgent && total > 65536) ||
        xQueueSend(urgent ? urgent_ : queue_, &work, 0) != pdTRUE) {
        queued_bytes_ -= bytes;
        Complete(work, Error("Runtime queue unavailable/full or arguments too large"));
    }
    return future;
}
void LampEdgeRuntime::Cancel(uint64_t turn) {
    auto current = invalid_through_.load();
    while (turn > current && !invalid_through_.compare_exchange_weak(current, turn)) {
    }
}
void LampEdgeRuntime::Complete(Work* work, const std::string& output) {
    work->result.set_value(output);
    delete work;
}
void LampEdgeRuntime::CancelOwned() {
    if (!motion_task_.empty() && motion_turn_ <= invalid_through_) {
        executor_.Submit(lamp::Query("cancel", executor_.NewRequestId(), motion_task_));
        motion_task_.clear();
    }
}
void LampEdgeRuntime::Process(Work* work) {
    if (work->turn <= invalid_through_) {
        Complete(work, Error("Turn cancelled; no dispatch"));
        return;
    }
    const auto operation = Operation(work->name);
    auto args = Parse(work->args);
    if (operation.empty() || !cJSON_IsObject(args.get()) ||
        cJSON_GetObjectItemCaseSensitive(args.get(), "context")) {
        Complete(work, Error("Unknown tool/invalid args; runtime owns context"));
        return;
    }
    Json command(cJSON_CreateObject(), cJSON_Delete);
    const auto request = executor_.NewRequestId();
    cJSON_AddStringToObject(command.get(), "protocol_version", lamp::kProtocol);
    cJSON_AddStringToObject(command.get(), "request_id", request.c_str());
    cJSON_AddStringToObject(command.get(), "operation", operation.c_str());
    cJSON_AddItemToObject(command.get(), "args", cJSON_Duplicate(args.get(), true));
    auto checked = lamp::Command(lamp::Encode(command.get()));
    if (!checked) {
        Complete(work, Error(checked.error().c_str()));
        return;
    }
    if (operation == "get_state") {
        auto full = Parse(State());
        Json compact(cJSON_CreateObject(), cJSON_Delete);
        for (auto key :
             {"mode", "busy", "motion_enabled", "manual_control", "pose_rad", "target_pose_rad",
              "requires_operator_enable", "torque_enabled_confirmed", "unloaded_confirmed",
              "calibrated_targets", "unsupported_primitives", "last_task", "runtime"}) {
            auto* value = cJSON_GetObjectItemCaseSensitive(full.get(), key);
            if (value)
                cJSON_AddItemToObject(compact.get(), key, cJSON_Duplicate(value, true));
        }
        Complete(work, lamp::Encode(compact.get()));
        return;
    }
    if (operation == "execute_group" && group_turn_ == work->turn) {
        Complete(work, Error("Only one motion group per turn"));
        return;
    }
    if (diagnostic_)
        diagnostic_("runtime_request", work->name, *checked);
    auto output = executor_.Submit(*checked);
    auto receipt = Parse(output);
    const auto task = lamp::Text(Result(receipt.get()), "task_id");
    const bool accepted =
        !task.empty() && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt.get(), "ok"));
    if (accepted && operation == "execute_group") {
        group_turn_ = work->turn;
        auto* steps = cJSON_GetObjectItemCaseSensitive(args.get(), "steps");
        std::lock_guard lock(state_mutex_);
        for (auto* step = steps ? steps->child : nullptr; step; step = step->next)
            if (lamp::Text(step, "action_id") == "set_mode")
                requested_mode_ = lamp::Text(step, "mode");
    }
    if (accepted && operation != "get_task" && operation != "get_state" && operation != "cancel") {
        motion_task_ = task;
        motion_turn_ = work->turn;
    }
    if (accepted && pending_.size() < 8 &&
        (operation == "set_torque" || operation == "rest" || operation == "record_calibration")) {
        pending_.push_back(
            {work, task, esp_timer_get_time() + (operation == "rest" ? 65000000 : 15000000)});
    } else
        Complete(work, output);
    CancelOwned();
}
void LampEdgeRuntime::Tick() {
    CancelOwned();
    Work* work = nullptr;
    // Emergency controls always precede ordinary queued work.
    while (urgent_ && xQueueReceive(urgent_, &work, 0) == pdTRUE) {
        queued_bytes_ -= work->name.size() + work->args.size();
        Process(work);
    }
    if (pending_.size() < 8 && queue_ && xQueueReceive(queue_, &work, 0) == pdTRUE) {
        queued_bytes_ -= work->name.size() + work->args.size();
        Process(work);
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
        const bool cancelled = it->work->turn <= invalid_through_;
        const bool timeout = esp_timer_get_time() > it->deadline;
        auto output = executor_.Submit(lamp::Query("get_task", executor_.NewRequestId(), it->task));
        auto result = Parse(output);
        const auto status = lamp::Text(Result(result.get()), "status");
        if (cancelled || timeout ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result.get(), "ok")) ||
            status == "completed" || status == "failed" || status == "cancelled") {
            if (timeout)
                executor_.Submit(lamp::Query("cancel", executor_.NewRequestId(), it->task));
            Complete(it->work,
                     cancelled ? Error("Turn cancelled")
                     : timeout ? Error("Control confirmation timed out; inspect hardware feedback")
                               : output);
            it = pending_.erase(it);
        } else
            ++it;
    }
    auto state = executor_.State();
    std::lock_guard lock(state_mutex_);
    snapshot_ = std::move(state);
    snapshot_at_ = esp_timer_get_time();
}
std::string LampEdgeRuntime::State() {
    std::string state;
    std::string requested_mode;
    int64_t snapshot_at;
    {
        std::lock_guard lock(state_mutex_);
        state = snapshot_;
        requested_mode = requested_mode_;
        snapshot_at = snapshot_at_;
    }
    auto j = Parse(state);
    if (!cJSON_IsObject(j.get()))
        j = Json(cJSON_CreateObject(), cJSON_Delete);
    auto* runtime = cJSON_AddObjectToObject(j.get(), "runtime");
    cJSON_AddStringToObject(runtime, "stage", "semantic_tools");
    cJSON_AddStringToObject(runtime, "requested_mode", requested_mode.c_str());
    cJSON_AddStringToObject(runtime, "confirmed_mode", lamp::Text(j.get(), "mode").c_str());
    cJSON_AddNumberToObject(runtime, "snapshot_age_ms",
                            snapshot_at ? (esp_timer_get_time() - snapshot_at) / 1000 : -1);
    cJSON_AddBoolToObject(runtime, "available", started_);
    cJSON_AddBoolToObject(runtime, "vision_enabled", false);
    cJSON_AddBoolToObject(runtime, "companion_enabled", false);
    cJSON_AddBoolToObject(runtime, "speech_motion_sync", false);
    cJSON_AddNumberToObject(runtime, "queued_bytes", queued_bytes_);
    return lamp::Encode(j.get());
}

std::string LampEdgeRuntime::Tools() {
    auto catalog = Parse(lamp::kCatalog);
    Json tools(cJSON_CreateArray(), cJSON_Delete);
    auto add = [&](const char* name, const char* description, cJSON* schema) {
        auto* tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        auto* function = cJSON_AddObjectToObject(tool, "function");
        cJSON_AddStringToObject(function, "name", name);
        cJSON_AddStringToObject(function, "description", description);
        cJSON_AddItemToObject(function, "parameters", schema);
        cJSON_AddItemToArray(tools.get(), tool);
    };
    auto object = [] {
        auto* s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON_AddBoolToObject(s, "additionalProperties", false);
        cJSON_AddObjectToObject(s, "properties");
        return s;
    };
    add("lamp_get_state", "查询缓存状态，不反复轮询；accepted不代表完成。", object());
    add("lamp_end_conversation",
        "广义告别或停止聊天：简短告别播完退出监听，不坐下卸力。讨论告别不结束。", object());
    add("lamp_rest", "明确让台灯休息：坐下，真实确认后卸力；成功后简短告别并结束对话。", object());
    add("lamp_stop", "停止运动但保持支撑力矩，不自动补播。", object());
    add("lamp_get_task", "只有需要实际完成结果时查询任务，避免反复调用。",
        cJSON_Parse(
            R"({"type":"object","additionalProperties":false,"properties":{"task_id":{"type":"string"}},"required":["task_id"]})"));
    add("lamp_set_torque",
        "仅用户明确要求使能或卸力时调用，true保持实际当前位置上力；false原地卸力。不用于普通对话表"
        "达。",
        cJSON_Parse(
            R"({"type":"object","additionalProperties":false,"properties":{"enabled":{"type":"boolean"}},"required":["enabled"]})"));
    add("lamp_record_calibration", "用户已手摆对准并明确说记录时保存当前模式的完整回读姿态。",
        cJSON_Parse(
            R"({"type":"object","additionalProperties":false,"properties":{"target_id":{"type":"string","enum":["user","book"]}},"required":["target_id"]})"));
    auto schema = Json(object(), cJSON_Delete);
    auto* steps = cJSON_CreateObject();
    cJSON_AddStringToObject(steps, "type", "array");
    cJSON_AddNumberToObject(steps, "minItems", 1);
    cJSON_AddNumberToObject(steps, "maxItems", 16);
    auto* items = cJSON_AddObjectToObject(steps, "items");
    auto* choices = cJSON_AddArrayToObject(items, "oneOf");
    for (const char* collection : {"actions", "primitives"}) {
        auto* rows = cJSON_GetObjectItemCaseSensitive(catalog.get(), collection);
        for (auto* row = rows ? rows->child : nullptr; row; row = row->next) {
            const bool primitive = std::string(collection) == "primitives";
            const auto id = lamp::Text(row, primitive ? "id" : "action_id");
            if (id == "A10")
                continue;  // Hardware has no supported lighting output.
            auto* step = object();
            auto* properties = cJSON_GetObjectItem(step, "properties");
            auto* action = cJSON_AddObjectToObject(properties, "action_id");
            cJSON_AddStringToObject(action, "const", primitive ? "execute_primitive" : id.c_str());
            auto* required = cJSON_AddArrayToObject(step, "required");
            cJSON_AddItemToArray(required, cJSON_CreateString("action_id"));
            if (primitive) {
                auto* skill = cJSON_AddObjectToObject(properties, "skill_id");
                cJSON_AddStringToObject(skill, "const", id.c_str());
                cJSON_AddItemToArray(required, cJSON_CreateString("skill_id"));
                cJSON_AddItemToObject(
                    properties, "options",
                    cJSON_Duplicate(cJSON_GetObjectItem(row, "inputSchema"), true));
                cJSON_AddItemToObject(properties, "target_id",
                                      cJSON_Parse(R"({"type":"string","enum":["user","book"]})"));
                if (cJSON_IsTrue(cJSON_GetObjectItem(row, "target_required")))
                    cJSON_AddItemToArray(required, cJSON_CreateString("target_id"));
            } else {
                auto* fields = cJSON_GetObjectItem(row, "fields");
                for (auto* f = fields ? fields->child : nullptr; f; f = f->next) {
                    cJSON_AddItemToObject(properties, f->string, cJSON_Duplicate(f, true));
                    if (std::string(f->string) != "size")
                        cJSON_AddItemToArray(required, cJSON_CreateString(f->string));
                }
            }
            cJSON_AddStringToObject(
                step, "description",
                primitive ? lamp::Text(row, "description").c_str() : id.c_str());
            cJSON_AddItemToArray(choices, step);
        }
    }
    cJSON_AddItemToObject(cJSON_GetObjectItem(schema.get(), "properties"), "steps", steps);
    cJSON_AddItemToObject(schema.get(), "required", cJSON_Parse("[\"steps\"]"));
    add("lamp_run_sequence",
        "执行一个有限语义动作组。次数幅度用本地默认，点两下头只用一次A01。靠近用A13而非低头，缺标定"
        "不替代。set_mode保持yaw。受理后可正常回答，不声称已完成。当前尚未支持伴随语音预备释放。",
        schema.release());
    return lamp::Encode(tools.get());
}
