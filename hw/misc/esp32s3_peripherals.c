/*
 * ESP32-S3 I2S/GDMA, I2C OLED, GPIO and WS2812 transport models.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "hw/misc/esp32s3_peripherals.h"
#include "qom/object.h"
#include "qapi/visitor.h"
#include "migration/vmstate.h"
#include "exec/address-spaces.h"

static void gpio_update(Esp32S3Peripherals *s)
{
    s->gpio[0x5c / 4] = s->gpio[0x68 / 4] = s->gpio[0x44 / 4];
    s->gpio[0x60 / 4] = s->gpio[0x6c / 4] = s->gpio[0x50 / 4];
    qemu_set_irq(s->gpio_irq, !!(s->gpio[0x44 / 4] | s->gpio[0x50 / 4]));
}

static void gpio_input_set(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    Esp32S3Peripherals *s = ESP32S3_PERIPHERALS(obj);
    uint64_t value;
    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    uint64_t old = s->inputs;
    s->inputs = value;
    for (unsigned pin = 0; pin < 49; pin++) {
        bool before = (old >> pin) & 1, after = (value >> pin) & 1;
        unsigned mode = (s->gpio[(0x74 + pin * 4) / 4] >> 7) & 7;
        bool enabled = s->gpio[(0x74 + pin * 4) / 4] & (31U << 13);
        bool fire = mode == 1 ? !before && after :
                    mode == 2 ? before && !after :
                    mode == 3 ? before != after :
                    mode == 4 ? !after : mode == 5 && after;
        if (enabled && fire) {
            s->gpio[(pin < 32 ? 0x44 : 0x50) / 4] |= 1U << (pin % 32);
        }
    }
    gpio_update(s);
}

static void gpio_inputs(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    uint64_t value = ESP32S3_PERIPHERALS(obj)->inputs;
    visit_type_uint64(v, name, &value, errp);
}

static uint64_t gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    Esp32S3Peripherals *s = opaque;
    uint64_t out = s->gpio[1] | ((uint64_t)s->gpio[4] << 32);
    uint64_t enable = s->gpio[8] | ((uint64_t)s->gpio[11] << 32);
    uint64_t levels = (s->inputs & ~enable) | (out & enable);
    if (addr == 0x38) {
        return 4;
    } else if (addr == 0x3c) {
        return (uint32_t)levels;
    } else if (addr == 0x40) {
        return levels >> 32;
    }
    return s->gpio[addr / 4];
}

static void gpio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Esp32S3Peripherals *s = opaque;
    switch (addr) {
    case 0x08: case 0x14: case 0x24: case 0x30: case 0x48: case 0x54:
        s->gpio[(addr - 4) / 4] |= value;
        break;
    case 0x0c: case 0x18: case 0x28: case 0x34: case 0x4c: case 0x58:
        s->gpio[(addr - 8) / 4] &= ~value;
        break;
    default:
        s->gpio[addr / 4] = value;
    }
    gpio_update(s);
}

static void oled_byte(Esp32S3Peripherals *s, uint8_t value)
{
    if (s->oled_data) {
        s->oled[(s->oled_page & 7) * 128 + (s->oled_col & 127)] = value;
        if (++s->oled_col > s->oled_col_end) {
            s->oled_col = s->oled_col_start;
            s->oled_page = s->oled_page >= s->oled_page_end ?
                           s->oled_page_start : s->oled_page + 1;
        }
    } else if (s->oled_arg_need) {
        s->oled_args[s->oled_arg_count++] = value;
        if (s->oled_arg_count == s->oled_arg_need) {
            if (s->oled_command == 0x21) {
                s->oled_col_start = s->oled_col = s->oled_args[0] & 127;
                s->oled_col_end = s->oled_args[1] & 127;
            } else if (s->oled_command == 0x22) {
                s->oled_page_start = s->oled_page = s->oled_args[0] & 7;
                s->oled_page_end = s->oled_args[1] & 7;
            } else if (s->oled_command == 0x20) {
                s->oled_mode = value;
            }
            s->oled_arg_need = s->oled_arg_count = 0;
        }
    } else {
        s->oled_command = value;
        if (value == 0x21 || value == 0x22) {
            s->oled_arg_need = 2;
        } else if (value == 0x20 || value == 0x81 || value == 0x8d ||
                   value == 0xa8 || value == 0xd3 || value == 0xd5 ||
                   value == 0xd9 || value == 0xda || value == 0xdb) {
            s->oled_arg_need = 1;
        } else if ((value & 0xf0) == 0xb0) {
            s->oled_page = value & 7;
        } else if ((value & 0xf0) == 0x10) {
            s->oled_col = (s->oled_col & 15) | ((value & 7) << 4);
        } else if (value < 16) {
            s->oled_col = (s->oled_col & 0x70) | value;
        }
    }
}

static void i2c_irq_update(Esp32S3Peripherals *s)
{
    s->i2c[0x2c / 4] = s->i2c[0x20 / 4] & s->i2c[0x28 / 4];
    qemu_set_irq(s->i2c_irq, !!s->i2c[0x2c / 4]);
}

static void i2c_transaction(Esp32S3Peripherals *s)
{
    unsigned consumed = 0;
    uint32_t interrupt = 0;
    bool nack = false;
    for (unsigned cmd = 0; cmd < 8; cmd++) {
        uint32_t word = s->i2c[0x58 / 4 + cmd];
        unsigned opcode = (word >> 11) & 7, count = word & 255;
        if (opcode == 6) {
            s->i2c_addressed = false;
        } else if (opcode == 1) {
            for (unsigned i = 0; i < count; i++) {
                if (consumed >= s->fifo_len) {
                    nack = true;
                    break;
                }
                uint8_t value = s->fifo[consumed++];
                if (!s->i2c_addressed) {
                    nack = (value >> 1) != 0x3c || (value & 1);
                    s->i2c_addressed = true;
                    s->oled_control = true;
                } else if (s->oled_control) {
                    s->oled_data = !!(value & 0x40);
                    s->oled_control = false;
                } else if (!nack) {
                    oled_byte(s, value);
                }
            }
        }
        s->i2c[0x58 / 4 + cmd] |= 1U << 31;
        if (opcode == 2 || opcode == 4 || nack) {
            interrupt = opcode == 2 ? 1U << 7 : 1U << 3;
            break;
        }
    }
    s->fifo_len = 0;
    s->i2c[0x20 / 4] |= nack ? 1U << 10 : interrupt;
    s->i2c[1] &= ~(1U << 5);
    i2c_irq_update(s);
}

static uint64_t i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    Esp32S3Peripherals *s = opaque;
    if (addr == 8) {
        return s->fifo_len << 18;
    }
    return s->i2c[addr / 4];
}

static void i2c_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Esp32S3Peripherals *s = opaque;
    if (addr == 0x1c) {
        if (s->fifo_len < sizeof(s->fifo)) {
            s->fifo[s->fifo_len++] = value;
        }
    } else if (addr == 0x24) {
        s->i2c[0x20 / 4] &= ~value;
    } else {
        s->i2c[addr / 4] = value;
        if (addr == 0x18 && (value & (3U << 12))) {
            s->fifo_len = 0;
        }
        if (addr == 4) {
            s->i2c[1] &= ~(1U << 11);
            if (value & (1U << 5)) {
                i2c_transaction(s);
            }
        }
    }
    i2c_irq_update(s);
}

static void rmt_irq_update(Esp32S3Peripherals *s)
{
    s->rmt[0x74 / 4] = s->rmt[0x70 / 4] & s->rmt[0x78 / 4];
    qemu_set_irq(s->rmt_irq, !!s->rmt[0x74 / 4]);
}

static uint64_t rmt_read(void *opaque, hwaddr addr, unsigned size)
{
    return ((Esp32S3Peripherals *)opaque)->rmt[addr / 4];
}

static void rmt_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Esp32S3Peripherals *s = opaque;
    if (addr == 0x7c) {
        s->rmt[0x70 / 4] &= ~value;
    } else {
        s->rmt[addr / 4] = value;
        if (addr >= 0x20 && addr < 0x30) {
            unsigned ch = (addr - 0x20) / 4;
            s->rmt[addr / 4] &= ~((1U << 24) | 7);
            if (value & 1) {
                uint32_t grb = 0;
                for (unsigned i = 0; i < 24; i++) {
                    uint32_t symbol = s->rmt[0x800 / 4 + ch * 48 + i];
                    grb = (grb << 1) | ((symbol & 0x7fff) >
                                             ((symbol >> 16) & 0x7fff));
                }
                s->led_rgb = ((grb & 0xff00) << 8) |
                             ((grb >> 8) & 0xff00) | (grb & 255);
                s->rmt[0x70 / 4] |= 1U << ch;
            }
        }
    }
    rmt_irq_update(s);
}

static uint64_t i2s_read(void *opaque, hwaddr addr, unsigned size)
{
    return ((Esp32S3Port *)opaque)->regs[addr / 4];
}

static void i2s_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Esp32S3Port *p = opaque;
    p->regs[addr / 4] = value;
    if (addr == 0x20 || addr == 0x24) {
        unsigned dir = addr == 0x20 ? 0 : 1;
        p->regs[addr / 4] &= ~(1U << 8);
        if (!(value & 4) || (value & 1)) {
            p->cursor[dir] = p->link[dir] = 0;
        }
    }
}

static void audio_tick(void *opaque)
{
    Esp32S3Peripherals *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned port = 0; port < 2; port++) {
        Esp32S3Port *p = &s->i2s[port];
        for (unsigned dir = 0; dir < 2; dir++) {
            if (!(p->regs[(dir ? 0x24 : 0x20) / 4] & 4) ||
                now < p->next_ns[dir]) {
                continue;
            }
            unsigned channel = 5;
            for (unsigned c = 0; c < 5; c++) {
                if ((s->gdma->ch_conf[dir][c].peripheral & 63) ==
                    GDMA_I2S0 + port) {
                    channel = c;
                    break;
                }
            }
            if (channel == 5) {
                continue;
            }
            DmaConfigState *conf = &s->gdma->ch_conf[dir][channel];
            uint32_t link = 0x3fc00000 | (conf->link & 0xfffff);
            if (!p->cursor[dir] || p->link[dir] != link) {
                p->cursor[dir] = p->link[dir] = link;
            }
            uint32_t desc[3];
            if (address_space_read(&s->gdma->dma_as, p->cursor[dir],
                    MEMTXATTRS_UNSPECIFIED, desc, sizeof(desc)) != MEMTX_OK) {
                continue;
            }
            uint32_t flags = le32_to_cpu(desc[0]);
            unsigned bytes = dir ? (flags >> 12) & 4095 : flags & 4095;
            if (!bytes || bytes > 4092 || bytes % 4 ||
                ((conf->conf1 & (1U << 12)) && !(flags & (1U << 31)))) {
                continue;
            }
            uint32_t samples[1024] = {0};
            uint32_t buffer = le32_to_cpu(desc[1]);
            if (!dir) {
                for (unsigned i = 0; i < bytes / 4; i++) {
                    uint16_t sample = 0;
                    if (s->input_active && s->input &&
                        fread(&sample, 2, 1, s->input) != 1 &&
                        ferror(s->input)) {
                        error_report("I2S PCM input read failed");
                        exit(EXIT_FAILURE);
                    }
                    int32_t value = (int16_t)le16_to_cpu(sample);
                    samples[i] = cpu_to_le32((uint32_t)value << 12);
                }
                if (address_space_write(&s->gdma->dma_as, buffer,
                        MEMTXATTRS_UNSPECIFIED, samples, bytes) != MEMTX_OK) {
                    continue;
                }
                flags = (flags & ~0x80fff000U) | (bytes << 12) | (1U << 30);
                s->rx_bytes += bytes;
            } else {
                if (address_space_read(&s->gdma->dma_as, buffer,
                        MEMTXATTRS_UNSPECIFIED, samples, bytes) != MEMTX_OK) {
                    continue;
                }
                if (s->output) {
                    for (unsigned i = 0; i < bytes / 4; i++) {
                        uint16_t sample = cpu_to_le16((int32_t)
                                               le32_to_cpu(samples[i]) >> 16);
                        if (fwrite(&sample, 2, 1, s->output) != 1) {
                            error_report("I2S PCM output write failed");
                            exit(EXIT_FAILURE);
                        }
                    }
                    if (fflush(s->output)) {
                        error_report("I2S PCM output flush failed");
                        exit(EXIT_FAILURE);
                    }
                }
                flags &= ~(1U << 31);
                s->tx_bytes += bytes;
            }
            desc[0] = cpu_to_le32(flags);
            if (address_space_write(&s->gdma->dma_as, p->cursor[dir],
                    MEMTXATTRS_UNSPECIFIED, desc, 4) != MEMTX_OK) {
                continue;
            }
            conf->suc_eof_desc_addr = p->cursor[dir];
            conf->int_state.raw |= 3;
            conf->int_state.st = conf->int_state.raw & conf->int_state.ena;
            p->cursor[dir] = le32_to_cpu(desc[2]);
            p->next_ns[dir] = now + (int64_t)(bytes / 4) * 1000000000 /
                                   (dir ? s->output_rate : s->input_rate);
            qemu_set_irq(conf->int_state.irq, !!conf->int_state.st);
        }
    }
    timer_mod(s->timer, now + 1000000);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read, .write = gpio_write, .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps i2c_ops = {
    .read = i2c_read, .write = i2c_write, .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps rmt_ops = {
    .read = rmt_read, .write = rmt_write, .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps i2s_ops = {
    .read = i2s_read, .write = i2s_write, .endianness = DEVICE_LITTLE_ENDIAN,
};

static char *oled_image(Object *obj, Error **errp)
{
    Esp32S3Peripherals *s = ESP32S3_PERIPHERALS(obj);
    GString *result = g_string_new("P1\n128 64\n");
    for (unsigned y = 0; y < 64; y++) {
        for (unsigned x = 0; x < 128; x++) {
            g_string_append_c(result,
                (s->oled[y / 8 * 128 + x] >> (y % 8)) & 1 ? '1' : '0');
            g_string_append_c(result, x == 127 ? '\n' : ' ');
        }
    }
    return g_string_free(result, false);
}

static void peripherals_realize(DeviceState *dev, Error **errp)
{
    Esp32S3Peripherals *s = ESP32S3_PERIPHERALS(dev);
    if (!s->gdma || !s->input_rate || !s->output_rate) {
        error_setg(errp, "I2S requires GDMA and nonzero sample rates");
        return;
    }
    if (s->pcm_input) {
        s->input = fopen(s->pcm_input, "rb");
    }
    if (s->pcm_input && !s->input) {
        error_setg_errno(errp, errno, "Cannot open I2S input");
        return;
    }
    if (s->pcm_output) {
        s->output = fopen(s->pcm_output, "wb");
    }
    if (s->pcm_output && !s->output) {
        error_setg_errno(errp, errno, "Cannot open I2S output");
        if (s->input) {
            fclose(s->input);
            s->input = NULL;
        }
        return;
    }
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
}

static void peripherals_reset(DeviceState *dev)
{
    Esp32S3Peripherals *s = ESP32S3_PERIPHERALS(dev);
    memset(s->gpio, 0, sizeof(s->gpio));
    memset(s->i2c, 0, sizeof(s->i2c));
    memset(s->rmt, 0, sizeof(s->rmt));
    memset(s->oled, 0, sizeof(s->oled));
    for (unsigned i = 0; i < 2; i++) {
        memset(s->i2s[i].regs, 0, sizeof(s->i2s[i].regs));
        memset(s->i2s[i].cursor, 0, sizeof(s->i2s[i].cursor));
        memset(s->i2s[i].link, 0, sizeof(s->i2s[i].link));
        memset(s->i2s[i].next_ns, 0, sizeof(s->i2s[i].next_ns));
    }
    s->inputs = UINT64_MAX;
    s->led_rgb = s->fifo_len = 0;
    s->oled_col = s->oled_page = s->oled_mode = s->oled_command = 0;
    s->oled_arg_count = s->oled_arg_need = 0;
    s->oled_col_start = s->oled_page_start = 0;
    s->oled_col_end = 127;
    s->oled_page_end = 7;
    s->oled_data = s->i2c_addressed = s->oled_control = false;
    s->input_active = s->rx_bytes = s->tx_bytes = 0;
    if (s->input) {
        rewind(s->input);
    }
    gpio_update(s);
    i2c_irq_update(s);
    rmt_irq_update(s);
}

static void peripherals_finalize(Object *obj)
{
    Esp32S3Peripherals *s = ESP32S3_PERIPHERALS(obj);
    timer_free(s->timer);
    if (s->input) {
        fclose(s->input);
    }
    if (s->output) {
        fclose(s->output);
    }
}

static const VMStateDescription peripherals_vmstate = {
    .name = TYPE_ESP32S3_PERIPHERALS,
    .unmigratable = true,
};

static void peripherals_init(Object *obj)
{
    Esp32S3Peripherals *s = ESP32S3_PERIPHERALS(obj);
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->gpio_region, obj, &gpio_ops, s,
                         "s3-gpio", 0x1000);
    memory_region_init_io(&s->i2c_region, obj, &i2c_ops, s, "s3-i2c", 0x100);
    memory_region_init_io(&s->rmt_region, obj, &rmt_ops, s, "s3-rmt", 0x1000);
    sysbus_init_mmio(dev, &s->gpio_region);
    sysbus_init_mmio(dev, &s->i2c_region);
    sysbus_init_mmio(dev, &s->rmt_region);
    for (unsigned i = 0; i < 2; i++) {
        s->i2s[i].parent = s;
        s->i2s[i].index = i;
        memory_region_init_io(&s->i2s[i].region, obj, &i2s_ops,
                             &s->i2s[i], "s3-i2s", 0x1000);
        sysbus_init_mmio(dev, &s->i2s[i].region);
    }
    sysbus_init_irq(dev, &s->gpio_irq);
    sysbus_init_irq(dev, &s->i2c_irq);
    sysbus_init_irq(dev, &s->rmt_irq);
    s->inputs = UINT64_MAX;
    s->oled_col_end = 127;
    s->oled_page_end = 7;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, audio_tick, s);
    object_property_add_uint64_ptr(obj, "rx-bytes", &s->rx_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "input-active", &s->input_active,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "tx-bytes", &s->tx_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "gpio-inputs", "uint64", gpio_inputs,
                        gpio_input_set, NULL, NULL);
    object_property_add_uint32_ptr(obj, "led-rgb", &s->led_rgb,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_str(obj, "oled-pbm", oled_image, NULL);
}

static Property peripherals_props[] = {
    DEFINE_PROP_LINK("gdma", Esp32S3Peripherals, gdma,
                     TYPE_ESP_GDMA, ESPGdmaState *),
    DEFINE_PROP_STRING("pcm-in", Esp32S3Peripherals, pcm_input),
    DEFINE_PROP_STRING("pcm-out", Esp32S3Peripherals, pcm_output),
    DEFINE_PROP_UINT32("input-rate", Esp32S3Peripherals, input_rate, 16000),
    DEFINE_PROP_UINT32("output-rate", Esp32S3Peripherals, output_rate, 24000),
    DEFINE_PROP_END_OF_LIST(),
};

static void peripherals_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = peripherals_realize;
    device_class_set_legacy_reset(dc, peripherals_reset);
    dc->vmsd = &peripherals_vmstate;
    device_class_set_props(dc, peripherals_props);
}

static const TypeInfo peripherals_type = {
    .name = TYPE_ESP32S3_PERIPHERALS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32S3Peripherals),
    .instance_init = peripherals_init,
    .instance_finalize = peripherals_finalize,
    .class_init = peripherals_class_init,
};
static void peripherals_register(void)
{
    type_register_static(&peripherals_type);
}
type_init(peripherals_register)
