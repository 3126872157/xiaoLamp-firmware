#include <cassert>
#include <iostream>
#include "lamp_edge_runtime.h"
#include "lamp_protocol.h"

struct Executor : LampActionExecutor {
    unsigned counter = 0, groups = 0, stops = 0;
    bool armed = false, manual = false, completed = false;
    std::string NewRequestId() override { return "test-" + std::to_string(++counter); }
    bool Ready() override { return true; }
    std::string State() override {
        return "{\"mode\":\"stand\",\"busy\":false,\"motion_enabled\":" +
               std::string(armed ? "true" : "false") + "}";
    }
    std::string Inspect(const std::string&) override { return "{}"; }
    std::string Submit(const std::string& text) override {
        auto checked = lamp::Command(text);
        assert(checked);
        CJsonUniquePtr j(cJSON_Parse(text.c_str()));
        const auto op = lamp::Text(j.get(), "operation"), id = lamp::Text(j.get(), "request_id");
        if (op == "get_task")
            return "{\"ok\":true,\"result\":{\"task_id\":\"task\",\"status\":\"" +
                   std::string(completed ? "completed" : "running") + "\"}}";
        if (op == "cancel") {
            stops++;
            return "{\"ok\":true}";
        }
        if (op == "execute_group") {
            if (!armed || manual)
                return "{\"ok\":false,\"error\":\"operator enable/control required\"}";
            groups++;
        }
        return "{\"ok\":true,\"result\":{\"task_id\":\"hardware-" + id +
               "\",\"status\":\"accepted\"}}";
    }
};
int main() {
    Executor executor;
    LampEdgeRuntime runtime(executor);
    runtime.Tick();
    const std::string group =
        R"({"steps":[{"action_id":"adjust_pose","direction":"yaw_positive","size":"small"}]})";
    auto disabled = runtime.Submit("lamp_run_sequence", group, 1);
    runtime.Tick();
    assert(disabled.get().find("operator") != std::string::npos && executor.groups == 0);
    auto injection =
        runtime.Submit("lamp_run_sequence", R"({"steps":[],"context":{"turn_id":"fake"}})", 2);
    runtime.Tick();
    assert(injection.get().find("context") != std::string::npos);
    auto unknown = runtime.Submit("write_uart", "{}", 2);
    runtime.Tick();
    assert(unknown.get().find("Unknown") != std::string::npos);
    auto raw = runtime.Submit(
        "lamp_run_sequence",
        R"({"steps":[{"action_id":"adjust_pose","direction":"up","degrees":90}]})", 2);
    runtime.Tick();
    assert(raw.get().find("Unsupported field") != std::string::npos);
    executor.armed = true;
    auto move = runtime.Submit("lamp_run_sequence", group, 3);
    runtime.Tick();
    assert(move.get().find("accepted") != std::string::npos && executor.groups == 1);
    auto twice = runtime.Submit("lamp_run_sequence", group, 3);
    runtime.Tick();
    assert(twice.get().find("one motion") != std::string::npos && executor.groups == 1);
    auto queued = runtime.Submit("lamp_run_sequence", group, 4);
    runtime.Cancel(4);
    runtime.Tick();
    assert(queued.get().find("cancelled") != std::string::npos && executor.groups == 1 &&
           executor.stops == 1);
    executor.manual = true;
    auto manual = runtime.Submit("lamp_run_sequence", group, 5);
    runtime.Tick();
    assert(manual.get().find("operator") != std::string::npos && executor.groups == 1);
    executor.manual = false;
    auto torque = runtime.Submit("lamp_set_torque", "{\"enabled\":true}", 6);
    runtime.Tick();
    assert(torque.wait_for(std::chrono::seconds(0)) != std::future_status::ready);
    executor.completed = true;
    runtime.Tick();
    assert(torque.get().find("completed") != std::string::npos);
    std::vector<std::future<std::string>> full;
    for (int i = 0; i < 64; i++)
        full.push_back(runtime.Submit("lamp_get_state", "{}", 7));
    auto overflow = runtime.Submit("lamp_get_state", "{}", 7);
    assert(overflow.get().find("queue") != std::string::npos);
    auto stop = runtime.Submit("lamp_stop", "{}", 7);
    runtime.Tick();
    assert(stop.get().find("true") != std::string::npos && executor.stops == 2);
    for (int i = 0; i < 64; i++)
        runtime.Tick();
    for (auto& f : full)
        assert(f.get().find("runtime") != std::string::npos);
    auto schemas = CJsonUniquePtr(cJSON_Parse(LampEdgeRuntime::Tools().c_str()));
    assert(cJSON_IsArray(schemas.get()) && cJSON_GetArraySize(schemas.get()) == 8);
    assert(LampEdgeRuntime::Tools().find("A10") == std::string::npos);
    std::cout << "PASS runtime: semantic bounds, ownership, disarmed rejection, one group, "
                 "cancellation, feedback, emergency reserve and catalog\n";
}
