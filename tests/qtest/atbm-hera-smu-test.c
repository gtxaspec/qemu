/*
 * QTest testcase for the ATBM6441 "Hera" SMU/PMU
 *
 * What the SDK's code finds there: the bootrom's state, the sleep clock
 * calibration the watchdog and sleep code wait on, the AUXADC, the
 * retention RAM, the wake pins and the CPU hold line, with registers that
 * read back what was written and are reached by bytes and halves too.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define CLK_SEL     0x010
#define CLK_EN      0x014
#define CALIB_CTRL  0x080
#define CALIB_NUM   0x088
#define CALIB_CNT   0x08c
#define CALIB_SW    0x090
#define CALIB_ST    0x098
#define HW_CTRL_ST  0x13c
#define WAKE_CTRL   0x140
#define CPU_CTRL    0x1000
#define BOOT_STATE  0x1028
#define STRAP       0x1030
#define WAKE_CFG    0x1084
#define ADC_CTRL    0x1400
#define ADC_DATA    0x1418
#define RET_RAM     0x1800
#define WAKE_ST     0x1a0

static uint32_t rd(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, HERA_SMU + reg);
}

static void wr(QTestState *qts, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, HERA_SMU + reg, v);
}

static void test_reset(void)
{
    QTestState *qts = hera_init("");

    g_assert_cmphex(rd(qts, BOOT_STATE), ==, 0);
    g_assert_cmphex(rd(qts, CLK_SEL), ==, 0);
    g_assert_cmphex(rd(qts, CLK_EN), ==, 0);
    g_assert_cmphex(rd(qts, CALIB_ST), ==, 0);
    g_assert_cmphex(rd(qts, STRAP), ==, 0);
    g_assert_cmphex(rd(qts, CPU_CTRL), ==, 0);

    /* nothing is known of it but that it keeps what is written */
    wr(qts, BOOT_STATE, 5);
    g_assert_cmphex(rd(qts, BOOT_STATE), ==, 5);
    qtest_quit(qts);
}

static void test_registers(void)
{
    QTestState *qts = hera_init("");

    wr(qts, CLK_EN, 0x3fffffff);
    g_assert_cmphex(rd(qts, CLK_EN), ==, 0x3fffffff);
    wr(qts, HW_CTRL_ST, 1);
    wr(qts, WAKE_CTRL, 3);
    g_assert_cmphex(rd(qts, HW_CTRL_ST), ==, 1);
    g_assert_cmphex(rd(qts, WAKE_CTRL), ==, 3);

    /* the retention RAM, by bytes, halves and words */
    wr(qts, RET_RAM, 0x11223344);
    qtest_writeb(qts, HERA_SMU + RET_RAM + 1, 0xaa);
    g_assert_cmphex(rd(qts, RET_RAM), ==, 0x1122aa44);
    qtest_writew(qts, HERA_SMU + RET_RAM + 2, 0xbbcc);
    g_assert_cmphex(rd(qts, RET_RAM), ==, 0xbbccaa44);
    g_assert_cmphex(qtest_readb(qts, HERA_SMU + RET_RAM + 3), ==, 0xbb);
    g_assert_cmphex(qtest_readw(qts, HERA_SMU + RET_RAM), ==, 0xaa44);
    wr(qts, RET_RAM + 4 * 191, 0xdeadbeef);
    g_assert_cmphex(rd(qts, RET_RAM + 4 * 191), ==, 0xdeadbeef);
    qtest_quit(qts);
}

/* hal_wdt_timer_calibration(): start, wait for CALIB_ST[0], read SW */
static void test_calibration(void)
{
    QTestState *qts = hera_init("");

    wr(qts, CALIB_NUM, 10);
    wr(qts, CALIB_CTRL, 1);
    g_assert_cmphex(rd(qts, CALIB_ST) & 1, ==, 1);
    g_assert_cmpuint(rd(qts, CALIB_SW), ==, 12210);
    g_assert_cmpuint(rd(qts, CALIB_CNT), ==, 12210);

    /* the status is not writable; clearing the start bit clears it */
    wr(qts, CALIB_ST, 0);
    g_assert_cmphex(rd(qts, CALIB_ST) & 1, ==, 1);
    wr(qts, CALIB_CTRL, 0);
    g_assert_cmphex(rd(qts, CALIB_ST) & 1, ==, 0);

    /* the software value the watchdog writes back is kept */
    wr(qts, CALIB_SW, 0x2f00);
    wr(qts, CALIB_CTRL, 3);
    g_assert_cmphex(rd(qts, CALIB_CTRL), ==, 3);
    qtest_quit(qts);
}

