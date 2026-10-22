#pragma once

#include <driver/gpio.h>

// Apple Lamp OV2640 custom board: OV2640 DVP camera with dedicated SCCB pins
// (GPIO4/5) plus an external I2S power amplifier and an external PDM microphone.
#define BOOT_BUTTON_GPIO GPIO_NUM_0

// External I2S digital amplifier (e.g. MAX98357A). Moved to GPIO38-42 because
// the camera DVP bus occupies GPIO8-18.
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_38
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_39
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_40

// External PDM microphone (e.g. INMP441 / SPK0641HT4H).
#define AUDIO_I2S_MIC_GPIO_SCK GPIO_NUM_41
#define AUDIO_I2S_MIC_GPIO_DIN GPIO_NUM_42

#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Lamp head LED dimmer. GPIO1 is the only free output left: the camera DVP bus
// and SCCB take GPIO4-18, audio GPIO38-42, the servo UART GPIO43/44, USB
// Serial/JTAG GPIO19/20 and the octal PSRAM GPIO33-37. GPIO1 is not an
// ESP32-S3 strapping pin.
#define LAMP_LIGHT_GPIO GPIO_NUM_1

// OV2640 DVP camera wiring (dedicated SCCB, no IO expander).
// Y2..Y9 (Arduino ESP32-CAM naming) map to DVP D0..D7 respectively.
#define CAMERA_PIN_PWDN GPIO_NUM_NC
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define CAMERA_PIN_SIOD GPIO_NUM_4
#define CAMERA_PIN_SIOC GPIO_NUM_5
#define CAMERA_PIN_XCLK GPIO_NUM_15
#define CAMERA_PIN_PCLK GPIO_NUM_13
#define CAMERA_PIN_VSYNC GPIO_NUM_6
#define CAMERA_PIN_HREF GPIO_NUM_7
#define CAMERA_PIN_D0 GPIO_NUM_11  // Y2
#define CAMERA_PIN_D1 GPIO_NUM_9   // Y3
#define CAMERA_PIN_D2 GPIO_NUM_8   // Y4
#define CAMERA_PIN_D3 GPIO_NUM_10  // Y5
#define CAMERA_PIN_D4 GPIO_NUM_12  // Y6
#define CAMERA_PIN_D5 GPIO_NUM_18  // Y7
#define CAMERA_PIN_D6 GPIO_NUM_17  // Y8
#define CAMERA_PIN_D7 GPIO_NUM_16  // Y9
#define XCLK_FREQ_HZ 20000000
