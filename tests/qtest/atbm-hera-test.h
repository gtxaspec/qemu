/*
 * Shared helpers for the ATBM6441 "Hera" device qtests
 *
 * The qtests run the machine without a CPU, as a bare set of devices at
 * the chip's addresses: MMIO is driven by the test and the interrupt
 * lines are observed at the machine's interrupt sink, whose inputs are
 * the chip's interrupt numbers. qtest wants a CPU for its address space,
 * so the tests give the riscv32 target's, left idle and unwired.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TESTS_ATBM_HERA_TEST_H
#define TESTS_ATBM_HERA_TEST_H

#include "libqtest.h"
#include "qemu/bitops.h"

#define HERA_ILM        0x00030000
#define HERA_FLASH      0x00080000
#define HERA_FLASH_NC   0x00400000
#define HERA_DTCM       0x00800000
#define HERA_SRAM       0x09000000
#define HERA_HIF        0x0ab00000
#define HERA_DMA        0x14000000
#define HERA_SMU        0x16100000
#define HERA_UART1      0x16200000
#define HERA_UART2      0x16300000
#define HERA_PIT1       0x16400000
#define HERA_PIT2       0x16500000
#define HERA_WDT        0x16600000
#define HERA_RTC        0x16700000
#define HERA_GPIO       0x16800000
#define HERA_I2C        0x16900000
#define HERA_SPI1       0x16a00000
#define HERA_EFUSE      0x16b00000
#define HERA_SPI2       0x16c00000
#define HERA_SPI3       0x16d00000
#define HERA_PIT3       0x17000000
#define HERA_RTC2       0x17100000
#define HERA_PINMUX     0x17400000
#define HERA_WDT2       0x17500000

/* the chip's interrupt numbers (dev_int.h) */
#define HERA_IRQ_DMA    7
#define HERA_IRQ_HIF    10
#define HERA_IRQ_SPI3   13
#define HERA_IRQ_RTC2_P 15
#define HERA_IRQ_RTC2_A 16
#define HERA_IRQ_PIT_US 17
#define HERA_IRQ_PIT    18
#define HERA_IRQ_UART1  19
#define HERA_IRQ_UART2  20
#define HERA_IRQ_I2C    22
#define HERA_IRQ_GPIO   23
#define HERA_IRQ_RTC_P  25
#define HERA_IRQ_RTC_A  26
#define HERA_IRQ_PMU    30
#define HERA_IRQ_UART3  31
#define HERA_IRQ_NMI    32      /* the watchdogs' line, past the 32 */

static inline QTestState *hera_init(const char *args)
{
    QTestState *qts = qtest_initf("-machine atbm-hera -cpu rv32 %s",
                                  args ? args : "");

    qtest_irq_intercept_in(qts, "/machine/intsink");
    return qts;
}

/*
 * qtest watches the lines of one device at a time: this is the interrupt
 * sink's. A test that needs another device's lines starts the machine
 * with qtest_init() and intercepts those itself.
 */
static inline bool hera_irq(QTestState *qts, int n)
{
    return qtest_get_irq(qts, n);
}

#endif