static void test_adc(void)
{
    QTestState *qts = hera_init("-global atbm-hera-smu.adc-code=0x1a7");

    g_assert_cmphex(rd(qts, ADC_DATA), ==, 0);
    wr(qts, ADC_CTRL, rd(qts, ADC_CTRL) | 1);
    /* 0x1a7 * 1000 / 423 = 1000 mV, as HAL_ADC_read works it out */
    g_assert_cmpuint(rd(qts, ADC_DATA) & 0x3ff, ==, 423);
    wr(qts, ADC_CTRL, rd(qts, ADC_CTRL) & ~1u);
    g_assert_cmphex(rd(qts, ADC_DATA), ==, 0);
    qtest_quit(qts);
}

static void wake_pin(QTestState *qts, int n, int level)
{
    qtest_set_irq_in(qts, "/machine/smu", "wake", n, level);
}

/* hal_ps_gpio_wakeup_set(): enable [15:0], level [31:16] (1 for low) */
static void test_wake(void)
{
    QTestState *qts = hera_init("");

    /* pin 5 wakes on low, and a pin nobody drives is low */
    wr(qts, WAKE_CFG, 1 << 3 | 1 << 5 | 1 << (16 + 5));
    g_assert_cmphex(rd(qts, WAKE_ST), ==, 1 << 5);
    wake_pin(qts, 3, 1);
    g_assert_cmphex(rd(qts, WAKE_ST), ==, 1 << 3 | 1 << 5);
    wake_pin(qts, 5, 1);
    g_assert_cmphex(rd(qts, WAKE_ST), ==, 1 << 3);
    wake_pin(qts, 5, 0);
    g_assert_cmphex(rd(qts, WAKE_ST), ==, 1 << 3 | 1 << 5);
    wake_pin(qts, 3, 0);
    wake_pin(qts, 7, 1);
    g_assert_cmphex(rd(qts, WAKE_ST), ==, 1 << 5);

    /* hal_ps_gpio_wakeup_clear() */
    wr(qts, WAKE_CFG, 0);
    g_assert_cmphex(rd(qts, WAKE_ST), ==, 0);
    qtest_quit(qts);
}

