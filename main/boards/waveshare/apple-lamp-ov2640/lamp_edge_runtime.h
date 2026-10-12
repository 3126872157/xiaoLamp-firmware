#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <vector>
#include "lamp_bridge.h"

// One owner for cloud semantic requests. Never performs UART IO itself.
class LampEdgeRuntime {
public:
    explicit LampEdgeRuntime(
        LampActionExecutor& executor,
        std::function<void(const std::string&, const std::string&, const std::string&)> diagnostic =
            {});
    std::future<std::string> Submit(const std::string& name, const std::string& args,
                                    uint64_t turn);
    void Cancel(uint64_t turn);
    std::string State();
    static std::string Tools();
    // Single bounded iteration, also used by production-path host tests.
    void Tick();

private:
    struct Work {
        std::string name, args;
        uint64_t turn = 0;
        std::promise<std::string> result;
        bool enable_checked = false;
    };
    struct Pending {
        Work* work;
        std::string task;
        int64_t deadline;
        bool enabling = false;
    };
    LampActionExecutor& executor_;
    std::function<void(const std::string&, const std::string&, const std::string&)> diagnostic_;
    QueueHandle_t queue_ = nullptr, urgent_ = nullptr;
    bool started_ = false;
    std::atomic<uint64_t> invalid_through_{0};
    std::atomic<size_t> queued_bytes_{0};
    std::mutex state_mutex_;
    std::string snapshot_ = "{}", motion_task_;
    std::string requested_mode_ = "unknown";
    int64_t snapshot_at_ = 0;
    uint64_t motion_turn_ = 0, group_turn_ = 0;
    std::vector<Pending> pending_;
    void Process(Work* work);
    void Complete(Work* work, const std::string& output);
    void CancelOwned();
};
