/*
 * ATBM6441 "Hera" UART
 *
 * An Andes APB UART: a 16550 whose registers sit on 4-byte strides from
 * +0x20 (RBR/THR/DLL, IER/DLM, IIR/FCR/PSR, LCR, MCR, LSR, MSR, SPR), with
 * a few Andes registers around them. The 16550 core is QEMU's serial-mm
 * behind a window at +0x20; the SDK's drivers touch these registers a byte
 * at a time and use the 16-byte FIFOs with the trigger levels FCR sets:
 *   0x14 DIV   8-bit, the oversampling count the driver sets with the
 *              baud divisor; kept, it does not change the byte rate here
 *   0x28 PSR   the prescaler, reached with LCR.DLAB set where a 16550 has
 *              IIR/FCR; kept apart from FCR so it cannot turn the FIFOs on
 *   0x40 MDR   mode definition, 0x44 ACR auxiliary control: kept
 *   0x00-0x10  ID/revision and configuration: not documented, read as 0
 * The clock is 40 MHz (UARTC_CLOCK), so the 16x baud base is 2.5 MHz.
 *
 * The receive FIFO is fed from the chardev. Nothing models the IR modes.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/serial-mm.h"
#include "migration/vmstate.h"

#define TYPE_ATBM_HERA_UART "atbm-hera-uart"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraUartState, ATBM_HERA_UART)

#define UART_DIV        0x14
#define UART_CORE       0x20
#define UART_CORE_END   0x40
#define UART_PSR        0x28
#define UART_LCR        0x2c
#define UART_MDR        0x40
#define UART_ACR        0x44
#define UART_SIZE       0x100

#define LCR_DLAB        0x80
#define UART_BAUDBASE   (40000000 / 16)

struct AtbmHeraUartState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SerialMM core;
    qemu_irq irq;

    uint8_t div;
    uint8_t psr;
    uint8_t lcr;
    uint32_t mdr;
    uint32_t acr;
};

static MemoryRegion *uart_core_mr(AtbmHeraUartState *s)
{
    return sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->core), 0);
}

static uint64_t uart_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraUartState *s = opaque;
    uint64_t val = 0;

    if (addr >= UART_CORE && addr < UART_CORE_END) {
        if (addr == UART_PSR && (s->lcr & LCR_DLAB)) {
            return s->psr;
        }
        memory_region_dispatch_read(uart_core_mr(s), addr - UART_CORE, &val,
                                    size_memop(size) | MO_LE,
                                    MEMTXATTRS_UNSPECIFIED);
        return val;
    }
    switch (addr) {
    case UART_DIV:
        return s->div;
    case UART_MDR:
        return s->mdr;
    case UART_ACR:
        return s->acr;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-uart: read at 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void uart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraUartState *s = opaque;

    if (addr >= UART_CORE && addr < UART_CORE_END) {
        if (addr == UART_PSR && (s->lcr & LCR_DLAB)) {
            s->psr = val;
            return;
        }
        if (addr == UART_LCR) {
            s->lcr = val;
        }
        memory_region_dispatch_write(uart_core_mr(s), addr - UART_CORE, val,
                                     size_memop(size) | MO_LE,
                                     MEMTXATTRS_UNSPECIFIED);
        return;
    }
    switch (addr) {
    case UART_DIV:
        s->div = val;
        break;
    case UART_MDR:
        s->mdr = val;
        break;
    case UART_ACR:
        s->acr = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-uart: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    }
}

static const MemoryRegionOps uart_ops = {
    .read = uart_read,
    .write = uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void uart_core_irq(void *opaque, int n, int level)
{
    AtbmHeraUartState *s = opaque;

    qemu_set_irq(s->irq, level);
}

static void uart_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraUartState *s = ATBM_HERA_UART(obj);

    s->div = 0;
    s->psr = 0;
    s->lcr = 0;
    s->mdr = 0;
    s->acr = 0;
}

static void uart_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraUartState *s = ATBM_HERA_UART(dev);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->core), errp)) {
        return;
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->core), 0,
                       qdev_get_gpio_in(dev, 0));
    memory_region_init_io(&s->iomem, OBJECT(dev), &uart_ops, s,
                          TYPE_ATBM_HERA_UART, UART_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void uart_init(Object *obj)
{
    AtbmHeraUartState *s = ATBM_HERA_UART(obj);

    object_initialize_child(obj, "core", &s->core, TYPE_SERIAL_MM);
    qdev_prop_set_uint8(DEVICE(&s->core), "regshift", 2);
    qdev_prop_set_uint32(DEVICE(&s->core), "baudbase", UART_BAUDBASE);
    object_property_add_alias(obj, "chardev", OBJECT(&s->core), "chardev");
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in(DEVICE(obj), uart_core_irq, 1);
}

static const VMStateDescription vmstate_uart = {
    .name = TYPE_ATBM_HERA_UART,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(div, AtbmHeraUartState),
        VMSTATE_UINT8(psr, AtbmHeraUartState),
        VMSTATE_UINT8(lcr, AtbmHeraUartState),
        VMSTATE_UINT32(mdr, AtbmHeraUartState),
        VMSTATE_UINT32(acr, AtbmHeraUartState),
        VMSTATE_END_OF_LIST()
    }
};

static void uart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = uart_realize;
    dc->vmsd = &vmstate_uart;
    rc->phases.hold = uart_reset_hold;
}

static const TypeInfo uart_info = {
    .name = TYPE_ATBM_HERA_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraUartState),
    .instance_init = uart_init,
    .class_init = uart_class_init,
};

static void uart_register_types(void)
{
    type_register_static(&uart_info);
}

type_init(uart_register_types)
