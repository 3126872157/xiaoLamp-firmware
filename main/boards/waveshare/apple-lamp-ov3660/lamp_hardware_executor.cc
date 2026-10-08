#include "lamp_hardware_executor.h"
#include "sdkconfig.h"
#if CONFIG_APPLE_LAMP_HARDWARE_MOTION
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <psa/crypto.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include "application.h"
#include "lamp_bus_protocol.h"
#include "lamp_pose_model.h"
namespace {
const cJSON* Item(const cJSON* o, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(o, key);
}
std::string Error(const char* code, const std::string& reason) {
    CJsonUniquePtr o(cJSON_CreateObject());
    cJSON_AddBoolToObject(o.get(), "ok", false);
    auto* e = cJSON_AddObjectToObject(o.get(), "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", reason.c_str());
    return lamp::Encode(o.get());
}
std::string Hash(const std::string& text) {
    unsigned char digest[32];
    size_t produced = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const unsigned char*>(text.data()),
                         text.size(), digest, sizeof(digest), &produced) != PSA_SUCCESS ||
        produced != sizeof(digest))
        return "";
    char out[65];
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    return out;
}
CJsonUniquePtr Canonical(const cJSON* value) {
    if (cJSON_IsObject(value)) {
        CJsonUniquePtr out(cJSON_CreateObject());
        std::vector<const cJSON*> items;
        for (auto* p = value->child; p; p = p->next)
            items.push_back(p);
        std::stable_sort(items.begin(), items.end(), [](auto* a, auto* b) {
            return std::string(a->string) < std::string(b->string);
        });
        for (auto* p : items)
            cJSON_AddItemToObject(out.get(), p->string, Canonical(p).release());
        return out;
    }
    if (cJSON_IsArray(value)) {
        CJsonUniquePtr out(cJSON_CreateArray());
        for (auto* p = value->child; p; p = p->next)
            cJSON_AddItemToArray(out.get(), Canonical(p).release());
        return out;
    }
    return CJsonUniquePtr(cJSON_Duplicate(value, true));
}
constexpr const char* Modes[] = {"sit", "stand", "work"};
std::string TeachingBinding(const cJSON* full) {
    CJsonUniquePtr binding(cJSON_CreateObject());
    for (const auto* key : {"model_version", "calibration_version"})
        cJSON_AddItemToObject(binding.get(), key, cJSON_Duplicate(Item(full, key), true));
    cJSON_AddItemToObject(binding.get(), "servos",
                          cJSON_Duplicate(Item(Item(full, "hardware"), "servos"), true));
    cJSON_AddStringToObject(binding.get(), "placement",
                            lamp::Text(Item(full, "profiles"), "placementId").c_str());
    auto* homes = cJSON_AddObjectToObject(binding.get(), "homes");
    for (const auto* mode : Modes)
        cJSON_AddItemToObject(
            homes, mode,
            cJSON_Duplicate(Item(Item(Item(Item(full, "homes"), "homes"), mode), "pose"), true));
    return Hash(lamp::Encode(Canonical(binding.get()).get()));
}
CJsonUniquePtr TeachingPayload(const cJSON* full, int mode) {
    CJsonUniquePtr saved(cJSON_CreateObject());
    cJSON_AddStringToObject(saved.get(), "format", "lamp-native-teaching/1");
    cJSON_AddStringToObject(saved.get(), "model_version", lamp::pose::kModel);
    cJSON_AddStringToObject(saved.get(), "binding", TeachingBinding(full).c_str());
    cJSON_AddNumberToObject(saved.get(), "runtime_mode", mode);
    auto* targets = cJSON_AddObjectToObject(saved.get(), "targets");
    for (const auto* m : Modes) {
        auto* to = cJSON_AddObjectToObject(targets, m);
        const auto* from = Item(Item(Item(Item(full, "profiles"), "modes"), m), "targets");
        for (auto* t = from ? from->child : nullptr; t; t = t->next)
            if (lamp::Text(t, "updatedAt").starts_with("uptime:") && !Item(t, "sceneSignature"))
                cJSON_AddItemToObject(to, t->string, cJSON_Duplicate(t, true));
    }
    return saved;
}
std::expected<lamp::pose::Configuration, std::string> Installation(const cJSON* base,
                                                                   const cJSON* mapping) {
    if (!cJSON_IsArray(mapping) || cJSON_GetArraySize(mapping) != 5)
        return std::unexpected("Expected five installation mappings");
    CJsonUniquePtr candidate(cJSON_Duplicate(base, true));
    auto* servos = const_cast<cJSON*>(Item(Item(candidate.get(), "hardware"), "servos"));
    for (unsigned i = 0; i < 5; i++) {
        const auto* row = cJSON_GetArrayItem(mapping, i);
        const auto *direction = Item(row, "direction"), *zero = Item(row, "zero_ticks");
        if (!cJSON_IsObject(row) || cJSON_GetArraySize(row) != 2 || !cJSON_IsNumber(direction) ||
            !cJSON_IsNumber(zero))
            return std::unexpected("Only numeric direction and zero_ticks may be changed");
        auto* servo = cJSON_GetArrayItem(servos, i);
        cJSON_ReplaceItemInObjectCaseSensitive(servo, "direction",
                                               cJSON_Duplicate(direction, true));
        cJSON_ReplaceItemInObjectCaseSensitive(servo, "zero_ticks", cJSON_Duplicate(zero, true));
    }
    auto* modes = const_cast<cJSON*>(Item(Item(candidate.get(), "profiles"), "modes"));
    for (auto* m = modes->child; m; m = m->next)
        cJSON_ReplaceItemInObjectCaseSensitive(m, "targets", cJSON_CreateObject());
    return lamp::pose::ParseConfiguration(lamp::Encode(candidate.get()));
}
constexpr uint8_t Lo(unsigned v) { return v & 255; }
constexpr uint8_t Hi(unsigned v) { return (v >> 8) & 255; }
}  // namespace
LampHardwareExecutor::LampHardwareExecutor() : LampLocalExecutor(false) {
    LoadInstallation();
    if (configuration_)
        LoadStored();
    TaskHandle_t worker = nullptr;
    if (uart_enabled_ &&
        xTaskCreate(RunHardware, "lamp_hardware", 12288, this, 1, &worker) == pdPASS)
        live_ = true;
    else
        setup_error_ = "Hardware worker/configuration unavailable";
    ESP_LOGI("LampHW",
             "LOCAL hardware ready for operator test; startup DISARMED, no automatic movement");
}
void LampHardwareExecutor::LoadStored() {
    nvs_handle_t h;
    const auto opened = nvs_open("lamp_native", NVS_READONLY, &h);
    if (opened == ESP_ERR_NVS_NOT_FOUND)
        return;
    if (opened != ESP_OK) {
        configuration_.reset();
        configuration_error_ = esp_err_to_name(opened);
        return;
    }
    size_t size = 0;
    auto status = nvs_get_blob(h, "device_v1", nullptr, &size);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return;
    }
    if (status != ESP_OK || size == 0 || size > 65536) {
        nvs_close(h);
        configuration_.reset();
        configuration_error_ = "Invalid NVS device configuration";
        return;
    }
    std::string data(size, '\0');
    status = nvs_get_blob(h, "device_v1", data.data(), &size);
    nvs_close(h);
    if (status != ESP_OK) {
        configuration_.reset();
        configuration_error_ = esp_err_to_name(status);
        return;
    }
    data.resize(size);
    CJsonUniquePtr saved(cJSON_Parse(data.c_str()));
    CJsonUniquePtr compact;
    if (lamp::Text(saved.get(), "format") == "lamp-device/1") {
        auto legacy = lamp::pose::ParseConfiguration(data);
        if (legacy)
            compact = TeachingPayload(legacy->source.get(),
                                      cJSON_IsNumber(Item(saved.get(), "runtime_mode"))
                                          ? Item(saved.get(), "runtime_mode")->valueint
                                          : 0);
    } else if (lamp::Text(saved.get(), "format") == "lamp-native-teaching/1")
        compact = std::move(saved);
    if (!configuration_ || !compact || lamp::Text(compact.get(), "binding").empty() ||
        lamp::Text(compact.get(), "binding") != TeachingBinding(configuration_->source.get())) {
        configuration_.reset();
        configuration_error_ =
            "Stored teaching/model/home/mapping binding mismatch; reset teaching before changing "
            "mapping";
        return;
    }
    auto* saved_targets = Item(compact.get(), "targets");
    bool valid = cJSON_IsObject(saved_targets) && cJSON_GetArraySize(saved_targets) <= 3;
    CJsonUniquePtr merged(cJSON_Duplicate(configuration_->source.get(), true));
    for (auto* m = saved_targets ? saved_targets->child : nullptr; m; m = m->next) {
        auto* profile =
            const_cast<cJSON*>(Item(Item(Item(merged.get(), "profiles"), "modes"), m->string));
        if (!profile || !cJSON_IsObject(m) || cJSON_GetArraySize(m) > 2) {
            valid = false;
            break;
        }
        auto* to = const_cast<cJSON*>(Item(profile, "targets"));
        for (auto* t = m->child; t; t = t->next) {
            cJSON_DeleteItemFromObjectCaseSensitive(to, t->string);
            cJSON_AddItemToObject(to, t->string, cJSON_Duplicate(t, true));
        }
    }
    const auto* logical = Item(compact.get(), "runtime_mode");
    valid &= cJSON_IsNumber(logical) && logical->valuedouble == logical->valueint &&
             logical->valueint >= 0 && logical->valueint < 3;
    if (!valid) {
        configuration_.reset();
        configuration_error_ = "Invalid stored teaching schema";
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(merged.get(), "runtime_mode");
    cJSON_AddNumberToObject(merged.get(), "runtime_mode", logical->valueint);
    auto cfg = lamp::pose::ParseConfiguration(lamp::Encode(merged.get()));
    if (!cfg) {
        configuration_.reset();
        configuration_error_ = cfg.error();
        return;
    }
    configuration_ = std::move(*cfg);
    const auto* saved_mode = Item(configuration_->source.get(), "runtime_mode");
    if (cJSON_IsNumber(saved_mode) && saved_mode->valueint >= 0 && saved_mode->valueint < 3)
        mode_ = saved_mode->valueint;
}
void LampHardwareExecutor::LoadInstallation() {
    nvs_handle_t h;
    const auto opened = nvs_open("lamp_native", NVS_READONLY, &h);
    if (opened == ESP_ERR_NVS_NOT_FOUND)
        return;
    if (opened != ESP_OK) {
        configuration_.reset();
        configuration_error_ = "Installation NVS open failed";
        return;
    }
    size_t size = 0;
    auto err = nvs_get_blob(h, "install_v1", nullptr, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return;
    }
    if (err != ESP_OK || size == 0 || size > 2048) {
        nvs_close(h);
        configuration_.reset();
        configuration_error_ = "Invalid installation storage";
        return;
    }
    std::string data(size, '\0');
    err = nvs_get_blob(h, "install_v1", data.data(), &size);
    nvs_close(h);
    CJsonUniquePtr root(cJSON_Parse(data.c_str()));
    if (err != ESP_OK || lamp::Text(root.get(), "format") != "lamp-installation/1" ||
        lamp::Text(root.get(), "model_version") != lamp::pose::kModel ||
        lamp::Text(root.get(), "calibration_version") != lamp::pose::kCalibration ||
        !configuration_) {
        configuration_.reset();
        configuration_error_ = "Installation version mismatch";
        return;
    }
    auto next = Installation(configuration_->source.get(), Item(root.get(), "mapping"));
    if (!next) {
        configuration_.reset();
        configuration_error_ = next.error();
        return;
    }
    configuration_ = std::move(*next);
}
bool LampHardwareExecutor::SaveInstallation(const cJSON* mapping) {
    for (unsigned i = 0; i < 5; i++)
        if (!Read(i, 28) || !Read(i, 32) || servos_[i].torque != 0 || !Read(i, 21))
            return false;
    CJsonUniquePtr fallback(cJSON_Parse(lamp::pose::kDeviceConfiguration));
    auto next =
        Installation(configuration_ ? configuration_->source.get() : fallback.get(), mapping);
    if (!next)
        return false;
    // Recheck the reachable range using the fresh internal limits, not just a browser preview.
    for (unsigned i = 0; i < 5; i++) {
        const auto* row = cJSON_GetArrayItem(mapping, i);
        const double d = Item(row, "direction")->valuedouble;
        const double zero = Item(row, "zero_ticks")->valuedouble;
        const double a = (servos_[i].limit_min - zero) * .24 * std::numbers::pi / 180 / d;
        const double b = (servos_[i].limit_max - zero) * .24 * std::numbers::pi / 180 / d;
        if (std::max(lamp::pose::kJoints[i].lower, std::min(a, b)) >
            std::min(lamp::pose::kJoints[i].upper, std::max(a, b)))
            return false;
    }
    CJsonUniquePtr saved(cJSON_CreateObject());
    cJSON_AddStringToObject(saved.get(), "format", "lamp-installation/1");
    cJSON_AddStringToObject(saved.get(), "model_version", lamp::pose::kModel);
    cJSON_AddStringToObject(saved.get(), "calibration_version", lamp::pose::kCalibration);
    cJSON_AddItemToObject(saved.get(), "mapping", cJSON_Duplicate(mapping, true));
    // All old target poses used the previous mapping; clear them in the same namespace.
    auto* modes = const_cast<cJSON*>(Item(Item(next->source.get(), "profiles"), "modes"));
    for (auto* m = modes->child; m; m = m->next)
        cJSON_ReplaceItemInObjectCaseSensitive(m, "targets", cJSON_CreateObject());
    auto cleared = lamp::pose::ParseConfiguration(lamp::Encode(next->source.get()));
    if (!cleared)
        return false;
    const auto install = lamp::Encode(saved.get());
    auto compact = TeachingPayload(cleared->source.get(), mode_);
    if (lamp::Text(compact.get(), "binding").empty() || ReadCancelled())
        return false;
    const auto teaching = lamp::Encode(compact.get());
    if (teaching.size() > 2048 || install.size() > 2048)
        return false;
    nvs_handle_t h;
    if (nvs_open("lamp_native", NVS_READWRITE, &h) != ESP_OK)
        return false;
    auto err = nvs_set_blob(h, "install_v1", install.data(), install.size());
    if (err == ESP_OK)
        err = nvs_set_blob(h, "device_v1", teaching.data(), teaching.size());
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK)
        return false;
    std::lock_guard lock(mutex_);
    configuration_ = std::move(*cleared);
    configuration_error_.clear();
    last_target_.reset();
    armed_ = false;
    return true;
}
std::string LampHardwareExecutor::PanelConfig(const std::string& candidate) {
    std::lock_guard lock(mutex_);
    CJsonUniquePtr fallback(cJSON_Parse(lamp::pose::kDeviceConfiguration));
    auto* base = configuration_ ? configuration_->source.get() : fallback.get();
    CJsonUniquePtr requested(candidate.empty() ? nullptr : cJSON_Parse(candidate.c_str()));
    auto cfg = candidate.empty() ? lamp::pose::ParseConfiguration(lamp::Encode(base))
                                 : Installation(base, Item(requested.get(), "mapping"));
    if (!cfg)
        return Error("INVALID_MAPPING", cfg.error());
    CJsonUniquePtr out(cJSON_CreateObject());
    cJSON_AddBoolToObject(out.get(), "ok", true);
    auto* rows = cJSON_AddArrayToObject(out.get(), "mapping");
    auto* servos = Item(Item(cfg->source.get(), "hardware"), "servos");
    const int convention[] = {1, -1, -1, 1, 1};
    for (unsigned i = 0; i < 5; i++) {
        const auto* s = cJSON_GetArrayItem(servos, i);
        const auto zero = Item(s, "zero_ticks")->valuedouble;
        const auto d = Item(s, "direction")->valuedouble;
        auto lo = std::max(lamp::pose::kJoints[i].lower, -zero * .24 / 180 * std::numbers::pi / d);
        auto hi = std::min(lamp::pose::kJoints[i].upper,
                           (1000 - zero) * .24 / 180 * std::numbers::pi / d);
        double a = (0 - zero) * .24 / 180 * std::numbers::pi / d;
        double b = (1000 - zero) * .24 / 180 * std::numbers::pi / d;
        lo = std::max(lamp::pose::kJoints[i].lower, std::min(a, b));
        hi = std::min(lamp::pose::kJoints[i].upper, std::max(a, b));
        if (servos_[i].limits_at) {
            a = (servos_[i].limit_min - zero) * .24 / 180 * std::numbers::pi / d;
            b = (servos_[i].limit_max - zero) * .24 / 180 * std::numbers::pi / d;
            lo = std::max(lo, std::min(a, b));
            hi = std::min(hi, std::max(a, b));
        }
        if (lo > hi)
            return Error("INVALID_MAPPING", "No reachable joint range for this installation");
        auto* row = cJSON_Duplicate(s, true);
        cJSON_AddNumberToObject(row, "lower_rad", lo);
        cJSON_AddNumberToObject(row, "upper_rad", hi);
        cJSON_AddNumberToObject(row, "display_sign", convention[i]);
        cJSON_AddItemToArray(rows, row);
    }
    cJSON_AddStringToObject(out.get(), "api_version", "lamp-twin/1");
    cJSON_AddStringToObject(out.get(), "pose_space", "urdf_rad");
    const double tool[] = {lamp::pose::kTool.x, lamp::pose::kTool.y, lamp::pose::kTool.z};
    cJSON_AddItemToObject(out.get(), "tool_xyz", cJSON_CreateDoubleArray(tool, 3));
    auto* geometry = cJSON_AddArrayToObject(out.get(), "urdf_joints");
    for (unsigned i = 0; i < 5; i++) {
        auto* row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "joint", lamp::pose::kNames[i]);
        const auto& j = lamp::pose::kJoints[i];
        const std::pair<const char*, lamp::pose::Vec> vectors[] = {
            {"xyz", j.xyz}, {"rpy", j.rpy}, {"axis", j.axis}};
        for (const auto& [key, v] : vectors) {
            double values[] = {v.x, v.y, v.z};
            cJSON_AddItemToObject(row, key, cJSON_CreateDoubleArray(values, 3));
        }
        cJSON_AddNumberToObject(row, "lower_rad", j.lower);
        cJSON_AddNumberToObject(row, "upper_rad", j.upper);
        cJSON_AddItemToArray(geometry, row);
    }
    // External mesh clients resolve their own assets by this version, using the same raw poses.
    cJSON_AddStringToObject(out.get(), "model_version", lamp::pose::kModel);
    cJSON_AddBoolToObject(out.get(), "configuration_loaded", configuration_.has_value());
    return lamp::Encode(out.get());
}
std::string LampHardwareExecutor::PanelSubmit(const std::string& body) {
    if (body.empty() || body.size() > 2048)
        return Error("INVALID_REQUEST", "Panel request too large");
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    const auto op = lamp::Text(root.get(), "operation"), id = lamp::Text(root.get(), "request_id");
    const auto* args = Item(root.get(), "args");
    if (!cJSON_IsObject(root.get()) || cJSON_GetArraySize(root.get()) != 3 || id.empty() ||
        id.size() > 87 || !cJSON_IsObject(args))
        return Error("INVALID_REQUEST", "Expected operation, request_id and args");
    if (op == "acquire" || op == "release") {
        if (cJSON_GetArraySize(args))
            return Error("INVALID_REQUEST", "Control ownership has no parameters");
        std::lock_guard lock(mutex_);
        if (!queued_.empty() || !active_.empty())
            return Error("BUSY", "Wait for task completion or stop first");
        manual_ = op == "acquire";
        armed_ = false;
        rest_at_ = 0;
        return "{\"ok\":true,\"ownership_changed\":true}";
    }
    bool valid = false;
    if (op == "cancel")
        valid = cJSON_GetArraySize(args) == 0;
    if (op == "set_torque")
        valid = cJSON_GetArraySize(args) == 1 && cJSON_IsBool(Item(args, "enabled"));
    if (op == "move") {
        const auto* duration = Item(args, "duration_sec");
        auto q = lamp::pose::ParsePose(Item(args, "pose_rad"));
        valid = cJSON_GetArraySize(args) == 2 && q.has_value() && cJSON_IsNumber(duration) &&
                std::isfinite(duration->valuedouble) && duration->valuedouble >= .2 &&
                duration->valuedouble <= 10;
    }
    if (op == "install") {
        valid = cJSON_GetArraySize(args) == 2 && cJSON_IsTrue(Item(args, "clear_targets"));
        if (valid) {
            const auto preview = PanelConfig(lamp::Encode(args));
            CJsonUniquePtr checked(cJSON_Parse(preview.c_str()));
            valid = cJSON_IsTrue(Item(checked.get(), "ok"));
        }
    }
    if (!valid)
        return Error("INVALID_REQUEST", "Unsupported panel operation or invalid parameters");
    return SubmitChecked(body, true);
}
bool LampHardwareExecutor::Ready() {
    std::lock_guard lock(mutex_);
    return live_ && configuration_.has_value();
}
std::string LampHardwareExecutor::Receipt(const std::string& id, const Record& r) {
    CJsonUniquePtr o(cJSON_CreateObject());
    cJSON_AddStringToObject(o.get(), "protocol_version", lamp::kProtocol);
    cJSON_AddStringToObject(o.get(), "request_id", id.c_str());
    cJSON_AddBoolToObject(o.get(), "ok", true);
    auto* t = cJSON_AddObjectToObject(o.get(), "result");
    cJSON_AddStringToObject(t, "request_id", id.c_str());
    cJSON_AddStringToObject(t, "task_id", ("hardware-" + id).c_str());
    cJSON_AddStringToObject(t, "status", r.status.c_str());
    cJSON_AddStringToObject(t, "reason", r.reason.c_str());
    cJSON_AddStringToObject(t, "completion_basis",
                            r.operation == "cancel" ? "command_sent" : "hardware_feedback");
    cJSON_AddNumberToObject(t, "step_index", r.step);
    cJSON_AddNumberToObject(t, "total_steps", r.total);
    return lamp::Encode(o.get());
}
std::string LampHardwareExecutor::Inspect(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto it = records_.find(id);
    return it == records_.end() ? Error("REQUEST_NOT_FOUND", "Unknown hardware request")
                                : Receipt(id, it->second);
}
std::string LampHardwareExecutor::Submit(const std::string& body) {
    auto checked = lamp::Command(body);
    if (!checked)
        return Error("INVALID_REQUEST", checked.error());
    return SubmitChecked(*checked, false);
}
std::string LampHardwareExecutor::SubmitChecked(const std::string& body, bool panel) {
    CJsonUniquePtr o(cJSON_Parse(body.c_str()));
    const auto op = lamp::Text(o.get(), "operation"), id = lamp::Text(o.get(), "request_id");
    const auto* args = Item(o.get(), "args");
    if (id.size() > 87)
        return Error(
            "INVALID_REQUEST",
            "Hardware request ID must be at most 87 characters so its task ID remains queryable");
    if (op == "get_state")
        return State();
    if (op == "get_task") {
        const auto task = lamp::Text(args, "task_id");
        return Inspect(task.starts_with("hardware-") ? task.substr(9) : task);
    }
    std::lock_guard lock(mutex_);
    const auto sig = Hash(lamp::Encode(Canonical(o.get()).get()));
    if (sig.empty())
        return Error("SIGNATURE_FAILED", "Cannot fingerprint request");
    if (auto it = records_.find(id); it != records_.end())
        return it->second.signature == sig
                   ? Receipt(id, it->second)
                   : Error("REQUEST_ID_CONFLICT", "Request ID reused with different content");
    const bool emergency =
        op == "cancel" || (op == "set_torque" && cJSON_IsFalse(Item(args, "enabled")));
    if (!live_ || (!configuration_ && !emergency && !(panel && op == "install")))
        return Error("UNAVAILABLE",
                     configuration_error_.empty() ? setup_error_ : configuration_error_);
    if (!panel && manual_ && !emergency)
        return Error("MANUAL_CONTROL", "Web test panel owns motion; release manual control first");
    if (panel && !manual_ && !emergency)
        return Error("NO_MANUAL_CONTROL", "Acquire manual control first");
    const bool preempt = op == "cancel" || op == "rest" ||
                         (op == "set_torque" && cJSON_IsFalse(Item(args, "enabled")));
    if (preempt && op == "cancel") {
        const auto target = lamp::Text(args, "task_id");
        if (!target.empty() && target != "hardware-" + active_ &&
            (queued_.empty() ||
             target != "hardware-" + lamp::Text(CJsonUniquePtr(cJSON_Parse(queued_.c_str())).get(),
                                                "request_id")))
            return Error("TASK_NOT_ACTIVE", "Cancellation target is not active");
    }
    if ((!queued_.empty() || !active_.empty()) && !preempt)
        return Error("BUSY", "Hardware task in progress; do not retry dialogue");
    const auto* context = Item(args, "context");
    if (context) {
        const auto turn = lamp::Text(context, "turn_id");
        if (turns_.contains(turn) ||
            (dialogue_at_ && esp_timer_get_time() - dialogue_at_ < 2500000))
            return Error("DIALOGUE_SKIPPED", "Turn already used or cooldown");
        if (turns_.size() >= 64)
            turns_.erase(turns_.begin());
        turns_.insert(turn);
        dialogue_at_ = esp_timer_get_time();
    }
    if (preempt && !queued_.empty()) {
        CJsonUniquePtr pending(cJSON_Parse(queued_.c_str()));
        auto& old = records_.at(lamp::Text(pending.get(), "request_id"));
        old.status = "cancelled";
        old.reason = "Preempted before execution";
        queued_.clear();
    }
    ++epoch_;
    rest_at_ = 0;
    if (op == "set_torque")
        armed_ = false;
    while (order_.size() >= 32) {
        records_.erase(order_.front());
        order_.pop_front();
    }
    records_[id] = {sig, op, "accepted", "Awaiting local worker"};
    order_.push_back(id);
    queued_ = body;
    RecordDiagnostic("task", id, Receipt(id, records_[id]));
    return Receipt(id, records_[id]);
}
std::string LampHardwareExecutor::State() {
    CJsonUniquePtr o(cJSON_Parse(LampLocalExecutor::State().c_str()));
    std::lock_guard lock(mutex_);
    cJSON_AddStringToObject(o.get(), "api_version", "lamp-twin/1");
    cJSON_AddStringToObject(o.get(), "pose_space", "urdf_rad");
    cJSON_AddBoolToObject(o.get(), "manual_control", manual_);
    if (last_target_) {
        auto* pose = cJSON_AddObjectToObject(o.get(), "target_pose_rad");
        for (unsigned i = 0; i < 5; i++)
            cJSON_AddNumberToObject(pose, lamp::pose::kNames[i], (*last_target_)[i]);
    }
    cJSON_ReplaceItemInObjectCaseSensitive(o.get(), "executor",
                                           cJSON_CreateString("local_uart_hardware"));
    cJSON_ReplaceItemInObjectCaseSensitive(o.get(), "software_stage",
                                           cJSON_CreateString("local_player"));
    cJSON_ReplaceItemInObjectCaseSensitive(o.get(), "motion_enabled", cJSON_CreateBool(armed_));
    cJSON_AddStringToObject(o.get(), "mode", Modes[mode_]);
    cJSON_AddBoolToObject(o.get(), "busy", !active_.empty() || !queued_.empty());
    cJSON_AddBoolToObject(o.get(), "requires_operator_enable", !armed_);
    cJSON_AddBoolToObject(o.get(), "light_supported", false);
    auto* calibrations = cJSON_AddObjectToObject(o.get(), "calibrated_targets");
    for (int m = 0; m < 3; m++) {
        auto* targets = cJSON_AddArrayToObject(calibrations, Modes[m]);
        if (configuration_)
            for (int t = 0; t < 2; t++)
                if (configuration_->modes[m].targets[t] &&
                    configuration_->modes[m].targets[t]->scene_matches)
                    cJSON_AddItemToArray(targets, cJSON_CreateString(t ? "book" : "user"));
    }
    if (configuration_) {
        auto* pose = cJSON_AddObjectToObject(o.get(), "pose_rad");
        auto* mappings = Item(Item(configuration_->source.get(), "hardware"), "servos");
        const auto now = esp_timer_get_time();
        for (int j = 0; j < 5; j++) {
            const auto* s = cJSON_GetArrayItem(mappings, j);
            if (servos_[j].position_at && now - servos_[j].position_at < 1500000)
                cJSON_AddNumberToObject(pose, lamp::pose::kNames[j],
                                        (servos_[j].position - Item(s, "zero_ticks")->valuedouble) *
                                            Item(s, "direction")->valuedouble *
                                            Item(s, "degrees_per_tick")->valuedouble *
                                            std::numbers::pi / 180);
        }
    }
    bool torque = true, unloaded = true;
    const auto now = esp_timer_get_time();
    for (const auto& servo : servos_) {
        const bool fresh = servo.torque_at && now - servo.torque_at < 1500000;
        torque &= fresh && servo.torque == 1;
        unloaded &= fresh && servo.torque == 0;
    }
    cJSON_AddBoolToObject(o.get(), "torque_enabled_confirmed", torque);
    cJSON_AddBoolToObject(o.get(), "unloaded_confirmed", unloaded);
    auto* supported = cJSON_AddArrayToObject(o.get(), "unsupported_primitives");
    cJSON_AddItemToArray(supported, cJSON_CreateString("A10"));
    if (!active_.empty())
        cJSON_AddStringToObject(o.get(), "active_task", ("hardware-" + active_).c_str());
    if (!order_.empty()) {
        const auto& id = order_.back();
        CJsonUniquePtr receipt(cJSON_Parse(Receipt(id, records_.at(id)).c_str()));
        cJSON_AddItemToObject(o.get(), "last_task",
                              cJSON_Duplicate(Item(receipt.get(), "result"), true));
    }
    return lamp::Encode(o.get());
}
bool LampHardwareExecutor::Write(unsigned id, uint8_t command, std::initializer_list<uint8_t> p) {
    auto frame = lamp::bus::WriteRequest(id, command, std::span(p.begin(), p.size()));
    if (!frame)
        return false;
    return uart_write_bytes(UART_NUM_0, frame->data(), frame->size()) == int(frame->size()) &&
           uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100)) == ESP_OK;
}
bool LampHardwareExecutor::StopMotors() {
    bool ok = true;
    for (unsigned id = 1; id <= 5; id++)
        if (!Write(id, 12)) {
            ESP_LOGE("LampHW", "Stop write failed ID%u", id);
            ok = false;
        }
    return ok;
}
bool LampHardwareExecutor::ConfirmedRead(unsigned index, uint8_t command) {
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (ReadCancelled()) {
            io_error_ = "操作已被抢占";
            return false;
        }
        if (Read(index, command))
            return true;
        if (!attempt)
            vTaskDelay(pdMS_TO_TICKS(40));
    }
    io_error_ = "ID " + std::to_string(index + 1) + " CMD " + std::to_string(command) +
                " 读取失败（已重试）";
    return false;
}
bool LampHardwareExecutor::Positions(lamp::pose::Pose& q) {
    for (unsigned i = 0; i < 5; i++)
        if (!ConfirmedRead(i, 28))
            return false;
    std::lock_guard lock(mutex_);
    auto* mappings = Item(Item(configuration_->source.get(), "hardware"), "servos");
    for (int i = 0; i < 5; i++) {
        auto* s = cJSON_GetArrayItem(mappings, i);
        q[i] = (servos_[i].position - Item(s, "zero_ticks")->valuedouble) *
               Item(s, "degrees_per_tick")->valuedouble * Item(s, "direction")->valuedouble *
               std::numbers::pi / 180;
    }
    const double tolerance = 3 * .24 * std::numbers::pi / 180;
    for (unsigned i = 0; i < 5; i++) {
        const auto& joint = lamp::pose::kJoints[i];
        if (q[i] < joint.lower - tolerance || q[i] > joint.upper + tolerance) {
            io_error_ =
                "ID " + std::to_string(i + 1) +
                " 当前角度超出 URDF 限位，请校准安装偏置；actual_rad=" + std::to_string(q[i]) +
                ", lower_rad=" + std::to_string(joint.lower) +
                ", upper_rad=" + std::to_string(joint.upper) +
                ", feedback_tolerance_rad=" + std::to_string(tolerance);
            return false;
        }
        q[i] = std::clamp(q[i], joint.lower, joint.upper);
    }
    return true;
}
bool LampHardwareExecutor::Target(const lamp::pose::Pose& q, unsigned ms, uint32_t token) {
    auto ticks = lamp::pose::ToTicks(*configuration_, q);
    if (!ticks)
        return false;
    for (unsigned i = 0; i < 5; i++)
        if (!servos_[i].limits_at || (*ticks)[i] < servos_[i].limit_min ||
            (*ticks)[i] > servos_[i].limit_max)
            return false;
    for (unsigned i = 0; i < 5; i++)
        if (epoch_ != token || !Write(i + 1, 1, {Lo((*ticks)[i]), Hi((*ticks)[i]), Lo(ms), Hi(ms)}))
            return false;
    std::lock_guard lock(mutex_);
    last_target_ = q;
    return true;
}
bool LampHardwareExecutor::Torque(bool enabled, uint32_t token) {
    lamp::pose::Pose q;
    if (enabled) {
        for (unsigned i = 0; i < 5; i++)
            if (!ConfirmedRead(i, 21))
                return false;
        if (!StopMotors())
            return false;
        if (!Positions(q) || epoch_ != token)
            return false;
        for (unsigned i = 0; i < 5; i++) {
            if (epoch_ != token)
                return false;
            if (!ConfirmedRead(i, 30))
                return false;
            if (servos_[i].mode != 0 &&
                (!Write(i + 1, 29, {0, 0, 0, 0}) || !ConfirmedRead(i, 30) || servos_[i].mode != 0))
                return false;
        }
        if (!Positions(q) || epoch_ != token)
            return false;
        for (unsigned i = 0; i < 5; i++) {
            const int tick = servos_[i].position;
            if (tick < 0 || tick > 1000 || tick < servos_[i].limit_min ||
                tick > servos_[i].limit_max) {
                io_error_ = "ID " + std::to_string(i + 1) + " 当前 tick 超出设备限位";
                return false;
            }
        }
        // Align the exact raw position, never a clamped planning angle, before enabling.
        for (unsigned i = 0; i < 5; i++)
            if (epoch_ != token ||
                !Write(i + 1, 1,
                       {Lo(servos_[i].position), Hi(servos_[i].position), Lo(500), Hi(500)})) {
                io_error_ = "ID " + std::to_string(i + 1) + " 保持当前位置发送失败";
                return false;
            }
        {
            std::lock_guard lock(mutex_);
            last_target_ = q;
        }
    }
    bool ok = true;
    std::string first_error;
    for (unsigned i = 0; i < 5; i++) {
        if (epoch_ != token)
            return false;
        const bool wrote = Write(i + 1, 31, {static_cast<uint8_t>(enabled)});
        const bool read = ConfirmedRead(i, 32);
        if (!wrote || !read || servos_[i].torque != int(enabled)) {
            ESP_LOGE("LampHW", "Torque confirmation failed ID%u", i + 1);
            if (first_error.empty())
                first_error = io_error_.empty()
                                  ? "ID " + std::to_string(i + 1) +
                                        " 扭矩确认失败，实际=" + std::to_string(servos_[i].torque)
                                  : io_error_;
            ok = false;
        }
    }
    std::lock_guard lock(mutex_);
    armed_ = enabled && ok;
    if (!ok)
        io_error_ = first_error;
    return ok;
}
bool LampHardwareExecutor::Play(const lamp::pose::Plan& plan, uint32_t token) {
    const auto start = esp_timer_get_time();
    int64_t last_feedback = start;
    lamp::pose::Pose actual;
    unsigned telemetry = 0;
    std::array<unsigned, 10> missed{};
    while (true) {
        if (epoch_ != token)
            return false;
        const double elapsed = (esp_timer_get_time() - start) / 1e6;
        const auto target = plan.At(std::min(elapsed + .04, plan.Duration()));
        if (!Target(target, 120, token))
            return false;
        if (esp_timer_get_time() - last_feedback >= 120000) {
            // One bounded read between target frames, instead of ten blocking round trips.
            const unsigned slot = telemetry++ % 10, i = slot / 2;
            const uint8_t command = slot % 2 ? 32 : 28;
            if (!Read(i, command, 40)) {
                if (++missed[slot] >= 2) {
                    io_error_ = "ID " + std::to_string(i + 1) + " 运动反馈持续读取失败";
                    return false;
                }
            } else {
                missed[slot] = 0;
                if (command == 32 && servos_[i].torque != 1) {
                    io_error_ = "ID " + std::to_string(i + 1) + " 运动时扭矩已关闭";
                    return false;
                }
            }
            last_feedback = esp_timer_get_time();
        }
        if (elapsed >= plan.Duration())
            break;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    const auto end = plan.At(plan.Duration());
    const auto ticks = lamp::pose::ToTicks(*configuration_, end);
    // 10 ticks = 2.4 degrees: allow loaded-servo tracking error, not arbitrary poses.
    constexpr int tolerance_ticks = 10;
    int64_t settled_since = 0;
    const auto deadline = esp_timer_get_time() + 5000000;
    while (esp_timer_get_time() < deadline) {
        if (epoch_ != token || !Positions(actual))
            return false;
        for (unsigned i = 0; i < 5; i++)
            if (!ConfirmedRead(i, 32) || servos_[i].torque != 1) {
                io_error_ = "ID " + std::to_string(i + 1) + " endpoint torque feedback failed";
                return false;
            }
        bool at = true;
        for (int j = 0; j < 5; j++)
            at &= std::abs(servos_[j].position - (*ticks)[j]) <= tolerance_ticks;
        if (at) {
            if (!settled_since)
                settled_since = esp_timer_get_time();
            if (esp_timer_get_time() - settled_since >= 200000)
                return true;
        } else
            settled_since = 0;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    endpoint_timeout_ = true;
    io_error_ = "Endpoint timeout (5 seconds, tolerance 10 ticks); residual ticks:";
    for (unsigned i = 0; i < 5; i++)
        io_error_ +=
            " ID" + std::to_string(i + 1) + "=" + std::to_string(servos_[i].position - (*ticks)[i]);
    return false;
}
void LampHardwareExecutor::RecordDiagnostic(const std::string& kind, const std::string& name,
                                            const std::string& data) {
    std::lock_guard lock(diagnostic_mutex_);
    CJsonUniquePtr event(cJSON_CreateObject());
    cJSON_AddNumberToObject(event.get(), "sequence", ++diagnostic_sequence_);
    cJSON_AddNumberToObject(event.get(), "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddStringToObject(event.get(), "kind", kind.c_str());
    cJSON_AddStringToObject(event.get(), "name", name.substr(0, 128).c_str());
    size_t length = std::min(data.size(), size_t(4096));
    while (length < data.size() && length > 0 &&
           (static_cast<unsigned char>(data[length]) & 0xc0) == 0x80)
        --length;
    cJSON_AddBoolToObject(event.get(), "truncated", length < data.size());
    cJSON_AddStringToObject(event.get(), "data", data.substr(0, length).c_str());
    auto json = lamp::Encode(event.get());
    diagnostic_bytes_ += json.size();
    diagnostics_.push_back({diagnostic_sequence_, std::move(json)});
    while (diagnostics_.size() > 128 || diagnostic_bytes_ > 65536) {
        diagnostic_bytes_ -= diagnostics_.front().json.size();
        diagnostics_.pop_front();
    }
}
std::string LampHardwareExecutor::Diagnostics(uint64_t after) {
    std::lock_guard lock(diagnostic_mutex_);
    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddBoolToObject(root.get(), "ok", true);
    cJSON_AddBoolToObject(root.get(), "gap",
                          !diagnostics_.empty() && after + 1 < diagnostics_.front().sequence);
    auto* events = cJSON_AddArrayToObject(root.get(), "events");
    uint64_t cursor = after;
    unsigned count = 0;
    for (const auto& event : diagnostics_) {
        if (event.sequence <= after)
            continue;
        cJSON_AddItemToArray(events, cJSON_Parse(event.json.c_str()));
        cursor = event.sequence;
        if (++count == 8)
            break;
    }
    cJSON_AddNumberToObject(root.get(), "cursor", cursor);
    cJSON_AddNumberToObject(root.get(), "latest", diagnostic_sequence_);
    return lamp::Encode(root.get());
}
void LampHardwareExecutor::Finish(const std::string& id, const std::string& status,
                                  const std::string& reason) {
    std::lock_guard lock(mutex_);
    auto& r = records_.at(id);
    r.status = status;
    r.reason = reason;
    active_.clear();
    RecordDiagnostic("task", id, Receipt(id, r));
}
bool LampHardwareExecutor::RecordCalibration(const std::string& target, uint32_t token) {
    lamp::pose::Pose q;
    if (!Positions(q) || epoch_ != token)
        return false;
    for (unsigned i = 0; i < 5; i++)
        if (!Read(i, 32) || servos_[i].torque != 0)
            return false;
    int mode;
    {
        std::lock_guard lock(mutex_);
        mode = mode_;
    }
    if ((mode == 0 && target != "user") || (mode == 2 && target != "book") ||
        !lamp::pose::InMode(q, configuration_->modes[mode]) ||
        !lamp::pose::Safe(q, configuration_->obstacles))
        return false;
    CJsonUniquePtr candidate(cJSON_Duplicate(configuration_->source.get(), true));
    cJSON_DeleteItemFromObjectCaseSensitive(candidate.get(), "runtime_mode");
    cJSON_AddNumberToObject(candidate.get(), "runtime_mode", mode);
    auto* profile =
        const_cast<cJSON*>(Item(Item(Item(candidate.get(), "profiles"), "modes"), Modes[mode]));
    auto* targets = const_cast<cJSON*>(Item(profile, "targets"));
    cJSON_DeleteItemFromObjectCaseSensitive(targets, target.c_str());
    auto* aim = cJSON_AddObjectToObject(targets, target.c_str());
    auto* pose = cJSON_AddObjectToObject(aim, "pose");
    for (int j = 0; j < 5; j++)
        cJSON_AddNumberToObject(pose, lamp::pose::kNames[j], q[j]);
    cJSON_AddNumberToObject(aim, "yaw", q[0] - configuration_->modes[mode].home[0]);
    cJSON_AddNumberToObject(aim, "pitch", q[3] - configuration_->modes[mode].home[3]);
    cJSON_AddStringToObject(aim, "updatedAt",
                            ("uptime:" + std::to_string(esp_timer_get_time() / 1000000)).c_str());
    auto data = lamp::Encode(candidate.get());
    auto parsed = lamp::pose::ParseConfiguration(data);
    if (!parsed || epoch_ != token)
        return false;
    nvs_handle_t h;
    if (nvs_open("lamp_native", NVS_READWRITE, &h) != ESP_OK)
        return false;
    auto payload = TeachingPayload(candidate.get(), mode);
    const auto compact = lamp::Encode(payload.get());
    if (compact.size() > 2048 || lamp::Text(payload.get(), "binding").empty()) {
        nvs_close(h);
        return false;
    }
    auto result = nvs_set_blob(h, "device_v1", compact.data(), compact.size());
    if (result == ESP_OK)
        result = nvs_commit(h);
    nvs_close(h);
    if (result != ESP_OK)
        return false;
    std::lock_guard lock(mutex_);
    configuration_ = std::move(*parsed);
    armed_ = false;
    return true;
}
void LampHardwareExecutor::Execute(const std::string& body, uint32_t token) {
    io_error_.clear();
    endpoint_timeout_ = false;
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    const auto id = lamp::Text(root.get(), "request_id"), op = lamp::Text(root.get(), "operation");
    const auto* args = Item(root.get(), "args");
    {
        std::lock_guard lock(mutex_);
        records_.at(id).status = "running";
    }
    auto failure = [&](const std::string& why) {
        const bool stopped = StopMotors();
        {
            std::lock_guard lock(mutex_);
            if (epoch_ == token && (!endpoint_timeout_ || !stopped))
                armed_ = false;
        }
        Finish(id, epoch_ == token ? "failed" : "cancelled", why);
    };
    auto reject = [&](const std::string& why) { Finish(id, "failed", why); };
    if (op == "cancel") {
        const bool stopped = StopMotors();
        if (!stopped) {
            std::lock_guard lock(mutex_);
            armed_ = false;
        }
        Finish(id, stopped ? "completed" : "failed",
               stopped ? "Stop commands sent; old task cancelled"
                       : "Stop transport write failed; physical stop not confirmed");
        return;
    }
    if (op == "set_torque") {
        if (!Torque(cJSON_IsTrue(Item(args, "enabled")), token)) {
            failure(io_error_.empty() ? "扭矩/模式确认失败" : io_error_);
            return;
        }
        Finish(id, "completed", "Torque state confirmed on all five IDs");
        return;
    }
    if (op == "install") {
        if (!SaveInstallation(Item(args, "mapping"))) {
            failure(
                "Installation save requires fresh unloaded feedback, valid mapping and NVS commit");
            return;
        }
        Finish(id, "completed",
               "Installation mapping saved; previous target teaching invalidated; motion disarmed");
        return;
    }
    if (op == "record_calibration") {
        if (!RecordCalibration(lamp::Text(args, "target_id"), token)) {
            failure(
                "Calibration requires unloaded fresh pose, allowed mode, valid geometry and "
                "successful NVS commit");
            return;
        }
        Finish(id, "completed", "Current measured pose persisted in NVS");
        return;
    }
    if (op == "end_lighting") {
        Finish(id, "failed", "No physical light driver; no simulated success");
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (!armed_) {
            records_.at(id).status = "failed";
            records_.at(id).reason =
                "运动未授权：使能未完成，或故障/卸力/保存映射/"
                "退出手动控制已关闭授权；请检查后重新使能";
            active_.clear();
            RecordDiagnostic("task", id, Receipt(id, records_.at(id)));
            return;
        }
    }
    lamp::pose::Pose current;
    if (!Positions(current)) {
        failure(io_error_.empty() ? "当前位置读取失败" : io_error_);
        return;
    }
    for (unsigned i = 0; i < 5; i++)
        if (!ConfirmedRead(i, 30) || servos_[i].mode != 0 || !ConfirmedRead(i, 32) ||
            servos_[i].torque != 1) {
            failure("Five servos must be in position mode with torque enabled");
            return;
        }
    for (unsigned i = 0; i < 5; i++)
        if (!ConfirmedRead(i, 21)) {
            reject(io_error_.empty() ? "Cannot read servo internal position limits; no motion sent"
                                     : io_error_);
            return;
        }
    if (op == "move") {
        auto target = lamp::pose::ParsePose(Item(args, "pose_rad"));
        lamp::pose::Plan plan;
        if (!target || !lamp::pose::ToTicks(*configuration_, *target)) {
            reject("Invalid manual target or hardware range");
            return;
        }
        for (unsigned n = 0; n <= 100; n++) {
            auto q = lamp::pose::Blend(current, *target, double(n) / 100);
            auto ticks = lamp::pose::ToTicks(*configuration_, q);
            bool valid = ticks.has_value() && lamp::pose::Safe(q, configuration_->obstacles);
            if (ticks)
                for (unsigned i = 0; i < 5; i++)
                    valid &=
                        (*ticks)[i] >= servos_[i].limit_min && (*ticks)[i] <= servos_[i].limit_max;
            if (!valid) {
                reject("Manual path exceeds geometry or position limits");
                return;
            }
        }
        plan.ok = true;
        plan.entry = current;
        lamp::pose::Phase phase;
        phase.from = current;
        phase.to = *target;
        phase.duration = Item(args, "duration_sec")->valuedouble;
        plan.phases.push_back(phase);
        if (!Play(plan, token)) {
            failure(io_error_.empty() ? "Manual movement interrupted or endpoint not confirmed"
                                      : io_error_);
            return;
        }
        Finish(id, "completed", "Manual target confirmed from actual feedback");
        return;
    }
    CJsonUniquePtr steps(cJSON_CreateArray());
    if (op == "rest") {
        auto* s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "action_id", "set_mode");
        cJSON_AddStringToObject(s, "mode", "sit");
        cJSON_AddItemToArray(steps.get(), s);
    } else if (op == "execute_group")
        steps.reset(cJSON_Duplicate(Item(args, "steps"), true));
    else
        cJSON_AddItemToArray(steps.get(), cJSON_Duplicate(args, true));
    std::vector<lamp::pose::Plan> plans;
    int mode;
    {
        std::lock_guard lock(mutex_);
        mode = mode_;
    }
    double duration = 0;
    bool limited = false;
    for (const auto* s = steps->child; s; s = s->next) {
        CJsonUniquePtr semantic(cJSON_Duplicate(s, true));
        cJSON_DeleteItemFromObjectCaseSensitive(semantic.get(), "context");
        auto plan = lamp::pose::Build(*configuration_, mode, current, semantic.get(),
                                      Item(args, "context") != nullptr);
        if (lamp::Text(semantic.get(), "skill_id") == "A16" && plan.requires_light) {
            plan.requires_light = false;
            plan.limited = true;
        }
        if (!plan.ok || !std::isfinite(plan.Duration()) || plan.requires_light) {
            reject(plan.requires_light ? "This preset requires a physical light driver"
                                       : plan.error);
            return;
        }
        for (unsigned n = 0; n <= 120; n++) {
            auto ticks = lamp::pose::ToTicks(*configuration_, plan.At(plan.Duration() * n / 120));
            bool reachable = ticks.has_value();
            if (ticks)
                for (unsigned j = 0; j < 5; j++)
                    reachable &=
                        (*ticks)[j] >= servos_[j].limit_min && (*ticks)[j] <= servos_[j].limit_max;
            if (!reachable) {
                reject("Group preflight exceeds installed servo or device limits");
                return;
            }
        }
        duration += plan.Duration();
        if (duration > 60) {
            reject("Group exceeds 60 seconds");
            return;
        }
        current = plan.At(plan.Duration());
        mode = plan.mode_after;
        limited |= plan.limited;
        plans.push_back(std::move(plan));
    }
    {
        std::lock_guard lock(mutex_);
        records_.at(id).total = plans.size();
    }
    for (unsigned i = 0; i < plans.size(); i++) {
        if (epoch_ != token || !Play(plans[i], token)) {
            failure(io_error_.empty() ? "Motion interrupted or feedback unavailable" : io_error_);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            mode_ = plans[i].mode_after;
            records_.at(id).step = i + 1;
        }
    }
    if (op == "rest") {
        if (limited || !Torque(false, token)) {
            failure("Rest did not complete unloading; chat remains open");
            return;
        }
        {
            std::lock_guard lock(mutex_);
            rest_epoch_ = token;
            rest_at_ = esp_timer_get_time();
        }
    }
    Finish(id, limited ? "limited" : "completed",
           limited ? "Reached available trajectory; amplitude may be limited; A16 light omitted "
                     "because no driver"
                   : "Final measured positions confirmed");
}
void LampHardwareExecutor::RunHardware(void* self) {
    static_cast<LampHardwareExecutor*>(self)->HardwareLoop();
}
void LampHardwareExecutor::CloseRestChat() {
    uint32_t token;
    {
        std::lock_guard lock(mutex_);
        if (!rest_at_ || armed_ || epoch_ != rest_epoch_ ||
            esp_timer_get_time() - rest_at_ < 2000000)
            return;
        token = rest_epoch_;
    }
    Application::GetInstance().Schedule([this, token] {
        std::lock_guard lock(mutex_);
        if (epoch_ != token || armed_ || !rest_at_)
            return;
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateIdle)
            rest_at_ = 0;
        else if (app.GetDeviceState() == kDeviceStateListening && !app.IsVoiceDetected()) {
            rest_at_ = 0;
            app.ToggleChatState();
        }
    });
}
void LampHardwareExecutor::HardwareLoop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
    unsigned telemetry = 0;
    while (true) {
        std::string body;
        uint32_t token;
        {
            std::lock_guard lock(mutex_);
            body = std::move(queued_);
            queued_.clear();
            token = epoch_;
            io_token_ = token;
            if (!body.empty()) {
                CJsonUniquePtr root(cJSON_Parse(body.c_str()));
                active_ = lamp::Text(root.get(), "request_id");
            }
        }
        if (!body.empty()) {
            Execute(body, token);
            continue;
        }
        for (unsigned i = 0; i < 5; i++) {
            Read(i, 28);
            Read(i, 32);
        }
        const uint8_t cmds[] = {26, 27, 30, 21};
        Read((telemetry / 4) % 5, cmds[telemetry % 4]);
        telemetry++;
#if !CONFIG_APPLE_LAMP_EDGE_RUNTIME
        CloseRestChat();  // Edge Runtime closes only after confirmed rest and farewell playback.
#endif
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}
#endif
