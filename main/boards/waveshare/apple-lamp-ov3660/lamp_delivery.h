#pragma once
#include "sdkconfig.h"
#if CONFIG_APPLE_LAMP_DELIVERY
#include <functional>
#include "camera.h"
#include "lamp_hardware_executor.h"
void StartLampDelivery(LampHardwareExecutor& executor, Camera* camera);
std::string LampDeliveryState();
std::string LampDeliverySubmit(const std::string& body, bool panel);
std::string LampDeliveryJob(const std::string& id);
bool LampDeliveryJpeg(const std::function<void(const uint8_t*, size_t)>& visitor);
#endif
