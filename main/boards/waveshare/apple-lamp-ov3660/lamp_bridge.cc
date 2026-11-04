#include "lamp_bridge.h"

#include "application.h"
#include "board.h"
#include "lamp_protocol.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>

#if __has_include("lamp_bridge_local.h")
#include "lamp_bridge_local.h"
#endif
#ifndef APPLE_LAMP_BRIDGE_URL
#define APPLE_LAMP_BRIDGE_URL ""
#endif
#ifndef APPLE_LAMP_BRIDGE_TOKEN
#define APPLE_LAMP_BRIDGE_TOKEN ""
#endif

namespace {
constexpr size_t kMaxResponse = 65536;
constexpr size_t kQueueBytes = 32768;
constexpr int64_t kFreshUs = 6000000;
constexpr int64_t kRequestUs = 20000000;
const char* kTag = "LampBridge";
std::string Failure(const char* code, const std::string& message) {
    CJsonUniquePtr out(cJSON_CreateObject());
    cJSON_AddBoolToObject(out.get(), "ok", false);
    auto* error = cJSON_AddObjectToObject(out.get(), "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message.c_str());
    return lamp::Encode(out.get());
}
bool TerminalDelivery(const std::string& value) {
    return value == "answered" || value == "unknown" || value == "expired" || value == "failed" ||
           value == "cancelled_before_dispatch";
}
bool IsQuery(const std::string& operation) {
    return operation == "get_state" || operation == "get_task" || operation == "discover";
}
bool IsControl(const std::string& operation) {
    return operation == "cancel" || operation == "end_lighting" || operation == "set_torque" ||
           operation == "rest";
}
std::string TorqueCommand(const std::string& id, bool enabled) {
    CJsonUniquePtr command(cJSON_Parse(lamp::Query("set_torque", id).c_str()));
    cJSON_AddBoolToObject(cJSON_GetObjectItemCaseSensitive(command.get(), "args"), "enabled",
                          enabled);
    return lamp::Encode(command.get());
}
// Same semantic step envelope run_sequence accepts, with no options, for the
// posture actions that deserve a dedicated tool.
std::string ActionCommand(const std::string& id, const char* action_id) {
    CJsonUniquePtr command(cJSON_Parse(lamp::Query("execute", id).c_str()));
    cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(command.get(), "args"), "action_id",
                            action_id);
    return lamp::Encode(command.get());
}
bool ValidUrl(std::string url) {
    if (url.empty())
        return true;  // Empty disables the executor.
    size_t prefix = url.starts_with("http://") ? 7 : url.starts_with("https://") ? 8 : 0;
    if (!prefix || url.size() > 240)
        return false;
    if (url.ends_with('/'))
        url.pop_back();
    auto authority = url.substr(prefix);
    return !authority.empty() && authority.find_first_of("/@?#\\ \r\n\t") == std::string::npos &&
           !authority.starts_with("127.") && !authority.starts_with("localhost") &&
           !authority.starts_with("[::1]");
}
bool ValidToken(const std::string& token) {
    return token.size() <= 256 && std::none_of(token.begin(), token.end(),
                                               [](unsigned char c) { return c < 32 || c == 127; });
}
}  // namespace

