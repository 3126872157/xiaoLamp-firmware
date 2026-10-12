#pragma once
#include "FreeRTOS.h"
inline QueueHandle_t xQueueCreate(unsigned n, unsigned) { return new FakeQueue{{}, n}; }
inline unsigned uxQueueMessagesWaiting(QueueHandle_t q) {
    std::lock_guard lock(q->mutex);
    return static_cast<unsigned>(q->items.size());
}
inline int xQueueSend(QueueHandle_t q, void* p, unsigned wait) {
    {
        std::lock_guard lock(q->mutex);
        if (q->items.size() < q->capacity) {
            q->items.push_back(*static_cast<void**>(p));
            return 1;
        }
    }
    if (wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(wait));
    return 0;
}
inline int xQueueReceive(QueueHandle_t q, void* p, unsigned) {
    std::lock_guard lock(q->mutex);
    if (q->items.empty())
        return 0;
    *static_cast<void**>(p) = q->items.front();
    q->items.pop_front();
    return 1;
}
inline void vQueueDelete(QueueHandle_t q) { delete q; }
