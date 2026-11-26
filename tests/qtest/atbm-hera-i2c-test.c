/*
 * QTest testcase for the ATBM6441 "Hera" I2C master
 *
 * Transactions as the SDK's i2c.c issues them (CTL phases, a byte count,
 * the CMD register) against an EEPROM on the bus: write, read, a repeated
 * start, a NACKed address, the FIFO pacing past its depth and the
 * interrupt on chip number 22.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define CFG     0x10
#define INTEN   0x14
#define INTST   0x18
#define ADDR    0x1c
#define DATA    0x20
#define CTL     0x24
#define CMD     0x28
#define SETUP   0x2c

#define F_EMPTY BIT(0)
#define F_FULL  BIT(1)
#define F_HALF  BIT(2)
#define F_STOP  BIT(5)
#define F_START BIT(6)
#define F_BYTETRANS BIT(7)
#define F_BYTERECV BIT(8)
#define F_CMPL  BIT(9)
#define S_ACK   BIT(10)
#define S_BUSY  BIT(11)

#define C_START BIT(12)
#define C_ADDR  BIT(11)
#define C_DATA  BIT(10)
#define C_STOP  BIT(9)
#define C_READ  BIT(8)
#define C_ALL   (C_START | C_ADDR | C_DATA | C_STOP)

#define CMD_TRANS    1
#define CMD_FIFO_CLR 4
#define CMD_RESET    5

#define SETUP_EN     BIT(0)
#define SETUP_MASTER BIT(2)

#define EEPROM  0x50

static uint32_t rd(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, HERA_I2C + reg);
}

static void wr(QTestState *qts, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, HERA_I2C + reg, v);
}

static QTestState *start(void)
{
    QTestState *qts = hera_init("-device at24c-eeprom,bus=i2c,"
                                "address=0x50,rom-size=256");

    wr(qts, SETUP, SETUP_EN | SETUP_MASTER);
    return qts;
}

static void transact(QTestState *qts, uint32_t ctl, uint32_t addr, int count)
{
    wr(qts, ADDR, addr);
    wr(qts, CTL, ctl | (count & 0xff));
    wr(qts, CMD, CMD_TRANS);
}

static void test_reset(void)
{
    QTestState *qts = hera_init("");

    g_assert_cmphex(rd(qts, CFG), ==, 1);
    g_assert_cmphex(rd(qts, INTEN), ==, 0);
    g_assert_cmphex(rd(qts, INTST) & 0x3ff, ==, F_EMPTY);
    g_assert_cmphex(rd(qts, INTST) & (S_ACK | S_BUSY), ==, 0);
    g_assert_cmphex(rd(qts, CTL), ==, C_ALL);
    g_assert_cmphex(rd(qts, SETUP) & 7, ==, 0);
    qtest_quit(qts);
}

static void test_write_read(void)
{
    QTestState *qts = start();

    /* the word address 0x10, then 3 data bytes: 4 bytes in all */
    wr(qts, DATA, 0x10);
    wr(qts, DATA, 0xaa);
    wr(qts, DATA, 0xbb);
    wr(qts, DATA, 0xcc);
    transact(qts, C_ALL, EEPROM, 4);
    g_assert_cmphex(rd(qts, INTST) & (F_CMPL | F_STOP | F_START | S_ACK), ==,
                    F_CMPL | F_STOP | F_START | S_ACK);
    g_assert_cmphex(rd(qts, INTST) & S_BUSY, ==, 0);
    wr(qts, INTST, F_CMPL | F_STOP | F_START | F_BYTETRANS);
    g_assert_cmphex(rd(qts, INTST) & 0x3e0, ==, 0);

    /* set the read address, then read: a write and a read transaction */
    wr(qts, DATA, 0x10);
    transact(qts, C_ALL, EEPROM, 1);
    wr(qts, INTST, F_CMPL | F_STOP | F_START | F_BYTETRANS);
    transact(qts, C_ALL | C_READ, EEPROM, 3);
    g_assert_cmphex(rd(qts, INTST) & F_CMPL, ==, F_CMPL);
    g_assert_cmphex(rd(qts, DATA), ==, 0xaa);
    g_assert_cmphex(rd(qts, DATA), ==, 0xbb);
    g_assert_cmphex(rd(qts, DATA), ==, 0xcc);
    g_assert_cmphex(rd(qts, INTST) & F_EMPTY, ==, F_EMPTY);
    qtest_quit(qts);
}

