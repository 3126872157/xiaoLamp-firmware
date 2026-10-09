#include <cassert>
#include <iostream>
#include "lamp_edge_runtime.h"
#include "lamp_protocol.h"

struct Executor : LampActionExecutor {
    unsigned counter = 0, groups = 0, stops = 0;
    bool armed = false, manual = false, completed = false, failed = false, online = false;
    std::string NewRequestId() override { return "test-" + std::to_string(++counter); }
    bool Ready() override { return true; }
    std::string State() override {
        return "{\"available\":" + std::string(online ? "true" : "false") +
               ",\"mode\":\"stand\",\"busy\":false,\"motion_enabled\":" +
               std::string(armed ? "true" : "false") +
               ",\"torque_enabled_confirmed\":" + std::string(armed ? "true" : "false") +
               ",\"manual_control\":" + std::string(manual ? "true" : "false") + "}";
    }
    std::string Inspect(const std::string&) override { return "{}"; }
    std::string Submit(const std::string& text) override {
        auto checked = lamp::Command(text);
        assert(checked);
        CJsonUniquePtr j(cJSON_Parse(text.c_str()));
        const auto op = lamp::Text(j.get(), "operation"), id = lamp::Text(j.get(), "request_id");
        if (op == "get_task")
            return "{\"ok\":true,\"result\":{\"task_id\":\"task\",\"status\":\"" +
                   std::string(failed      ? "failed"
                               : completed ? "completed"
                                           : "running") +
                   "\",\"total_steps\":1,\"reason\":\"test feedback\"}}";
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
    auto offline = runtime.Submit("lamp_get_state", "{}", 1);
    runtime.Tick();
    const auto state = offline.get();
    assert(state.find("\"available\":false") != std::string::npos);
    assert(state.find("\"confirmed_mode\":\"unknown\"") != std::string::npos);
    assert(state.find("runtime_ready") != std::string::npos);
    auto no_prepare = runtime.Submit("lamp_prepare_expression", R"({"skill_id":"B02"})", 2);
    runtime.Tick();
    assert(no_prepare.get().find("no automatic enable") != std::string::npos &&
           executor.groups == 0);
    executor.online = executor.armed = true;
    auto prepared = runtime.Submit("lamp_prepare_expression", R"({"skill_id":"A02"})", 2);
    runtime.Tick();
    const auto preflight = prepared.get();
    assert(preflight.find("prepared") != std::string::npos && executor.groups == 0);
    auto bad_prepare = runtime.Submit("lamp_prepare_expression",
                                      R"({"skill_id":"A13","enable_if_needed":true})", 2);
    runtime.Tick();
    assert(bad_prepare.get().find("Unsupported") != std::string::npos && executor.groups == 0);
    auto silent = runtime.Submit("lamp_prepare_expression", R"({"speech":null,"action":"nod"})", 2);
    runtime.Tick();
    auto silent_result = silent.get();
    assert(silent_result.find("\"speech\":null") != std::string::npos &&
           silent_result.find("A01") != std::string::npos && executor.groups == 0);
    auto invalid_speech =
        runtime.Submit("lamp_prepare_expression", R"({"speech":42,"action":"nod"})", 2);
    runtime.Tick();
    assert(invalid_speech.get().find("speech accepts") != std::string::npos);
    auto four_nods = runtime.Submit("lamp_prepare_expression",
                                    R"({"skill_id":"A01","speak":true,"repetitions":4})", 2);
    runtime.Tick();
    assert(four_nods.get().find("\"repetitions\":4") != std::string::npos);
    auto six_nods =
        runtime.Submit("lamp_prepare_expression", R"({"skill_id":"A01","repetitions":6})", 2);
    runtime.Tick();
    assert(six_nods.get().find("1..5") != std::string::npos);
    executor.online = executor.armed = false;
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
    assert(move.get().find("Not confirmed complete") != std::string::npos && executor.groups == 1);
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
    executor.failed = true;
    auto failure = runtime.Submit("lamp_run_sequence",
                                  R"({"steps":[{"action_id":"set_mode","mode":"stand"}]})", 7);
    runtime.Tick();
    const auto rejected = failure.get();
    assert(rejected.find("\"ok\":false") != std::string::npos);
    assert(rejected.find("\"execution_completed\":false") != std::string::npos);
    executor.failed = false;
    auto stand = runtime.Submit("lamp_run_sequence",
                                R"({"steps":[{"action_id":"set_mode","mode":"stand"}]})", 8);
    executor.completed = false;
    runtime.Tick();
    assert(stand.wait_for(std::chrono::seconds(0)) != std::future_status::ready);
    executor.completed = true;
    runtime.Tick();
    assert(stand.get().find("\"execution_completed\":true") != std::string::npos);
    executor.armed = false;
    executor.online = false;
    auto offline_mode = runtime.Submit("lamp_set_mode", R"({"mode":"stand"})", 9);
    runtime.Tick();
    assert(offline_mode.get().find("Cannot auto-enable") != std::string::npos);
    executor.online = true;
    executor.completed = false;
    auto auto_mode = runtime.Submit("lamp_set_mode", R"({"mode":"stand"})", 10);
    const auto before = executor.groups;
    runtime.Tick();
    assert(executor.groups == before &&
           auto_mode.wait_for(std::chrono::seconds(0)) != std::future_status::ready);
    executor.armed = true;
    executor.completed = true;
    runtime.Tick();  // Torque confirmed; dispatch the real mode change only now.
    assert(executor.groups == before + 1);
    runtime.Tick();
    assert(auto_mode.get().find("execution_completed") != std::string::npos);
    executor.armed = false;
    executor.completed = false;
    executor.failed = true;
    const auto failed_before = executor.groups;
    auto failed_enable = runtime.Submit("lamp_set_mode", R"({"mode":"stand"})", 11);
    runtime.Tick();
    assert(failed_enable.get().find("\"ok\":false") != std::string::npos &&
           executor.groups == failed_before);
    executor.failed = false;
    executor.manual = true;
    auto manual_mode = runtime.Submit("lamp_set_mode", R"({"mode":"stand"})", 12);
    runtime.Tick();
    assert(manual_mode.get().find("manual control") != std::string::npos);
    executor.manual = false;
    auto cancel_enable = runtime.Submit("lamp_set_mode", R"({"mode":"stand"})", 13);
    runtime.Tick();
    runtime.Cancel(13);
    runtime.Tick();
    assert(cancel_enable.get().find("cancelled") != std::string::npos &&
           executor.groups == failed_before);
    executor.armed = true;
    std::vector<std::future<std::string>> full;
    for (int i = 0; i < 64; i++)
        full.push_back(runtime.Submit("lamp_get_state", "{}", 14));
    auto overflow = runtime.Submit("lamp_get_state", "{}", 14);
    assert(overflow.get().find("queue") != std::string::npos);
    auto stop = runtime.Submit("lamp_stop", "{}", 14);
    runtime.Tick();
    assert(stop.get().find("true") != std::string::npos && executor.stops == 3);
    for (int i = 0; i < 64; i++)
        runtime.Tick();
    for (auto& f : full)
        assert(f.get().find("runtime") != std::string::npos);
    auto schemas = CJsonUniquePtr(cJSON_Parse(LampEdgeRuntime::Tools().c_str()));
    assert(cJSON_IsArray(schemas.get()) && cJSON_GetArraySize(schemas.get()) == 10);
    assert(LampEdgeRuntime::Tools().find("A10") == std::string::npos);
    std::cout << "PASS runtime: semantic bounds, ownership, disarmed rejection, one group, "
                 "cancellation, feedback, emergency reserve and catalog\n";
}
