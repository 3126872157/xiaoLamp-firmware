#pragma once
#include <atomic>
#include <set>
#include "lamp_local_executor.h"
class LampHardwareExecutor final : public LampLocalExecutor {
public:
    LampHardwareExecutor();
    std::string Submit(const std::string& command) override;
    std::string Inspect(const std::string& request_id) override;
    std::string State() override;
    bool Ready() override;
    std::string PanelSubmit(const std::string& body);
    std::string PanelConfig(const std::string& candidate = "");

private:
    struct Record {
        std::string signature, operation, status, reason;
        unsigned step = 0, total = 0;
    };
    std::map<std::string, Record> records_;
    std::deque<std::string> order_;
    std::string queued_, active_;
    std::atomic<uint32_t> epoch_{0};
    bool armed_ = false;
    bool manual_ = false;
    std::optional<lamp::pose::Pose> last_target_;
    std::string SubmitChecked(const std::string& body, bool panel);
    bool SaveInstallation(const cJSON* mapping);
    void LoadInstallation();
    int mode_ = 0;
    std::set<std::string> turns_;
    int64_t dialogue_at_ = 0;
    uint32_t rest_epoch_ = 0;
    int64_t rest_at_ = 0;
    static void RunHardware(void* self);
    void HardwareLoop();
    void Execute(const std::string& body, uint32_t token);
    bool Positions(lamp::pose::Pose& q);
    bool ConfirmedRead(unsigned index, uint8_t command);
    std::string io_error_;
    bool endpoint_timeout_ = false;
    bool Write(unsigned id, uint8_t cmd, std::initializer_list<uint8_t> params = {});
    bool Target(const lamp::pose::Pose& q, unsigned milliseconds, uint32_t token);
    bool Torque(bool enabled, uint32_t token);
    bool Play(const lamp::pose::Plan& plan, uint32_t token);
    bool RecordCalibration(const std::string& target, uint32_t token);
    void Finish(const std::string& id, const std::string& status, const std::string& reason);
    std::string Receipt(const std::string& id, const Record& record);
    bool StopMotors();
    void LoadStored();
    void CloseRestChat();
    bool live_ = false;
    uint32_t io_token_ = 0;
    bool ReadCancelled() const override { return io_token_ != epoch_.load(); }
};
