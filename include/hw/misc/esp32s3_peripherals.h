/*
 * ESP32-S3 board-level digital and audio peripherals.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ESP32S3_PERIPHERALS_H
#define ESP32S3_PERIPHERALS_H
#include "hw/sysbus.h"
#include "hw/dma/esp_gdma.h"
#include "qemu/timer.h"
#define TYPE_ESP32S3_PERIPHERALS "esp32s3.peripherals"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32S3Peripherals, ESP32S3_PERIPHERALS)
typedef struct Esp32S3Port {
    Esp32S3Peripherals *parent;
    MemoryRegion region;
    unsigned index;
    uint32_t regs[1024];
    uint32_t cursor[2];
    uint32_t link[2];
    int64_t next_ns[2];
} Esp32S3Port;
struct Esp32S3Peripherals {
    SysBusDevice parent_obj;
    ESPGdmaState *gdma;
    MemoryRegion gpio_region, rmt_region, i2c_region;
    qemu_irq gpio_irq, rmt_irq, i2c_irq;
    Esp32S3Port i2s[2];
    Esp32S3Port mcpwm[2];
    QEMUTimer *timer;
    uint32_t gpio[1024], rmt[1024], i2c[64];
    uint64_t inputs;
    uint32_t led_rgb;
    uint8_t fifo[32], fifo_len;
    uint8_t oled[1024], oled_col, oled_page, oled_mode, oled_command;
    uint8_t oled_args[2], oled_arg_count, oled_arg_need;
    uint8_t oled_col_start, oled_col_end, oled_page_start, oled_page_end;
    bool oled_data, i2c_addressed, oled_control;
    char *pcm_input, *pcm_output;
    FILE *input, *output;
    uint32_t input_rate, output_rate;
    uint64_t rx_bytes, tx_bytes;
    uint64_t input_active;
};
#endif