static void test_cpu_hold(void)
{
    QTestState *qts = qtest_init("-machine atbm-hera -cpu " HERA_CPU "");

    qtest_irq_intercept_out_named(qts, "/machine/smu", "cpu-hold");
    g_assert_false(qtest_get_irq(qts, 0));
    wr(qts, CPU_CTRL, 1 << 8);
    g_assert_true(qtest_get_irq(qts, 0));
    wr(qts, CPU_CTRL, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

/*
 * The power controller: the sleep request, the core's standby, the wake
 * sources, deep sleep's timer (atbm-hera-smu.c; the sequences are
 * pwr_ARESA.o's)
 */
#define BOOT_RST    0x102c
#define DS_CFG      0x10a8
#define DS_CNT_LO   0x10ac
#define DS_CNT_HI   0x10b0
#define CLK_DIV     0x10e4
#define PWR_BD      0x10fc
#define CALIB_RES   0x084
#define BD_LIGHT    0x80000000u
#define BD_RTC1     (1u << 1)
#define RTC_MS_CNT  0x04
#define RTC_MS_NUM  0x0c
#define RTC_CNTR    0x10
#define RTC_ALARM   0x14
#define RTC_EN      0x18
#define MSEC        1000000LL

static QTestState *power_init(const char *args)
{
    QTestState *qts = qtest_initf("-machine atbm-hera%s -cpu " HERA_CPU,
                                  args);

    qtest_irq_intercept_out_named(qts, "/machine/smu", "sleep");
    return qts;
}

static void standby(QTestState *qts)
{
    qtest_set_irq_in(qts, "/machine/smu", "standby", 0, 1);
    qtest_set_irq_in(qts, "/machine/smu", "standby", 0, 0);
}

/* CPU_RETENTION_FUNC's: the retained flag, the sleep request, the ILM */
static void light_sleep(QTestState *qts)
{
    wr(qts, PWR_BD, BD_LIGHT | BD_RTC1);
    wr(qts, STRAP, rd(qts, STRAP) | 1);
    wr(qts, BOOT_RST, HERA_ILM | 3);
    wr(qts, CPU_CTRL, rd(qts, CPU_CTRL) | 1);
    standby(qts);
}

/* PWR_StartRTC: the alarm after t of the RTC's 32.768 kHz "ms" */
static void rtc_alarm(QTestState *qts, uint32_t t)
{
    qtest_writel(qts, HERA_RTC + RTC_MS_CNT, 0);
    qtest_writel(qts, HERA_RTC + RTC_CNTR, 0);
    qtest_writel(qts, HERA_RTC + RTC_EN, 0);
    qtest_writel(qts, HERA_RTC + RTC_ALARM, t / 3600000 << 12 |
                 t % 3600000 / 60000 << 6 | t % 60000 / 1000);
    qtest_writel(qts, HERA_RTC + RTC_MS_NUM, t % 1000);
    qtest_writel(qts, HERA_RTC + RTC_EN, 7);
}

static void test_standby(void)
{
    QTestState *qts = power_init("");

    /* standby alone is the core's own business */
    wr(qts, PWR_BD, BD_LIGHT | BD_RTC1);
    standby(qts);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_light_rtc(void)
{
    QTestState *qts = power_init("");

    /* 100 ms: 3277 ticks */
    rtc_alarm(qts, 3277 - 1);
    light_sleep(qts);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 90 * MSEC);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 20 * MSEC);
    g_assert_false(qtest_get_irq(qts, 0));
    /* the request is done, the retained flag and the boot address stay */
    g_assert_cmphex(rd(qts, CPU_CTRL) & 1, ==, 0);
    g_assert_cmphex(rd(qts, STRAP), ==, 1);
    g_assert_cmphex(rd(qts, BOOT_RST), ==, HERA_ILM | 3);
    /* PWR_GetSleepedTime: the count is valid, and as long as the sleep */
    g_assert_cmphex(qtest_readl(qts, HERA_RTC + RTC_MS_CNT) >> 31, ==, 1);
    qtest_quit(qts);
}

static void test_light_rtc_off(void)
{
    QTestState *qts = power_init("");

    /* PWR_LightSleep_RTCWakeup(0): the alarm does not wake */
    rtc_alarm(qts, 3277 - 1);
    light_sleep(qts);
    wr(qts, PWR_BD, BD_LIGHT);
    qtest_clock_step(qts, 500 * MSEC);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

/* a GPIO wake pin, driven by the board's PIR line on pad 2 (wake id 2) */
static void test_light_gpio(void)
{
    QTestState *qts = power_init(",pir-pad=2");

    wr(qts, WAKE_CFG, 1 << 2);
    light_sleep(qts);
    qtest_clock_step(qts, 1000 * MSEC);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_qom_set_bool(qts, "/machine", "pir", true);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(rd(qts, WAKE_ST), ==, 1 << 2);

    /* a wake pin already at its level: the sleep ends at once */
    light_sleep(qts);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

/* PWR_GotoDeepSleep(ms): the timer, then a cold boot (a machine reset) */
static void test_deep(void)
{
    QTestState *qts = power_init("");
    uint64_t v;

    /* the calibration the firmware reads its "cal" from, and div 8 */
    wr(qts, CALIB_NUM, 1);
    wr(qts, CALIB_CTRL, 1);
    g_assert_cmphex(rd(qts, CALIB_RES) >> 5, ==, 1221);
    wr(qts, CLK_DIV, 3 << 20);
    /* 500 ms */
    v = (500ULL * 1221 / 8) >> 5;
    wr(qts, DS_CNT_LO, v);
    wr(qts, DS_CNT_HI, v >> 32);
    wr(qts, DS_CFG, 1 << 10);
    wr(qts, CPU_CTRL, 1);
    standby(qts);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 400 * MSEC);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(rd(qts, DS_CFG), ==, 1 << 10);
    qtest_clock_step(qts, 200 * MSEC);
    /* the reset has put everything back */
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(rd(qts, DS_CFG), ==, 0);
    g_assert_cmphex(rd(qts, CPU_CTRL), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/smu/reset", test_reset);
    qtest_add_func("/atbm-hera/smu/registers", test_registers);
    qtest_add_func("/atbm-hera/smu/calibration", test_calibration);
    qtest_add_func("/atbm-hera/smu/adc", test_adc);
    qtest_add_func("/atbm-hera/smu/wake", test_wake);
    qtest_add_func("/atbm-hera/smu/cpu-hold", test_cpu_hold);
    qtest_add_func("/atbm-hera/smu/power/standby", test_standby);
    qtest_add_func("/atbm-hera/smu/power/light-rtc", test_light_rtc);
    qtest_add_func("/atbm-hera/smu/power/light-rtc-off", test_light_rtc_off);
    qtest_add_func("/atbm-hera/smu/power/light-gpio", test_light_gpio);
    qtest_add_func("/atbm-hera/smu/power/deep", test_deep);
    return g_test_run();
}
