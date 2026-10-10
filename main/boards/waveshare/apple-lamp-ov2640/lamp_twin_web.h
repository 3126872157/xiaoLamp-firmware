#pragma once
#include "lamp_hardware_executor.h"
// Called only after Wi-Fi has connected; owns an HTTP task, never UART.
void StartLampTwinWeb(LampHardwareExecutor& executor);

#if CONFIG_APPLE_LAMP_EDGE_RUNTIME
class LampVision;
void SetLampVision(LampVision* vision);
#endif
