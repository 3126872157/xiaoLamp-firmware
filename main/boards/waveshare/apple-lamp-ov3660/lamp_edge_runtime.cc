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
std::string ToolResult(const std::string& output) {
    auto j = Parse(output);
    if (!cJSON_IsObject(j.get()))
        return Error("Invalid executor result");
    const auto status = lamp::Text(Result(j.get()), "status");
    const bool failed = status == "failed" || status == "cancelled";
    if (failed) {
        cJSON_ReplaceItemInObject(j.get(), "ok", cJSON_CreateBool(false));
        cJSON_AddStringToObject(j.get(), "error", lamp::Text(Result(j.get()), "reason").c_str());
    }
    cJSON_AddBoolToObject(j.get(), "execution_completed", status == "completed");
    cJSON_AddStringToObject(j.get(), "instruction",
                            failed ? "Report failure and reason. No movement success."
                            : status == "completed"
                                ? "Hardware confirmed completion."
                                : "Not confirmed complete. Say starting/accepted only; use "
                                  "lamp_get_task before claiming completion.");
    return lamp::Encode(j.get());
}
bool Emergency(const std::string& name, const std::string& args) {
    auto j = Parse(args);
    return name == "lamp_stop" ||
           (name == "lamp_set_torque" &&
            cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j.get(), "enabled")));
}
std::string Operation(const std::string& name) {
    if (name == "lamp_run_sequence" || name == "lamp_set_mode" || name == "lamp_prepare_expression")
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
    const bool prepare_expression = work->name == "lamp_prepare_expression";
    bool silent_expression = false;
    if (prepare_expression) {
        auto skill = lamp::Text(args.get(), "skill_id");
        if (auto* action = cJSON_GetObjectItem(args.get(), "action")) {
            if (!cJSON_IsString(action) || cJSON_GetObjectItem(args.get(), "skill_id")) {
                Complete(work, Error("Use exactly one action or skill_id"));
                return;
            }
            const auto name = lamp::Text(args.get(), "action");
            if (name == "点头" || name == "nod")
                skill = "A01";
            else if (name == "兴奋摆动" || name == "sway")
                skill = "A02";
            else if (name == "低头" || name == "lower_head")
                skill = "A03";
            else if (name == "摇头" || name == "shake_head")
                skill = "B01";
            else if (name == "好奇歪头" || name == "tilt")
                skill = "B02";
            else if (name == "强调" || name == "emphasize")
                skill = "A16";
        }
        if (auto* speech = cJSON_GetObjectItem(args.get(), "speech")) {
            silent_expression =
                cJSON_IsNull(speech) || cJSON_IsFalse(speech) ||
                (cJSON_IsString(speech) && (lamp::Text(args.get(), "speech").empty() ||
                                            lamp::Text(args.get(), "speech") == "null" ||
                                            lamp::Text(args.get(), "speech") == "none"));
            if (!silent_expression &&
                (!cJSON_IsString(speech) || lamp::Text(args.get(), "speech") != "auto")) {
                Complete(work, Error("speech accepts null or auto"));
                return;
            }
        }
        if (auto* speak = cJSON_GetObjectItem(args.get(), "speak")) {
            if (!cJSON_IsBool(speak) || cJSON_GetObjectItem(args.get(), "speech")) {
                Complete(work, Error("speak requires boolean; use speak or speech, not both"));
                return;
            }
            silent_expression = cJSON_IsFalse(speak);
        }
        if (skill != "A01" && skill != "A02" && skill != "A03" && skill != "B01" &&
            skill != "B02" && skill != "A16") {
            Complete(work, Error("Unsupported expression skill"));
            return;
        }
        const auto* count = cJSON_GetObjectItem(args.get(), "repetitions");
        if (count && (skill != "A01" || !cJSON_IsNumber(count) || count->valuedouble < 1 ||
                      count->valuedouble > 5 || count->valuedouble != count->valueint)) {
            Complete(work, Error("repetitions accepts integer 1..5 for A01 only"));
            return;
        }
        const int repetitions = count ? count->valueint : 0;
        for (auto* field = args->child; field; field = field->next)
            if (std::string(field->string) != "skill_id" &&
                std::string(field->string) != "end_mode" &&
                std::string(field->string) != "speech" && std::string(field->string) != "speak" &&
                std::string(field->string) != "action" &&
                std::string(field->string) != "repetitions") {
                Complete(work, Error("Expression accepts action or skill_id, speech and end_mode"));
                return;
            }
        const auto end = lamp::Text(args.get(), "end_mode");
        if (cJSON_GetObjectItem(args.get(), "end_mode") && end != "return_to_reference" &&
            end != "hold_at_end") {
            Complete(work, Error("Invalid expression end_mode"));
            return;
        }
        args = Parse("{\"steps\":[{\"action_id\":\"execute_primitive\",\"skill_id\":\"" + skill +
                     "\",\"options\":{\"end_mode\":\"" +
                     (end.empty() ? "return_to_reference" : end) + "\"}}]}");
        if (repetitions)
            cJSON_AddNumberToObject(
                cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(args.get(), "steps"), 0),
                                    "options"),
                "repetitions", repetitions);
    }
    bool auto_enable = work->name == "lamp_set_mode" || work->name == "lamp_rest";
    if (auto* flag = cJSON_GetObjectItemCaseSensitive(args.get(), "enable_if_needed")) {
        if (operation != "execute_group" || !cJSON_IsBool(flag)) {
            Complete(work, Error("enable_if_needed requires boolean motion intent"));
            return;
        }
        auto_enable = cJSON_IsTrue(flag);
        cJSON_DeleteItemFromObjectCaseSensitive(args.get(), "enable_if_needed");
    }
    if (work->name == "lamp_set_mode") {
        if (cJSON_GetArraySize(args.get()) != 1 ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(args.get(), "mode"))) {
            Complete(work, Error("lamp_set_mode accepts only mode"));
            return;
        }
        const auto mode = lamp::Text(args.get(), "mode");
        args = Json(cJSON_CreateObject(), cJSON_Delete);
        auto* step = cJSON_CreateObject();
        cJSON_AddStringToObject(step, "action_id", "set_mode");
        cJSON_AddStringToObject(step, "mode", mode.c_str());
        auto* steps = cJSON_AddArrayToObject(args.get(), "steps");
        cJSON_AddItemToArray(steps, step);
    }
    if (operation == "execute_group") {
        auto* steps = cJSON_GetObjectItem(args.get(), "steps");
        for (auto* step = steps ? steps->child : nullptr; step; step = step->next) {
            const auto skill = lamp::Text(step, "skill_id");
            if (skill == "A01" || skill == "A02" || skill == "A03" || skill == "A16" ||
                skill == "B01" || skill == "B02")
                if (lamp::Text(step, "target_id") == "user" ||
                    lamp::Text(step, "target_id") == "book")
                    cJSON_DeleteItemFromObject(step, "target_id");
        }
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
             {"available", "configuration_loaded", "mode", "busy", "motion_enabled",
              "manual_control", "pose_rad", "target_pose_rad", "requires_operator_enable",
              "torque_enabled_confirmed", "unloaded_confirmed", "calibrated_targets",
              "unsupported_primitives", "last_task", "runtime"}) {
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
    if (prepare_expression) {
        auto state = Parse(executor_.State());
        if (!cJSON_IsTrue(cJSON_GetObjectItem(state.get(), "available")) ||
            !cJSON_IsTrue(cJSON_GetObjectItem(state.get(), "motion_enabled")) ||
            !cJSON_IsTrue(cJSON_GetObjectItem(state.get(), "torque_enabled_confirmed")) ||
            cJSON_IsTrue(cJSON_GetObjectItem(state.get(), "busy")) ||
            cJSON_IsTrue(cJSON_GetObjectItem(state.get(), "manual_control"))) {
            Complete(work, Error("Expression requires enabled fresh hardware and free control; no "
                                 "automatic enable"));
            return;
        }
        auto receipt = Parse(
            R"({"ok":true,"status":"prepared","execution_completed":false,"instruction":"Only prepared; speak naturally now. Local playback releases the action; never claim completed."})");
        cJSON_AddItemToObject(receipt.get(), "speech",
                              silent_expression ? cJSON_CreateNull() : cJSON_CreateString("auto"));
        if (silent_expression) {
            cJSON_DeleteItemFromObject(receipt.get(), "instruction");
            cJSON_AddStringToObject(receipt.get(), "instruction",
                                    "Silent action: local dispatch immediately; do not create a "
                                    "spoken success reply. Failures may be reported.");
        }
        cJSON_AddItemToObject(receipt.get(), "args", cJSON_Duplicate(args.get(), true));
        Complete(work, lamp::Encode(receipt.get()));
        return;
    }
    // Explicit mode/rest requests, or an explicit motion-intent flag, authorize enable.
    // The same physical feedback and manual-ownership gates apply as a separate torque tool.
    if (auto_enable && !work->enable_checked) {
        work->enable_checked = true;
        auto state = Parse(executor_.State());
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state.get(), "motion_enabled"))) {
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state.get(), "available")) ||
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state.get(), "manual_control"))) {
                Complete(work, Error("Cannot auto-enable: five-axis feedback unavailable or manual "
                                     "control owns motion"));
                return;
            }
            Json torque(cJSON_CreateObject(), cJSON_Delete);
            cJSON_AddStringToObject(torque.get(), "protocol_version", lamp::kProtocol);
            cJSON_AddStringToObject(torque.get(), "request_id", executor_.NewRequestId().c_str());
            cJSON_AddStringToObject(torque.get(), "operation", "set_torque");
            auto* enable = cJSON_AddObjectToObject(torque.get(), "args");
            cJSON_AddBoolToObject(enable, "enabled", true);
            const auto command = lamp::Encode(torque.get());
            if (diagnostic_)
                diagnostic_("runtime_request", "auto_enable_for_explicit_motion", command);
            const auto output = executor_.Submit(command);
            auto receipt = Parse(output);
            const auto task = lamp::Text(Result(receipt.get()), "task_id");
            if (!task.empty() &&
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt.get(), "ok"))) {
                motion_task_ = task;
                motion_turn_ = work->turn;
                pending_.push_back({work, task, esp_timer_get_time() + 15000000, true});
            } else
                Complete(work, ToolResult(output));
            return;
        }
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
        (operation == "execute_group" || operation == "set_torque" || operation == "rest" ||
         operation == "record_calibration")) {
        pending_.push_back(
            {work, task,
             esp_timer_get_time() +
                 ((operation == "rest" || operation == "execute_group") ? 65000000 : 15000000)});
    } else
        Complete(work, ToolResult(output));
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
    std::vector<Work*> enabled_requests;
    for (auto it = pending_.begin(); it != pending_.end();) {
        const bool cancelled = it->work->turn <= invalid_through_;
        const bool timeout = esp_timer_get_time() > it->deadline;
        auto output = executor_.Submit(lamp::Query("get_task", executor_.NewRequestId(), it->task));
        auto result = Parse(output);
        const auto status = lamp::Text(Result(result.get()), "status");
        bool mode_only = false;
        auto arguments = Parse(it->work->args);
        auto* steps = cJSON_GetObjectItemCaseSensitive(arguments.get(), "steps");
        mode_only = cJSON_GetArraySize(steps) == 1 &&
                    lamp::Text(cJSON_GetArrayItem(steps, 0), "action_id") == "set_mode";
        auto* total = cJSON_GetObjectItemCaseSensitive(Result(result.get()), "total_steps");
        const bool started = !it->enabling && it->work->name == "lamp_run_sequence" && !mode_only &&
                             status == "running" && cJSON_IsNumber(total) && total->valuedouble > 0;
        if (cancelled || timeout || started ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result.get(), "ok")) ||
            status == "completed" || status == "limited" || status == "failed" ||
            status == "cancelled") {
            if (timeout)
                executor_.Submit(lamp::Query("cancel", executor_.NewRequestId(), it->task));
            if (it->enabling && !cancelled && !timeout && status == "completed" &&
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result.get(), "ok"))) {
                enabled_requests.push_back(it->work);
            } else
                Complete(it->work,
                         cancelled ? Error("Turn cancelled")
                         : timeout
                             ? Error("Control confirmation timed out; inspect hardware feedback")
                             : ToolResult(output));
            it = pending_.erase(it);
        } else
            ++it;
    }
    for (auto* request : enabled_requests)
        Process(request);
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
    // Persisted mode is a planning reference, never evidence of physical posture.
    cJSON_AddStringToObject(runtime, "mode_basis", "saved_action_reference_not_measured_posture");
    cJSON_AddStringToObject(runtime, "confirmed_mode", "unknown");
    cJSON_AddBoolToObject(runtime, "hardware_online",
                          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j.get(), "available")));
    cJSON_AddNumberToObject(runtime, "snapshot_age_ms",
                            snapshot_at ? (esp_timer_get_time() - snapshot_at) / 1000 : -1);
    cJSON_AddBoolToObject(runtime, "runtime_ready", started_);
    // Observation state belongs to the Omni connection and is merged at its API boundary.
    cJSON_AddBoolToObject(runtime, "speech_motion_sync", true);
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
    add("lamp_get_state",
        "查询机械臂缓存：available才表示五轴反馈新鲜；mode仅保存的动作参考，不能断言已经站起。",
        object());
    add("lamp_end_conversation",
        "广义告别或停止聊天：简短告别播完退出监听，不坐下卸力。讨论告别不结束。", object());
    add("lamp_rest",
        "用户说你去休息吧/休息一下/"
        "睡吧时必须调用这个完整动作，不能仅回答：必要时使能，坐下并确认，卸力，成功后告别并停止监听"
        "。用户说自己休息或讨论休息不调用。",
        object());
    add("lamp_set_mode",
        "明确命令站起来/站好/坐下/"
        "开始工作时调用，不能仅口头答应。必要时自动使能，再切换姿态并等待真实完成。保持yaw；不用于"
        "无动作意图的聊天或后台表达。",
        cJSON_Parse(
            R"({"type":"object","additionalProperties":false,"properties":{"mode":{"type":"string","enum":["sit","stand","work"]}},"required":["mode"]})"));
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
    add("lamp_express",
        "纯动作传speak:false与action或skill_"
        "id，立即执行且成功不说话；默认伴随说话的表达：先调用预备，再自然回答；实际播放开始后本地释"
        "放动作。不等待运动完成，不用于"
        "单独站起/坐下/工作。开心/"
        "兴奋A02，好奇B02，同意A01，否定B01，低落A03，强调A16。无需目标或标定，必须已使能，不自动上"
        "力。每轮最多一次。",
        cJSON_Parse(
            R"({"type":"object","additionalProperties":false,"properties":{"skill_id":{"type":"string","enum":["A01","A02","A03","B01","B02","A16"]},"action":{"type":"string","enum":["点头","兴奋摆动","低头","摇头","好奇歪头","强调","nod","sway","lower_head","shake_head","tilt","emphasize"]},"speak":{"type":"boolean","description":"false只做动作不回复；true或省略伴随说话"},"repetitions":{"type":"integer","minimum":1,"maximum":5,"description":"仅A01点头；一次或两下保持默认2，明确三/四/五次才设置。不拆成多次调用。"},"end_mode":{"type":"string","enum":["return_to_reference","hold_at_end"]}},"oneOf":[{"required":["skill_id"]},{"required":["action"]}]})"));
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
                if (id != "A01" && id != "A02" && id != "A03" && id != "A16" && id != "B01" &&
                    id != "B02")
                    cJSON_AddItemToObject(
                        properties, "target_id",
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
    cJSON_AddItemToObject(
        cJSON_GetObjectItem(schema.get(), "properties"), "enable_if_needed",
        cJSON_Parse(
            R"({"type":"boolean","description":"仅用户明确要求实际运动时true；自发表达/后台动作必须false，默认false。"})"));
    cJSON_AddItemToObject(schema.get(), "required", cJSON_Parse("[\"steps\"]"));
    add("lamp_run_sequence",
        "执行一个有限语义动作组。次数幅度用本地默认，点两下头只用一次A01。靠近用A13而非低头，缺标定"
        "不替代。set_mode保持yaw。受理后可正常回答，不声称已完成。若要边说边表达请用lamp_"
        "express，不在这里串行等待表达。",
        schema.release());
    return lamp::Encode(tools.get());
}
