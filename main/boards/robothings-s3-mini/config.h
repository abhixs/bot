#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// RoboThings S3 Mini: ESP32-S3 Super Mini Zero (4MB flash, 2MB PSRAM),
// INMP441 mic, MAX98357A amp, SSD1306 128x64 OLED, 4 buttons, lamp relay.
// Same wiring as the original Xiaozhi-Esp32s3mini firmware.

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// 如果使用 Duplex I2S 模式，请注释下面一行
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_1
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_2
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_3

#else

#define AUDIO_I2S_GPIO_WS GPIO_NUM_4
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_7

#endif


#define BUILTIN_LED_GPIO        GPIO_NUM_21
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_11
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_9
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_10

#define DISPLAY_SDA_PIN GPIO_NUM_7
#define DISPLAY_SCL_PIN GPIO_NUM_8
#define DISPLAY_WIDTH   128

#if CONFIG_OLED_SSD1306_128X32
#define DISPLAY_HEIGHT  32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT  64
#elif CONFIG_OLED_SH1106_128X64
#define DISPLAY_HEIGHT  64
#define SH1106
#else
#error "OLED display type is not selected"
#endif

#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true


// Lamp relay (voice controlled, can also switch on with an alarm)
#define LAMP_GPIO GPIO_NUM_12

#endif // _BOARD_CONFIG_H_
