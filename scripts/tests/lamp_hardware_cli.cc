// Exercise production executor against a deterministic bus/device simulator, NOT physical hardware.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include "lamp_bus_protocol.h"
#include "lamp_hardware_executor.h"
#include "lamp_hardware_platform.h"
namespace {
std::mutex bus_mutex;
std::deque<uint8_t> rx;
std::vector<std::thread> tasks;
std::atomic<bool> halt = false;
int position[] = {500, 834, 130, 625, 500}, target[] = {500, 834, 130, 625, 500}, torque[5] = {},
    mode[5] = {1, 0, 0, 0, 0};
unsigned writes[256] = {};
int drop_id = 0;
bool frozen = false, nvs_fail = false;
std::string saved;
const auto boot = std::chrono::steady_clock::now();
struct TaskExit {};
void StopTasks() {
    halt = true;
    for (auto& t : tasks)
        t.join();
    tasks.clear();
    halt = false;
}
const cJSON* Item(const cJSON* o, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(o, key);
}
CJsonUniquePtr Json(const std::string& s) { return CJsonUniquePtr(cJSON_Parse(s.c_str())); }
std::string Command(const std::string& id, const std::string& op, const std::string& args = "{}") {
    return "{\"protocol_version\":\"lamp-action/2\",\"request_id\":\"" + id +
           "\",\"operation\":\"" + op + "\",\"args\":" + args + "}";
}
std::string Wait(LampHardwareExecutor& e, const std::string& id) {
    for (int i = 0; i < 2500; i++) {
        auto r = Json(e.Inspect(id));
        const auto s = lamp::Text(Item(r.get(), "result"), "status");
        if (s == "completed" || s == "failed" || s == "limited" || s == "cancelled")
            return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(false);
    return "timeout";
}
void Submit(LampHardwareExecutor& e, const std::string& id, const std::string& op,
            const std::string& args = "{}") {
    auto r = Json(e.Submit(Command(id, op, args)));
    assert(cJSON_IsTrue(Item(r.get(), "ok")));
}
}  // namespace
int64_t esp_timer_get_time() {
    return 1 + std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - boot)
                       .count() *
                   20;
}
uint32_t esp_random() {
    static std::atomic<uint32_t> n = 0;
    return ++n;
}
const char* esp_err_to_name(int) { return "EMULATED_ERROR"; }
int xTaskCreate(void (*fn)(void*), const char*, unsigned, void* arg, int, TaskHandle_t*) {
    tasks.emplace_back([=] {
        try {
            fn(arg);
        } catch (TaskExit&) {
        }
    });
    return pdPASS;
}
void vTaskDelay(unsigned ms) {
    if (halt)
        throw TaskExit{};
    std::this_thread::sleep_for(std::chrono::microseconds(std::max(100u, ms * 50)));
    if (halt)
        throw TaskExit{};
}
int uart_driver_install(int, int, int, int, void*, int) { return 0; }
int uart_driver_delete(int) { return 0; }
int uart_param_config(int, const uart_config_t* c) {
    assert(c->baud_rate == 115200);
    return 0;
}
int uart_set_pin(int, int tx, int rx, int, int) {
    assert(tx == 43 && rx == 44);
    return 0;
}
int uart_wait_tx_done(int, unsigned) { return 0; }
int uart_flush_input(int) {
    std::lock_guard l(bus_mutex);
    rx.clear();
    return 0;
}
int uart_write_bytes(int, const void* raw, size_t count) {
    std::lock_guard l(bus_mutex);
    const auto* b = static_cast<const uint8_t*>(raw);
    lamp::bus::Parser parser;
    auto packets = parser.Push(std::span(b, count));
    assert(packets.size() == 1);
    const auto& p = packets[0];
    assert(p.id >= 1 && p.id <= 5);
    int i = p.id - 1;
    auto data = p.parameters;
    if (p.command == 1) {
        writes[1]++;
        target[i] = lamp::bus::Word(data);
        if (torque[i] && !frozen)
            position[i] = target[i];
    } else if (p.command == 12)
        writes[12]++;
    else if (p.command == 29) {
        writes[29]++;
        mode[i] = data[0];
    } else if (p.command == 31) {
        writes[31]++;
        torque[i] = data[0];
        if (torque[i] && !frozen)
            position[i] = target[i];
    } else {
        assert(p.command == 26 || p.command == 27 || p.command == 28 || p.command == 30 ||
               p.command == 32);
        if (drop_id == int(p.id))
            return count;
        unsigned value = p.command == 28   ? position[i]
                         : p.command == 27 ? 7400
                         : p.command == 26 ? 30
                         : p.command == 30 ? mode[i]
                                           : torque[i];
        std::vector<uint8_t> frame = {0x55, 0x55, p.id,
                                      static_cast<uint8_t>(lamp::bus::ReadLength(p.command) + 3),
                                      p.command};
        frame.push_back(value & 255);
        if (lamp::bus::ReadLength(p.command) >= 2)
            frame.push_back((value >> 8) & 255);
        if (p.command == 30) {
            frame.push_back(0);
            frame.push_back(0);
        }
        uint8_t sum = 0;
        for (size_t j = 2; j < frame.size(); j++)
            sum += frame[j];
        frame.push_back(~sum);
        for (auto v : frame)
            rx.push_back(v);
    }
    assert(p.command != 7 && p.command != 8 && p.command != 11);
    return count;
}
int uart_read_bytes(int, void* out, size_t count, unsigned) {
    std::unique_lock l(bus_mutex);
    size_t n = std::min(count, rx.size());
    for (size_t i = 0; i < n; i++) {
        static_cast<uint8_t*>(out)[i] = rx.front();
        rx.pop_front();
    }
    l.unlock();
    if (!n)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return n;
}
int nvs_open(const char*, int rw, nvs_handle_t* h) {
    if (!rw && saved.empty())
        return ESP_ERR_NVS_NOT_FOUND;
    *h = 1;
    return 0;
}
void nvs_close(nvs_handle_t) {}
int nvs_get_blob(nvs_handle_t, const char*, void* out, size_t* n) {
    if (!out) {
        *n = saved.size();
        return 0;
    }
    assert(*n >= saved.size());
    std::memcpy(out, saved.data(), saved.size());
    *n = saved.size();
    return 0;
}
int nvs_set_blob(nvs_handle_t, const char*, const void* data, size_t n) {
    if (nvs_fail)
        return ESP_FAIL;
    saved.assign(static_cast<const char*>(data), n);
    return 0;
}
int nvs_commit(nvs_handle_t) { return 0; }
int psa_crypto_init() { return 0; }
int psa_hash_compute(int, const unsigned char* text, size_t n, unsigned char* out, size_t,
                     size_t* size) {
    uint32_t h = 2166136261;
    for (size_t i = 0; i < n; i++)
        h = (h ^ text[i]) * 16777619;
    for (int i = 0; i < 32; i++)
        out[i] = (h >> ((i % 4) * 8)) ^ (i * 17);
    *size = 32;
    return 0;
}
Application& Application::GetInstance() {
    static Application app;
    return app;
}
int main() {
    auto* e = new LampHardwareExecutor();
    assert(e->Ready());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    {
        std::lock_guard l(bus_mutex);
        assert(writes[1] == 0 && writes[31] == 0);
    }
    Submit(*e, "disarmed", "execute", R"({"action_id":"execute_primitive","skill_id":"A01"})");
    assert(Wait(*e, "disarmed") == "failed");
    Submit(*e, "enable", "set_torque", R"({"enabled":true})");
    assert(Wait(*e, "enable") == "completed");
    {
        std::lock_guard l(bus_mutex);
        assert(position[1] == 834 && position[3] == 625);
    }
    Submit(*e, "nod", "execute", R"({"action_id":"execute_primitive","skill_id":"A01"})");
    assert(Wait(*e, "nod") == "completed");
    unsigned before;
    {
        std::lock_guard l(bus_mutex);
        before = writes[1];
    }
    Submit(*e, "nod", "execute", R"({"action_id":"execute_primitive","skill_id":"A01"})");
    {
        std::lock_guard l(bus_mutex);
        assert(writes[1] == before);
    }
    auto reordered = Json(e->Submit(
        R"({"args":{"skill_id":"A01","action_id":"execute_primitive"},"operation":"execute","request_id":"nod","protocol_version":"lamp-action/2"})"));
    assert(cJSON_IsTrue(Item(reordered.get(), "ok")));
    auto conflict = Json(e->Submit(
        Command("nod", "execute", R"({"action_id":"execute_primitive","skill_id":"B01"})")));
    assert(lamp::Text(Item(conflict.get(), "error"), "code") == "REQUEST_ID_CONFLICT");
    Submit(
        *e, "preflight", "execute_group",
        R"({"steps":[{"action_id":"execute_primitive","skill_id":"A01"},{"action_id":"gaze_calibrated","target_id":"book"}]})");
    assert(Wait(*e, "preflight") == "failed");
    {
        std::lock_guard l(bus_mutex);
        assert(writes[1] == before);
    }
    Submit(*e, "enable2", "set_torque", R"({"enabled":true})");
    assert(Wait(*e, "enable2") == "completed");
    Submit(*e, "wait", "execute", R"({"action_id":"wait","duration_sec":10})");
    Submit(*e, "cancel", "cancel");
    assert(Wait(*e, "cancel") == "completed");
    assert(Wait(*e, "wait") == "cancelled");
    Submit(*e, "release", "set_torque", R"({"enabled":false})");
    assert(Wait(*e, "release") == "completed");
    {
        std::lock_guard l(bus_mutex);
        position[3] = 650;
    }
    Submit(*e, "teach", "record_calibration", R"({"target_id":"user"})");
    assert(Wait(*e, "teach") == "completed");
    assert(!saved.empty());
    StopTasks();
    auto* reboot = new LampHardwareExecutor();
    auto state = Json(reboot->State());
    assert(cJSON_GetArraySize(Item(Item(state.get(), "calibrated_targets"), "sit")) == 1);
    assert(cJSON_IsFalse(Item(state.get(), "motion_enabled")));
    Submit(*reboot, "enable3", "set_torque", R"({"enabled":true})");
    assert(Wait(*reboot, "enable3") == "completed");
    {
        std::lock_guard l(bus_mutex);
        assert(position[3] == 650);
    }
    Submit(
        *reboot, "group", "execute_group",
        R"({"steps":[{"action_id":"adjust_pose","direction":"up","size":"small"},{"action_id":"wait","duration_sec":0.1},{"action_id":"execute_primitive","skill_id":"B02"}]})");
    assert(Wait(*reboot, "group") == "completed");
    {
        std::lock_guard l(bus_mutex);
        drop_id = 1;
    }
    Submit(*reboot, "lost", "execute", R"({"action_id":"execute_primitive","skill_id":"A01"})");
    assert(Wait(*reboot, "lost") == "failed");
    {
        std::lock_guard l(bus_mutex);
        drop_id = 0;
    }
    Submit(*reboot, "enable4", "set_torque", R"({"enabled":true})");
    assert(Wait(*reboot, "enable4") == "completed");
    {
        std::lock_guard l(bus_mutex);
        frozen = true;
    }
    Submit(*reboot, "unreached", "execute",
           R"({"action_id":"adjust_pose","direction":"up","size":"medium"})");
    assert(Wait(*reboot, "unreached") == "failed");
    {
        std::lock_guard l(bus_mutex);
        frozen = false;
    }
    Submit(*reboot, "enable5", "set_torque", R"({"enabled":true})");
    assert(Wait(*reboot, "enable5") == "completed");
    Submit(*reboot, "rest", "rest");
    assert(Wait(*reboot, "rest") == "completed");
    {
        std::lock_guard l(bus_mutex);
        for (int t : torque)
            assert(t == 0);
    }
    nvs_fail = true;
    const auto old = saved;
    Submit(*reboot, "savefail", "record_calibration", R"({"target_id":"user"})");
    assert(Wait(*reboot, "savefail") == "failed");
    assert(saved == old);
    StopTasks();
    nvs_fail = false;
    saved = "{}";
    auto* bad = new LampHardwareExecutor();
    assert(!bad->Ready());
    Submit(*bad, "emergency-release", "set_torque", R"({"enabled":false})");
    assert(Wait(*bad, "emergency-release") == "completed");
    StopTasks();
    std::cout << "PASS hardware emulator: disarmed boot, no-jump enable, play, dedup, preflight, "
                 "cancel, unload, NVS reboot/failure, group, lost feedback, unreachable end, rest, "
                 "invalid configuration emergency unload\n";
}
