#pragma once
#include <array>
#include "lamp_bridge.h"
class LampLocalExecutor final : public LampActionExecutor {
public:
    LampLocalExecutor();
    std::string Submit(const std::string& command) override;
    std::string Inspect(const std::string& request_id) override;
    std::string State() override;
    std::string NewRequestId() override;
    bool Ready() override {
        return false;
    }  // No physical motion capability during read-only bringup.
private:
    struct Servo {
        int position = 0, temperature = -1, voltage_mv = -1, mode = -1, torque = -1;
        int64_t position_at = 0, temperature_at = 0, voltage_at = 0, mode_at = 0, torque_at = 0;
        unsigned failures = 0;
        std::string last_error;
    };
    std::mutex mutex_;
    std::array<Servo, 5> servos_;
    uint32_t counter_ = 0;
    bool uart_enabled_ = false;
    std::string setup_error_;
    static void Run(void* self);
    void Loop();
    bool Read(unsigned index, uint8_t command);
};
