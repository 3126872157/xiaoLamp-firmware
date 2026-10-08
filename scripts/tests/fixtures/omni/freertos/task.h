#pragma once
#include "FreeRTOS.h"
inline void vTaskDelay(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