LampBridgeExecutor::LampBridgeExecutor(Board& board) : board_(board) {
    Settings settings("lamp_bridge");
    url_ = settings.GetString("url", APPLE_LAMP_BRIDGE_URL);
    token_ = settings.GetString("token", APPLE_LAMP_BRIDGE_TOKEN);
    if (!ValidUrl(url_) || !ValidToken(token_)) {
        url_.clear();
        token_.clear();
    }
    while (url_.ends_with('/'))
        url_.pop_back();
    counter_ = esp_random();
    if (xTaskCreate(Run, "lamp_bridge", 8192, this, 1, &worker_) != pdPASS) {
        worker_ = nullptr;
        transport_error_ = "Bridge worker allocation failed";
    }
}
std::string LampBridgeExecutor::NewRequestId() {
    std::lock_guard lock(mutex_);
    char id[48];
    std::snprintf(id, sizeof(id), "esp-%08lx-%08lx", static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(++counter_));
    return id;
}
bool LampBridgeExecutor::Ready() {
    std::lock_guard lock(mutex_);
    return ready_ && esp_timer_get_time() - sampled_at_ < kFreshUs;
}
std::string LampBridgeExecutor::Configure(const std::string& url, const std::string& token) {
    if (!ValidUrl(url) || !ValidToken(token))
        return Failure("INVALID_CONFIGURATION",
                       "Use a LAN HTTP root URL and a token without control characters");
    Settings settings("lamp_bridge", true);
    settings.SetString("url", url);
    settings.SetString("token", token);
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        url_ = url;
        token_ = token;
        while (url_.ends_with('/'))
            url_.pop_back();
        ready_ = false;
        state_.clear();
        sampled_at_ = 0;
        for (auto& [id, record] : records_)
            if (!TerminalDelivery(record.delivery)) {
                record.delivery = record.posted ? "unknown" : "expired";
                record.error = "Bridge configuration changed; no automatic replay";
            }
        queue_.clear();
        rest_request_.clear();
        ++rest_generation_;
    }
    if (worker_)
        xTaskNotifyGive(worker_);
    return "{\"ok\":true,\"configuration_saved\":true,\"token_redacted\":true}";
}
std::string LampBridgeExecutor::Receipt(const std::string& id, const Record& record) {
    CJsonUniquePtr out(cJSON_CreateObject());
    cJSON_AddStringToObject(out.get(), "protocol_version", lamp::kProtocol);
    cJSON_AddStringToObject(out.get(), "bridge_request_id", id.c_str());
    cJSON_AddStringToObject(out.get(), "executor", "web_sim");
    cJSON_AddStringToObject(out.get(), "completion_basis", "simulation");
    cJSON_AddStringToObject(out.get(), "delivery", record.delivery.c_str());
    cJSON_AddBoolToObject(out.get(), "motion_completed", false);
    if (!record.response.empty()) {
        auto* response = cJSON_Parse(record.response.c_str());
        if (response) {
            auto* result = cJSON_GetObjectItemCaseSensitive(response, "result");
            auto status = lamp::Text(result, "status");
            cJSON_ReplaceItemInObjectCaseSensitive(
                out.get(), "motion_completed",
                cJSON_CreateBool(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ok")) &&
                                 (status == "completed" || status == "limited")));
            cJSON_AddItemToObject(out.get(), "response", response);
        }
    }
    if (!record.error.empty())
        cJSON_AddStringToObject(out.get(), "transport_error", record.error.c_str());
    cJSON_AddStringToObject(out.get(), "poll_tool", "self.lamp.get_state");
    return lamp::Encode(out.get());
}
std::string LampBridgeExecutor::Submit(const std::string& command) {
    auto checked = lamp::Command(command);
    if (!checked)
        return Failure("INVALID_REQUEST", checked.error());
    CJsonUniquePtr root(cJSON_Parse(checked->c_str()));
    const auto id = lamp::Text(root.get(), "request_id"),
               operation = lamp::Text(root.get(), "operation");
    std::lock_guard lock(mutex_);
    if (auto old = records_.find(id); old != records_.end()) {
        CJsonUniquePtr prior(cJSON_Parse(old->second.body.c_str()));
        if (!cJSON_Compare(prior.get(), root.get(), true))
            return Failure("REQUEST_ID_CONFLICT",
                           "request_id was already used for another command");
        return Receipt(id, old->second);
    }
    if (!worker_ || url_.empty())
        return Failure("EXECUTOR_UNCONFIGURED", "Configure the computer LAN bridge URL first");
    if (!ready_ || esp_timer_get_time() - sampled_at_ >= kFreshUs)
        return Failure("EXECUTOR_UNAVAILABLE",
                       "Simulator is offline or state is stale; do not retry motion automatically");
    // Query receipts are transient; motion receipts are retained for this ESP boot session.
    size_t motions = 0, pending_bytes = 0, controls = 0;
    for (auto it = records_.begin(); it != records_.end();) {
        if (!IsQuery(it->second.operation) && !IsControl(it->second.operation))
            ++motions;
        if (IsControl(it->second.operation))
            ++controls;
        if (!TerminalDelivery(it->second.delivery))
            pending_bytes += it->second.body.size();
        if (records_.size() >= 80 && IsQuery(it->second.operation) &&
            TerminalDelivery(it->second.delivery))
            it = records_.erase(it);
        else
            ++it;
    }
    if (!IsQuery(operation) && !IsControl(operation) && motions >= 64)
        return Failure(
            "SESSION_CAPACITY",
            "ESP motion journal reached 64 entries; finish tasks before a new device session");
    // Bounded control history is separate: reaching the motion limit never disables stop.
    if (controls >= 16) {
        for (auto it = records_.begin(); it != records_.end() && controls >= 16;) {
            if (IsControl(it->second.operation) && TerminalDelivery(it->second.delivery)) {
                it = records_.erase(it);
                --controls;
            } else
                ++it;
        }
    }
    // Reserve a control slot: stop must remain admissible when the ordinary queue is full.
    if ((!IsControl(operation) &&
         (queue_.size() >= 4 || pending_bytes + checked->size() > kQueueBytes)) ||
        (IsControl(operation) && queue_.size() >= 8))
        return Failure("BUSY", "Bridge command queue is full; do not enqueue dialogue retries");
    Record record;
    record.body = *checked;
    record.operation = operation;
    record.deadline = esp_timer_get_time() + kRequestUs;
    record.server_session = session_;
    if (IsControl(operation)) {
        auto* args = cJSON_GetObjectItemCaseSensitive(root.get(), "args");
        const bool release = operation == "set_torque" &&
                             cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "enabled"));
        if ((operation == "cancel" && !args->child) || operation == "rest" || release) {
            for (auto it = queue_.begin(); it != queue_.end();) {
                auto& queued = records_.at(*it);
                if (queued.operation == "execute" || queued.operation == "execute_group" ||
                    queued.operation == "rest" || queued.operation == "set_torque") {
                    queued.delivery = "cancelled_before_dispatch";
                    it = queue_.erase(it);
                } else
                    ++it;
            }
        }
        if (operation == "set_torque" && !release)
            queue_.push_back(id);
        else
            queue_.push_front(id);
    } else
        queue_.push_back(id);
    records_.emplace(id, std::move(record));
    if (operation == "rest" || operation == "set_torque" || operation == "cancel" ||
        operation == "execute" || operation == "execute_group") {
        rest_request_ = operation == "rest" ? id : "";
        rest_completed_at_ = 0;
        ++rest_generation_;
    }
    xTaskNotifyGive(worker_);
    return Receipt(id, records_.at(id));
}
std::string LampBridgeExecutor::Inspect(const std::string& id) {
    if (!lamp::ValidId(id))
        return Failure("INVALID_REQUEST", "Invalid bridge_request_id");
    std::lock_guard lock(mutex_);
    auto it = records_.find(id);
    // Reconciliation reads only. An uncertain POST must never be repeated by the worker.
    if (it != records_.end() && it->second.delivery == "unknown" && ready_ &&
        it->second.server_session == session_) {
        it->second.delivery = "dispatched";
        it->second.error = "Reconciling uncertain receipt; no motion resubmission";
        it->second.deadline = esp_timer_get_time() + kRequestUs;
        it->second.posted = true;
        if (worker_)
            xTaskNotifyGive(worker_);
    }
    return it == records_.end()
               ? Failure("REQUEST_NOT_FOUND", "Receipt is not retained on this device")
               : Receipt(id, it->second);
}
std::string LampBridgeExecutor::State() {
    std::lock_guard lock(mutex_);
    CJsonUniquePtr out(cJSON_CreateObject());
    cJSON_AddStringToObject(out.get(), "executor", "web_sim");
    cJSON_AddStringToObject(out.get(), "completion_basis", "simulation");
    cJSON_AddBoolToObject(out.get(), "configured", !url_.empty());
    cJSON_AddBoolToObject(out.get(), "available",
                          ready_ && esp_timer_get_time() - sampled_at_ < kFreshUs);
    cJSON_AddStringToObject(out.get(), "server_session_id", session_.c_str());
    if (!transport_error_.empty())
        cJSON_AddStringToObject(out.get(), "transport_error", transport_error_.c_str());
    if (!state_.empty())
        cJSON_AddItemToObject(out.get(), "snapshot", cJSON_Parse(state_.c_str()));
    return lamp::Encode(out.get());
}
std::string LampBridgeExecutor::HttpRequest(const std::string& method, const std::string& path,
                                            const std::string& body, int& status,
                                            uint32_t& generation) {
    std::string url, token;
    {
        std::lock_guard lock(mutex_);
        url = url_;
        token = token_;
        generation = generation_;
    }
    status = 0;
    if (url.empty())
        return {};
    // The upstream EspTcp may destroy its callback owner before its receive task
    // has exited on passive close. Keep this board's HTTP entirely synchronous in
    // the bridge worker; MQTT/audio continue to use the existing network stack.
    struct Response {
        std::string data;
        bool oversized = false;
    } response;
    const std::string request_url = url + path;
    esp_http_client_config_t config = {};
    config.url = request_url.c_str();
    config.timeout_ms = 1500;
    config.disable_auto_redirect = true;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    config.user_data = &response;
    config.event_handler = [](esp_http_client_event_t* event) -> esp_err_t {
        if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
            return ESP_OK;
        auto* out = static_cast<Response*>(event->user_data);
        if (out->data.size() + static_cast<size_t>(event->data_len) > kMaxResponse) {
            out->oversized = true;
            return ESP_FAIL;
        }
        out->data.append(static_cast<const char*>(event->data), event->data_len);
        return ESP_OK;
    };
    auto http = esp_http_client_init(&config);
    if (!http)
        return {};
    esp_err_t error =
        esp_http_client_set_method(http, method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (error == ESP_OK)
        error = esp_http_client_set_header(http, "Content-Type", "application/json");
    const std::string authorization = "Bearer " + token;
    if (error == ESP_OK && !token.empty())
        error = esp_http_client_set_header(http, "Authorization", authorization.c_str());
    if (error == ESP_OK && method == "POST")
        error = esp_http_client_set_post_field(http, body.data(), body.size());
    if (error == ESP_OK)
        error = esp_http_client_perform(http);
    if (error == ESP_OK && !response.oversized)
        status = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);
    if (error != ESP_OK || response.oversized)
        return {};
    return response.data;
}
void LampBridgeExecutor::Refresh() {
    int status;
    uint32_t generation;
    auto body = HttpRequest("GET", "/v1/state", "", status, generation);
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    std::lock_guard lock(mutex_);
    if (generation != generation_)
        return;
    const bool was_ready = ready_;
    if (status != 200 || !root ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root.get(), "stale"))) {
        ready_ = false;
        transport_error_ =
            status == 401 ? "Bridge authentication failed" : "Bridge state unavailable";
        return;
    }
    const auto session = lamp::Text(root.get(), "server_session_id");
    if (session.empty()) {
        ready_ = false;
        transport_error_ = "Invalid bridge session";
        return;
    }
    if (!session_.empty() && session_ != session) {
        for (auto& [id, record] : records_)
            if (!TerminalDelivery(record.delivery)) {
                record.delivery = record.posted ? "unknown" : "expired";
                record.error = "Bridge restarted; no automatic replay";
            }
        queue_.clear();
        rest_request_.clear();
        ++rest_generation_;
    }
    session_ = session;
    sampled_at_ = esp_timer_get_time();
    ready_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(), "simulator_connected")) &&
             cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root.get(), "stale"));
    transport_error_ = ready_ ? "" : "Simulator disconnected or snapshot stale";
    auto* snapshot = cJSON_GetObjectItemCaseSensitive(root.get(), "snapshot");
    auto* state = cJSON_GetObjectItemCaseSensitive(snapshot, "state");
    if (state && lamp::Text(state, "protocol_version") == lamp::kProtocol &&
        lamp::Text(state, "completion_basis") == "simulation") {
        CJsonUniquePtr wrapper(cJSON_CreateObject());
        cJSON_AddItemToObject(wrapper.get(), "result", cJSON_Duplicate(state, true));
        state_ = lamp::Summarize(wrapper.get());
    } else {
        ready_ = false;
        state_.clear();
    }
    if (ready_ && !was_ready)
        ESP_LOGI(kTag, "Simulator available; authenticated state received (HTTP %d)", status);
    // Only this device's completed rest may close its chat. Never close on receipt acceptance,
    // foreign browser rest, or a stale state; never interrupt farewell speech.
    auto* rest = cJSON_GetObjectItemCaseSensitive(state, "rest_task");
    if (ready_ && !rest_request_.empty() && lamp::Text(rest, "request_id") == rest_request_) {
        const auto rest_status = lamp::Text(rest, "status");
        if (rest_status == "failed" || rest_status == "cancelled" || rest_status == "limited") {
            rest_request_.clear();
        } else if (rest_status == "completed" &&
                   cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(state, "torque_enabled"))) {
            if (!rest_completed_at_)
                rest_completed_at_ = esp_timer_get_time();
            if (esp_timer_get_time() - rest_completed_at_ >= 2000000) {
                const uint32_t rest_generation = rest_generation_;
                Application::GetInstance().Schedule([this, rest_generation]() {
                    std::lock_guard lock(mutex_);
                    if (rest_generation != rest_generation_ || rest_request_.empty() || !ready_ ||
                        esp_timer_get_time() - sampled_at_ >= kFreshUs)
                        return;
                    auto& app = Application::GetInstance();
                    if (app.GetDeviceState() == kDeviceStateIdle)
                        rest_request_.clear();
                    else if (app.GetDeviceState() == kDeviceStateListening &&
                             !app.IsVoiceDetected()) {
                        rest_request_.clear();
                        app.ToggleChatState();
                        ESP_LOGI(kTag, "Rest complete; closing chat, wake word remains enabled");
                    }
                });
            }
        }
    }
}
void LampBridgeExecutor::Process(const std::string& id, bool post) {
    std::string body;
    {
        std::lock_guard lock(mutex_);
        auto it = records_.find(id);
        if (it == records_.end())
            return;
        body = post ? it->second.body : "";
        if (post)
            it->second.posted = true;
    }
    int status;
    uint32_t generation;
    auto response = HttpRequest(post ? "POST" : "GET",
                                post ? "/v1/commands" : "/v1/requests/" + lamp::UrlEncode(id), body,
                                status, generation);
    CJsonUniquePtr root(cJSON_Parse(response.c_str()));
    std::lock_guard lock(mutex_);
    auto it = records_.find(id);
    if (generation != generation_ || it == records_.end() || TerminalDelivery(it->second.delivery))
        return;
    auto& record = it->second;
    if (status != 200 && status != 202) {
        record.delivery = status == 0 || (!post && status == 404) ? "unknown" : "failed";
        record.error = status == 0
                           ? "Transport outcome unknown; query before issuing any new motion"
                           : "Bridge HTTP error " + std::to_string(status);
        if (root)
            record.response = lamp::Summarize(root.get());
        return;
    }
    const auto delivery = lamp::Text(root.get(), "delivery");
    if (lamp::Text(root.get(), "request_id") != id ||
        (delivery != "accepted" && delivery != "dispatched" && !TerminalDelivery(delivery))) {
        record.delivery = "unknown";
        record.error = "Invalid or mismatched bridge receipt";
        return;
    }
    record.delivery = delivery;
    auto* app = cJSON_GetObjectItemCaseSensitive(root.get(), "response");
    if (delivery == "answered") {
        if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(app, "ok")) ||
            lamp::Text(app, "request_id") != id ||
            lamp::Text(app, "protocol_version") != lamp::kProtocol) {
            record.delivery = "unknown";
            record.error = "Mismatched application response";
        } else
            record.response = lamp::Summarize(app);
    }
}
void LampBridgeExecutor::Run(void* arg) { static_cast<LampBridgeExecutor*>(arg)->Loop(); }
void LampBridgeExecutor::Loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
    int64_t refresh_at = 0;
    while (true) {
        auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = {};
        bool configured;
        {
            std::lock_guard lock(mutex_);
            configured = !url_.empty();
        }
        if (configured && netif && esp_netif_is_netif_up(netif) &&
            esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            if (esp_timer_get_time() >= refresh_at) {
                Refresh();
                refresh_at = esp_timer_get_time() + 2000000;
            }
            std::string id;
            bool post = false;
            {
                std::lock_guard lock(mutex_);
                if (!queue_.empty()) {
                    id = queue_.front();
                    queue_.pop_front();
                    post = true;
                } else {
                    // Round-robin receipt reads; a slow response cannot starve other queries.
                    auto begin = records_.upper_bound(last_polled_);
                    for (size_t i = 0; i < records_.size(); ++i) {
                        if (begin == records_.end())
                            begin = records_.begin();
                        if (begin->second.posted && !TerminalDelivery(begin->second.delivery)) {
                            id = begin->first;
                            last_polled_ = id;
                            break;
                        }
                        ++begin;
                    }
                }
                if (!id.empty() && esp_timer_get_time() >= records_.at(id).deadline) {
                    records_.at(id).delivery = records_.at(id).posted ? "unknown" : "expired";
                    id.clear();
                }
            }
            if (!id.empty())
                Process(id, post);
        } else {
            std::lock_guard lock(mutex_);
            ready_ = false;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    }
}

