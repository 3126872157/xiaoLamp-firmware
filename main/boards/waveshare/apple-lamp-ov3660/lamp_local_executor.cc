#include "lamp_local_executor.h"
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cstdio>
#include "lamp_bus_protocol.h"
#include "lamp_pose_model.h"
#include "lamp_protocol.h"
namespace {
constexpr int64_t kFreshUs = 1500000;
std::string Failure(const char* code, const char* message) {
    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddBoolToObject(root.get(), "ok", false);
    auto* error = cJSON_AddObjectToObject(root.get(), "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return lamp::Encode(root.get());
}
}  // namespace
LampLocalExecutor::LampLocalExecutor(bool start_worker) {
#if CONFIG_APPLE_LAMP_LOCAL_EXECUTOR
    auto configuration = lamp::pose::ParseConfiguration(lamp::pose::kDeviceConfiguration);
    if (configuration)
        configuration_ = std::move(*configuration);
    else
        configuration_error_ = configuration.error();
#endif
#if CONFIG_APPLE_LAMP_LOCAL_EXECUTOR && CONFIG_APPLE_LAMP_UART_PROBE
#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || !CONFIG_ESP_CONSOLE_SECONDARY_NONE
#error "Local lamp UART0 requires native USB console and no secondary UART console"
#endif
    uart_config_t config = {};
    config.baud_rate = 115200;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    esp_err_t status = uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0);
    if (status == ESP_OK)
        status = uart_param_config(UART_NUM_0, &config);
    if (status == ESP_OK)
        status = uart_set_pin(UART_NUM_0, 43, 44, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (status != ESP_OK) {
        setup_error_ = esp_err_to_name(status);
        uart_driver_delete(UART_NUM_0);
        return;
    }
    TaskHandle_t worker = nullptr;
    if (start_worker && xTaskCreate(Run, "lamp_readonly", 6144, this, 1, &worker) != pdPASS) {
        setup_error_ = "Could not start bounded read-only task";
        uart_driver_delete(UART_NUM_0);
        return;
    }
    uart_enabled_ = true;
    ESP_LOGI("LampLocal", "UART0 TX43/RX44 115200; owner=%s",
             start_worker ? "read-only" : "hardware player");
#else
    setup_error_ = "UART probe is disabled in this firmware variant";
#endif
}
std::string LampLocalExecutor::NewRequestId() {
    std::lock_guard lock(mutex_);
    char id[48];
    std::snprintf(id, sizeof(id), "local-%08lx-%08lx", static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(++counter_));
    return id;
}
std::string LampLocalExecutor::Inspect(const std::string&) {
    return Failure("REQUEST_NOT_FOUND", "Read-only executor has no motion receipts");
}
std::string LampLocalExecutor::Submit(const std::string& command) {
    auto checked = lamp::Command(command);
    if (!checked)
        return Failure("INVALID_REQUEST", checked.error().c_str());
    CJsonUniquePtr root(cJSON_Parse(checked->c_str()));
    if (lamp::Text(root.get(), "operation") == "get_state")
        return State();
    return Failure("READ_ONLY",
                   "Physical UART bringup only; no motion, torque, calibration write or simulated "
                   "completion is available");
}
std::string LampLocalExecutor::State() {
    std::lock_guard lock(mutex_);
    const auto now = esp_timer_get_time();
    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "protocol_version", lamp::kProtocol);
    cJSON_AddStringToObject(root.get(), "executor", "local_uart_read_only");
    cJSON_AddStringToObject(root.get(), "completion_basis", "hardware_feedback");
    cJSON_AddBoolToObject(root.get(), "motion_enabled", false);
    cJSON_AddBoolToObject(root.get(), "configuration_loaded", configuration_.has_value());
    cJSON_AddStringToObject(root.get(), "software_stage", "portable_planner_no_hardware_player");
    cJSON_AddStringToObject(root.get(), "model_version", lamp::pose::kModel);
    if (!configuration_error_.empty())
        cJSON_AddStringToObject(root.get(), "configuration_error", configuration_error_.c_str());
    cJSON_AddBoolToObject(root.get(), "uart_enabled", uart_enabled_);
    bool all_fresh = uart_enabled_;
    auto* feedback = cJSON_AddArrayToObject(root.get(), "servos");
    for (unsigned i = 0; i < servos_.size(); i++) {
        const auto& s = servos_[i];
        const bool fresh = s.position_at && now - s.position_at < kFreshUs;
        all_fresh &= fresh;
        auto* entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "id", i + 1);
        cJSON_AddBoolToObject(entry, "position_fresh", fresh);
        if (s.position_at) {
            cJSON_AddNumberToObject(entry, "position_ticks", s.position);
            cJSON_AddNumberToObject(entry, "position_age_ms", (now - s.position_at) / 1000);
        }
        const std::pair<const char*, std::pair<int, int64_t>> fields[] = {
            {"temperature_c", {s.temperature, s.temperature_at}},
            {"voltage_mv", {s.voltage_mv, s.voltage_at}},
            {"mode", {s.mode, s.mode_at}},
            {"torque", {s.torque, s.torque_at}}};
        for (const auto& [key, value] : fields)
            if (value.second) {
                cJSON_AddNumberToObject(entry, key, value.first);
                const auto age = std::string(key) + "_age_ms";
                cJSON_AddNumberToObject(entry, age.c_str(), (now - value.second) / 1000);
            }
        cJSON_AddNumberToObject(entry, "read_failures", s.failures);
        if (s.limits_at) {
            cJSON_AddNumberToObject(entry, "limit_min", s.limit_min);
            cJSON_AddNumberToObject(entry, "limit_max", s.limit_max);
        }
        if (!s.last_error.empty())
            cJSON_AddStringToObject(entry, "last_error", s.last_error.c_str());
        cJSON_AddItemToArray(feedback, entry);
    }
    cJSON_AddBoolToObject(root.get(), "available", all_fresh);
    if (!setup_error_.empty())
        cJSON_AddStringToObject(root.get(), "setup_error", setup_error_.c_str());
    return lamp::Encode(root.get());
}
void LampLocalExecutor::Run(void* self) { static_cast<LampLocalExecutor*>(self)->Loop(); }
bool LampLocalExecutor::Read(unsigned index, uint8_t command) {
#if CONFIG_APPLE_LAMP_LOCAL_EXECUTOR && CONFIG_APPLE_LAMP_UART_PROBE
    const auto request = lamp::bus::ReadRequest(index + 1, command);
    if (!request || ReadCancelled())
        return false;
    uart_flush_input(UART_NUM_0);
    bool ok = uart_write_bytes(UART_NUM_0, request->data(), request->size()) ==
              static_cast<int>(request->size());
    lamp::bus::Parser parser;
    const auto deadline = esp_timer_get_time() + 400000;
    while (ok && !ReadCancelled() && esp_timer_get_time() < deadline) {
        uint8_t bytes[128];
        const int count = uart_read_bytes(UART_NUM_0, bytes, sizeof(bytes), pdMS_TO_TICKS(20));
        if (count < 0)
            break;
        for (const auto& packet : parser.Push(std::span(bytes, count))) {
            if (packet.id != index + 1 || packet.command != command ||
                packet.parameters.size() != lamp::bus::ReadLength(command))
                continue;
            std::lock_guard lock(mutex_);
            auto& s = servos_[index];
            const auto now = esp_timer_get_time();
            switch (command) {
                case 21:
                    if (lamp::bus::Word(packet.parameters) >
                            lamp::bus::Word(std::span(packet.parameters).subspan(2)) ||
                        lamp::bus::Word(std::span(packet.parameters).subspan(2)) > 1000) {
                        s.last_error = "Invalid device position limits";
                        return false;
                    }
                    s.limit_min = lamp::bus::Word(packet.parameters);
                    s.limit_max = lamp::bus::Word(std::span(packet.parameters).subspan(2));
                    s.limits_at = now;
                    break;
                case 28:
                    s.position = lamp::bus::SignedWord(packet.parameters);
                    s.position_at = now;
                    break;
                case 26:
                    s.temperature = packet.parameters[0];
                    s.temperature_at = now;
                    break;
                case 27:
                    s.voltage_mv = lamp::bus::Word(packet.parameters);
                    s.voltage_at = now;
                    break;
                case 30:
                    s.mode = packet.parameters[0];
                    s.mode_at = now;
                    break;
                case 32:
                    s.torque = packet.parameters[0];
                    s.torque_at = now;
                    break;
            }
            s.last_error.clear();
            return true;
        }
    }
    if (ReadCancelled())
        return false;
    std::lock_guard lock(mutex_);
    auto& s = servos_[index];
    ++s.failures;
    s.last_error = "CMD " + std::to_string(command) + " read timeout/transport error";
    ESP_LOGW("LampLocal", "ID%u CMD%u read failed; no write/retry motion", index + 1, command);
#endif
    return false;
}
void LampLocalExecutor::Loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
    unsigned telemetry = 0;
    const uint8_t commands[] = {26, 27, 30, 32};
    while (true) {
        for (unsigned i = 0; i < servos_.size(); i++) {
            Read(i, 28);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        Read((telemetry / 4) % 5, commands[telemetry % 4]);
        telemetry++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
