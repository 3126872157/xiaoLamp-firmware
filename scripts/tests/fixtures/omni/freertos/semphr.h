#pragma once
#include "FreeRTOS.h"
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return new int(0); }
inline int xSemaphoreTake(SemaphoreHandle_t, unsigned) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t p) {
    *p = 1;
    return 1;
}
inline void vSemaphoreDelete(SemaphoreHandle_t p) { delete p; }
