/*
 * lmac_qemu: a stand-in for the ATBM6441's lower MAC, for QEMU
 *
 * The SDK's libwifi.a holds both halves of the chip's Wi-Fi: the upper MAC
 * (HMAC: mac80211-like, the supplicant) and the lower MAC (LMAC: the WSM
 * firmware of the cw1200 family, driving the undocumented MAC and PHY).
 * They meet in hi_internal.o, a pair of message rings in memory: HMAC
 * writes a WSM request into a buffer the LMAC posted
 * (HiInternal_ProgramInput) and the LMAC gets it in SYS_HiReceive; the LMAC
 * hands confirms and indications back through HiInternal_ProgOutput and
 * hears in SYS_HiIndMsgSent when HMAC is done with an indication.
 *
 * Linked in place of the LMAC objects, this file keeps hi_internal.o and
 * implements the LMAC side of it by passing every message to QEMU's
 * paravirtual port (atbm-hera-lmac), which answers with its WSM firmware
 * model and a simulated access point.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <string.h>
#include "lmac_qemu.h"

/*
 * Buffers: where the vendor LMAC kept them in its packet memory
 * (SM_Region, sm_region.o, kept), which it no longer uses. They must be
 * in SRAM: the SDIO host interface addresses what it sends the host by
 * its offset in SRAM, and the SDIO build sends some of these. Offsets and
 * counts are the vendor HiMsg_Init's in each build:
 *   input        HMAC's requests; 1632 bytes, 0x668 apart; as many as
 *                hi_internal.o's input ring holds (32 in the AT build,
 *                4 in the SDIO build, where the SDIO input shares them)
 *   small/large  indications: up to 0x200 bytes, and the larger ones
 *                (received frames). The vendor's large ones are 0x600
 *                apart, short of a received frame of 1500 bytes of data
 *                with its descriptor, CCMP header and MIC (1568): its
 *                LMAC hands frames over in its receive FIFO instead.
 *                Ours are 0x680, in the room that FIFO leaves behind.
 */
#define IN_STRIDE           0x668
#define IN_SIZE             0x660
#define SMALL_SIZE          0x200
#define LARGE_SIZE          0x680
#ifdef LMAC_QEMU_SDIO
#define IN_OFF              0x7680
#define NUM_IN              4
#define SMALL_OFF           0x15d20
#define NUM_SMALL           16
#define LARGE_OFF           0x17d20
#define NUM_LARGE           8
#else
#define IN_OFF              0xb530
#define NUM_IN              32
#define SMALL_OFF           0x18230
#define NUM_SMALL           4
#define LARGE_OFF           (SMALL_OFF + NUM_SMALL * SMALL_SIZE)
#define NUM_LARGE           4
#endif

/* the lmac scheduler event for "the port has messages" */
#define EV_PORT             0x00100000u

/*
 * The power-busy bit the shim holds while the radio must stay on: during
 * a scan, and while joined with the station's power save off. Power-busy
 * bits keep port.o's idle hook from putting the chip in light sleep.
 */
#define PWR_BUSY_QEMU       0x80000000u

/* WSM ids the power-busy rule follows, less interface and sequence bits */
#define WSM_START_SCAN      0x0007
#define WSM_RESET           0x000a
#define WSM_JOIN            0x000b
#define WSM_SET_PM          0x0010
#define WSM_SCAN_COMPLETE   0x0806
#define WSM_PSM_PS          0x01

/* the SMU's hardware control status, which the LMAC's wake sets */
#define SMU_HW_CTRL_ST      0x1610013cu

/* listen: the beacon interval the port waits before a frame wakes us */
#define LISTEN_MS           102

static int port_ps;         /* the port has the chip's power save (ABI 2) */
static int scanning, joined, pm_ps;

/*
 * hi_internal.o numbers what it hands HMAC itself (bits 13-15 of the id,
 * per ring) and only clears two of the bits: what goes to it must have
 * none set
 */
#define WSM_ID_MASK         0x1fff
/* the id less its interface (bits 6-9): what kind of message */
#define WSM_ID_TYPE         0x1c3f
#define WSM_RECEIVE_IND     0x0804
#define WSM_CNF             0x0400
#define WSM_IND             0x0800

typedef struct pool {
    unsigned size;
    unsigned nfree;
    void *free[NUM_SMALL > NUM_LARGE ? NUM_SMALL : NUM_LARGE];
} pool;

static pool small_pool = { SMALL_SIZE };
static pool large_pool = { LARGE_SIZE };
static int ind_starved;

/*
 * The LMAC's globals that the kept objects reach into. Only what they use
 * matters; sizes are the originals' so no neighbour is overrun.
 * sPlatformGlobal: +0 pending scheduler events, +4 power-busy bits, +8 the
 * hardware id and sub id, +0xc a count, +0x10 the capabilities.
 * sPlatformLocal: +0 a flag, +4 the random seed.
 */
