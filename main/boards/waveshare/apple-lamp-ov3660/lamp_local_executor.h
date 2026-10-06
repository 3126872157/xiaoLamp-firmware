#pragma once
#include <array>
#include "lamp_bridge.h"
#include "lamp_pose_plan.h"
class LampLocalExecutor : public LampActionExecutor {
public:
    explicit LampLocalExecutor(bool start_worker = true);
    std::string Submit(const std::string& command) override;
    std::string Inspect(const std::string& request_id) override;
    std::string State() override;
    std::string NewRequestId() override;
    bool Ready() override {
        return false;
    }  // No physical motion capability during read-only bringup.
protected:
    struct Servo {
        int position = 0, temperature = -1, voltage_mv = -1, mode = -1, torque = -1;
        int limit_min = -1, limit_max = -1;
        int64_t limits_at = 0;
        int64_t position_at = 0, temperature_at = 0, voltage_at = 0, mode_at = 0, torque_at = 0;
        unsigned failures = 0;
        std::string last_error;
    };
    std::mutex mutex_;
    std::array<Servo, 5> servos_;
    std::optional<lamp::pose::Configuration> configuration_;
    std::string configuration_error_;
    uint32_t counter_ = 0;
    bool uart_enabled_ = false;
    std::string setup_error_;
    static void Run(void* self);
    void Loop();
    bool Read(unsigned index, uint8_t command);
    virtual bool ReadCancelled() const { return false; }
};
