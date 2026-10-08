#pragma once
#include "FreeRTOS.h"
inline QueueHandle_t xQueueCreate(unsigned n, unsigned) { return new FakeQueue{{}, n}; }
inline int xQueueSend(QueueHandle_t q, void* p, unsigned) {
    if (q->items.size() == q->capacity)
        return 0;
    q->items.push_back(*static_cast<void**>(p));
    return 1;
}
inline int xQueueReceive(QueueHandle_t q, void* p, unsigned) {
    if (q->items.empty())
        return 0;
    *static_cast<void**>(p) = q->items.front();
    q->items.pop_front();
    return 1;
}
inline void vQueueDelete(QueueHandle_t q) { delete q; }
