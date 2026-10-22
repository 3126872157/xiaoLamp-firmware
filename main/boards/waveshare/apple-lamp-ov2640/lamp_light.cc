#include "lamp_light.h"

#include <driver/ledc.h>
#include <esp_log.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace lamp::light {
namespace {
constexpr const char* kTag = "LampLight";

// The camera drives its XCLK through LEDC timer 0 / channel 0.
constexpr ledc_timer_t kTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_1;
constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kFrequencyHz = 20000;
// Full scale keeps one real PWM edge per period instead of a permanently high
// pin; 1/1023 of the range is not visible.
constexpr uint32_t kFullDuty = (1u << 10) - 1;

// Negative until the channel is configured, so Available() cannot report a
// ready dimmer after a failed configuration.
std::atomic<int> g_duty{-1};
}  // namespace

bool Init(int gpio) {
    static std::atomic<bool> configured{false};
    bool expected = false;
    if (!configured.compare_exchange_strong(expected, true))
        return Available();

    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = kResolution;
    timer.timer_num = kTimer;
    timer.freq_hz = kFrequencyHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGE(kTag, "LEDC timer %d rejected %u Hz at 10-bit resolution", kTimer, kFrequencyHz);
        return false;
    }

    ledc_channel_config_t channel = {};
    channel.gpio_num = gpio;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = kChannel;
    channel.timer_sel = kTimer;
    channel.duty = 0;
    channel.hpoint = 0;
    if (ledc_channel_config(&channel) != ESP_OK) {
        ESP_LOGE(kTag, "LEDC channel %d cannot drive GPIO%d; lamp dimmer disabled", kChannel, gpio);
        return false;
    }

    g_duty.store(0);
    ESP_LOGI(kTag, "Lamp dimmer ready on GPIO%d, %u Hz, 10-bit duty, off at boot", gpio,
             kFrequencyHz);
    return true;
}

bool Available() { return g_duty.load() >= 0; }

void SetLevel(double level) {
    const int applied = g_duty.load();
    if (applied < 0)
        return;
    const double demanded = std::isfinite(level) ? std::clamp(level, 0.0, 1.0) : 0.0;
    const auto duty = static_cast<uint32_t>(std::lround(demanded * kFullDuty));
    if (duty == static_cast<uint32_t>(applied))
        return;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, kChannel, duty) != ESP_OK ||
        ledc_update_duty(LEDC_LOW_SPEED_MODE, kChannel) != ESP_OK) {
        ESP_LOGE(kTag, "LEDC duty write failed; the lamp output may be stale");
        return;
    }
    g_duty.store(static_cast<int>(duty));
}

double Level() {
    const int duty = g_duty.load();
    return duty <= 0 ? 0.0 : static_cast<double>(duty) / kFullDuty;
}

}  // namespace lamp::light