uint32_t sPlatformGlobal[0x50 / 4] __attribute__((section(".dtcm_data"))) = {
    [2] = 0x64410008, [3] = 0x14a, [4] = 0x3d,
};
uint32_t sPlatformLocal[0x28 / 4] __attribute__((section(".dtcm_data"))) = {
    [0] = 1, [1] = 0x12345678,
};
uint8_t sLmcGlobal[0x2ee8] __attribute__((aligned(4), section(".dtcm_zi")));
uint8_t sLmcLocal[0x4cc] __attribute__((aligned(4), section(".dtcm_zi")));
/* +0x54 and +0x5c are hi_internal.o's counts of received frames */
uint8_t sMibTable[0xb0] __attribute__((aligned(4), section(".dtcm_zi")));
const char SYS_SwTitle[] = "lmac_qemu (QEMU paravirtual LMAC)";

static void pool_put(pool *p, void *buf)
{
    uint32_t psw = OS_DisableFiqIrq_Internal();

    p->free[p->nfree++] = buf;
    OS_RestoreFiqIrq_Internal(psw);
}

static void *pool_get(pool *p)
{
    uint32_t psw = OS_DisableFiqIrq_Internal();
    void *buf = p->nfree ? p->free[--p->nfree] : 0;

    OS_RestoreFiqIrq_Internal(psw);
    return buf;
}

static pool *pool_for(unsigned size)
{
    return size <= SMALL_SIZE ? &small_pool : &large_pool;
}

static void pools_init(void)
{
    unsigned i;

    small_pool.nfree = large_pool.nfree = 0;
    for (i = 0; i < NUM_SMALL; i++) {
        pool_put(&small_pool, SM_Region + SMALL_OFF + i * SMALL_SIZE);
    }
    for (i = 0; i < NUM_LARGE; i++) {
        pool_put(&large_pool, SM_Region + LARGE_OFF + i * LARGE_SIZE);
    }
}

static void busy_update(void)
{
    if (scanning || (joined && !pm_ps)) {
        lmac_SetPowerBusyEvent(PWR_BUSY_QEMU);
    } else {
        lmac_ClearPowerBusyEvent(PWR_BUSY_QEMU);
    }
}

/*
 * What a request says about the radio's power: its id, its first body
 * byte (a power save mode) and whether its confirm said success
 */
static void busy_track(unsigned id, uint8_t arg, int ok)
{
    switch (id & 0x3f) {
    case WSM_START_SCAN:
        scanning = 1;
        break;
    case WSM_JOIN:
        joined = ok;
        break;
    case WSM_RESET:
        joined = 0;
        break;
    case WSM_SET_PM:
        pm_ps = arg & WSM_PSM_PS;
        break;
    default:
        return;
    }
    busy_update();
}

/*
 * The LMAC's indication buffer allocator, which the SDIO host interface
 * and the SDIO demo use for their own messages to the host
 */
void *HiMsg_RequestIndBuf(unsigned size)
{
    return size <= SMALL_SIZE ? pool_get(&small_pool) : 0;
}

int HiMsg_IndBufRemainCount(void)
{
    return small_pool.nfree;
}

/* a buffer for a copy of a received frame, for the SDIO host */
void *lmac_qemu_rx_buf(void)
{
    return pool_get(&large_pool);
}

/*
 * Where a received frame goes besides HMAC: nowhere in the AT build; the
 * SDIO build's sdio.c replaces this
 */
__attribute__((weak)) int lmac_qemu_rx_route(void *msg)
{
    (void)msg;
    return LMAC_QEMU_RX_HMAC;
}

/* The port's line: mask it, and let the LMAC task drain the port */
static void port_irq(unsigned prio)
{
    (void)prio;
    ql_write(QL_IRQ_EN, 0);
    lmac_IrqSetEvent(EV_PORT);
}

static void port_drain(void)
{
    uint32_t st;

    while ((st = ql_read(QL_STATUS)) & QL_STATUS_PEND) {
        pool *p = pool_for(st >> 16);
        uint8_t *buf = pool_get(p);
        uint32_t len;

        if (!buf) {
            ind_starved = 1;
            return;
        }
        ql_write(QL_IND_ADDR, (uint32_t)buf);
        ql_write(QL_IND_SIZE, p->size);
        len = ql_read(QL_IND_POP);
        /*
         * A confirm here would be recycled as an input buffer: the port
         * gives every confirm in place, so this is a model bug
         */
        if (!len || (((uint16_t *)buf)[1] & (WSM_CNF | WSM_IND)) == WSM_CNF) {
            if (len) {
                printf("lmac_qemu: stray confirm 0x%x\n", ((uint16_t *)buf)[1]);
            }
            pool_put(p, buf);
            continue;
        }
        ((uint16_t *)buf)[1] &= WSM_ID_MASK;
        if ((((uint16_t *)buf)[1] & WSM_ID_TYPE) == WSM_SCAN_COMPLETE) {
            scanning = 0;
            busy_update();
        }
        if ((((uint16_t *)buf)[1] & WSM_ID_TYPE) == WSM_RECEIVE_IND) {
            int r = lmac_qemu_rx_route(buf);

            if (r == LMAC_QEMU_RX_TAKEN) {
                continue;
            }
            if (r == LMAC_QEMU_RX_DROP) {
                SYS_HiIndMsgSent(buf);
                continue;
            }
        }
        HiInternal_ProgOutput(buf);
    }
    ql_write(QL_IRQ_EN, 1);
}

