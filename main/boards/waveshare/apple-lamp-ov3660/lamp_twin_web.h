#pragma once
#include "lamp_hardware_executor.h"
// Called only after Wi-Fi has connected; owns an HTTP task, never UART.
void StartLampTwinWeb(LampHardwareExecutor& executor);