void RegisterLampTools(LampActionExecutor& executor) {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.lamp.list_primitives",
        "查询能力表与表情原语。cursor=0 返回能力表：所有 step 动作与其允许值（含 set_mode 的 "
        "sit/stand/work），以及 postures、completion、enable、expression 四块策略；cursor=1..13 "
        "按序返回单个原语；skill_id 直接查一个原语及其选项。只用本地预设，禁止角度、参数、坐标。"
        "原语只是表情，切换姿态必须用 set_mode，绝不是原语。需要目标的原语必须先标定。",
        PropertyList({Property("cursor", kPropertyTypeInteger, 0, 0, 13),
                      Property("skill_id", kPropertyTypeString, std::string()).SetMaxLength(16)}),
        [&executor](const PropertyList& args) -> ReturnValue {
            CJsonUniquePtr catalog(cJSON_Parse(
                lamp::Catalog(args["cursor"].value<int>(), args["skill_id"].value<std::string>())
                    .c_str()));
            cJSON_AddBoolToObject(catalog.get(), "available", executor.Ready());
            CJsonUniquePtr state(cJSON_Parse(executor.State().c_str()));
            const auto role = lamp::Text(state.get(), "executor");
            // actions/controls now ride only on the summary page, so a blind replace
            // would leak the replacement on every primitive page.
            auto replace = [&catalog](const char* key, cJSON* value) {
                if (!cJSON_ReplaceItemInObjectCaseSensitive(catalog.get(), key, value))
                    cJSON_Delete(value);
            };
            replace("executor", cJSON_CreateString(role.c_str()));
            if (role == "local_uart_read_only") {
                replace("primitives", cJSON_CreateArray());
                replace("actions", cJSON_CreateArray());
                replace("controls", cJSON_CreateArray());
                replace("total", cJSON_CreateNumber(0));
                replace("next_cursor", cJSON_CreateNumber(-1));
                replace("execution", cJSON_CreateString("hardware_read_only"));
            }
            if (role == "local_uart_hardware") {
                auto* primitives = cJSON_GetObjectItemCaseSensitive(catalog.get(), "primitives");
                for (int i = cJSON_GetArraySize(primitives) - 1; i >= 0; --i)
                    if (lamp::Text(cJSON_GetArrayItem(primitives, i), "id") == "A10")
                        cJSON_DeleteItemFromArray(primitives, i);
                cJSON_ReplaceItemInObjectCaseSensitive(catalog.get(), "total",
                                                       cJSON_CreateNumber(12));
                cJSON_ReplaceItemInObjectCaseSensitive(catalog.get(), "execution",
                                                       cJSON_CreateString("hardware_feedback"));
                cJSON_AddBoolToObject(catalog.get(), "requires_operator_enable",
                                      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                                          state.get(), "requires_operator_enable")));
                cJSON_AddStringToObject(catalog.get(), "lighting",
                                        "No physical light driver; A10 unavailable, A16 body only");
            }
            return lamp::Encode(catalog.get());
        });
    mcp.AddTool(
        "self.lamp.run_sequence",
        "提交有限语义组（1-16 步，≤60 秒）。姿态用 set_mode，单个表情用 execute_action，"
        "多步组合或说话配动作才用这里。说话配动作带 "
        "context:{source:'dialogue',turn_id:'本轮唯一',cue}，cue 取 "
        "agreement/question/denial/excitement/regret/emphasis/neutral/attention/avoidance/"
        "surprise。每轮只做一个轻微表情并随语义变化，不要每轮都点头；cue→动作："
        "agreement A01|A16，question B02|A03，denial B01|A14，excitement A02|A01，"
        "regret A03|B02，emphasis A16|A01，neutral A01(repetitions:1)|B02，"
        "attention A09|B02，avoidance A11|A14，surprise A14|A02。至多三步，同一轮不重复同一 "
        "skill；A09/A11/A14 必须带 target_id 且已标定。带 context 时本地等语音开始后再执行，"
        "多步自动错开。BUSY 或 DIALOGUE_SKIPPED 直接跳过，不要换 id 重试。回执不等于到位，用 "
        "get_state 查。",
        PropertyList(
            {Property("sequence_json", kPropertyTypeString).SetMaxLength(lamp::kMaxCommandBytes)}),
        [&executor](const PropertyList& args) -> ReturnValue {
            auto command = lamp::Sequence(args["sequence_json"].value<std::string>());
            return command ? executor.Submit(*command)
                           : Failure("INVALID_REQUEST", command.error());
        });
    mcp.AddTool(
        "self.lamp.set_mode",
        "切换标准身体姿态。mode 必须是小写 sit、stand、work：sit 最低且灯头略仰，stand 最高且接近"
        "水平，work 降低并俯视桌面。保留当前 yaw，不需要目标标定，空 calibrated_targets "
        "不能挡住它。"
        "未使能时会先在实测位置使能；手动控制或示教进行中会被拒绝。这是姿态唯一入口，A03 低头低落、"
        "A04 收拢休息、A13 靠近都不能代替。回执不等于到位，用 task_id 查 get_state。",
        PropertyList({Property("mode", kPropertyTypeString).SetMaxLength(8)}),
        [&executor](const PropertyList& args) -> ReturnValue {
            const auto mode = args["mode"].value<std::string>();
            if (mode != "sit" && mode != "stand" && mode != "work")
                return Failure("INVALID_REQUEST",
                               "mode 必须是小写 sit、stand 或 work；收到的是 \"" + mode + "\"");
            CJsonUniquePtr command(
                cJSON_Parse(ActionCommand(executor.NewRequestId(), "set_mode").c_str()));
            cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(command.get(), "args"), "mode",
                                    mode.c_str());
            return executor.Submit(lamp::Encode(command.get()));
        });
    mcp.AddTool("self.lamp.restore_home",
                "回到当前模式的标准姿态，保留当前 yaw。用于手掰后越出模式包络，或收到"
                "「Current pose outside mode」拒绝后的恢复。不换模式、不需要标定、不使能。",
                PropertyList(), [&executor](const PropertyList&) -> ReturnValue {
                    return executor.Submit(ActionCommand(executor.NewRequestId(), "restore_home"));
                });
    mcp.AddTool(
        "self.lamp.get_state",
        "读取当前状态、轮询回执或查询单个任务。不带参数返回缓存状态；bridge_request_id 读凭据；"
        "task_id 查询该任务。status 为 completed 或 limited 才表示硬件确认到位，accepted、running "
        "都不算。",
        PropertyList(
            {Property("bridge_request_id", kPropertyTypeString, std::string()).SetMaxLength(96),
             Property("task_id", kPropertyTypeString, std::string()).SetMaxLength(96)}),
        [&executor](const PropertyList& args) -> ReturnValue {
            auto request = args["bridge_request_id"].value<std::string>();
            auto task = args["task_id"].value<std::string>();
            if (!request.empty() && !task.empty())
                return Failure("INVALID_REQUEST", "Choose either receipt or task_id");
            if (!request.empty())
                return executor.Inspect(request);
            if (!task.empty())
                return executor.Submit(lamp::Query("get_task", executor.NewRequestId(), task));
            return executor.State();
        });
    mcp.AddTool(
        "self.lamp.stop",
        "高优先级取消正在执行或排队的运动，保持姿态与支撑力矩，不卸力、不进入示教。"
        "回执不等于已停止。已取消的表达不要补播。",
        PropertyList({Property("task_id", kPropertyTypeString, std::string()).SetMaxLength(96)}),
        [&executor](const PropertyList& args) -> ReturnValue {
            return executor.Submit(lamp::Query("cancel", executor.NewRequestId(),
                                               args["task_id"].value<std::string>()));
        });
    mcp.AddTool(
        "self.lamp.execute_action",
        "提交一条 execute 命令的完整信封：{protocol_version:'lamp-action/2',request_id:'唯一',"
        "operation:'execute',args:{action_id:'execute_primitive',skill_id:'A01'..'B02',"
        "target_id?:'user'|'book',options?:{...}}}。skill_id 只能用 list_primitives 里真实存在的"
        "13 个；需要目标的原语必须带 target_id；所有选项放在 options 内，不能放顶层。"
        "A10 的 options.hold_mode=holding 只作单条执行，结束用 end_lighting。用户明确要求做动作时"
        "不要带 dialogue context。回执不等于到位。",
        PropertyList(
            {Property("action_json", kPropertyTypeString).SetMaxLength(lamp::kMaxCommandBytes)}),
        [&executor](const PropertyList& args) -> ReturnValue {
            auto command = lamp::Command(args["action_json"].value<std::string>());
            if (!command)
                return Failure("INVALID_REQUEST", command.error());
            CJsonUniquePtr root(cJSON_Parse(command->c_str()));
            if (lamp::Text(root.get(), "operation") != "execute")
                return Failure("INVALID_REQUEST", "operation must be execute");
            return executor.Submit(*command);
        });
    mcp.AddTool("self.lamp.end_lighting",
                "End persistent lighting using the executor's end_lighting operation. Does not "
                "release torque.",
                PropertyList(), [&executor](const PropertyList&) -> ReturnValue {
                    return executor.Submit(lamp::Query("end_lighting", executor.NewRequestId()));
                });
    mcp.AddTool("self.lamp.rest",
                "去休息：取消旧动作，回到标准 sit，确认到位后才卸力，随后关闭会话等待唤醒。"
                "回执不等于已休息，必须等 sit 成功再结束对话。下次唤醒先 enable_torque 再站起。"
                "只想手动掰动用 release_torque，不坐下。",
                PropertyList(), [&executor](const PropertyList&) -> ReturnValue {
                    return executor.Submit(lamp::Query("rest", executor.NewRequestId()));
                });
    mcp.AddTool(
        "self.lamp.release_torque",
        "原地卸力并取消运动，不回到坐姿。用户说别用力、让我自己掰、要示教、要校准时立即调用。"
        "卸力期间不要执行动作或自动配动作。用户手摆对准并明确说记录时，再用 record_calibration。",
        PropertyList(), [&executor](const PropertyList&) -> ReturnValue {
            return executor.Submit(TorqueCommand(executor.NewRequestId(), false));
        });
    mcp.AddTool(
        "self.lamp.enable_torque",
        "在实测位置使能支撑力矩，不移动、不站起、不回到旧目标。用于再次唤醒后，或用户明确说"
        "掰好了。示教中不要自动使能。accepted 不等于已使能：用 task_id 轮询 get_state 到 completed "
        "再发动作。只使能一次，不要每个动作前都调。",
        PropertyList(), [&executor](const PropertyList&) -> ReturnValue {
            return executor.Submit(TorqueCommand(executor.NewRequestId(), true));
        });
    mcp.AddTool(
        "self.lamp.record_calibration",
        "仅在用户已手摆对准并明确说记录时调用：把当前实测姿态保存为当前模式的 user（人脸）或 "
        "book（书本）目标，写入 NVS。要求已卸力且空闲。sit 只认 user，stand 认 user/book，work "
        "只认 "
        "book。不移动、不使能、不猜坐标。",
        PropertyList({Property("target_id", kPropertyTypeString).SetMaxLength(8)}),
        [&executor](const PropertyList& args) -> ReturnValue {
            const auto target = args["target_id"].value<std::string>();
            if (target != "user" && target != "book")
                return Failure("INVALID_REQUEST", "target_id must be user/book");
            CJsonUniquePtr command(
                cJSON_Parse(lamp::Query("record_calibration", executor.NewRequestId()).c_str()));
            cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(command.get(), "args"),
                                    "target_id", target.c_str());
            return executor.Submit(lamp::Encode(command.get()));
        });
    ESP_LOGI(kTag, "Semantic lamp MCP tools registered; executor owns hardware capability");
}

void RegisterLampBridgeConfiguration(LampBridgeExecutor& executor) {
    auto& mcp = McpServer::GetInstance();
    mcp.AddUserOnlyTool(
        "self.lamp.configure_bridge",
        "Operator setup only: LAN HTTP root URL and Bearer token. "
        "Stored in NVS; empty URL disables. Never change the official voice OTA URL for this "
        "adapter.",
        PropertyList({Property("url", kPropertyTypeString).SetMaxLength(240),
                      Property("token", kPropertyTypeString, std::string()).SetMaxLength(256)}),
        [&executor](const PropertyList& args) -> ReturnValue {
            return executor.Configure(args["url"].value<std::string>(),
                                      args["token"].value<std::string>());
        });
}
