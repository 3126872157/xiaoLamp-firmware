#pragma once
#include <cstdint>
inline int64_t omni_test_time = 1000000;
inline int64_t esp_timer_get_time() { return omni_test_time; }
