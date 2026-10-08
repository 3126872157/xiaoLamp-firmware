#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffff
#define pdMS_TO_TICKS(x) (x)
using TaskHandle_t = void*;
struct FakeQueue {
    std::deque<void*> items;
    unsigned capacity;
};
using QueueHandle_t = FakeQueue*;
using SemaphoreHandle_t = int*;
inline int xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t*) {
    return pdPASS;
}
inline void vTaskDelete(void*) {}
