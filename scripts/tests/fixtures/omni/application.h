#pragma once
#include <functional>
enum DeviceState { kDeviceStateIdle };
class Application {
public:
    static Application& GetInstance() {
        static Application a;
        return a;
    }
    DeviceState GetDeviceState() const { return kDeviceStateIdle; }
    void StartListening() {}
    void StopListening() {}
    void ToggleChatState() {}
    void CloseConversation(bool = false) {}
};