/*
 * A WSM request from HMAC, in one of our input buffers: the port answers
 * in place, and the buffer goes back as the confirm, or straight back to
 * the input ring when there is none (a transmit without a confirm)
 */
int lmac_qemu_request(void *msg, unsigned size)
{
    uint8_t *b = msg;
    unsigned id = b[2];
    uint8_t arg = b[4];

    ql_write(QL_REQ_SIZE, size);
    ql_write(QL_REQ, (uint32_t)msg);
    if (!(ql_read(QL_REQ_RESULT) & QL_RESULT_CNF)) {
        busy_track(id, arg, 0);
        return 0;
    }
    ((uint16_t *)msg)[1] &= WSM_ID_MASK;
    /* the confirm is in the buffer now: its status opens the body */
    busy_track(id, arg, !(b[4] | b[5] | b[6] | b[7]));
    return 1;
}

void SYS_HiReceive(void *msg)
{
    if (lmac_qemu_request(msg, IN_SIZE)) {
        HiInternal_ProgOutput(msg);
    } else {
        HiInternal_ProgramInput(msg);
    }
}

/* HMAC, or the SDIO host, is done with an indication */
void SYS_HiIndMsgSent(void *msg)
{
    uint8_t *b = msg;

    pool_put(b >= SM_Region + LARGE_OFF &&
             b < SM_Region + LARGE_OFF + NUM_LARGE * LARGE_SIZE ?
             &large_pool : &small_pool, msg);
    if (ind_starved) {
        ind_starved = 0;
        lmac_SetEvent(EV_PORT);
    }
}

void lmac_SetPowerBusyEvent(uint32_t bits)
{
    uint32_t psw = OS_DisableFiqIrq_Internal();

    sPlatformGlobal[1] |= bits;
    OS_RestoreFiqIrq_Internal(psw);
}

void lmac_ClearPowerBusyEvent(uint32_t bits)
{
    uint32_t psw = OS_DisableFiqIrq_Internal();

    sPlatformGlobal[1] &= ~bits;
    OS_RestoreFiqIrq_Internal(psw);
}

/*
 * The power: what the original's lmac_pwr.o and main.o did, short of the
 * radio. pwr_ARESA.o (kept) does the sleep itself.
 */
void lmac_PowerInit(void)
{
    /* the flag the original sets in the LMAC's globals first */
    sLmcGlobal[0x60b] = 1;
    PWR_Init();
}

/* port.o: the chip is about to sleep, with the radio's state kept */
void SYS_PowerSuspend(void)
{
    if (port_ps) {
        ql_write(QL_PS, QL_PS_SLEEP);
    }
}

/* port.o, after the sleep: the chip is back and the radio with it */
void lmac_Wakeup(void)
{
    PWR_Wakeup();
    sPlatformLocal[8]++;
    APB_PowerResume();
    TI_PowerResume();
    TI_PowerResume_Ext();
    if (port_ps) {
        ql_write(QL_PS, 0);
    }
    *(volatile uint32_t *)SMU_HW_CTRL_ST = 1; /* MMIO */
}

/* The LMAC task: what the original's lmac_Main does short of the radio */
void lmac_Main(void)
{
    void *in[NUM_IN];
    unsigned i;

    change_cpu_freq(1);
    OS_DisableFiqIrq_Internal();
    APB_Init();
    printf("%s\n", SYS_SwTitle);
    lmac_PowerInit();
    HIF_Init();
    TI_Init();

    if (ql_read(QL_ID) != QL_MAGIC || ql_read(QL_VERSION) < QL_ABI_MIN) {
        printf("lmac_qemu: no QEMU LMAC port at 0x%x (id 0x%x)\n",
               QL_BASE, ql_read(QL_ID));
    }
    port_ps = ql_read(QL_VERSION) >= QL_ABI;
    if (port_ps) {
        ql_write(QL_WAKE_MASK, QL_WAKE_UNICAST | QL_WAKE_GROUP);
        ql_write(QL_WAKE_DELAY, LISTEN_MS);
    }
    ql_write(QL_CTRL, 0);
    pools_init();
    lmac_RegisterEventHandler(EV_PORT, port_drain);
    IRQ_RegisterHandler(QL_IRQ, port_irq);

    /* waits for HMAC to come up; leaves interrupts as it found them */
    for (i = 0; i < NUM_IN; i++) {
        in[i] = SM_Region + IN_OFF + i * IN_STRIDE;
    }
    HiInternal_Init(in, NUM_IN);

    /* the model's startup indication follows */
    ql_write(QL_IRQ_EN, 1);
    ql_write(QL_CTRL, QL_CTRL_RUN);
    printf("lmac_qemu: port running\n");
    OS_EnableFiqIrq();
    lmac_Scheduler();
}