/* a repeated start: the write with no stop, then the read with a start */
static void test_repeated_start(void)
{
    QTestState *qts = start();

    wr(qts, DATA, 0x20);
    wr(qts, DATA, 0x5a);
    transact(qts, C_ALL, EEPROM, 2);
    wr(qts, INTST, F_CMPL | F_STOP | F_START | F_BYTETRANS);

    wr(qts, DATA, 0x20);
    transact(qts, C_START | C_ADDR | C_DATA, EEPROM, 1);
    g_assert_cmphex(rd(qts, INTST) & (S_BUSY | F_STOP), ==, S_BUSY);
    transact(qts, C_ALL | C_READ, EEPROM, 1);
    g_assert_cmphex(rd(qts, DATA), ==, 0x5a);
    g_assert_cmphex(rd(qts, INTST) & S_BUSY, ==, 0);
    qtest_quit(qts);
}

static void test_nack(void)
{
    QTestState *qts = start();

    wr(qts, DATA, 0x00);
    transact(qts, C_ALL, 0x33, 1);
    g_assert_cmphex(rd(qts, INTST) & (F_CMPL | S_ACK), ==, F_CMPL);
    g_assert_cmphex(rd(qts, INTST) & S_BUSY, ==, 0);

    /* and an address that answers is an ACK */
    wr(qts, INTST, F_CMPL);
    wr(qts, DATA, 0x00);
    transact(qts, C_ALL, EEPROM, 1);
    g_assert_cmphex(rd(qts, INTST) & (F_CMPL | S_ACK), ==, F_CMPL | S_ACK);
    qtest_quit(qts);
}

/* more bytes than the FIFO holds: the port is written as it drains */
static void test_fifo_pacing(void)
{
    QTestState *qts = start();

    transact(qts, C_ALL, EEPROM, 9);
    g_assert_cmphex(rd(qts, INTST) & F_CMPL, ==, 0);
    wr(qts, DATA, 0x40);
    for (int i = 0; i < 8; i++) {
        wr(qts, DATA, 0x60 + i);
    }
    g_assert_cmphex(rd(qts, INTST) & F_CMPL, ==, F_CMPL);
    wr(qts, INTST, F_CMPL | F_STOP);

    wr(qts, DATA, 0x40);
    transact(qts, C_ALL, EEPROM, 1);
    wr(qts, INTST, F_CMPL | F_STOP);
    /* 7 bytes read through a FIFO of 4 */
    transact(qts, C_ALL | C_READ, EEPROM, 7);
    g_assert_cmphex(rd(qts, INTST) & (F_CMPL | F_FULL), ==, F_FULL);
    for (int i = 0; i < 7; i++) {
        g_assert_cmphex(rd(qts, DATA), ==, 0x60 + i);
    }
    g_assert_cmphex(rd(qts, INTST) & (F_CMPL | F_EMPTY), ==, F_CMPL | F_EMPTY);
    qtest_quit(qts);
}

static void test_irq_and_fifo_clear(void)
{
    QTestState *qts = start();

    wr(qts, INTEN, F_CMPL);
    g_assert_false(hera_irq(qts, HERA_IRQ_I2C));
    wr(qts, DATA, 0x00);
    transact(qts, C_ALL, EEPROM, 1);
    g_assert_true(hera_irq(qts, HERA_IRQ_I2C));
    wr(qts, INTST, F_CMPL);
    g_assert_false(hera_irq(qts, HERA_IRQ_I2C));

    /* the FIFO level interrupt: empty is up until a byte is written */
    wr(qts, INTEN, F_EMPTY);
    g_assert_true(hera_irq(qts, HERA_IRQ_I2C));
    wr(qts, DATA, 1);
    g_assert_false(hera_irq(qts, HERA_IRQ_I2C));
    wr(qts, CMD, CMD_FIFO_CLR);
    g_assert_true(hera_irq(qts, HERA_IRQ_I2C));

    /* without master mode nothing starts */
    wr(qts, INTEN, 0);
    wr(qts, SETUP, 0);
    transact(qts, C_ALL, EEPROM, 1);
    g_assert_cmphex(rd(qts, INTST) & F_CMPL, ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/i2c/reset", test_reset);
    qtest_add_func("/atbm-hera/i2c/write-read", test_write_read);
    qtest_add_func("/atbm-hera/i2c/repeated-start", test_repeated_start);
    qtest_add_func("/atbm-hera/i2c/nack", test_nack);
    qtest_add_func("/atbm-hera/i2c/fifo-pacing", test_fifo_pacing);
    qtest_add_func("/atbm-hera/i2c/irq-and-fifo-clear",
                   test_irq_and_fifo_clear);
    return g_test_run();
}
