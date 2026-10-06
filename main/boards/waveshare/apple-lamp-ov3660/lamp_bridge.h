#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class Board;

// The MCP facade depends on this interface, not on UART or browser internals.
class LampActionExecutor {
public:
    virtual ~LampActionExecutor() = default;
    virtual std::string Submit(const std::string& command) = 0;
    virtual std::string Inspect(const std::string& request_id) = 0;
    virtual std::string State() = 0;
    virtual std::string NewRequestId() = 0;
    virtual bool Ready() = 0;
};

class LampBridgeExecutor final : public LampActionExecutor {
public:
    explicit LampBridgeExecutor(Board& board);
    std::string Submit(const std::string& command) override;
    std::string Inspect(const std::string& request_id) override;
    std::string State() override;
    std::string Configure(const std::string& url, const std::string& token);
    std::string NewRequestId() override;
    bool Ready() override;

private:
    struct Record {
        std::string body;
        std::string operation;
        std::string delivery = "queued";
        std::string response;
        std::string error;
        bool posted = false;
        int64_t deadline = 0;
        std::string server_session;
    };
    Board& board_;
    std::mutex mutex_;
    std::map<std::string, Record> records_;
    std::deque<std::string> queue_;
    std::string url_, token_, session_, state_;
    std::string transport_error_;
    std::string last_polled_;
    std::string rest_request_;
    int64_t rest_completed_at_ = 0;
    uint32_t rest_generation_ = 0;
    TaskHandle_t worker_ = nullptr;
    int64_t sampled_at_ = 0;
    uint32_t generation_ = 0, counter_ = 0;
    bool ready_ = false;

    static void Run(void* arg);
    void Loop();
    void Refresh();
    void Process(const std::string& id, bool post);
    std::string HttpRequest(const std::string& method, const std::string& path,
                            const std::string& body, int& status, uint32_t& generation);
    std::string Receipt(const std::string& id, const Record& record);
};

void RegisterLampTools(LampActionExecutor& executor);
void RegisterLampBridgeConfiguration(LampBridgeExecutor& executor);
