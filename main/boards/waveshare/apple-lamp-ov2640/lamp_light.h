#pragma once

// Lamp head LED dimmer: one LEDC channel driving the lamp output with a
// 10-bit duty cycle. The camera XCLK already owns LEDC timer 0 / channel 0, so
// the dimmer uses timer 1 / channel 1 in low-speed mode. All LEDC timers on
// ESP32-S3 share a single clock source, so this driver also asks the LEDC
// driver to pick the clock automatically instead of pinning one.
namespace lamp::light {

// Configures the PWM output. Idempotent. Returns false and leaves the dimmer
// permanently unavailable when the LEDC timer or channel cannot be configured,
// so callers can report the capability honestly instead of pretending to dim.
bool Init(int gpio);

// True only after a successful Init.
bool Available();

// Clamped to [0, 1]: 0 is off, 1 is full brightness. Ignored when unavailable.
void SetLevel(double level);

// Last level written to the hardware; 0 while unavailable.
double Level();

}  // namespace lamp::light
