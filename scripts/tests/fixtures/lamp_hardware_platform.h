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
// LEDC is only reached by the boards that carry a PWM lamp dimmer; the harness
// records the duty so the test can check the real mapping.
enum gpio_num_t {
    GPIO_NUM_NC = -1,
    GPIO_NUM_0 = 0,
    GPIO_NUM_1,
    GPIO_NUM_2,
    GPIO_NUM_3,
    GPIO_NUM_4,
    GPIO_NUM_5,
    GPIO_NUM_6,
    GPIO_NUM_7,
    GPIO_NUM_8,
    GPIO_NUM_9,
    GPIO_NUM_10,
    GPIO_NUM_11,
    GPIO_NUM_12,
    GPIO_NUM_13,
    GPIO_NUM_14,
    GPIO_NUM_15,
    GPIO_NUM_16,
    GPIO_NUM_17,
    GPIO_NUM_18,
    GPIO_NUM_19,
    GPIO_NUM_20,
    GPIO_NUM_21
};
constexpr int LEDC_LOW_SPEED_MODE = 0, LEDC_AUTO_CLK = 0, LEDC_TIMER_10_BIT = 10, LEDC_TIMER_0 = 0,
              LEDC_TIMER_1 = 1, LEDC_CHANNEL_0 = 0, LEDC_CHANNEL_1 = 1;
using ledc_mode_t = int;
using ledc_timer_t = int;
using ledc_channel_t = int;
using ledc_timer_bit_t = int;
using ledc_clk_cfg_t = int;
struct ledc_timer_config_t {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
    bool deconfigure;
};
struct ledc_channel_config_t {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
    struct {
        unsigned int output_invert : 1;
    } flags;
    bool deconfigure;
};
int ledc_timer_config(const ledc_timer_config_t*);
int ledc_channel_config(const ledc_channel_config_t*);
int ledc_set_duty(ledc_mode_t, ledc_channel_t, uint32_t);
int ledc_update_duty(ledc_mode_t, ledc_channel_t);
int ledc_stop(ledc_mode_t, ledc_channel_t, uint32_t);
constexpr int kDeviceStateIdle = 0, kDeviceStateListening = 1, kDeviceStateSpeaking = 2;
// The harness drives idle/speaking so the staged dialogue path can be exercised
// without a real audio pipeline.
inline int lamp_test_device_state = kDeviceStateIdle;
class Application {
public:
    static Application& GetInstance();
    template <class F>
    void Schedule(F f) {
        f();
    }
    int GetDeviceState() { return lamp_test_device_state; }
    bool IsVoiceDetected() { return false; }
    void ToggleChatState() {}
};
