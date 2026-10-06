#pragma once
#include <cstddef>
#include <cstdint>
#define ESP_PLATFORM 1
#define CONFIG_APPLE_LAMP_LOCAL_EXECUTOR 1
#define CONFIG_APPLE_LAMP_UART_PROBE 1
#define CONFIG_APPLE_LAMP_HARDWARE_MOTION 1
#define CONFIG_COMPILER_CXX_EXCEPTIONS 1
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1
#define CONFIG_ESP_CONSOLE_SECONDARY_NONE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using esp_err_t = int;
using nvs_handle_t = unsigned;
using TaskHandle_t = void*;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 1, NVS_READONLY = 0,
              NVS_READWRITE = 1;
constexpr int UART_NUM_0 = 0, UART_DATA_8_BITS = 8, UART_PARITY_DISABLE = 0, UART_STOP_BITS_1 = 1,
              UART_HW_FLOWCTRL_DISABLE = 0, UART_SCLK_DEFAULT = 0, UART_PIN_NO_CHANGE = -1,
              pdPASS = 1;
struct uart_config_t {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
};
#define pdMS_TO_TICKS(v) (v)
int64_t esp_timer_get_time();
uint32_t esp_random();
const char* esp_err_to_name(int);
int xTaskCreate(void (*)(void*), const char*, unsigned, void*, int, TaskHandle_t*);
void vTaskDelay(unsigned);
int uart_driver_install(int, int, int, int, void*, int);
int uart_driver_delete(int);
int uart_param_config(int, const uart_config_t*);
int uart_set_pin(int, int, int, int, int);
int uart_flush_input(int);
int uart_write_bytes(int, const void*, size_t);
int uart_read_bytes(int, void*, size_t, unsigned);
int uart_wait_tx_done(int, unsigned);
int nvs_open(const char*, int, nvs_handle_t*);
void nvs_close(nvs_handle_t);
int nvs_get_blob(nvs_handle_t, const char*, void*, size_t*);
int nvs_set_blob(nvs_handle_t, const char*, const void*, size_t);
int nvs_commit(nvs_handle_t);
constexpr int PSA_SUCCESS = 0, PSA_ALG_SHA_256 = 1;
int psa_crypto_init();
int psa_hash_compute(int, const unsigned char*, size_t, unsigned char*, size_t, size_t*);
constexpr int kDeviceStateIdle = 0, kDeviceStateListening = 1;
class Application {
public:
    static Application& GetInstance();
    template <class F>
    void Schedule(F f) {
        f();
    }
    int GetDeviceState() { return kDeviceStateIdle; }
    bool IsVoiceDetected() { return false; }
    void ToggleChatState() {}
};
